/* fuzz_hcb.c -- in-process mutation fuzzer for the .hcb script loader + VM.
 *
 * Feeds structurally mutated .hcb scripts to the exact same loader (src/hcb.c)
 * and interpreter (src/vm.c) that fvm.exe uses, without paying for a process
 * spawn per case.  Two builds are made by fuzz_hcb.py:
 *
 *   fuzz_hcb.exe         heap-canary allocator  -> bulk run (>= 20k cases)
 *   fuzz_hcb_guard.exe   page-guard allocator   -> confirmation subset
 *
 * Classification per case: rejected (loader refused, with a diagnostic),
 * finished (threads all died), capped (step budget exhausted: the engine's
 * hang protection).  A process crash or a heap/guard fault is a failure: the
 * journal file lets fuzz_hcb.py regenerate the exact mutant with --repro.
 *
 * Usage (driven by tests/fuzz_hcb.py; see fvp/docs/testing.md):
 *   fuzz_hcb.exe <script.hcb|-> [--iters N] [--seed S] [--steps N]
 *                [--slice BYTES] [--slice-offset N] [--enc cp932|gbk]
 *                [--tmp PATH] [--journal PATH] [--csv PATH]
 *                [--dump-dir DIR] [--dump-every K] [--real-share P]
 *                [--synthetic-only] [--repro IDX --out PATH] [--quiet]
 *
 * Exit code: 0 = all cases safe, 2 = usage/setup error, 71 = memory fault,
 *            72 = heap corruption detected by the allocator.
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "fuzz_alloc.h"

#include "fvp.h"
#include "hcb.h"
#include "vm.h"

#ifdef _WIN32
#include <windows.h>
#endif

/* ------------------------------------------------------------------ */
/* PRNG (splitmix64; per-case state so --repro is trivial)             */
/* ------------------------------------------------------------------ */

typedef struct Rng {
    uint64_t s;
} Rng;

static uint64_t sm64(uint64_t x)
{
    x += 0x9E3779B97F4A7C15ull;
    x = (x ^ (x >> 30)) * 0xBF58476D1CE4E5B9ull;
    x = (x ^ (x >> 27)) * 0x94D049BB133111EBull;
    return x ^ (x >> 31);
}

static uint32_t rnd32(Rng *r)
{
    r->s = sm64(r->s);
    return (uint32_t)(r->s >> 32);
}

static uint32_t rnd_below(Rng *r, uint32_t n)
{
    return n == 0u ? 0u : rnd32(r) % n;
}

/* ------------------------------------------------------------------ */
/* corpus                                                             */
/* ------------------------------------------------------------------ */

typedef struct FuzzBase {
    unsigned char *data; /* full .hcb bytes: [u32 script_len][code][u32 main] */
    size_t len;
    const char *name;
    uint32_t orig_script_len;
    uint32_t *pcs; /* instruction offsets inside the code region */
    uint8_t *ops;
    uint32_t ninstr;
} FuzzBase;

#define MAX_BASES 32
#define CODE_CAP 8192

static FuzzBase g_bases[MAX_BASES];
static int g_nbases;

typedef struct Code {
    unsigned char b[CODE_CAP];
    size_t n;
} Code;

static void emit8(Code *c, uint8_t v)
{
    if (c->n < CODE_CAP) {
        c->b[c->n++] = v;
    }
}

static void emit16(Code *c, uint16_t v)
{
    emit8(c, (uint8_t)(v & 0xFFu));
    emit8(c, (uint8_t)(v >> 8));
}

static void emit32(Code *c, uint32_t v)
{
    emit8(c, (uint8_t)(v & 0xFFu));
    emit8(c, (uint8_t)((v >> 8) & 0xFFu));
    emit8(c, (uint8_t)((v >> 16) & 0xFFu));
    emit8(c, (uint8_t)((v >> 24) & 0xFFu));
}

/* pushstring body: u8 length (including the NUL), bytes, NUL */
static void emit_str_body(Code *c, const char *s)
{
    size_t n = strlen(s) + 1u;
    size_t i;
    if (n > 255u) {
        n = 255u;
    }
    emit8(c, (uint8_t)n);
    for (i = 0; i + 1u < n; ++i) {
        emit8(c, (uint8_t)s[i]);
    }
    emit8(c, 0);
}

/* complete pushstring instruction */
static void emit_pushstr(Code *c, const char *s)
{
    emit8(c, 0x0e);
    emit_str_body(c, s);
}

static void emit_op(Code *c, uint8_t op)
{
    emit8(c, op);
    switch (op) {
    case 0x01: /* initstack: b b */
        emit8(c, 4);
        emit8(c, 0);
        break;
    case 0x02:
    case 0x06:
    case 0x07:
    case 0x0a:
    case 0x0d:
        emit32(c, 4); /* self-ish target / int32 / float bits */
        break;
    case 0x03:
    case 0x0b:
    case 0x0f:
    case 0x15:
    case 0x17:
        emit16(c, 0);
        break;
    case 0x0c:
    case 0x10:
    case 0x12:
    case 0x16:
    case 0x18:
        emit8(c, 0);
        break;
    case 0x0e:
        emit_str_body(c, "x");
        break;
    default:
        break;
    }
}

static FuzzBase *base_new(const char *name, const Code *code, uint32_t main_start)
{
    FuzzBase *b;
    size_t total;
    if (g_nbases >= MAX_BASES) {
        return NULL;
    }
    total = 4u + code->n + 4u;
    b = &g_bases[g_nbases++];
    b->data = (unsigned char *)malloc(total);
    if (b->data == NULL) {
        fprintf(stderr, "fuzz_hcb: out of memory building corpus\n");
        exit(2);
    }
    b->data[0] = (unsigned char)((code->n + 4u) & 0xFFu);
    b->data[1] = (unsigned char)(((code->n + 4u) >> 8) & 0xFFu);
    b->data[2] = (unsigned char)(((code->n + 4u) >> 16) & 0xFFu);
    b->data[3] = (unsigned char)(((code->n + 4u) >> 24) & 0xFFu);
    memcpy(b->data + 4u, code->b, code->n);
    b->data[4u + code->n] = (unsigned char)(main_start & 0xFFu);
    b->data[4u + code->n + 1u] = (unsigned char)((main_start >> 8) & 0xFFu);
    b->data[4u + code->n + 2u] = (unsigned char)((main_start >> 16) & 0xFFu);
    b->data[4u + code->n + 3u] = (unsigned char)((main_start >> 24) & 0xFFu);
    b->len = total;
    b->name = name;
    b->orig_script_len = (uint32_t)(code->n + 4u);
    b->pcs = NULL;
    b->ops = NULL;
    b->ninstr = 0;
    return b;
}

