/* fuzz_alloc.h -- allocation interposer used by the robustness harnesses.
 *
 * Why this exists: the bundled w64devkit gcc has no libasan/libubsan
 * (neither libasan.a nor libubsan.a is shipped), so the usual
 * -fsanitize=address build is not available on this machine.  This header
 * implements a small substitute that is linked into the fuzz harnesses only
 * (it never touches fvp/src): it replaces malloc/calloc/realloc/free for the
 * whole executable and checks every block it hands out.
 *
 * Two modes, chosen at compile time:
 *
 *   canary (default)  each block carries a 16-byte 0xA5 trailer and a magic
 *                     header; free() verifies both, so a heap buffer that was
 *                     written past its end is reported immediately.  Cheap
 *                     (~50-100 ns per allocation), used for the bulk run.
 *
 *   guard             (-DFUZZ_ALLOC_GUARD) every block ends exactly at a
 *                     PAGE_NOACCESS guard page (Electric Fence style), so an
 *                     out-of-bounds *read* or *write* faults at the exact
 *                     instruction.  Slower and needs one page per block, so it
 *                     is used for a smaller confirmation run.
 *
 * Both modes cap single allocations at FAQ_MAX_ALLOC (default 192 MiB) so a
 * forged size field cannot commit the whole machine; the caller sees NULL
 * (the engine's normal out-of-memory path), which is counted separately.
 *
 * Detection scope / known limits (documented in fvp/docs/testing.md):
 *   - canary mode catches heap *writes* past the end, double free, and
 *     wild pointers (magic check), but not OOB reads.
 *   - guard mode catches OOB reads and writes past the end, plus use after
 *     free (the pages are released); it does not catch underruns (the slack
 *     is at the beginning of the block).
 *   - stack overflows are not instrumented; the harness binaries are built
 *     with -fstack-protector-all where the toolchain supports it.
 *
 * Usage: include this header from exactly ONE translation unit of the
 * harness (the unit that owns main); link every other object normally.
 */
#ifndef FVP_FUZZ_ALLOC_H
#define FVP_FUZZ_ALLOC_H

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#ifdef _WIN32
#include <windows.h>
#endif

#define FAQ_MAX_ALLOC (192ull * 1024ull * 1024ull) /* single-block cap */
#define FAQ_TRAILER 16u

#define FAQ_MAGIC_CANARY 0x46565143414E4152ull /* "FQVCANAR" */
#define FAQ_MAGIC_GUARD 0x4656514755415244ull  /* "FQVGUARD" */

typedef struct FaqBlk {
    uint64_t magic;
    size_t req;      /* requested payload size (never 0)               */
    void *base;      /* real allocation base (HeapAlloc / VirtualAlloc)*/
    size_t total;    /* size of that allocation                        */
    struct FaqBlk *next; /* live-block list for the exit-time sweep    */
    uint64_t pad;        /* keeps the header 16-byte aligned              */
} FaqBlk;

#define FAQ_HDR_SIZE ((size_t)sizeof(FaqBlk)) /* 48 bytes, 16-byte aligned */

static FaqBlk *faq_live;
static unsigned long long faq_allocs;
static unsigned long long faq_frees;
static unsigned long long faq_bytes_live;
static unsigned long long faq_peak;
static unsigned long long faq_corrupt;
static unsigned long long faq_oom;
static FILE *faq_journal;
static long faq_case_idx;
static char faq_case_cat[64];
static int faq_faulted;

static void faq_set_journal(FILE *f)
{
    faq_journal = f;
}

static void faq_set_case(long idx, const char *cat)
{
    faq_case_idx = idx;
    if (cat == NULL) {
        cat = "";
    }
    strncpy(faq_case_cat, cat, sizeof faq_case_cat - 1u);
    faq_case_cat[sizeof faq_case_cat - 1u] = '\0';
}

static void faq_report(const char *what)
{
    fprintf(stderr,
            "FAULT what=%s case=%ld category=%s allocs=%llu frees=%llu "
            "live=%llu corrupt=%llu\n",
            what, faq_case_idx, faq_case_cat, faq_allocs, faq_frees,
            faq_bytes_live, faq_corrupt);
    if (faq_journal != NULL) {
        fprintf(faq_journal, "FAULT\t%s\t%ld\t%s\n", what, faq_case_idx,
                faq_case_cat);
        fflush(faq_journal);
    }
    fflush(stderr);
}

