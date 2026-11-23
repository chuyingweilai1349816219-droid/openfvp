/* FVP VM in C -- interpreter core.
 *
 * This is a line-by-line port of fvp/ref/fvm_ref.py's VM: same value model,
 * same stack/frame rules, same thread rotation, same syscall contract
 * (return value -> temp, not the stack).
 */
#include "vm.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "bin.h"
#include "syscall.h"

/* Audio / resource symbols are optional for the same reason the gfx ones are
 * (see syscall.c): bin/fvm.exe links neither src/snd.c nor src/bin.c, so a
 * program that attaches an audio context is the only one that can reach
 * them.  Weak undefined symbols resolve to NULL there. */
#pragma weak bin_close
#pragma weak bin_find
#pragma weak bin_open
#pragma weak bin_read_alloc

/* ------------------------------------------------------------------ */
/* Map with Python equality semantics                                  */
/* ------------------------------------------------------------------ */

static uint64_t fvp_hash_u64(uint64_t x)
{
    x ^= x >> 33;
    x *= 0xFF51AFD7ED558CCDULL;
    x ^= x >> 33;
    x *= 0xC4CEB9FE1A85EC53ULL;
    x ^= x >> 33;
    return x;
}

static uint64_t fvp_hash_double(double d)
{
    uint64_t bits;
    if (d == 0.0) {
        d = 0.0; /* Python: hash(0.0) == hash(-0.0) */
    }
    memcpy(&bits, &d, sizeof(bits));
    return fvp_hash_u64(bits);
}

static uint64_t fvp_hash_key(const FvpKey *k)
{
    uint64_t h;
    if (k->type == (uint8_t)FVP_STR) {
        const FvpStr *s = k->s;
        uint32_t i;
        h = 1469598103934665603ULL;
        for (i = 0; i < s->len; ++i) {
            h ^= (unsigned char)s->data[i];
            h *= 1099511628211ULL;
        }
    } else if (k->type == (uint8_t)FVP_INT) {
        h = fvp_hash_double((double)k->i);
    } else {
        h = fvp_hash_double(k->f);
    }
    return fvp_hash_u64(h ^ ((uint64_t)k->id * 0x9E3779B97F4A7C15ULL));
}

static int fvp_key_eq(const FvpKey *a, const FvpKey *b)
{
    FvpValue va;
    FvpValue vb;
    if (a->id != b->id) {
        return 0;
    }
    va.type = a->type;
    va.i = a->i;
    va.f = a->f;
    va.str = (FvpStr *)a->s;
    vb.type = b->type;
    vb.i = b->i;
    vb.f = b->f;
    vb.str = (FvpStr *)b->s;
    return fvp_value_eq(&va, &vb);
}

void fvp_map_init(FvpMap *m)
{
    m->cap = 1024;
    m->count = 0;
    m->entries = (FvpMapEntry *)fvp_xcalloc(m->cap, sizeof(FvpMapEntry));
}

void fvp_map_free(FvpMap *m)
{
    free(m->entries);
    m->entries = NULL;
    m->cap = 0;
    m->count = 0;
}

static void fvp_map_grow(FvpMap *m)
{
    FvpMap old = *m;
    size_t cap = old.cap * 2u;
    size_t i;
    m->cap = cap;
    m->count = 0;
    m->entries = (FvpMapEntry *)fvp_xcalloc(cap, sizeof(FvpMapEntry));
    for (i = 0; i < old.cap; ++i) {
        if (old.entries[i].used) {
            fvp_map_set(m, &old.entries[i].key, old.entries[i].val);
        }
    }
    free(old.entries);
}

FvpValue *fvp_map_get(FvpMap *m, const FvpKey *k)
{
    size_t mask;
    size_t i;
    if (m->cap == 0) {
        return NULL;
    }
    mask = m->cap - 1u;
    i = (size_t)fvp_hash_key(k) & mask;
    for (;;) {
        FvpMapEntry *e = &m->entries[i];
        if (!e->used) {
            return NULL;
        }
        if (fvp_key_eq(&e->key, k)) {
            return &e->val;
        }
        i = (i + 1u) & mask;
    }
}

void fvp_map_set(FvpMap *m, const FvpKey *k, FvpValue v)
{
    size_t mask;
    size_t i;
    if ((m->count + 1u) * 4u >= m->cap * 3u) {
        fvp_map_grow(m);
    }
    mask = m->cap - 1u;
    i = (size_t)fvp_hash_key(k) & mask;
    for (;;) {
        FvpMapEntry *e = &m->entries[i];
        if (!e->used) {
            e->used = 1;
            e->key = *k;
            e->val = v;
            m->count += 1;
            return;
        }
        if (fvp_key_eq(&e->key, k)) {
            e->val = v;
            return;
        }
        i = (i + 1u) & mask;
    }
}

/* ------------------------------------------------------------------ */
/* Threads and frames                                                  */
/* ------------------------------------------------------------------ */