/* --- synthetic bodies: each one covers a distinct corner of the format --- */

static void synth_all_opcodes(void)
{
    Code c;
    int op;
    memset(&c, 0, sizeof c);
    for (op = 0; op <= 0x27; ++op) {
        emit_op(&c, (uint8_t)op);
    }
    base_new("all-opcodes", &c, 4);
}

static void synth_arith(void)
{
    Code c;
    int i;
    memset(&c, 0, sizeof c);
    for (i = 0; i < 12; ++i) {
        emit8(&c, 0x0c); /* pushint8 */
        emit8(&c, (uint8_t)(i + 1));
    }
    for (i = 0; i < 6; ++i) {
        emit8(&c, (uint8_t)(0x1a + (i % 14))); /* add..ge */
    }
    emit8(&c, 0x19); /* neg */
    emit8(&c, 0x1f); /* test */
    emit8(&c, 0x0a); /* pushint32 */
    emit32(&c, 0x7FFFFFFF);
    emit8(&c, 0x0d); /* pushfloat nan bits */
    emit32(&c, 0x7FC00000u);
    emit8(&c, 0x1a);
    emit8(&c, 0x03); /* syscall TextPrint */
    emit16(&c, 123);
    emit8(&c, 0x04); /* ret */
    base_new("arith", &c, 4);
}

static void synth_loops(void)
{
    Code c;
    memset(&c, 0, sizeof c);
    emit8(&c, 0x06); /* jmp 4: itself */
    emit32(&c, 4);
    base_new("jmp-self", &c, 4);

    memset(&c, 0, sizeof c);
    emit8(&c, 0x09); /* pushfalse */
    emit8(&c, 0x07); /* jmpcond 4 */
    emit32(&c, 4);
    base_new("jmpcond-self", &c, 4);

    memset(&c, 0, sizeof c);
    emit8(&c, 0x02); /* call 4: recursion */
    emit32(&c, 4);
    base_new("call-self", &c, 4);

    memset(&c, 0, sizeof c);
    emit8(&c, 0x06); /* jmp far outside */
    emit32(&c, 0xFFFFFFF0u);
    base_new("jmp-outside", &c, 4);

    memset(&c, 0, sizeof c);
    emit8(&c, 0x06); /* jmp into the middle of this instruction */
    emit32(&c, 5);
    base_new("jmp-mid", &c, 4);
}

static void synth_frames(void)
{
    Code c;
    int i;
    memset(&c, 0, sizeof c);
    emit8(&c, 0x01); /* initstack 255,0 */
    emit8(&c, 255);
    emit8(&c, 0);
    for (i = 0; i < 40; ++i) {
        emit8(&c, 0x10); /* pushstack */
        emit8(&c, (uint8_t)(i * 6));
        emit8(&c, 0x13); /* pushtop */
        emit8(&c, 0x14); /* pushtemp */
        emit8(&c, 0x16); /* copystack */
        emit8(&c, (uint8_t)(255 - i));
    }
    emit8(&c, 0x04);
    base_new("frames-edge", &c, 4);

    memset(&c, 0, sizeof c);
    emit8(&c, 0x01); /* initstack 0,0 */
    emit8(&c, 0);
    emit8(&c, 0);
    emit8(&c, 0x10); /* pushstack with a frame but no locals/args */
    emit8(&c, 0);
    emit8(&c, 0x16);
    emit8(&c, 0);
    emit8(&c, 0x13);
    emit8(&c, 0x04);
    base_new("frames-zero", &c, 4);

    memset(&c, 0, sizeof c);
    emit8(&c, 0x10); /* pushstack with no frame at all */
    emit8(&c, 255);
    emit8(&c, 0x16);
    emit8(&c, 255);
    emit8(&c, 0x13);
    emit8(&c, 0x04);
    base_new("frames-none", &c, 4);
}

static void synth_strings(void)
{
    Code c;
    memset(&c, 0, sizeof c);
    emit_pushstr(&c, "");
    emit_pushstr(&c, "a");
    emit_pushstr(&c, "FVP");
    emit_pushstr(&c, "");
    emit8(&c, 0x1a); /* add: concat */
    emit8(&c, 0x0c); /* pushint8 3 */
    emit8(&c, 3);
    emit8(&c, 0x1c); /* mul: repeat */
    emit8(&c, 0x03); /* TextPrint */
    emit16(&c, 123);
    emit8(&c, 0x04);
    base_new("strings", &c, 4);

    /* pushstring whose declared length runs past the script region */
    memset(&c, 0, sizeof c);
    emit8(&c, 0x0e);
    emit8(&c, 200);
    emit8(&c, 'a');
    emit8(&c, 0);
    emit8(&c, 0x04);
    base_new("string-trunc", &c, 4);

    /* 255-byte string at the end of the file */
    memset(&c, 0, sizeof c);
    {
        int i;
        emit8(&c, 0x0e);
        emit8(&c, 255);
        for (i = 0; i < 254; ++i) {
            emit8(&c, (uint8_t)('A' + (i % 26)));
        }
        emit8(&c, 0);
    }
    emit8(&c, 0x03);
    emit16(&c, 123);
    base_new("string-255", &c, 4);
}

static void synth_syscalls(void)
{
    Code c;
    static const uint16_t ids[] = {0,  1,  13,  14, 15, 22, 28, 29, 30, 31,
                                   34, 36, 44,  47, 50, 53, 56, 59, 61, 62,
                                   64, 111, 123, 131, 133, 137, 143, 144, 145,
                                   146, 147, 200, 65535};
    size_t i;
    memset(&c, 0, sizeof c);
    for (i = 0; i < sizeof ids / sizeof ids[0]; ++i) {
        emit8(&c, 0x03); /* syscall */
        emit16(&c, ids[i]);
    }
    emit8(&c, 0x03);
    emit16(&c, 123); /* TextPrint with an empty stack */
    emit8(&c, 0x04);
    base_new("syscalls", &c, 4);
}

