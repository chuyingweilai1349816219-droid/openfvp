/* FVP VM in C -- interpreter state (threads, frames, variables). */
#ifndef FVP_VM_H
#define FVP_VM_H

#include <stdint.h>
#include <stdio.h>

#include "encoding.h"
#include "fvp.h"
#include "hcb.h"

typedef struct FvpFrame {
    uint32_t ret;
    int32_t nlocals;
    FvpValue *locals;
    int32_t nargs;
    FvpValue *args;
    int pending;
} FvpFrame;

typedef struct FvpThread {
    uint32_t pc;
    const char *name;
    FvpValue *stack;
    int32_t sp;
    int32_t scap;
    FvpFrame *frames;
    int32_t nframes;
    int32_t fcap;
    int alive;
    int blocked;
    int32_t wait;
    FvpValue temp;
} FvpThread;

/* (id, value) keyed dict with Python equality/hash semantics; used by the
 * garr_* / larr_* opcodes (the reference uses a Python dict). */
typedef struct FvpKey {
    uint16_t id;
    uint8_t type;
    int64_t i;
    double f;
    const FvpStr *s;
} FvpKey;

typedef struct FvpMapEntry {
    FvpKey key;
    FvpValue val;
    uint8_t used;
} FvpMapEntry;

typedef struct FvpMap {
    FvpMapEntry *entries;
    size_t cap;
    size_t count;
} FvpMap;

typedef struct FvpTextLine {
    FvpValue layer;
    FvpStr *text;
} FvpTextLine;

/* ------------------------------------------------------------------ */
/* Optional render-layer attachment (src/gfx.c)                        */
/*                                                                     */
/* fvm.exe never attaches one: every rendering command in syscall.c     */
/* then takes the exact same code path as before (no-op / temp value).  */
/* A window shell or fvp/tests/render_vm.c can attach a Gfx context so  */
/* the script paints while it runs.                                     */
/* ------------------------------------------------------------------ */

struct Gfx;

#define FVP_GFX_PRIM_MAX 4096 /* must match GFX_PRIM_MAX in src/gfx.h */

typedef struct FvpGfxBridge {
    struct Gfx *gfx;
    /* Shadow of prim.graph: MotionAnim needs the graph slot of a prim and
     * gfx.h intentionally exposes no primitive getter.  Kept in sync by the
     * syscall layer (PrimSetNull/Sprt/Text/Tile resets, PrimSetSprt). */
    int32_t prim_graph[FVP_GFX_PRIM_MAX];
    /* Optional TextPrint/TextClear observer.  Used by the snapshot test to
     * reproduce render.py's --hold-text convention; NULL in fvm.exe.  The
     * text pointer is only valid for the duration of the call. */
    void (*on_text)(void *ud, int layer, const char *text, int clear);
    void *text_ud;
} FvpGfxBridge;

typedef struct FvpVM {
    FvpHcb *hcb;
    FvpValue globals[FVP_GLOBAL_COUNT];
    FvpMap garr;
    FvpMap larr;
    FvpThread **threads;
    size_t nthreads;
    size_t thread_cap;
    FvpThread *cur;
    uint64_t steps;
    uint64_t max_steps;
    int64_t click_period;
    int64_t cx;
    int64_t cy;
    int64_t key_mask;
    int quiet;
    FvpArena arena;         /* runtime strings (concat, int -> str) */
    FvpTextLine *text;
    size_t ntext;
    size_t text_cap;
    uint32_t *unknown_count; /* 65536 counters */
    uint16_t *unknown_order;
    size_t nunknown;
    size_t ucap;
    FILE *pctrace;
    int warned_neg_str;
    FvpGfxBridge *gfx_bridge; /* NULL = rendering commands are no-ops */
} FvpVM;

void fvp_vm_init(FvpVM *vm, FvpHcb *hcb, int quiet);
void fvp_vm_free(FvpVM *vm);
void fvp_vm_start(FvpVM *vm);
uint64_t fvp_vm_run(FvpVM *vm, uint64_t max_steps);

/* Attach (or detach, gfx == NULL) a rendering context.  Only the syscall
 * layer uses it; the VM's own semantics (steps, stacks, temp values, text
 * log) are unchanged whether or not a context is attached. */
void fvp_vm_attach_gfx(FvpVM *vm, struct Gfx *gfx);

void fvp_vm_add_text(FvpVM *vm, const FvpValue *layer, FvpStr *text);
void fvp_vm_count_unknown(FvpVM *vm, uint16_t cid);

/* Stack helpers (shared with syscall.c). */
void fvp_thread_push(FvpThread *t, FvpValue v);
FvpValue fvp_thread_pop(FvpThread *t);

void fvp_map_init(FvpMap *m);
void fvp_map_free(FvpMap *m);
FvpValue *fvp_map_get(FvpMap *m, const FvpKey *k);
void fvp_map_set(FvpMap *m, const FvpKey *k, FvpValue v);

#endif /* FVP_VM_H */