void fvp_thread_push(FvpThread *t, FvpValue v)
{
    if (t->sp == t->scap) {
        int32_t cap = t->scap ? t->scap * 2 : 64;
        t->stack = (FvpValue *)fvp_xrealloc(t->stack,
                                            (size_t)cap * sizeof(FvpValue));
        t->scap = cap;
    }
    t->stack[t->sp] = v;
    t->sp += 1;
}

FvpValue fvp_thread_pop(FvpThread *t)
{
    if (t->sp == 0) {
        return fvp_make_int(0);
    }
    t->sp -= 1;
    return t->stack[t->sp];
}

static FvpFrame *fvp_frame_top(FvpThread *t)
{
    return t->nframes > 0 ? &t->frames[t->nframes - 1] : NULL;
}

static void fvp_frame_push(FvpThread *t, const FvpFrame *f)
{
    if (t->nframes == t->fcap) {
        int32_t cap = t->fcap ? t->fcap * 2 : 8;
        t->frames = (FvpFrame *)fvp_xrealloc(t->frames,
                                             (size_t)cap * sizeof(FvpFrame));
        t->fcap = cap;
    }
    t->frames[t->nframes] = *f;
    t->nframes += 1;
}

static void fvp_frame_release(FvpFrame *f)
{
    free(f->locals);
    free(f->args);
    f->locals = NULL;
    f->args = NULL;
    f->nlocals = 0;
    f->nargs = 0;
}

static FvpFrame fvp_frame_pop(FvpThread *t)
{
    FvpFrame f = t->frames[t->nframes - 1];
    t->nframes -= 1;
    return f;
}

static FvpThread *fvp_thread_new(uint32_t pc, const char *name)
{
    FvpThread *t = (FvpThread *)fvp_xcalloc(1, sizeof(FvpThread));
    t->pc = pc;
    t->name = name;
    t->alive = 1;
    t->temp = fvp_make_int(0);
    return t;
}

static void fvp_thread_free(FvpThread *t)
{
    int32_t i;
    for (i = 0; i < t->nframes; ++i) {
        fvp_frame_release(&t->frames[i]);
    }
    free(t->frames);
    free(t->stack);
    free(t);
}

/* ------------------------------------------------------------------ */
/* Python arithmetic                                                   */
/* ------------------------------------------------------------------ */

static double fvp_as_double(const FvpValue *v)
{
    return v->type == (uint8_t)FVP_INT ? (double)v->i : v->f;
}

static FvpStr *fvp_str_concat(FvpVM *vm, const FvpStr *a, const FvpStr *b)
{
    uint64_t n = (uint64_t)a->len + (uint64_t)b->len;
    FvpStr *s;
    if (n > 0xF0000000ULL) {
        fvp_fatal("string concatenation too large");
    }
    s = (FvpStr *)fvp_arena_alloc(&vm->arena, sizeof(FvpStr) + (size_t)n + 1u);
    s->len = (uint32_t)n;
    if (a->len != 0) {
        memcpy(s->data, a->data, a->len);
    }
    if (b->len != 0) {
        memcpy(s->data + a->len, b->data, b->len);
    }
    s->data[n] = '\0';
    return s;
}

static FvpStr *fvp_str_repeat(FvpVM *vm, const FvpStr *s, int64_t count)
{
    uint64_t n;
    FvpStr *r;
    uint64_t i;
    if (count <= 0 || s->len == 0) {
        return fvp_arena_str(&vm->arena, "", 0);
    }
    n = (uint64_t)s->len * (uint64_t)count;
    if (n > 0x10000000ULL) {
        fvp_fatal("string repeat too large (%llu bytes)", (unsigned long long)n);
    }
    r = (FvpStr *)fvp_arena_alloc(&vm->arena, sizeof(FvpStr) + (size_t)n + 1u);
    r->len = (uint32_t)n;
    for (i = 0; i < (uint64_t)count; ++i) {
        memcpy(r->data + i * s->len, s->data, s->len);
    }
    r->data[n] = '\0';
    return r;
}

static int64_t fvp_py_mod_int(int64_t a, int64_t b)
{
    int64_t r = a % b;
    if (r != 0 && ((r < 0) != (b < 0))) {
        r += b;
    }
    return r;
}

static double fvp_py_mod_float(double a, double b)
{
    double r = fmod(a, b);
    if (r != 0.0) {
        if ((r < 0.0) != (b < 0.0)) {
            r += b;
        }
    } else {
        r = copysign(0.0, b);
    }
    return r;
}