static void synth_globals(void)
{
    Code c;
    static const uint16_t ids[] = {0, 1, 255, 256, 8191, 8192, 8193, 65535};
    size_t i;
    memset(&c, 0, sizeof c);
    for (i = 0; i < sizeof ids / sizeof ids[0]; ++i) {
        emit8(&c, 0x0f); /* pushglobal */
        emit16(&c, ids[i]);
        emit8(&c, 0x15); /* popglobal */
        emit16(&c, ids[i]);
    }
    emit8(&c, 0x0f);
    emit16(&c, 65535);
    emit8(&c, 0x04);
    base_new("globals-edge", &c, 4);
}

static void synth_arrays(void)
{
    Code c;
    int i;
    memset(&c, 0, sizeof c);
    /* int keys */
    for (i = 0; i < 4; ++i) {
        emit8(&c, 0x0c);
        emit8(&c, (uint8_t)i);
        emit8(&c, 0x0c);
        emit8(&c, (uint8_t)(i + 1));
        emit8(&c, 0x17); /* garr_write */
        emit16(&c, (uint16_t)(i * 7));
        emit8(&c, 0x0c);
        emit8(&c, (uint8_t)i);
        emit8(&c, 0x11); /* garr_read */
        emit16(&c, (uint16_t)(i * 7));
    }
    /* string keys + larr */
    emit_pushstr(&c, "key");
    emit8(&c, 0x0c);
    emit8(&c, 9);
    emit8(&c, 0x18); /* larr_write */
    emit8(&c, 3);
    emit_pushstr(&c, "key");
    emit8(&c, 0x12); /* larr_read */
    emit8(&c, 3);
    /* float key */
    emit8(&c, 0x0d);
    emit32(&c, 0x3F800000u);
    emit8(&c, 0x0c);
    emit8(&c, 1);
    emit8(&c, 0x17);
    emit16(&c, 5);
    emit8(&c, 0x04);
    base_new("arrays-edge", &c, 4);
}

static void synth_returns(void)
{
    Code c;
    memset(&c, 0, sizeof c);
    emit8(&c, 0x04); /* ret with no frame */
    base_new("ret-none", &c, 4);

    memset(&c, 0, sizeof c);
    emit8(&c, 0x05); /* ret2 with no frame */
    base_new("ret2-none", &c, 4);

    memset(&c, 0, sizeof c);
    emit8(&c, 0x01); /* initstack then ret2 */
    emit8(&c, 2);
    emit8(&c, 0);
    emit8(&c, 0x05);
    base_new("ret2-frame", &c, 4);
}

static void synth_bad_header(void)
{
    Code c;
    memset(&c, 0, sizeof c);
    emit8(&c, 0x04);
    base_new("main-outside", &c, 0xFFFFFFFFu);
    base_new("main-zero", &c, 0u);
    base_new("main-mid", &c, 5u);
    base_new("main-end", &c, 5u);
}

static void synth_push_many(void)
{
    Code c;
    int i;
    memset(&c, 0, sizeof c);
    for (i = 0; i < 800; ++i) {
        emit8(&c, 0x0c); /* pushint8 */
        emit8(&c, (uint8_t)(i & 0xFF));
    }
    emit8(&c, 0x04);
    base_new("push-many", &c, 4);
}

static void build_synthetic_corpus(void)
{
    synth_all_opcodes();
    synth_arith();
    synth_loops();
    synth_frames();
    synth_strings();
    synth_syscalls();
    synth_globals();
    synth_arrays();
    synth_returns();
    synth_bad_header();
    synth_push_many();
}

/* ------------------------------------------------------------------ */
/* helpers                                                            */
/* ------------------------------------------------------------------ */

static unsigned char *read_file_bytes(const char *path, size_t *len_out)
{
    FILE *f = fopen(path, "rb");
    long n;
    unsigned char *buf;
    if (f == NULL) {
        return NULL;
    }
    if (fseek(f, 0, SEEK_END) != 0) {
        fclose(f);
        return NULL;
    }
    n = ftell(f);
    if (n < 0 || fseek(f, 0, SEEK_SET) != 0) {
        fclose(f);
        return NULL;
    }
    buf = (unsigned char *)malloc((size_t)n + 1u);
    if (buf == NULL) {
        fclose(f);
        return NULL;
    }
    if (n > 0 && fread(buf, 1, (size_t)n, f) != (size_t)n) {
        free(buf);
        fclose(f);
        return NULL;
    }
    fclose(f);
    buf[n] = 0;
    *len_out = (size_t)n;
    return buf;
}

static int write_bytes(const char *path, const unsigned char *data, size_t len)
{
    FILE *f = fopen(path, "wb");
    if (f == NULL) {
        return 0;
    }
    if (len != 0 && fwrite(data, 1, len, f) != len) {
        fclose(f);
        return 0;
    }
    return fclose(f) == 0;
}

static uint32_t rd32(const unsigned char *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) |
           ((uint32_t)p[3] << 24);
}

static void wr32(unsigned char *p, uint32_t v)
{
    p[0] = (unsigned char)(v & 0xFFu);
    p[1] = (unsigned char)((v >> 8) & 0xFFu);
    p[2] = (unsigned char)((v >> 16) & 0xFFu);
    p[3] = (unsigned char)((v >> 24) & 0xFFu);
}