static void faq_corruption(const char *what)
{
    faq_corrupt += 1u;
    faq_report(what);
    faq_faulted = 1;
    _Exit(72);
}

#ifdef _WIN32

/* Check a canary block's trailer; returns 0 when intact. */
static int faq_check_trailer(const FaqBlk *h)
{
    const unsigned char *p = (const unsigned char *)(h + 1) + h->req;
    unsigned i;
    for (i = 0; i < FAQ_TRAILER; ++i) {
        if (p[i] != 0xA5u) {
            return 0;
        }
    }
    return 1;
}

static void *faq_canary_alloc(size_t n)
{
    size_t req = n ? n : 1u;
    unsigned char *base;
    FaqBlk *h;
    if ((unsigned long long)req > FAQ_MAX_ALLOC) {
        faq_oom += 1u;
        return NULL;
    }
    base = (unsigned char *)HeapAlloc(GetProcessHeap(), 0,
                                      FAQ_HDR_SIZE + req + FAQ_TRAILER);
    if (base == NULL) {
        faq_oom += 1u;
        return NULL;
    }
    h = (FaqBlk *)base;
    h->magic = FAQ_MAGIC_CANARY;
    h->req = req;
    h->base = base;
    h->total = FAQ_HDR_SIZE + req + FAQ_TRAILER;
    memset(base + FAQ_HDR_SIZE + req, 0xA5, FAQ_TRAILER);
    h->next = faq_live;
    faq_live = h;
    faq_allocs += 1u;
    faq_bytes_live += (unsigned long long)req;
    if (faq_bytes_live > faq_peak) {
        faq_peak = faq_bytes_live;
    }
    return (void *)(base + FAQ_HDR_SIZE);
}

static void *faq_guard_alloc(size_t n)
{
    size_t req = n ? n : 1u;
    size_t need;
    size_t pages;
    size_t total;
    unsigned char *base;
    unsigned char *guard_start;
    uintptr_t start;
    FaqBlk *h;
    DWORD oldprot;

    if ((unsigned long long)req > FAQ_MAX_ALLOC) {
        faq_oom += 1u;
        return NULL;
    }
    need = req + FAQ_HDR_SIZE + 16u;
    pages = (need + 4095u) / 4096u;
    total = (pages + 1u) * 4096u;
    base = (unsigned char *)VirtualAlloc(NULL, total, MEM_RESERVE,
                                         PAGE_NOACCESS);
    if (base == NULL) {
        faq_oom += 1u;
        return NULL;
    }
    if (VirtualAlloc(base, pages * 4096u, MEM_COMMIT, PAGE_READWRITE) == NULL) {
        VirtualFree(base, 0, MEM_RELEASE);
        faq_oom += 1u;
        return NULL;
    }
    /* The reserved tail page is PAGE_NOACCESS already; make it explicit so a
     * reader of this file sees the guard. */
    (void)VirtualProtect(base + pages * 4096u, 4096u, PAGE_NOACCESS, &oldprot);
    guard_start = base + pages * 4096u;
    start = ((uintptr_t)(guard_start - req)) & ~(uintptr_t)15u;
    h = (FaqBlk *)(start - FAQ_HDR_SIZE);
    h->magic = FAQ_MAGIC_GUARD;
    h->req = req;
    h->base = base;
    h->total = total;
    h->next = faq_live;
    faq_live = h;
    faq_allocs += 1u;
    faq_bytes_live += (unsigned long long)req;
    if (faq_bytes_live > faq_peak) {
        faq_peak = faq_bytes_live;
    }
    return (void *)start;
}

static void faq_unlink(FaqBlk *h)
{
    FaqBlk **pp = &faq_live;
    while (*pp != NULL) {
        if (*pp == h) {
            *pp = h->next;
            return;
        }
        pp = &(*pp)->next;
    }
    faq_corruption("free of unknown block");
}