static FvpValue fvp_op_add(FvpVM *vm, const FvpValue *a, const FvpValue *b)
{
    if (a->type == (uint8_t)FVP_INT && b->type == (uint8_t)FVP_INT) {
        return fvp_make_int(a->i + b->i);
    }
    if (a->type != (uint8_t)FVP_STR && b->type != (uint8_t)FVP_STR) {
        return fvp_make_float(fvp_as_double(a) + fvp_as_double(b));
    }
    if (a->type == (uint8_t)FVP_STR && b->type == (uint8_t)FVP_STR) {
        return fvp_make_str(fvp_str_concat(vm, a->str, b->str));
    }
    return fvp_make_int(0); /* Python TypeError -> 0 */
}

static FvpValue fvp_op_sub(FvpVM *vm, const FvpValue *a, const FvpValue *b)
{
    (void)vm;
    if (a->type == (uint8_t)FVP_INT && b->type == (uint8_t)FVP_INT) {
        return fvp_make_int(a->i - b->i);
    }
    if (a->type != (uint8_t)FVP_STR && b->type != (uint8_t)FVP_STR) {
        return fvp_make_float(fvp_as_double(a) - fvp_as_double(b));
    }
    return fvp_make_int(0);
}

static FvpValue fvp_op_mul(FvpVM *vm, const FvpValue *a, const FvpValue *b)
{
    if (a->type == (uint8_t)FVP_INT && b->type == (uint8_t)FVP_INT) {
        return fvp_make_int(a->i * b->i);
    }
    if (a->type != (uint8_t)FVP_STR && b->type != (uint8_t)FVP_STR) {
        return fvp_make_float(fvp_as_double(a) * fvp_as_double(b));
    }
    if (a->type == (uint8_t)FVP_STR && b->type == (uint8_t)FVP_INT) {
        return fvp_make_str(fvp_str_repeat(vm, a->str, b->i));
    }
    if (a->type == (uint8_t)FVP_INT && b->type == (uint8_t)FVP_STR) {
        return fvp_make_str(fvp_str_repeat(vm, b->str, a->i));
    }
    return fvp_make_int(0);
}

static FvpValue fvp_op_div(FvpVM *vm, const FvpValue *a, const FvpValue *b)
{
    (void)vm;
    if (!fvp_value_truth(b)) {
        return fvp_make_int(0);
    }
    if (a->type == (uint8_t)FVP_STR || b->type == (uint8_t)FVP_STR) {
        return fvp_make_int(0);
    }
    return fvp_make_float(fvp_as_double(a) / fvp_as_double(b));
}

static FvpValue fvp_op_mod(FvpVM *vm, const FvpValue *a, const FvpValue *b)
{
    (void)vm;
    if (!fvp_value_truth(b)) {
        return fvp_make_int(0);
    }
    if (a->type == (uint8_t)FVP_INT && b->type == (uint8_t)FVP_INT) {
        return fvp_make_int(fvp_py_mod_int(a->i, b->i));
    }
    if (a->type != (uint8_t)FVP_STR && b->type != (uint8_t)FVP_STR) {
        return fvp_make_float(fvp_py_mod_float(fvp_as_double(a), fvp_as_double(b)));
    }
    return fvp_make_int(0);
}

static void fvp_binop(FvpVM *vm, FvpThread *t, uint8_t op)
{
    FvpValue b = fvp_thread_pop(t);
    FvpValue a = fvp_thread_pop(t);
    FvpValue r = fvp_make_int(0);
    switch (op) {
    case 0x1a:
        r = fvp_op_add(vm, &a, &b);
        break;
    case 0x1b:
        r = fvp_op_sub(vm, &a, &b);
        break;
    case 0x1c:
        r = fvp_op_mul(vm, &a, &b);
        break;
    case 0x1d:
        r = fvp_op_div(vm, &a, &b);
        break;
    case 0x1e:
        r = fvp_op_mod(vm, &a, &b);
        break;
    case 0x1f:
        r = fvp_make_int(fvp_value_truth(&a) ? 1 : 0);
        break;
    case 0x20:
        r = fvp_make_int((fvp_value_truth(&a) && fvp_value_truth(&b)) ? 1 : 0);
        break;
    case 0x21:
        r = fvp_make_int((fvp_value_truth(&a) || fvp_value_truth(&b)) ? 1 : 0);
        break;
    case 0x22:
        r = fvp_make_int(fvp_value_eq(&a, &b) ? 1 : 0);
        break;
    case 0x23:
        r = fvp_make_int(fvp_value_eq(&a, &b) ? 0 : 1);
        break;
    case 0x24:
    case 0x25:
    case 0x26:
    case 0x27: {
        int ok = 0;
        int c = fvp_value_cmp(&a, &b, &ok);
        int v = 0;
        if (ok) {
            if (op == 0x24) {
                v = c > 0;
            } else if (op == 0x25) {
                v = c <= 0;
            } else if (op == 0x26) {
                v = c < 0;
            } else {
                v = c >= 0;
            }
        }
        r = fvp_make_int(v ? 1 : 0);
        break;
    }
    default:
        break;
    }
    fvp_thread_push(t, r);
}

/* ------------------------------------------------------------------ */
/* Interpreter                                                         */
/* ------------------------------------------------------------------ */