/* Decode a base once so the mutator can target real instruction boundaries. */
static int load_base(FuzzBase *b, const char *tmp, FvpEncoding enc)
{
    FvpHcb hcb;
    char err[256];
    uint32_t i;
    if (!write_bytes(tmp, b->data, b->len)) {
        return 0;
    }
    if (!fvp_hcb_load(&hcb, tmp, enc, err, sizeof err)) {
        fprintf(stderr, "fuzz_hcb: base '%s' does not load: %s\n", b->name,
                err);
        return 0;
    }
    b->pcs = (uint32_t *)malloc((size_t)hcb.ninstr * sizeof(uint32_t) + 4u);
    b->ops = (uint8_t *)malloc((size_t)hcb.ninstr + 4u);
    if (b->pcs == NULL || b->ops == NULL) {
        fprintf(stderr, "fuzz_hcb: out of memory\n");
        exit(2);
    }
    for (i = 0; i < hcb.ninstr; ++i) {
        b->pcs[i] = hcb.instrs[i].pc;
        b->ops[i] = hcb.instrs[i].op;
    }
    b->ninstr = hcb.ninstr;
    fvp_hcb_free(&hcb);
    return 1;
}

/* Build the real-script slice base: candidate lengths are tried downwards
 * until the loader accepts the slice as an instruction stream. */
static int add_real_slice(const char *script, size_t slice, size_t offset,
                          const char *tmp, FvpEncoding enc)
{
    unsigned char *file;
    size_t file_len = 0;
    uint32_t script_len;
    size_t avail;
    size_t want;
    size_t s;
    FuzzBase *b;

    file = read_file_bytes(script, &file_len);
    if (file == NULL) {
        fprintf(stderr, "fuzz_hcb: cannot read %s\n", script);
        return 0;
    }
    if (file_len < 8u) {
        fprintf(stderr, "fuzz_hcb: script shorter than 8 bytes\n");
        free(file);
        return 0;
    }
    script_len = rd32(file);
    if (script_len < 8u || (size_t)script_len + 4u > file_len) {
        fprintf(stderr, "fuzz_hcb: script header looks wrong (script_len=%lu "
                        "file=%lu)\n",
                (unsigned long)script_len, (unsigned long)file_len);
        free(file);
        return 0;
    }
    if (offset < 4u || offset >= (size_t)script_len) {
        offset = 4u;
    }
    avail = (size_t)script_len - offset;
    want = slice < avail ? slice : avail;
    if (want < 64u) {
        want = avail < 64u ? avail : 64u;
    }
    for (s = want; s >= 64u; --s) {
        FvpHcb probe;
        char err[256];
        b = &g_bases[g_nbases];
        if (g_nbases >= MAX_BASES) {
            break;
        }
        b->data = (unsigned char *)malloc(4u + s + 4u);
        if (b->data == NULL) {
            break;
        }
        wr32(b->data, (uint32_t)(s + 4u));
        memcpy(b->data + 4u, file + offset, s);
        wr32(b->data + 4u + s, 4u); /* stream starts at the first instruction */
        b->len = 4u + s + 4u;
        b->name = "real-slice";
        b->orig_script_len = (uint32_t)(s + 4u);
        b->pcs = NULL;
        b->ops = NULL;
        b->ninstr = 0;
        if (!write_bytes(tmp, b->data, b->len)) {
            free(b->data);
            break;
        }
        if (fvp_hcb_load(&probe, tmp, enc, err, sizeof err)) {
            fvp_hcb_free(&probe);
            g_nbases += 1;
            free(file);
            return 1;
        }
        free(b->data);
        b->data = NULL;
        if (s == 64u) {
            break;
        }
    }
    fprintf(stderr, "fuzz_hcb: could not find a loadable slice of %s\n",
            script);
    free(file);
    return 0;
}

/* ------------------------------------------------------------------ */
/* mutation operators                                                 */
/* ------------------------------------------------------------------ */

enum {
    CAT_BYTEFLIP = 0,
    CAT_BYTEWRITE,
    CAT_TRUNCATE,
    CAT_SCRIPT_LEN,
    CAT_MAIN_START,
    CAT_JUMP_TARGET,
    CAT_STRING_LEN,
    CAT_OPCODE,
    CAT_INSERT,
    CAT_DELETE,
    CAT_ZERO_RUN,
    CAT_SPLICE,
    CAT_APPEND,
    CAT_ALIGN_MAIN,
    CAT_HEADER_GARBAGE,
    CAT_COMBINED,
    CAT_COUNT
};

static const char *const g_cat_name[CAT_COUNT] = {
    "byteflip",    "bytewrite", "truncate",  "script_len", "main_start",
    "jump_target", "string_len", "opcode",   "insert",     "delete",
    "zero_run",    "splice",    "append",    "align_main", "header_garbage",
    "combined"};
/* Weights: byte-level mutations dominate, structural ones stay well
 * represented so every loader rejection path is hit many times. */
static const unsigned g_cat_weight[CAT_COUNT] = {
    14, 10, 10, 8, 8, 9, 6, 5, 5, 5, 5, 5, 4, 4, 4, 8};

static uint32_t pick_category(Rng *r)
{
    unsigned total = 0;
    unsigned i;
    uint32_t x;
    for (i = 0; i < CAT_COUNT; ++i) {
        total += g_cat_weight[i];
    }
    x = rnd_below(r, total);
    for (i = 0; i < CAT_COUNT; ++i) {
        if (x < g_cat_weight[i]) {
            return i;
        }
        x -= g_cat_weight[i];
    }
    return CAT_BYTEFLIP;
}