void *malloc(size_t n)
{
#ifdef FUZZ_ALLOC_GUARD
    return faq_guard_alloc(n);
#else
    return faq_canary_alloc(n);
#endif
}

void free(void *p)
{
    FaqBlk *h;
    if (p == NULL) {
        return;
    }
    h = ((FaqBlk *)p) - 1;
    if (h->magic == FAQ_MAGIC_CANARY) {
        if (!faq_check_trailer(h)) {
            faq_corruption("heap write past end");
        }
        faq_unlink(h);
        faq_frees += 1u;
        faq_bytes_live -= (unsigned long long)h->req;
        HeapFree(GetProcessHeap(), 0, h->base);
        return;
    }
    if (h->magic == FAQ_MAGIC_GUARD) {
        faq_unlink(h);
        faq_frees += 1u;
        faq_bytes_live -= (unsigned long long)h->req;
        VirtualFree(h->base, 0, MEM_RELEASE);
        return;
    }
    faq_corruption("free of a bad pointer (magic mismatch)");
}

void *calloc(size_t count, size_t size)
{
    size_t n;
    void *p;
    if (size != 0u && count > (SIZE_MAX / size)) {
        faq_oom += 1u;
        return NULL;
    }
    n = count * size;
    p = malloc(n);
    if (p != NULL && n != 0u) {
        memset(p, 0, n);
    }
    return p;
}

void *realloc(void *p, size_t n)
{
    FaqBlk *h;
    void *q;
    size_t old;
    if (p == NULL) {
        return malloc(n);
    }
    if (n == 0u) {
        free(p);
        return NULL;
    }
    h = ((FaqBlk *)p) - 1;
    if (h->magic != FAQ_MAGIC_CANARY && h->magic != FAQ_MAGIC_GUARD) {
        faq_corruption("realloc of a bad pointer");
    }
    old = h->req;
    q = malloc(n);
    if (q == NULL) {
        return NULL;
    }
    memcpy(q, p, old < n ? old : n);
    free(p);
    return q;
}

/* Verify every still-live block (trailer in canary mode, magic in guard
 * mode).  Returns the number of live blocks; *corrupt receives anomalies.
 * Prints one machine-readable line. */
static unsigned long long faq_check_live(int *leaked)
{
    FaqBlk *h = faq_live;
    unsigned long long live = 0;
    int bad = 0;
    while (h != NULL) {
        live += 1u;
#ifdef FUZZ_ALLOC_GUARD
        if (h->magic != FAQ_MAGIC_GUARD) {
            bad += 1;
        }
#else
        if (h->magic != FAQ_MAGIC_CANARY || !faq_check_trailer(h)) {
            bad += 1;
        }
#endif
        h = h->next;
    }
    if (bad != 0) {
        faq_corrupt += (unsigned long long)bad;
    }
    if (leaked != NULL) {
        *leaked = (int)live;
    }
    return live;
}

static void faq_print_stats(void)
{
    int leaked = 0;
    unsigned long long live = faq_check_live(&leaked);
    printf("ALLOC mode=%s allocs=%llu frees=%llu live=%llu live_bytes=%llu "
           "peak=%llu corrupt=%llu oom=%llu\n",
#ifdef FUZZ_ALLOC_GUARD
           "guard",
#else
           "canary",
#endif
           faq_allocs, faq_frees, live, faq_bytes_live, faq_peak,
           faq_corrupt, faq_oom);
    fflush(stdout);
}

#else /* !_WIN32: no interposition, keep the API but do not track. */

static void faq_set_journal(FILE *f)
{
    (void)f;
}
static void faq_set_case(long idx, const char *cat)
{
    (void)idx;
    (void)cat;
}
static void faq_corruption(const char *what)
{
    fprintf(stderr, "FAULT what=%s\n", what);
    _Exit(72);
}
static void faq_print_stats(void)
{
    printf("ALLOC mode=none allocs=0 frees=0 live=0 live_bytes=0 peak=0 "
           "corrupt=0 oom=0\n");
}

#endif /* _WIN32 */

#endif /* FVP_FUZZ_ALLOC_H */