static void fvp_step(FvpVM *vm, FvpThread *t)
{
    const FvpInstr *ins = fvp_hcb_at(vm->hcb, t->pc);
    if (ins == NULL) {
        t->alive = 0;
        return;
    }
    vm->steps += 1;
    if (vm->pctrace != NULL) {
        fprintf(vm->pctrace, "%x\n", (unsigned)t->pc);
    }
    switch (ins->op) {
    case 0x00: /* nop: the reference has no branch for it -> thread dies */
        t->alive = 0;
        break;
    case 0x01: { /* initstack locals, ? */
        FvpFrame *top = fvp_frame_top(t);
        if (top != NULL && top->pending) {
            free(top->locals);
            top->nlocals = (int32_t)ins->a;
            top->locals = ins->a != 0
                              ? (FvpValue *)fvp_xcalloc((size_t)ins->a, sizeof(FvpValue))
                              : NULL;
            top->pending = 0;
        } else {
            FvpFrame f;
            f.ret = ins->pc;
            f.nlocals = (int32_t)ins->a;
            f.locals = ins->a != 0
                           ? (FvpValue *)fvp_xcalloc((size_t)ins->a, sizeof(FvpValue))
                           : NULL;
            f.nargs = 0;
            f.args = NULL;
            f.pending = 0;
            fvp_frame_push(t, &f);
        }
        t->pc += 3u;
        break;
    }
    case 0x02: { /* call */
        FvpFrame f;
        f.ret = ins->pc + 5u;
        f.nlocals = 0;
        f.locals = NULL;
        f.nargs = t->sp;
        if (t->sp > 0) {
            f.args = (FvpValue *)fvp_xmalloc((size_t)t->sp * sizeof(FvpValue));
            memcpy(f.args, t->stack, (size_t)t->sp * sizeof(FvpValue));
        } else {
            f.args = NULL;
        }
        f.pending = 1;
        fvp_frame_push(t, &f);
        t->sp = 0;
        t->pc = ins->u;
        break;
    }
    case 0x03: /* syscall */
        fvp_syscall(vm, t, ins->w);
        t->pc += 3u;
        break;
    case 0x04:
    case 0x05: /* ret / ret2 */
        if (t->nframes > 0) {
            FvpFrame f = fvp_frame_pop(t);
            t->pc = f.ret;
            fvp_frame_release(&f);
        } else {
            t->alive = 0;
            t->pc = vm->hcb->script_len;
        }
        break;
    case 0x06: /* jmp */
        t->pc = ins->u;
        break;
    case 0x07: { /* jmpcond: jump when false */
        FvpValue c = fvp_thread_pop(t);
        if (!fvp_value_truth(&c)) {
            t->pc = ins->u;
        } else {
            t->pc += 5u;
        }
        break;
    }
    case 0x08:
        fvp_thread_push(t, fvp_make_int(1));
        t->pc += 1u;
        break;
    case 0x09:
        fvp_thread_push(t, fvp_make_int(0));
        t->pc += 1u;
        break;
    case 0x0a:
        fvp_thread_push(t, fvp_make_int((int64_t)ins->u));
        t->pc += 5u;
        break;
    case 0x0b:
        fvp_thread_push(t, fvp_make_int((int64_t)ins->w));
        t->pc += 3u;
        break;
    case 0x0c:
        fvp_thread_push(t, fvp_make_int((int64_t)ins->a));
        t->pc += 2u;
        break;
    case 0x0d: { /* pushfloat: u32 bit pattern of an IEEE-754 float32 */
        float f;
        uint32_t bits = ins->u;
        memcpy(&f, &bits, sizeof(f));
        fvp_thread_push(t, fvp_make_float((double)f));
        t->pc += 5u;
        break;
    }
    case 0x0e:
        fvp_thread_push(t, fvp_make_str(ins->str));
        t->pc += ins->size;
        break;
    case 0x0f:
        if (ins->w < FVP_GLOBAL_COUNT) {
            fvp_thread_push(t, vm->globals[ins->w]);
        } else {
            fvp_thread_push(t, fvp_make_int(0));
        }
        t->pc += 3u;
        break;
    case 0x10: { /* pushstack */
        FvpFrame *f = fvp_frame_top(t);
        if (f == NULL) {
            fvp_thread_push(t, fvp_make_int(0));
        } else if ((int32_t)ins->a < f->nlocals) {
            fvp_thread_push(t, f->locals[ins->a]);
        } else {
            int32_t idx = (int32_t)ins->a - (255 - f->nargs);
            if (idx >= 0 && idx < f->nargs) {
                fvp_thread_push(t, f->args[idx]);
            } else {
                fvp_thread_push(t, fvp_make_int(0));
            }
        }
        t->pc += 2u;
        break;
    }
    case 0x11: { /* garr_read */
        FvpValue i = fvp_thread_pop(t);
        FvpKey k;
        FvpValue *p;
        k.id = ins->w;
        k.type = i.type;
        k.i = i.i;
        k.f = i.f;
        k.s = i.str;
        p = fvp_map_get(&vm->garr, &k);
        fvp_thread_push(t, p != NULL ? *p : fvp_make_int(0));
        t->pc += 3u;
        break;
    }
    case 0x12: { /* larr_read */
        FvpValue i = fvp_thread_pop(t);
        FvpKey k;
        FvpValue *p;
        k.id = ins->a;
        k.type = i.type;
        k.i = i.i;
        k.f = i.f;
        k.s = i.str;
        p = fvp_map_get(&vm->larr, &k);
        fvp_thread_push(t, p != NULL ? *p : fvp_make_int(0));
        t->pc += 2u;
        break;
    }
    case 0x13: { /* pushtop */
        FvpFrame *f = fvp_frame_top(t);
        if (f != NULL && f->nlocals > 0) {
            fvp_thread_push(t, f->locals[0]);
        } else {
            fvp_thread_push(t, fvp_make_int(0));
        }
        t->pc += 1u;
        break;
    }
    case 0x14: /* pushtemp */
        fvp_thread_push(t, t->temp);
        t->pc += 1u;
        break;
    case 0x15: { /* popglobal */
        FvpValue v = fvp_thread_pop(t);
        if (ins->w < FVP_GLOBAL_COUNT) {
            vm->globals[ins->w] = v;
        }
        t->pc += 3u;
        break;
    }
    case 0x16: { /* copystack */
        FvpValue v = fvp_thread_pop(t);
        FvpFrame *f = fvp_frame_top(t);
        if (f != NULL && (int32_t)ins->a < f->nlocals) {
            f->locals[ins->a] = v;
        }
        t->pc += 2u;
        break;
    }
    case 0x17: { /* garr_write */
        FvpValue v = fvp_thread_pop(t);
        FvpValue i = fvp_thread_pop(t);
        FvpKey k;
        k.id = ins->w;
        k.type = i.type;
        k.i = i.i;
        k.f = i.f;
        k.s = i.str;
        fvp_map_set(&vm->garr, &k, v);
        t->pc += 3u;
        break;
    }
    case 0x18: { /* larr_write */
        FvpValue v = fvp_thread_pop(t);
        FvpValue i = fvp_thread_pop(t);
        FvpKey k;
        k.id = ins->a;
        k.type = i.type;
        k.i = i.i;
        k.f = i.f;
        k.s = i.str;
        fvp_map_set(&vm->larr, &k, v);
        t->pc += 2u;
        break;
    }
    case 0x19: { /* neg */
        FvpValue v = fvp_thread_pop(t);
        if (v.type == (uint8_t)FVP_INT) {
            fvp_thread_push(t, fvp_make_int(-v.i));
        } else if (v.type == (uint8_t)FVP_FLOAT) {
            fvp_thread_push(t, fvp_make_float(-v.f));
        } else {
            if (!vm->warned_neg_str) {
                vm->warned_neg_str = 1;
                fputs("fvm: warning: neg on a string (Python would raise "
                      "TypeError); using 0\n", stderr);
            }
            fvp_thread_push(t, fvp_make_int(0));
        }
        t->pc += 1u;
        break;
    }
    default:
        if (ins->op >= 0x1a && ins->op <= 0x27) {
            fvp_binop(vm, t, ins->op);
            t->pc += 1u;
        } else {
            t->alive = 0;
        }
        break;
    }
}