static size_t apply_one(uint32_t cat, unsigned char *buf, size_t n,
                        const FuzzBase *b, Rng *r, size_t cap)
{
    uint32_t k;
    uint32_t pos;
    static const uint32_t sl_values[] = {0u,  1u,   2u,   3u,   4u,
                                         5u,  8u,   0x7FFFFFFFu,
                                         0x80000000u, 0xFFFFFFF0u,
                                         0xFFFFFFFFu};
    switch (cat) {
    case CAT_BYTEFLIP:
        if (n == 0u) {
            break;
        }
        k = 1u + rnd_below(r, 8u);
        while (k-- > 0u) {
            pos = rnd_below(r, (uint32_t)n);
            buf[pos] ^= (unsigned char)(1u << rnd_below(r, 8u));
        }
        break;
    case CAT_BYTEWRITE:
        if (n == 0u) {
            break;
        }
        k = 1u + rnd_below(r, 8u);
        while (k-- > 0u) {
            buf[rnd_below(r, (uint32_t)n)] = (unsigned char)(rnd32(r) & 0xFFu);
        }
        break;
    case CAT_TRUNCATE: {
        uint32_t mode = rnd_below(r, 4u);
        size_t nl;
        if (mode == 0u) {
            nl = rnd_below(r, 9u);
        } else if (mode == 1u) {
            nl = rnd_below(r, (uint32_t)n + 1u);
        } else if (mode == 2u) {
            nl = n > 4u ? 4u + rnd_below(r, (uint32_t)(n - 4u) + 1u) : 0u;
        } else {
            nl = n > 1u ? n - 1u - rnd_below(r, 16u) : 0u;
        }
        n = nl < n ? nl : n;
        break;
    }
    case CAT_SCRIPT_LEN:
        if (n >= 4u) {
            uint32_t v;
            if ((rnd32(r) & 3u) == 0u) {
                v = (uint32_t)n + rnd_below(r, 17u) - 8u;
            } else {
                v = sl_values[rnd_below(r, (uint32_t)(sizeof sl_values /
                                                      sizeof sl_values[0]))];
            }
            wr32(buf, v);
        }
        break;
    case CAT_MAIN_START: {
        uint32_t sl = n >= 4u ? rd32(buf) : 0u;
        uint32_t v;
        size_t at;
        at = (sl >= 4u && (size_t)sl + 4u <= n) ? (size_t)sl
                                                : (n >= 8u ? n - 4u : 0u);
        if ((rnd32(r) & 1u) == 0u) {
            v = 4u; /* keep it runnable: first instruction */
        } else {
            v = sl_values[rnd_below(r,
                                    (uint32_t)(sizeof sl_values /
                                               sizeof sl_values[0]))];
        }
        if (at + 4u <= n) {
            wr32(buf + at, v);
        }
        break;
    }
    case CAT_JUMP_TARGET: {
        uint32_t nops = b->ninstr;
        uint8_t *ops = b->ops;
        int at = 0;
        int found = 0;
        uint32_t tries;
        for (tries = 0; tries < nops && !found; ++tries) {
            uint32_t k = rnd_below(r, nops);
            if (ops[k] == 0x02u || ops[k] == 0x06u || ops[k] == 0x07u) {
                at = (int)k;
                found = 1;
            }
        }
        if (!found) {
            break;
        }
        pos = b->pcs[at];
        if ((size_t)pos + 5u > n) {
            break;
        }
        {
            uint32_t mode = rnd_below(r, 5u);
            uint32_t v;
            if (mode == 0u) {
                v = b->pcs[rnd_below(r, b->ninstr)]; /* valid instruction */
            } else if (mode == 1u) {
                v = b->pcs[rnd_below(r, b->ninstr)] + 1u; /* mid-instruction */
            } else if (mode == 2u) {
                v = 0xFFFFFFFFu;
            } else if (mode == 3u) {
                v = (uint32_t)n; /* one past the end */
            } else {
                v = rnd32(r);
            }
            wr32(buf + pos + 1u, v);
        }
        break;
    }
    case CAT_STRING_LEN: {
        static const uint8_t lens[] = {0u, 1u, 2u, 3u, 16u, 128u, 254u, 255u};
        uint32_t nops = b->ninstr;
        uint32_t k;
        for (k = 0; k < nops; ++k) {
            uint32_t idx = rnd_below(r, nops);
            if (b->ops[idx] == 0x0Eu) {
                pos = b->pcs[idx];
                if ((size_t)pos + 2u <= n) {
                    buf[pos + 1u] = lens[rnd_below(r,
                                                   (uint32_t)(sizeof lens))];
                }
                break;
            }
        }
        break;
    }
    case CAT_OPCODE: {
        static const uint8_t ops[] = {0x00u, 0x05u, 0x27u, 0x28u, 0x29u, 0x80u,
                                      0xFFu};
        if (b->ninstr == 0u) {
            break;
        }
        pos = b->pcs[rnd_below(r, b->ninstr)];
        if ((size_t)pos < n) {
            buf[pos] = ops[rnd_below(r, (uint32_t)(sizeof ops))];
        }
        break;
    }
    case CAT_INSERT: {
        size_t count;
        pos = rnd_below(r, (uint32_t)n + 1u);
        count = 1u + rnd_below(r, 8u);
        if (n + count <= cap) {
            size_t j;
            memmove(buf + pos + count, buf + pos, n - pos);
            for (j = 0; j < count; ++j) {
                buf[pos + j] = (unsigned char)(rnd32(r) & 0xFFu);
            }
            n += count;
        }
        break;
    }
    case CAT_DELETE:
        if (n > 4u) {
            uint32_t maxk;
            pos = rnd_below(r, (uint32_t)n);
            maxk = (uint32_t)(n - pos);
            if (maxk > 8u) {
                maxk = 8u;
            }
            k = 1u + rnd_below(r, maxk);
            memmove(buf + pos, buf + pos + k, n - pos - k);
            n -= k;
        }
        break;
    case CAT_ZERO_RUN:
        if (n > 8u) {
            pos = rnd_below(r, (uint32_t)n);
            k = 8u + rnd_below(r, 504u);
            if ((size_t)pos + k > n) {
                k = (uint32_t)(n - pos);
            }
            memset(buf + pos, 0, k);
        }
        break;
    case CAT_SPLICE:
        if (n > 8u) {
            uint32_t a = rnd_below(r, (uint32_t)n);
            uint32_t src = rnd_below(r, (uint32_t)n);
            k = 1u + rnd_below(r, 64u);
            if ((size_t)a + k > n) {
                k = (uint32_t)(n - a);
            }
            if ((size_t)src + k > n) {
                k = (uint32_t)(n - src);
            }
            memmove(buf + a, buf + src, k);
        }
        break;
    case CAT_APPEND:
        k = 1u + rnd_below(r, 64u);
        if (n + k <= cap) {
            size_t i;
            for (i = 0; i < k; ++i) {
                buf[n + i] = (unsigned char)(rnd32(r) & 0xFFu);
            }
            n += k;
        }
        break;
    case CAT_ALIGN_MAIN: {
        uint32_t sl = n >= 4u ? rd32(buf) : 0u;
        size_t at = (sl >= 4u && (size_t)sl + 4u <= n) ? (size_t)sl : 0u;
        if (b->ninstr > 0u && at + 4u <= n) {
            uint32_t v = b->pcs[rnd_below(r, b->ninstr)] + 1u;
            wr32(buf + at, v);
        }
        break;
    }
    case CAT_HEADER_GARBAGE:
        if (n >= 8u) {
            k = 1u + rnd_below(r, 8u);
            while (k-- > 0u) {
                buf[rnd_below(r, 8u)] = (unsigned char)(rnd32(r) & 0xFFu);
            }
        }
        break;
    case CAT_COMBINED: {
        uint32_t c1 = CAT_BYTEFLIP + rnd_below(r, 3u); /* flip/write/trunc? */
        n = apply_one(c1, buf, n, b, r, cap);
        n = apply_one(CAT_BYTEWRITE, buf, n, b, r, cap);
        break;
    }
    default:
        break;
    }
    return n;
}