/* ------------------------------------------------------------------ */
/* VM lifecycle                                                        */
/* ------------------------------------------------------------------ */

void fvp_vm_init(FvpVM *vm, FvpHcb *hcb, int quiet)
{
    memset(vm, 0, sizeof(*vm));
    vm->hcb = hcb;
    vm->quiet = quiet;
    vm->cx = 960;
    vm->cy = 540;
    vm->click_period = 0;
    fvp_arena_init(&vm->arena);
    fvp_map_init(&vm->garr);
    fvp_map_init(&vm->larr);
    fvp_textattr_reset(vm->textattr, FVP_TEXT_WINDOWS);
    vm->save_slot = -1;
    vm->save_thumb_w = 96; /* 引擎初值 96x72（docs/save-format.md §2.3） */
    vm->save_thumb_h = 72;
    vm->unknown_count = (uint32_t *)fvp_xcalloc(65536u, sizeof(uint32_t));
}

void fvp_vm_attach_gfx(FvpVM *vm, struct Gfx *gfx)
{
    if (vm == NULL) {
        return;
    }
    if (gfx == NULL) {
        free(vm->gfx_bridge);
        vm->gfx_bridge = NULL;
        return;
    }
    if (vm->gfx_bridge == NULL) {
        int i;
        vm->gfx_bridge = (FvpGfxBridge *)fvp_xcalloc(1, sizeof *vm->gfx_bridge);
        for (i = 0; i < FVP_GFX_PRIM_MAX; i++) {
            vm->gfx_bridge->prim_graph[i] = -1; /* render.py Prim.graph default */
        }
    }
    vm->gfx_bridge->gfx = gfx;
}