/* ------------------------------------------------------------------ */
/* options / results                                                  */
/* ------------------------------------------------------------------ */

typedef struct Options {
    const char *script;
    const char *tmp;
    const char *journal;
    const char *csv;
    const char *dump_dir;
    const char *out;
    const char *enc_name;
    uint64_t iters;
    uint64_t seed;
    uint64_t steps;
    size_t slice;
    size_t slice_offset;
    unsigned real_share;
    uint64_t dump_every;
    long repro;
    int quiet;
    int synthetic_only;
} Options;

enum { R_REJECTED = 0, R_FINISHED = 1, R_CAPPED = 2, R_COUNT };

static const char *const g_result_name[R_COUNT] = {"rejected", "finished",
                                                   "capped"};

typedef struct CaseRec {
    const char *base;
    uint32_t cat;
    uint32_t nbytes;
    int result;
    uint32_t steps;
    char reason[96];
} CaseRec;

typedef struct Agg {
    unsigned long long total;
    unsigned long long result[R_COUNT];
} Agg;

#ifdef _WIN32
static LONG WINAPI fuzz_seh(EXCEPTION_POINTERS *ep)
{
    DWORD code = 0;
    if (ep != NULL && ep->ExceptionRecord != NULL) {
        code = ep->ExceptionRecord->ExceptionCode;
    }
    fprintf(stderr, "FAULT what=exception code=0x%08lX case=%ld cat=%s\n",
            (unsigned long)code, faq_case_idx, faq_case_cat);
    fflush(stderr);
    if (faq_journal != NULL) {
        fprintf(faq_journal, "FAULT\texception\t%ld\t%s\t0x%08lX\n",
                faq_case_idx, faq_case_cat, (unsigned long)code);
        fflush(faq_journal);
    }
    _Exit(71);
    return EXCEPTION_EXECUTE_HANDLER;
}
#endif

/* ------------------------------------------------------------------ */
/* one case                                                           */
/* ------------------------------------------------------------------ */

static int run_case(const Options *o, uint32_t idx, unsigned char *buf,
                    size_t n, const FuzzBase *b, CaseRec *rec, FvpEncoding enc)
{
    FvpHcb hcb;
    char err[256];
    rec->base = b->name;
    rec->cat = 0;
    rec->nbytes = (uint32_t)n;
    rec->steps = 0;
    rec->reason[0] = '\0';
    if (!write_bytes(o->tmp, buf, n)) {
        fprintf(stderr, "fuzz_hcb: cannot write %s\n", o->tmp);
        exit(2);
    }
    if (!fvp_hcb_load(&hcb, o->tmp, enc, err, sizeof err)) {
        size_t i;
        rec->result = R_REJECTED;
        /* Collapse the diagnostic to one CSV-safe token-ish string. */
        for (i = 0; err[i] != '\0' && i + 1u < sizeof rec->reason; ++i) {
            char ch = err[i];
            rec->reason[i] = (ch == ',' || ch == '\t' || ch == '\n' ||
                              ch == '\r')
                                 ? ';'
                                 : ch;
        }
        rec->reason[i] = '\0';
        fvp_hcb_free(&hcb);
        return 1;
    }
    {
        FvpVM vm;
        uint64_t steps;
        fvp_vm_init(&vm, &hcb, 1);
        fvp_vm_start(&vm);
        steps = fvp_vm_run(&vm, o->steps);
        rec->steps = (uint32_t)steps;
        rec->result = steps >= o->steps ? R_CAPPED : R_FINISHED;
        fvp_vm_free(&vm);
    }
    fvp_hcb_free(&hcb);
    (void)idx;
    return 1;
}

/* Deterministic base selection: index 0 is the real-script slice when one was
 * added, the rest are synthetic scripts.  --repro uses the same function, so
 * (seed, idx) fully identifies a case. */
static const FuzzBase *choose_base(int have_real, unsigned real_share, Rng *r)
{
    if (have_real && g_nbases > 1) {
        if ((unsigned)rnd_below(r, 100u) < real_share) {
            return &g_bases[0];
        }
        return &g_bases[1 + rnd_below(r, (uint32_t)g_nbases - 1u)];
    }
    if (g_nbases > 1) {
        return &g_bases[rnd_below(r, (uint32_t)g_nbases)];
    }
    return &g_bases[0];
}

/* ------------------------------------------------------------------ */
/* main                                                               */
/* ------------------------------------------------------------------ */

static int parse_u64(const char *s, uint64_t *out)
{
    char *end = NULL;
    unsigned long long v;
    if (s == NULL || *s == '\0' || *s == '-') {
        return 0;
    }
    v = strtoull(s, &end, 10);
    if (end == NULL || *end != '\0') {
        return 0;
    }
    *out = (uint64_t)v;
    return 1;
}

static void usage(void)
{
    fprintf(stderr,
            "usage: fuzz_hcb <script.hcb|-> [--iters N] [--seed S] "
            "[--steps N]\n"
            "                [--slice BYTES] [--slice-offset N] "
            "[--enc cp932|gbk]\n"
            "                [--tmp PATH] [--journal PATH] [--csv PATH]\n"
            "                [--dump-dir DIR] [--dump-every K] "
            "[--real-share P]\n"
            "                [--synthetic-only] [--repro IDX --out PATH] "
            "[--quiet]\n");
}