/* ------------------------------------------------------------------ */
/* Optional audio attachment (src/snd.c + src/bin.c)                   */
/*                                                                     */
/* The bridge holds the resource bank (paths only; packages are opened  */
/* lazily) and the per-slot state the audio commands need.  The            */
/* syscall layer (src/syscall.c) does the command forwarding and calls  */
/* back into snd.c; this file only owns the lifetime and the name       */
/* resolution, because both need to keep working in fvm.exe (no snd.c,  */
/* no bin.c linked: every bin_* symbol above is weak).                  */
/* ------------------------------------------------------------------ */

static const char *fvp_snd_path_base(const char *path)
{
    const char *a;
    const char *b;
    const char *p;
    if (path == NULL) {
        return "";
    }
    a = strrchr(path, '/');
    b = strrchr(path, '\\');
    p = a;
    if (b != NULL && (p == NULL || b > p)) {
        p = b;
    }
    return p != NULL ? p + 1 : path;
}

static void fvp_snd_lower(char *dst, size_t cap, const char *src)
{
    size_t i;
    if (cap == 0) {
        return;
    }
    for (i = 0; src[i] != '\0' && i + 1u < cap; i++) {
        char c = src[i];
        if (c >= 'A' && c <= 'Z') {
            c = (char)(c - 'A' + 'a');
        }
        dst[i] = c;
    }
    dst[i] = '\0';
}

/* Which audio package family does an archive path belong to?
 * Keep in sync with add_audio_dir().  -1 = not an audio package. */
enum {
    FVP_SND_FAM_BGM = 0,
    FVP_SND_FAM_BGM2 = 1,
    FVP_SND_FAM_VOICE = 2,
    FVP_SND_FAM_SE = 3,
    FVP_SND_FAM_SE_ENV = 4,
    FVP_SND_FAM_SE_SYS = 5
};

static int fvp_snd_archive_family(const char *path)
{
    char low[64];
    fvp_snd_lower(low, sizeof low, fvp_snd_path_base(path));
    if (strncmp(low, "bgm2", 4) == 0) return FVP_SND_FAM_BGM2;
    if (strncmp(low, "bgm", 3) == 0) return FVP_SND_FAM_BGM;
    if (strncmp(low, "voice", 5) == 0) return FVP_SND_FAM_VOICE;
    if (strncmp(low, "se_env", 6) == 0) return FVP_SND_FAM_SE_ENV;
    if (strncmp(low, "se_sys", 6) == 0) return FVP_SND_FAM_SE_SYS;
    if (strncmp(low, "se", 2) == 0) return FVP_SND_FAM_SE;
    return -1;
}

/* Family implied by the resource name prefix ("bgm/001" -> bgm.bin).
 * Scripts use upper and lower case interchangeably ("BGM/050", "bgm/001"),
 * so compare case-insensitively. */
static int fvp_snd_name_family(const char *name)
{
    char tok[32];
    size_t n = 0;
    const char *p = name;
    while (*p != '\0' && *p != '/' && *p != '\\' && n + 1u < sizeof tok) {
        char c = *p++;
        if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
        tok[n++] = c;
    }
    tok[n] = '\0';
    if (n == 0) {
        return -1;
    }
    if (strcmp(tok, "bgm2") == 0) return FVP_SND_FAM_BGM2;
    if (strcmp(tok, "bgm") == 0) return FVP_SND_FAM_BGM;
    if (strcmp(tok, "voice2") == 0) return FVP_SND_FAM_VOICE;
    if (strcmp(tok, "voice") == 0) return FVP_SND_FAM_VOICE;
    if (strcmp(tok, "se_env") == 0) return FVP_SND_FAM_SE_ENV;
    if (strcmp(tok, "se_sys") == 0) return FVP_SND_FAM_SE_SYS;
    if (strcmp(tok, "env") == 0) return FVP_SND_FAM_SE_ENV;
    if (strcmp(tok, "se") == 0) return FVP_SND_FAM_SE;
    if (strcmp(tok, "sound") == 0) return FVP_SND_FAM_SE;
    return -1;
}

static void fvp_snd_slot_init(FvpSndSlot *sl)
{
    memset(sl, 0, sizeof *sl);
    sl->type = -1;
    sl->volume = 100;
    sl->snd_chan = -1;
}

static void fvp_snd_slot_free(FvpSndSlot *sl)
{
    free(sl->name);
    sl->name = NULL;
}

static void fvp_snd_bridge_free(FvpSndBridge *b)
{
    int i;
    if (b == NULL) {
        return;
    }
    for (i = 0; i < b->narchives; i++) {
        free(b->archives[i].path);
        if (b->archives[i].bin != NULL && bin_close != NULL) {
            bin_close(b->archives[i].bin);
        }
        b->archives[i].bin = NULL;
        b->archives[i].path = NULL;
    }
    for (i = 0; i < FVP_SND_AUDIO_SLOTS; i++) {
        fvp_snd_slot_free(&b->audio[i]);
    }
    for (i = 0; i < FVP_SND_SOUND_SLOTS; i++) {
        fvp_snd_slot_free(&b->sound[i]);
    }
    free(b);
}

void fvp_vm_attach_snd(FvpVM *vm, struct Snd *snd)
{
    if (vm == NULL) {
        return;
    }
    if (snd == NULL) {
        fvp_snd_bridge_free(vm->snd_bridge);
        vm->snd_bridge = NULL;
        return;
    }
    if (vm->snd_bridge == NULL) {
        FvpSndBridge *b = (FvpSndBridge *)fvp_xcalloc(1, sizeof *b);
        int i;
        b->snd = snd;
        b->master_vol = 100;
        for (i = 0; i < FVP_SND_TYPES; i++) {
            b->type_vol[i] = 100;
        }
        for (i = 0; i < FVP_SND_AUDIO_SLOTS; i++) {
            fvp_snd_slot_init(&b->audio[i]);
        }
        for (i = 0; i < FVP_SND_SOUND_SLOTS; i++) {
            fvp_snd_slot_init(&b->sound[i]);
        }
        vm->snd_bridge = b;
    } else {
        vm->snd_bridge->snd = snd;
    }
}

int fvp_vm_snd_add_archive(FvpVM *vm, const char *path)
{
    FvpSndBridge *b = (vm != NULL) ? vm->snd_bridge : NULL;
    FvpSndArchive *a;
    if (b == NULL || path == NULL || path[0] == '\0') {
        return -1;
    }
    if (b->narchives >= FVP_SND_ARCHIVE_MAX) {
        return -1;
    }
    a = &b->archives[b->narchives];
    a->path = fvp_xstrdup(path);
    a->bin = NULL;   /* opened on first lookup */
    a->tried = 0;
    b->narchives += 1;
    return 0;
}

int fvp_vm_snd_add_audio_dir(FvpVM *vm, const char *dir)
{
    static const char *const names[6] = {
        "bgm.bin", "bgm2.bin", "voice.bin",
        "se.bin", "se_env.bin", "se_sys.bin"
    };
    char path[1024];
    int i;
    int added = 0;
    if (dir == NULL) {
        return 0;
    }
    /* A directory prefix longer than this would simply be truncated by
     * snprintf; skip such entries rather than registering a wrong path. */
    for (i = 0; i < 6; i++) {
        int n = snprintf(path, sizeof path, "%s/%s", dir, names[i]);
        if (n <= 0 || (size_t)n >= sizeof path) {
            continue;
        }
        if (fvp_vm_snd_add_archive(vm, path) == 0) {
            added += 1;
        }
    }
    return added;
}

/* Resolve "bgm/001" -> the bytes of entry "001" of bgm.bin.  BGM and SE
 * share entry names ("001" exists in four packages), so the script's prefix
 * picks the package; unknown prefixes fall back to a scan of all registered
 * packages, and a known prefix that misses still scans the others (the name
 * table of a 200 MB package is cheap to probe). */
int fvp_snd_bank_read(FvpSndBridge *b, const char *name, uint8_t **data,
                      size_t *size, const char **archive_out)
{
    char stem[512];
    const char *base;
    char *dot;
    size_t n;
    int family;
    int order[FVP_SND_ARCHIVE_MAX];
    int norder = 0;
    int opened = 0;
    int i;
    int k;

    if (data != NULL) *data = NULL;
    if (size != NULL) *size = 0;
    if (archive_out != NULL) *archive_out = NULL;
    if (b == NULL || name == NULL || b->narchives <= 0) return -2;
    if (bin_open == NULL || bin_find == NULL || bin_read_alloc == NULL ||
        bin_close == NULL) {
        return -2; /* built without src/bin.c (bin/fvm.exe) */
    }

    base = fvp_snd_path_base(name);
    n = strlen(base);
    if (n == 0 || n >= sizeof stem) return -1;
    memcpy(stem, base, n + 1u);
    dot = strrchr(stem, '.');
    if (dot != NULL && dot != stem) {
        *dot = '\0'; /* the archives store extension-less names */
    }
    if (stem[0] == '\0') return -1;

    family = fvp_snd_name_family(name);
    if (family >= 0) {
        for (i = 0; i < b->narchives; i++) {
            if (fvp_snd_archive_family(b->archives[i].path) == family) {
                order[norder++] = i;
            }
        }
    }
    for (i = 0; i < b->narchives; i++) {
        int seen = 0;
        for (k = 0; k < norder; k++) {
            if (order[k] == i) { seen = 1; break; }
        }
        if (!seen) order[norder++] = i;
    }

    for (k = 0; k < norder; k++) {
        FvpSndArchive *a = &b->archives[order[k]];
        int idx;
        if (!a->tried) {
            a->tried = 1;
            a->bin = bin_open(a->path);
        }
        if (a->bin == NULL) continue;
        opened += 1;
        idx = bin_find(a->bin, stem);
        if (idx < 0) continue;
        *data = (uint8_t *)bin_read_alloc(a->bin, (uint32_t)idx, size);
        if (*data == NULL) return -2;
        if (archive_out != NULL) *archive_out = fvp_snd_path_base(a->path);
        return 0;
    }
    return opened > 0 ? -1 : -2;
}