int main(int argc, char **argv)
{
    Options o;
    FvpEncoding enc;
    unsigned char *buf = NULL;
    size_t cap = 0;
    uint64_t i;
    Agg agg;
    Agg *cat_agg;
    Agg *base_agg;
    FILE *csv = NULL;
    FILE *journal = NULL;
    FILE *dump_csv = NULL;
    int rc = 0;
    int ai;
    int have_real = 0;

#ifdef _WIN32
    SetUnhandledExceptionFilter(fuzz_seh);
#endif

    memset(&o, 0, sizeof o);
    o.iters = 20000u;
    o.seed = 20261123u;
    o.steps = 2048u;
    o.slice = 32768u;
    o.slice_offset = 4u;
    o.real_share = 70u;
    o.repro = -1;
    o.enc_name = "cp932";

    for (ai = 1; ai < argc; ++ai) {
        const char *a = argv[ai];
        if (strcmp(a, "--iters") == 0 && ai + 1 < argc) {
            if (!parse_u64(argv[++ai], &o.iters)) {
                usage();
                return 2;
            }
        } else if (strcmp(a, "--seed") == 0 && ai + 1 < argc) {
            if (!parse_u64(argv[++ai], &o.seed)) {
                usage();
                return 2;
            }
        } else if (strcmp(a, "--steps") == 0 && ai + 1 < argc) {
            if (!parse_u64(argv[++ai], &o.steps)) {
                usage();
                return 2;
            }
        } else if (strcmp(a, "--slice") == 0 && ai + 1 < argc) {
            uint64_t v;
            if (!parse_u64(argv[++ai], &v)) {
                usage();
                return 2;
            }
            o.slice = (size_t)v;
        } else if (strcmp(a, "--slice-offset") == 0 && ai + 1 < argc) {
            uint64_t v;
            if (!parse_u64(argv[++ai], &v)) {
                usage();
                return 2;
            }
            o.slice_offset = (size_t)v;
        } else if (strcmp(a, "--enc") == 0 && ai + 1 < argc) {
            o.enc_name = argv[++ai];
        } else if (strcmp(a, "--tmp") == 0 && ai + 1 < argc) {
            o.tmp = argv[++ai];
        } else if (strcmp(a, "--journal") == 0 && ai + 1 < argc) {
            o.journal = argv[++ai];
        } else if (strcmp(a, "--csv") == 0 && ai + 1 < argc) {
            o.csv = argv[++ai];
        } else if (strcmp(a, "--dump-dir") == 0 && ai + 1 < argc) {
            o.dump_dir = argv[++ai];
        } else if (strcmp(a, "--dump-every") == 0 && ai + 1 < argc) {
            if (!parse_u64(argv[++ai], &o.dump_every)) {
                usage();
                return 2;
            }
        } else if (strcmp(a, "--real-share") == 0 && ai + 1 < argc) {
            uint64_t v;
            if (!parse_u64(argv[++ai], &v) || v > 100u) {
                usage();
                return 2;
            }
            o.real_share = (unsigned)v;
        } else if (strcmp(a, "--synthetic-only") == 0) {
            o.synthetic_only = 1;
        } else if (strcmp(a, "--repro") == 0 && ai + 1 < argc) {
            uint64_t v;
            if (!parse_u64(argv[++ai], &v)) {
                usage();
                return 2;
            }
            o.repro = (long)v;
        } else if (strcmp(a, "--out") == 0 && ai + 1 < argc) {
            o.out = argv[++ai];
        } else if (strcmp(a, "--quiet") == 0) {
            o.quiet = 1;
        } else if (a[0] == '-' && a[1] != '\0') {
            fprintf(stderr, "fuzz_hcb: unknown option %s\n", a);
            usage();
            return 2;
        } else if (o.script == NULL) {
            o.script = a;
        } else {
            fprintf(stderr, "fuzz_hcb: unexpected argument %s\n", a);
            return 2;
        }
    }
    if (o.tmp == NULL) {
        o.tmp = "fvp/out/fuzz/tmp/hcb_case.hcb";
    }
    if (!fvp_encoding_from_name(o.enc_name, &enc)) {
        fprintf(stderr, "fuzz_hcb: unknown encoding %s\n", o.enc_name);
        return 2;
    }

    if (!o.synthetic_only && o.script != NULL && strcmp(o.script, "-") != 0) {
        if (!add_real_slice(o.script, o.slice, o.slice_offset, o.tmp, enc)) {
            return 2;
        }
        have_real = 1;
    }
    build_synthetic_corpus();
    for (i = 0; i < (uint64_t)g_nbases; ++i) {
        if (!load_base(&g_bases[i], o.tmp, enc)) {
            return 2;
        }
        if (!o.quiet) {
            printf("BASE name=%s len=%lu instrs=%lu\n", g_bases[i].name,
                   (unsigned long)g_bases[i].len,
                   (unsigned long)g_bases[i].ninstr);
        }
    }
    cap = 4u;
    for (i = 0; i < (uint64_t)g_nbases; ++i) {
        if (g_bases[i].len > cap) {
            cap = g_bases[i].len;
        }
    }
    cap += 4096u;
    buf = (unsigned char *)malloc(cap);
    if (buf == NULL) {
        fprintf(stderr, "fuzz_hcb: out of memory\n");
        return 2;
    }

    if (o.journal != NULL) {
        journal = fopen(o.journal, "wb");
        if (journal == NULL) {
            fprintf(stderr, "fuzz_hcb: cannot write %s\n", o.journal);
            return 2;
        }
        faq_set_journal(journal);
    }
    if (o.csv != NULL) {
        csv = fopen(o.csv, "wb");
        if (csv == NULL) {
            fprintf(stderr, "fuzz_hcb: cannot write %s\n", o.csv);
            return 2;
        }
        fprintf(csv, "idx,category,base,bytes,result,steps,reason\n");
    }
    if (o.dump_dir != NULL && o.dump_every != 0u) {
        char path[1024];
        snprintf(path, sizeof path, "%s/cases.csv", o.dump_dir);
        dump_csv = fopen(path, "wb");
        if (dump_csv == NULL) {
            fprintf(stderr, "fuzz_hcb: cannot write %s\n", path);
            return 2;
        }
        fprintf(dump_csv, "file,category,base,bytes,result\n");
    }

    memset(&agg, 0, sizeof agg);
    cat_agg = (Agg *)calloc((size_t)CAT_COUNT, sizeof(Agg));
    base_agg = (Agg *)calloc((size_t)g_nbases, sizeof(Agg));
    if (cat_agg == NULL || base_agg == NULL) {
        fprintf(stderr, "fuzz_hcb: out of memory\n");
        return 2;
    }

    if (o.repro >= 0) {
        uint64_t idx = (uint64_t)o.repro;
        Rng r;
        const FuzzBase *b;
        uint32_t cat;
        size_t n;
        r.s = sm64(o.seed ^ sm64(idx + 1u));
        b = choose_base(have_real, o.real_share, &r);
        cat = pick_category(&r);
        memcpy(buf, b->data, b->len);
        n = b->len;
        n = apply_one(cat, buf, n, b, &r, cap);
        if (o.out == NULL || !write_bytes(o.out, buf, n)) {
            fprintf(stderr, "fuzz_hcb: cannot write repro file\n");
            return 2;
        }
        printf("REPRO idx=%llu seed=%llu base=%s category=%s bytes=%lu file=%s\n",
               (unsigned long long)idx, (unsigned long long)o.seed, b->name,
               g_cat_name[cat], (unsigned long)n, o.out);
        free(buf);
        return 0;
    }

    for (i = 0; i < o.iters; ++i) {
        Rng r;
        const FuzzBase *b;
        uint32_t cat;
        size_t n;
        CaseRec rec;
        int bi;

        r.s = sm64(o.seed ^ sm64(i + 1u));
        b = choose_base(have_real, o.real_share, &r);
        bi = (int)(b - g_bases);
        cat = pick_category(&r);
        faq_set_case((long)i, g_cat_name[cat]);
        if (journal != NULL) {
            fprintf(journal, "%llu\t%llu\t%s\t%s\t%lu\n",
                    (unsigned long long)i, (unsigned long long)o.seed,
                    g_cat_name[cat], b->name, (unsigned long)b->len);
            fflush(journal);
        }

        memcpy(buf, b->data, b->len);
        n = apply_one(cat, buf, b->len, b, &r, cap);
        if (!run_case(&o, (uint32_t)i, buf, n, b, &rec, enc)) {
            free(buf);
            return 2;
        }
        agg.total += 1u;
        agg.result[rec.result] += 1u;
        if ((size_t)bi < (size_t)g_nbases) {
            base_agg[bi].total += 1u;
            base_agg[bi].result[rec.result] += 1u;
        }
        cat_agg[cat].total += 1u;
        cat_agg[cat].result[rec.result] += 1u;

        if (csv != NULL) {
            fprintf(csv, "%llu,%s,%s,%u,%s,%u,%s\n",
                    (unsigned long long)i, g_cat_name[cat], b->name,
                    rec.nbytes, g_result_name[rec.result], rec.steps,
                    rec.reason);
        }
        if (dump_csv != NULL && o.dump_every != 0u &&
            (i % o.dump_every) == 0u) {
            char path[1200];
            snprintf(path, sizeof path, "%s/c%06llu.hcb", o.dump_dir,
                     (unsigned long long)i);
            if (!write_bytes(path, buf, n)) {
                fprintf(stderr, "fuzz_hcb: cannot write %s\n", path);
                rc = 2;
                break;
            }
            fprintf(dump_csv, "c%06llu.hcb,%s,%s,%lu,%s\n",
                    (unsigned long long)i, g_cat_name[cat], b->name,
                    (unsigned long)n, g_result_name[rec.result]);
        }
        if (!o.quiet && (i % 5000u) == 0u) {
            printf("progress %llu/%llu\n", (unsigned long long)i,
                   (unsigned long long)o.iters);
            fflush(stdout);
        }
    }

    if (csv != NULL) {
        fclose(csv);
    }
    if (dump_csv != NULL) {
        fclose(dump_csv);
    }

    printf("SUMMARY total=%llu rejected=%llu finished=%llu capped=%llu "
           "seed=%llu steps=%llu\n",
           agg.total, agg.result[R_REJECTED], agg.result[R_FINISHED],
           agg.result[R_CAPPED], (unsigned long long)o.seed,
           (unsigned long long)o.steps);
    {
        uint32_t c;
        for (c = 0; c < CAT_COUNT; ++c) {
            if (cat_agg[c].total == 0u) {
                continue;
            }
            printf("CAT name=%s total=%llu rejected=%llu finished=%llu "
                   "capped=%llu\n",
                   g_cat_name[c], cat_agg[c].total,
                   cat_agg[c].result[R_REJECTED],
                   cat_agg[c].result[R_FINISHED], cat_agg[c].result[R_CAPPED]);
        }
        for (i = 0; i < (uint64_t)g_nbases; ++i) {
            if (base_agg[i].total == 0u) {
                continue;
            }
            printf("BASEAGG name=%s total=%llu rejected=%llu finished=%llu "
                   "capped=%llu\n",
                   g_bases[i].name, base_agg[i].total,
                   base_agg[i].result[R_REJECTED],
                   base_agg[i].result[R_FINISHED],
                   base_agg[i].result[R_CAPPED]);
        }
    }
    for (i = 0; i < (uint64_t)g_nbases; ++i) {
        free(g_bases[i].data);
        free(g_bases[i].pcs);
        free(g_bases[i].ops);
    }
    free(cat_agg);
    free(base_agg);
    free(buf);
    if (journal != NULL) {
        fclose(journal);
    }
    /* Printed after the harness's own blocks are released: any block still
     * live here is an engine-side leak (none are expected). */
    faq_print_stats();
    if (rc == 0) {
        printf("RESULT PASS\n");
    } else {
        printf("RESULT FAIL\n");
    }
    fflush(stdout);
    return rc;
}