void fvp_vm_wheel_add(FvpVM *vm, int delta)
{
    if (vm == NULL || delta == 0) {
        return;
    }
    vm->wheel_delta += delta;
}

int fvp_vm_wheel_take(FvpVM *vm)
{
    int v;

    if (vm == NULL) {
        return 0;
    }
    v = vm->wheel_delta;
    vm->wheel_delta = 0;
    return v;
}

void fvp_vm_free(FvpVM *vm)
{
    size_t i;
    for (i = 0; i < vm->nthreads; ++i) {
        fvp_thread_free(vm->threads[i]);
    }
    free(vm->threads);
    free(vm->gfx_bridge);
    fvp_snd_bridge_free(vm->snd_bridge);
    fvp_map_free(&vm->garr);
    fvp_map_free(&vm->larr);
    fvp_arena_free(&vm->arena);
    free(vm->text);
    free(vm->unknown_count);
    free(vm->unknown_order);
    if (vm->pctrace != NULL) {
        fclose(vm->pctrace);
        vm->pctrace = NULL;
    }
    memset(vm, 0, sizeof(*vm));
}

void fvp_vm_start(FvpVM *vm)
{
    FvpThread *t = fvp_thread_new(vm->hcb->main_start, "main");
    if (vm->nthreads == vm->thread_cap) {
        vm->thread_cap = vm->thread_cap ? vm->thread_cap * 2u : 8u;
        vm->threads = (FvpThread **)fvp_xrealloc(vm->threads,
                                                 vm->thread_cap * sizeof(FvpThread *));
    }
    vm->threads[vm->nthreads] = t;
    vm->nthreads += 1;
    vm->cur = t;
}

uint64_t fvp_vm_run(FvpVM *vm, uint64_t max_steps)
{
    vm->max_steps = max_steps;
    while (vm->nthreads > 0 && vm->steps < vm->max_steps) {
        FvpThread *t = vm->threads[0];
        size_t i;
        /* pop(0) */
        for (i = 0; i + 1u < vm->nthreads; ++i) {
            vm->threads[i] = vm->threads[i + 1u];
        }
        vm->nthreads -= 1u;
        if (!t->alive) {
            fvp_thread_free(t);
            continue;
        }
        /* append(t) */
        if (vm->nthreads == vm->thread_cap) {
            vm->thread_cap = vm->thread_cap ? vm->thread_cap * 2u : 8u;
            vm->threads = (FvpThread **)fvp_xrealloc(
                vm->threads, vm->thread_cap * sizeof(FvpThread *));
        }
        vm->threads[vm->nthreads] = t;
        vm->nthreads += 1u;
        vm->cur = t;
        if (t->blocked) {
            t->wait += 1;
            if (t->wait < 2) {
                continue;
            }
            t->blocked = 0;
            t->wait = 0;
        }
        fvp_step(vm, t);
    }
    return vm->steps;
}

void fvp_vm_add_text(FvpVM *vm, const FvpValue *layer, FvpStr *text)
{
    if (vm->ntext == vm->text_cap) {
        vm->text_cap = vm->text_cap ? vm->text_cap * 2u : 64u;
        vm->text = (FvpTextLine *)fvp_xrealloc(vm->text,
                                               vm->text_cap * sizeof(FvpTextLine));
    }
    vm->text[vm->ntext].layer = *layer;
    vm->text[vm->ntext].text = text;
    vm->ntext += 1u;
    if (!vm->quiet) {
        fwrite(text->data, 1, text->len, stdout);
        fputc('\n', stdout);
    }
}

void fvp_vm_count_unknown(FvpVM *vm, uint16_t cid)
{
    if (vm->unknown_count[cid] == 0) {
        if (vm->nunknown == vm->ucap) {
            vm->ucap = vm->ucap ? vm->ucap * 2u : 16u;
            vm->unknown_order = (uint16_t *)fvp_xrealloc(
                vm->unknown_order, vm->ucap * sizeof(uint16_t));
        }
        vm->unknown_order[vm->nunknown] = cid;
        vm->nunknown += 1u;
    }
    if (vm->unknown_count[cid] != UINT32_MAX) {
        vm->unknown_count[cid] += 1u;
    }
}