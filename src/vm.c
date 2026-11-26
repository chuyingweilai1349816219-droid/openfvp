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

/* 修正版帧槽模型（FVP_FRAME_SLOTS=1）：
 *   参数槽 = 绝对槽 255-A..254（A = initstack 第一个字节）；
 *   局部变量块 = 参数块下面紧挨的 B 个槽，同时允许 0..B-1 直接寻址。
 * 默认（环境变量没设）走旧行为，保持冻结文本基线不变。 */
static int fvp_frame_slots_mode(void)
{
    static int mode = -1;
    if (mode < 0) {
        const char *v = getenv("FVP_FRAME_SLOTS");
        mode = (v != NULL && v[0] != '\0' && v[0] != '0') ? 1 : 0;
    }
    return mode;
}

static int fvp_frame_local_index(const FvpFrame *f, int32_t n)
{
    int32_t base;
    if (f == NULL || f->nlocals <= 0) {
        return -1;
    }
    if (n >= 0 && n < f->nlocals) {
        return (int)n;
    }
    if (!fvp_frame_slots_mode()) {
        return -1;
    }
    base = FVP_FRAME_ARG_END + 1 - f->ndecl - f->nlocals;
    if (n >= base && n < base + f->nlocals) {
        return (int)(n - base);
    }
    return -1;
}

static int fvp_frame_arg_index(const FvpFrame *f, int32_t n)
{
    int32_t start, idx;
    if (f == NULL || f->nargs <= 0) {
        return -1;
    }
    if (!fvp_frame_slots_mode()) {
        /* 旧行为（冻结基线用）：槽号只按"离 255 多远"映射到实参数组。 */
        idx = n - (255 - f->nargs);
    } else {
        if (f->ndecl <= 0) {
            return -1;
        }
        start = FVP_FRAME_ARG_END + 1 - f->ndecl;
        if (n < start || n > FVP_FRAME_ARG_END) {
            return -1;
        }
        idx = f->nargs - f->ndecl + (n - start);
    }
    if (idx < 0 || idx >= f->nargs) {
        return -1;
    }
    return (int)idx;
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
    t->slot = -1;
    t->sleep_ms = -1; /* 还没睡过：ThreadRaise 不会命中它 */
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

/* 布尔结果该怎么表示。默认（bool_type == 0）= int 1/0 + 数值真假，这是冻结基准
 * 用的模型；bool_type == 1 = 原版的"类型即真假"：假 → FVP_NULL(type 0)，
 * 真 → int 1（type != 0）。见 vm.h 里 bool_type 的说明与 docs/roadmap.md。 */
static FvpValue fvp_make_bool(FvpVM *vm, int b)
{
    if (b) {
        return fvp_make_int(1);
    }
    return (vm != NULL && vm->bool_type) ? fvp_make_null() : fvp_make_int(0);
}

/* 忠实比较（bool_type 模式）：照原版 7×7 类型对表（0x481450 / 0x481388 /
 * 0x481518，索引 a.type*7 + b.type）的语义。
 *
 * 表里只有下面这些类型对有专用比较器（表项 16/17/23/24/32/33/39/40）：
 *   (int,int) (int,float) (float,int) (float,float) (str,str) 及其反向，
 * 其余组合一律落到默认项 **0x444f50**：
 *   444f50: call 0x444600 (释放 a) ; call 0x444600 (释放 b) ;
 *           xor %eax,%eax ; ret $8      ← 返回 0
 * 也就是"比较结果 = 假"。所以 null / true / 异类型对的 `eq/gt/le/lt/ge`
 * 全部为假（`neq` 取反后为真），而不是我们默认模型里的"按 int 0 比较"。
 * 专用比较器本身返回**布尔**（例如 (int,int) 的 `0x444fa0: cmp; sete %al`），
 * 不是三态，所以这里也只返回 0/1。 */
static int fvp_pair_cmp_faithful(const FvpValue *a, const FvpValue *b, int *ok)
{
    int anum = a->type == (uint8_t)FVP_INT || a->type == (uint8_t)FVP_FLOAT;
    int bnum = b->type == (uint8_t)FVP_INT || b->type == (uint8_t)FVP_FLOAT;

    *ok = 0;
    if (anum && bnum) {
        if (a->type == (uint8_t)FVP_INT && b->type == (uint8_t)FVP_INT) {
            *ok = 1;
            if (a->i < b->i) {
                return -1;
            }
            return a->i > b->i;
        }
        {
            double x = a->type == (uint8_t)FVP_INT ? (double)a->i : a->f;
            double y = b->type == (uint8_t)FVP_INT ? (double)b->i : b->f;
            *ok = 1;
            if (x < y) {
                return -1;
            }
            return x > y;
        }
    }
    if (a->type == (uint8_t)FVP_STR && b->type == (uint8_t)FVP_STR) {
        const FvpStr *s = a->str;
        const FvpStr *t = b->str;
        uint32_t n = s->len < t->len ? s->len : t->len;

        *ok = 1;
        if (n != 0) {
            int c = memcmp(s->data, t->data, n);
            if (c != 0) {
                return c < 0 ? -1 : 1;
            }
        }
        if (s->len == t->len) {
            return 0;
        }
        return s->len < t->len ? -1 : 1;
    }
    return 0;
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
        /* 位测试（handler 0x444e40）：两个操作数都是 int 时算
         * (a.i & (1 << (b.i & 31))) != 0，否则结果 0。不是"取真值"。 */
        if (a.type == (uint8_t)FVP_INT && b.type == (uint8_t)FVP_INT) {
            r = fvp_make_bool(vm, (a.i & ((int64_t)1 << (int)(b.i & 31))) != 0);
        } else {
            r = fvp_make_bool(vm, 0);
        }
        break;
    case 0x20:
        r = fvp_make_bool(vm, fvp_value_truth(&a) && fvp_value_truth(&b));
        break;
    case 0x21:
        r = fvp_make_bool(vm, fvp_value_truth(&a) || fvp_value_truth(&b));
        break;
    case 0x22:
        if (vm->bool_type) {
            int ok = 0;
            int c = fvp_pair_cmp_faithful(&a, &b, &ok);

            r = fvp_make_bool(vm, ok ? (c == 0) : 0);
        } else {
            r = fvp_make_bool(vm, fvp_value_eq(&a, &b));
        }
        break;
    case 0x23:
        if (vm->bool_type) {
            int ok = 0;
            int c = fvp_pair_cmp_faithful(&a, &b, &ok);

            r = fvp_make_bool(vm, ok ? (c != 0) : 1);
        } else {
            r = fvp_make_bool(vm, !fvp_value_eq(&a, &b));
        }
        break;
    case 0x24:
    case 0x25:
    case 0x26:
    case 0x27: {
        int ok = 0;
        int c = vm->bool_type ? fvp_pair_cmp_faithful(&a, &b, &ok)
                              : fvp_value_cmp(&a, &b, &ok);
        int v = 0;
        if (vm->bool_type) {
            /* 忠实模式下按静态分析的**真实**映射来（见 docs/vm-commands.md §1.2）：
             *   0x24 gt = 表 B + setne → a > b
             *   0x25    = 表 A + sete  → !(a < b) = a >= b   （不是"le"！）
             *   0x26 lt = 表 A + setne → a < b
             *   0x27    = 表 B + sete  → !(a > b) = a <= b   （不是"ge"！）
             * 表外组合（null/true/异类型）的比较器返回 0，所以 sete 之后
             * 0x25/0x27 在异类型上为**真**。 */
            if (op == 0x24) {
                v = ok ? (c > 0) : 0;
            } else if (op == 0x25) {
                v = ok ? (c >= 0) : 1;
            } else if (op == 0x26) {
                v = ok ? (c < 0) : 0;
            } else {
                v = ok ? (c <= 0) : 1;
            }
        } else if (ok) {
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
        r = fvp_make_bool(vm, v);
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
    vm->last_pc = ins->pc;
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
            top->ndecl = (int32_t)ins->a;
            top->nlocals = fvp_frame_slots_mode() ? (int32_t)ins->b
                                                 : (int32_t)ins->a;
            top->locals = top->nlocals != 0
                              ? (FvpValue *)fvp_xcalloc((size_t)top->nlocals,
                                                        sizeof(FvpValue))
                              : NULL;
            top->pending = 0;
        } else {
            FvpFrame f;
            /* 栈底帧（没有 call 建它）：这是线程/函数的入口。返回地址必须指到
             * 脚本末尾，这样末尾的 ret 会结束线程；早先写成 ins->pc（指向
             * initstack 自己），ret 就会跳回入口，协程无限自循环。 */
            f.ret = vm->hcb->script_len;
            f.ndecl = (int32_t)ins->a;
            f.nlocals = fvp_frame_slots_mode() ? (int32_t)ins->b
                                               : (int32_t)ins->a;
            f.locals = f.nlocals != 0
                           ? (FvpValue *)fvp_xcalloc((size_t)f.nlocals,
                                                     sizeof(FvpValue))
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
        f.ndecl = 0;
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
        vm->last_syscall_step = vm->steps; /* 卡死侦测的"有进展"信号 */
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
        /* 默认（bool_type == 0）：数值真假 —— int 0 算假。
         * bool_type == 1：照原版 handler 0x445a30 的 `cmpb $0x0,(%edx)`，
         * 只认"类型字节 == 0"（也就是 FVP_NULL）为假；int 0 是 type 2 → 不算假。 */
        int cond = vm->bool_type ? (c.type == (uint8_t)FVP_NULL) : !fvp_value_truth(&c);
        if (cond) {
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
    case 0x09: /* pushfalse：原版 0x444940 写的是 Variant type 0，不是 int 0 */
        fvp_thread_push(t, fvp_make_null());
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
            /* 忠实模式（bool_type）：未赋值过的 global 在原版里读出来是 null
             * （Variant type 0），不是 int 0 —— 见 docs/roadmap.md 第二步。 */
            if (vm->bool_type && !vm->globals_set[ins->w]) {
                fvp_thread_push(t, fvp_make_null());
            } else {
                fvp_thread_push(t, vm->globals[ins->w]);
            }
        } else {
            fvp_thread_push(t, fvp_make_int(0));
        }
        t->pc += 3u;
        break;
    case 0x10: { /* pushstack：局部变量（两种写法）或绝对槽位的参数 */
        FvpFrame *f = fvp_frame_top(t);
        int li = fvp_frame_local_index(f, (int32_t)ins->a);
        if (li >= 0) {
            fvp_thread_push(t, f->locals[li]);
        } else {
            int ai = fvp_frame_arg_index(f, (int32_t)ins->a);
            if (ai >= 0) {
                fvp_thread_push(t, f->args[ai]);
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
            vm->globals_set[ins->w] = 1u;
        }
        t->pc += 3u;
        break;
    }
    case 0x16: { /* copystack：局部变量（两种写法）或绝对槽位的参数 */
        FvpValue v = fvp_thread_pop(t);
        FvpFrame *f = fvp_frame_top(t);
        int li = fvp_frame_local_index(f, (int32_t)ins->a);
        if (li >= 0) {
            f->locals[li] = v;
        } else if (fvp_frame_slots_mode()) {
            int ai = fvp_frame_arg_index(f, (int32_t)ins->a);
            if (ai >= 0) {
                f->args[ai] = v;
            }
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
    fvp_history_init(&vm->history);
    vm->save_slot = -1;
    vm->cursor_shape = -1; /* -1 = 还没被 CursorChange 设过 */
    vm->window_mode_last = -1;
    vm->exit_mode_last = -1;
    vm->rand_state = 1u;
    vm->request_slot = -1;
    vm->cur_font = -1;
    vm->cursor_show = 1u;
    vm->save_thumb_w = 96; /* 引擎初值 96x72（docs/save-format.md §2.3） */
    vm->save_thumb_h = 72;
    /* 协程槽默认开启：实测（docs/05-threads.md §10）对本作脚本行为中立 ——
     * 900k/2M 两档文本与关闭时逐字节相同。--no-threads 只用于 A/B 与回归。 */
    vm->threads_on = 1;
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
        /* BGM 交叉淡入淡出：两条 BGM 通道轮换，默认 600 ms（0 = 硬切）。 */
        b->bgm_chan = SND_CH_BGM;
        b->bgm_xfade_ms = 600;
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

void fvp_vm_snd_set_bgm_xfade(FvpVM *vm, int ms)
{
    if (vm != NULL && vm->snd_bridge != NULL) {
        vm->snd_bridge->bgm_xfade_ms = ms < 0 ? 0 : ms;
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

/* 静态分析 0x43a3d0：分量"是 int 且在 0..200 内"才采用，否则默认 100。 */
static unsigned char rgb_component(const FvpValue *v)
{
    if (v == NULL || v->type != (uint8_t)FVP_INT) {
        return 100u;
    }
    if (v->i < 0 || v->i > 200) {
        return 100u;
    }
    return (unsigned char)v->i;
}

int fvp_vm_graph_rgb(FvpVM *vm, const FvpValue *slot, const FvpValue *r,
                     const FvpValue *g, const FvpValue *b)
{
    int id;

    if (vm == NULL || slot == NULL) {
        return 0;
    }
    if (slot->type != (uint8_t)FVP_INT) {
        return 0;
    }
    id = (int)slot->i;
    if (id < 0 || id >= FVP_GRAPH_SLOTS) {
        return 0;
    }
    vm->graph_rgb_calls += 1u;
    vm->graph_rgb[id][0] = rgb_component(r);
    vm->graph_rgb[id][1] = rgb_component(g);
    vm->graph_rgb[id][2] = rgb_component(b);
    vm->graph_rgb_set[id] = 1u;
    return 1;
}

static char *arena_cstr(FvpVM *vm, const FvpValue *v)
{
    FvpStr *s;

    if (v == NULL || v->type != (uint8_t)FVP_STR || v->str == NULL) {
        return NULL;
    }
    s = fvp_arena_str(&vm->arena, v->str->data, v->str->len);
    return (char *)s->data;
}

static int on_off_of(const FvpValue *v, int *out);

static int parts_id_of(const FvpValue *v, int min, int *out)
{
    if (v == NULL || v->type != (uint8_t)FVP_INT) {
        return 0;
    }
    if (v->i < min || v->i > 0x3F) {
        return 0;
    }
    *out = (int)v->i;
    return 1;
}

int fvp_vm_color_set(FvpVM *vm, int index, int r, int g, int b, int a,
                     int has_r, int has_g, int has_b, int has_a)
{
    int wrote = 0;

    if (vm == NULL || index < 0 || index > 0xFF) {
        return 0;
    }
    /* 原版是 engine+0x6A44C0 + idx*4 的 BGRA 四字节，逐分量判 type==2。 */
    if (has_b) {
        vm->palette[index][0] = (unsigned char)(b & 0xFF);
        wrote = 1;
    }
    if (has_g) {
        vm->palette[index][1] = (unsigned char)(g & 0xFF);
        wrote = 1;
    }
    if (has_r) {
        vm->palette[index][2] = (unsigned char)(r & 0xFF);
        wrote = 1;
    }
    if (has_a) {
        vm->palette[index][3] = (unsigned char)(a & 0xFF);
        wrote = 1;
    }
    if (wrote) {
        vm->palette_set[index] = 1;
        vm->color_set_calls += 1u;
    }
    return wrote;
}

int fvp_vm_parts_motion(FvpVM *vm, const FvpValue *parts, const FvpValue *arg2,
                        const FvpValue *arg3)
{
    int id;

    if (vm == NULL || !parts_id_of(parts, 0, &id)) {
        return 0;
    }
    /* 原版 0x43B3E0：arg2 必须 int 且 0..0xFF，arg3 必须 int 且 1..0x493E0
     * （<= 300000 ms），否则整条调用被丢弃（连计数都不进）。 */
    if (arg2 == NULL || arg2->type != (uint8_t)FVP_INT || arg2->i < 0 ||
        arg2->i > 0xFF) {
        return 0;
    }
    if (arg3 == NULL || arg3->type != (uint8_t)FVP_INT || arg3->i < 1 ||
        arg3->i > 300000) {
        return 0;
    }
    vm->parts_motion_calls += 1u;
    vm->parts_motion[id] = (unsigned char)(arg2->i & 0xFF);
    vm->parts_motion_time[id] = (unsigned int)arg3->i;
    return 1;
}

int fvp_vm_parts_motion_pause(FvpVM *vm, const FvpValue *parts,
                              const FvpValue *on)
{
    int id;

    if (vm == NULL || !parts_id_of(parts, 0, &id)) {
        return 0;
    }
    /* 原版 0x43B440：arg2 必须 int 且 0..1，否则整条丢弃。 */
    if (on == NULL || on->type != (uint8_t)FVP_INT || on->i < 0 ||
        on->i > 1) {
        return 0;
    }
    vm->parts_motion_pause_calls += 1u;
    vm->parts_motion[id] = (unsigned char)on->i;
    return 1;
}

int fvp_vm_parts_motion_stop(FvpVM *vm, const FvpValue *parts)
{
    int id;

    if (vm == NULL || !parts_id_of(parts, 1, &id)) {
        return 0;
    }
    vm->parts_motion_stop_calls += 1u;
    vm->parts_motion[id] = 0u;
    return 1;
}
int fvp_vm_parts_motion_test(FvpVM *vm, const FvpValue *parts)
{
    int id;

    if (vm == NULL || parts == NULL || parts->type != (uint8_t)FVP_INT) {
        return 0;
    }
    id = (int)parts->i;
    /* 原版 handler 0x43B4B0：槽号 1..0x3F（0 不合法），越界不写返回值。 */
    if (id < 1 || id > 0x3F) {
        return 0;
    }
    vm->parts_motion_test_calls += 1u;
    /* 我们不跑部件动画（parts_motion 只记参数），所以"还在跑"恒为 0。 */
    return 0;
}


int fvp_vm_parts_rgb(FvpVM *vm, const FvpValue *parts, const FvpValue *r,
                     const FvpValue *g, const FvpValue *b)
{
    int id;

    if (vm == NULL || !parts_id_of(parts, 0, &id)) {
        return 0;
    }
    vm->parts_rgb_calls += 1u;
    vm->parts_rgb[id][0] = rgb_component(r);
    vm->parts_rgb[id][1] = rgb_component(g);
    vm->parts_rgb[id][2] = rgb_component(b);
    vm->parts_rgb_set[id] = 1u;
    return 1;
}

int fvp_vm_prim_group_move(FvpVM *vm, const FvpValue *id,
                           const FvpValue *arg2)
{
    long v;
    long w;

    if (vm == NULL || id == NULL || id->type != (uint8_t)FVP_INT) {
        return 0;
    }
    v = (long)id->i;
    if (v < 1 || v >= 0x1000) {
        return 0;
    }
    /* 原版 0x43B980：arg2 同样是 int 且 1..0xFFF，否则整条丢弃。 */
    if (arg2 == NULL || arg2->type != (uint8_t)FVP_INT) {
        return 0;
    }
    w = (long)arg2->i;
    if (w < 1 || w >= 0x1000) {
        return 0;
    }
    vm->prim_group_move_calls += 1u;
    vm->prim_group_move_last[0] = (int)v;
    vm->prim_group_move_last[1] = (int)w;
    return 1;
}

int fvp_vm_snow(FvpVM *vm, const FvpValue *layer, int on)
{
    if (vm == NULL || layer == NULL || layer->type != (uint8_t)FVP_INT) {
        return 0;
    }
    if (layer->i < 0 || layer->i > 1) {
        return 0;
    }
    if (on) {
        vm->snow_start_calls += 1u;
    } else {
        vm->snow_stop_calls += 1u;
    }
    vm->snow_on[(int)layer->i] = on ? 1u : 0u;
    return 1;
}

int fvp_vm_prim_snow(FvpVM *vm, const FvpValue *prim, const FvpValue *on)
{
    int id;
    int val = 0;

    if (vm == NULL || prim == NULL || prim->type != (uint8_t)FVP_INT) {
        return 0;
    }
    id = (int)prim->i;
    if (id < 1 || id >= FVP_PRIM_COUNT) {
        return 0;
    }
    (void)on_off_of(on, &val);
    vm->prim_snow_calls += 1u;
    vm->prim_snow[id] = (unsigned char)val;
    return 1;
}

/* Snow(97) 的必填参数区间（arg1..arg14），逐条来自 handler 0x43C230 的
 * `cmp` 序列；arg8 <= arg9 是额外的交叉约束。 */
static const int snow_req_min[14] = { 0, 0, 0, 0, 2, 2, 1, 10, 10, 1, -0x1000,
                                      -0x1000, -0x1000, 0 };
static const int snow_req_max[14] = { 1, 0x1000, 0x1000, 0xFFF, 0x40, 0x40, 0x10,
                                      0x2710, 0x2710, 0x400, 0x1000, 0x1000,
                                      0x1000, 0x400 };
/* 可选参数 arg15..arg18 的区间；不合法或没给就不置 bit（用内部缺省值）。 */
static const int snow_opt_min[4] = { 0, 0, 10, 0 };
static const int snow_opt_max[4] = { 0xFF, 0xFF, 0x2710, 0xFF };

static int snow_int_in(const FvpValue *v, int lo, int hi)
{
    return v != NULL && v->type == (uint8_t)FVP_INT && v->i >= lo &&
           v->i <= hi;
}

int fvp_vm_snow_ex(FvpVM *vm, const FvpValue *args, int argc)
{
    int i;

    if (vm == NULL || args == NULL || argc < 14) {
        return 0;
    }
    for (i = 0; i < 14; ++i) {
        if (!snow_int_in(&args[i], snow_req_min[i], snow_req_max[i])) {
            return 0;
        }
    }
    /* arg8(x 起点/数量) 不能大于 arg9：0x43C356 `cmp %eax,%ebp; jg` 直接拒绝。 */
    if (args[7].i > args[8].i) {
        return 0;
    }
    vm->snow_calls += 1;
    vm->snow_layer = (int)args[0].i;
    for (i = 0; i < 14; ++i) {
        vm->snow_vals[i] = (int)args[i].i;
    }
    vm->snow_opt_mask = 0u;
    for (i = 0; i < 4; ++i) {
        int slot = 14 + i;

        vm->snow_vals[slot] = 0;
        if (argc > slot &&
            snow_int_in(&args[slot], snow_opt_min[i], snow_opt_max[i])) {
            vm->snow_opt_mask |= (unsigned short)(1u << i);
            vm->snow_vals[slot] = (int)args[slot].i;
        }
    }
    return 1;
}

int fvp_vm_cursor_show(FvpVM *vm, const FvpValue *v)
{
    if (vm == NULL || v == NULL) {
        return 0;
    }
    /* 原版 0x437560 附近：只比较参数的**类型字节**（cmpb $0x0,(%eax)），
     * null（type 0）→ engine+0x45c = 0，其它 → 1；参数字面值不参与。
     * pushfalse 现在压的就是 FVP_NULL，所以这一支真的能走到了。 */
    vm->cursor_show = fvp_value_is_null(v) ? 0u : 1u;
    return 1;
}

void fvp_vm_exit_dialog(FvpVM *vm)
{
    if (vm != NULL) {
        vm->exit_dialog_calls += 1u;
    }
}

void fvp_vm_movie_stop(FvpVM *vm)
{
    if (vm != NULL) {
        vm->movie_stop_calls += 1u;
    }
}

/* LipAnim(38) 的必填/可选区间，逐条来自 handler 0x43A6E0 的 cmp 序列。 */
static const int lip_arg_min[8] = { 1, 0, 1, 1, 1, 1, 1, 1 };
static const int lip_arg_max[8] = { 0xFFF, 3, 0xFFF, 300000, 0xFFF, 300000,
                                    0xFFF, 300000 };

int fvp_vm_lip_anim(FvpVM *vm, const FvpValue *args, int argc)
{
    int i;

    if (vm == NULL || args == NULL) {
        return 0;
    }
    /* arg1 必填：int 1..0xFFF。 */
    if (!snow_int_in(&args[0], lip_arg_min[0], lip_arg_max[0])) {
        return 0;
    }
    /* arg2 为 null → 只停（handler 0x42D700(id)：找到槽、状态清 0、槽号进空闲
     * 栈）；这条分支以前不可达，因为 pushfalse 被压成了 int 0。 */
    if (argc >= 2 && fvp_value_is_null(&args[1])) {
        vm->lip_anim_calls += 1u;
        vm->lip_anim_stop_calls += 1u;
        vm->lip_anim_id = (int)args[0].i;
        return 1;
    }
    if (argc < 6) {
        return 0;
    }
    for (i = 1; i < 6; ++i) {
        if (!snow_int_in(&args[i], lip_arg_min[i], lip_arg_max[i])) {
            return 0;
        }
    }
    vm->lip_anim_calls += 1u;
    vm->lip_anim_id = (int)args[0].i;
    vm->lip_anim_mode = (int)args[1].i;
    for (i = 0; i < 6; ++i) {
        vm->lip_anim_arg[i] = (int)args[i].i;
    }
    /* arg7 / arg8：缺省或越界都补 0（原版 0x43A7FE 分支就是 push 0,0）。 */
    for (i = 6; i < 8; ++i) {
        vm->lip_anim_arg[i] = 0;
        if (argc > i && snow_int_in(&args[i], lip_arg_min[i],
                                    lip_arg_max[i])) {
            vm->lip_anim_arg[i] = (int)args[i].i;
        }
    }
    return 1;
}

int fvp_vm_lip_sync(FvpVM *vm, const FvpValue *id, const FvpValue *on)
{
    if (vm == NULL || id == NULL || id->type != (uint8_t)FVP_INT) {
        return 0;
    }
    if (id->i < 1 || id->i > 0xFFF) {
        return 0;
    }
    vm->lip_sync_calls += 1u;
    vm->lip_sync_id = (int)id->i;
    /* 原版 0x43A830 只看第二参数**是不是 null**（cmpb $0x0,0x8）：
     * null → 0（停），其它 → 1（播）。pushfalse 现在真的压出 null 了；
     * 参数指针本身为空（缺参数）按旧的 0 记。 */
    vm->lip_sync_on = (on == NULL || fvp_value_is_null(on)) ? 0 : 1;
    return 1;
}

int fvp_vm_history_set(FvpVM *vm, const FvpValue *args, int argc)
{
    int kind;

    if (vm == NULL) {
        return 0;
    }
    vm->history.sets += 1u;
    if (args == NULL || argc < 1) {
        return 0;
    }
    /* 原版 0x43A5E4：`mov (%eax),%dl; test %dl,%dl` —— 只有 Variant **type 0
     * （null）**才走"开新的一条"，int 0 不是。 */
    if (fvp_value_is_null(&args[0])) {
        return fvp_history_begin(&vm->history);
    }
    if (args[0].type != (uint8_t)FVP_INT) {
        return 0;
    }
    kind = (int)args[0].i;
    if (kind < 0 || kind > 2) {
        return 0;
    }
    if (kind == 2) {
        /* 0x43A6B5：第二参数必须是 int（type 2）且 >= 0。 */
        if (argc < 2 || args[1].type != (uint8_t)FVP_INT) {
            return 0;
        }
        return fvp_history_set_value(&vm->history, (int)args[1].i);
    }
    /* kind 0/1：第二参数必须是字符串（0x438DF0 取不到就整条忽略）。 */
    if (argc < 2 || args[1].type != (uint8_t)FVP_STR ||
        args[1].str == NULL) {
        return 0;
    }
    return fvp_history_set_text(&vm->history, kind, args[1].str->data,
                                args[1].str->len);
}

int fvp_vm_history_get(FvpVM *vm, const FvpValue *args, int argc,
                       FvpValue *out)
{
    int kind;
    int idx;

    if (vm == NULL) {
        return 0;
    }
    vm->history.gets += 1u;
    if (args == NULL || argc < 1) {
        return 0;
    }
    /* 0x43A4AB：type 0（null）→ 返回 count。 */
    if (fvp_value_is_null(&args[0])) {
        if (out != NULL) {
            *out = fvp_make_int((int64_t)vm->history.count);
        }
        return 1;
    }
    if (args[0].type != (uint8_t)FVP_INT) {
        return 0;
    }
    kind = (int)args[0].i;
    if (kind < 0 || kind > 2) {
        return 0;
    }
    if (argc < 2 || args[1].type != (uint8_t)FVP_INT) {
        return 0; /* 0x43A4E6：第二参数必须 int */
    }
    idx = (int)args[1].i;
    /* 0x43A4F3 - 0x43A50C：idx < 0 或 idx > count-1 → 不写返回值。 */
    if (idx < 0 || (int64_t)idx >= (int64_t)vm->history.count) {
        return 0;
    }
    if (kind == 0 || kind == 1) {
        const char *s = fvp_history_text(&vm->history, kind, idx);

        if (s == NULL) {
            return 0;
        }
        if (out != NULL) {
            *out = fvp_make_str(fvp_arena_str(&vm->arena, s, fvp_strlen(s)));
        }
        return 1;
    }
    {
        int32_t v = 0;

        if (!fvp_history_value(&vm->history, idx, &v)) {
            return 0;
        }
        if (out != NULL) {
            *out = fvp_make_int(v);
        }
    }
    return 1;
}

void fvp_vm_input_flash(FvpVM *vm)
{
    if (vm != NULL) {
        vm->input_flash_calls += 1u;
    }
}

int fvp_vm_input_repeat(const FvpVM *vm)
{
    return vm == NULL ? 0 : vm->input_repeat;
}

int fvp_vm_input_set_click(FvpVM *vm, const FvpValue *v)
{
    if (vm == NULL || v == NULL || v->type != (uint8_t)FVP_INT) {
        return 0;
    }
    if (v->i != 0 && v->i != 1) {
        return 0;
    }
    vm->input_setclick_calls += 1u;
    vm->input_click = (unsigned char)v->i;
    return 1;
}

static int on_off_of(const FvpValue *v, int *out)
{
    if (v == NULL || v->type != (uint8_t)FVP_INT) {
        return 0;
    }
    if (v->i < 0 || v->i > 1) {
        return 0;
    }
    *out = (int)v->i;
    return 1;
}

int fvp_vm_motion_pause(FvpVM *vm, const FvpValue *prim, const FvpValue *on)
{
    int id;
    int val = 0;

    if (vm == NULL || prim == NULL || prim->type != (uint8_t)FVP_INT) {
        return 0;
    }
    id = (int)prim->i;
    if (id < 0 || id >= FVP_PRIM_COUNT) {
        return 0;
    }
    (void)on_off_of(on, &val); /* 非法/缺省 → 0（原版程序里也是"等价于 0"） */
    vm->motion_pause_calls += 1u;
    vm->prim_pause[id] = (unsigned char)val;
    return 1;
}

int fvp_vm_text_pause(FvpVM *vm, const FvpValue *idx, const FvpValue *on)
{
    int wi;
    int val = 0;

    if (vm == NULL || idx == NULL || idx->type != (uint8_t)FVP_INT) {
        return 0;
    }
    wi = (int)idx->i;
    if (wi < 0 || wi >= FVP_TEXT_WINDOWS) {
        return 0;
    }
    (void)on_off_of(on, &val);
    vm->text_pause_calls += 1u;
    vm->tw_pause[wi] = (unsigned char)val;
    return 1;
}

int fvp_vm_font_register(FvpVM *vm, const char *name)
{
    size_t n;

    if (vm == NULL || name == NULL) {
        return -1;
    }
    if (vm->nfont_names >= FVP_MAX_FONT_NAMES) {
        return -1;
    }
    n = strlen(name);
    vm->font_name[vm->nfont_names] = (char *)fvp_xmalloc(n + 1u);
    memcpy(vm->font_name[vm->nfont_names], name, n + 1u);
    vm->nfont_names += 1;
    return vm->nfont_names;
}

int fvp_vm_font_count(const FvpVM *vm)
{
    return vm == NULL ? 0 : vm->nfont_names;
}

int fvp_vm_font_set(FvpVM *vm, const FvpValue *idx)
{
    long v;

    if (vm == NULL || idx == NULL || idx->type != (uint8_t)FVP_INT) {
        return 0;
    }
    v = (long)idx->i;
    if (v < -5 || v > (long)vm->nfont_names - 1) {
        return 0;
    }
    vm->cur_font = (int)v;
    return 1;
}

int fvp_vm_font_get(const FvpVM *vm)
{
    return vm == NULL ? 0 : vm->cur_font;
}

const char *fvp_vm_font_name(const FvpVM *vm, const FvpValue *idx)
{
    long v;

    if (vm == NULL || idx == NULL || idx->type != (uint8_t)FVP_INT) {
        return NULL;
    }
    v = (long)idx->i;
    if (v < 0 || v >= (long)vm->nfont_names) {
        return NULL;
    }
    return vm->font_name[v];
}

static int request_slot_of(const FvpValue *slot, int *out)
{
    long v;

    if (slot == NULL || slot->type != (uint8_t)FVP_INT) {
        return 0;
    }
    v = (long)slot->i;
    if (v < 0 || v > 999) {
        return 0;
    }
    *out = (int)v;
    return 1;
}

int fvp_vm_request_load(FvpVM *vm, const FvpValue *slot)
{
    int s;

    if (vm == NULL || !request_slot_of(slot, &s)) {
        return 0;
    }
    vm->load_calls += 1u;
    vm->load_request = 1u;
    vm->request_slot = s;
    return 1;
}

int fvp_vm_request_save(FvpVM *vm, const FvpValue *slot)
{
    int s;

    if (vm == NULL || !request_slot_of(slot, &s)) {
        return 0;
    }
    vm->save_calls += 1u;
    vm->save_request = 1u;
    vm->request_slot = s;
    return 1;
}

int fvp_vm_take_load_request(FvpVM *vm)
{
    int s;

    if (vm == NULL || vm->load_request == 0u) {
        return -1;
    }
    s = vm->request_slot;
    vm->load_request = 0u;
    return s;
}

int fvp_vm_take_save_request(FvpVM *vm)
{
    int s;

    if (vm == NULL || vm->save_request == 0u) {
        return -1;
    }
    s = vm->request_slot;
    vm->save_request = 0u;
    return s;
}

int fvp_vm_rand(FvpVM *vm)
{
    uint32_t x;

    if (vm == NULL) {
        return 0;
    }
    x = vm->rand_state;
    if (x == 0u) {
        x = 1u; /* xorshift 的 0 是吸收态 */
    }
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    vm->rand_state = x;
    return (int)(x & 0x7FFFu); /* MSVC RAND_MAX == 0x7FFF */
}

int fvp_vm_gaiji_load(FvpVM *vm, const FvpValue *s1, const FvpValue *size,
                      const FvpValue *s2)
{
    if (vm == NULL || s1 == NULL || size == NULL || s2 == NULL) {
        return 0;
    }
    if (s1->type != (uint8_t)FVP_STR || s2->type != (uint8_t)FVP_STR) {
        return 0;
    }
    if (size->type != (uint8_t)FVP_INT) {
        return 0;
    }
    if (size->i < 12 || size->i > 0x40) {
        return 0;
    }
    vm->gaiji_calls += 1u;
    vm->gaiji_size = (int)size->i;
    vm->gaiji_name1 = arena_cstr(vm, s1);
    vm->gaiji_name2 = arena_cstr(vm, s2);
    return 1;
}

int fvp_vm_v3d_motion(FvpVM *vm, const FvpValue *time_ms, const FvpValue *mode)
{
    if (vm == NULL || time_ms == NULL || mode == NULL) {
        return 0;
    }
    if (time_ms->type != (uint8_t)FVP_INT || mode->type != (uint8_t)FVP_INT) {
        return 0;
    }
    if (time_ms->i < 1 || time_ms->i > 0x493e0) {
        return 0;
    }
    if (mode->i < 1 || mode->i > 5) {
        return 0;
    }
    vm->v3d_calls += 1u;
    vm->v3d_time_ms = (int)time_ms->i;
    vm->v3d_mode = (int)mode->i;
    return 1;
}

int fvp_vm_window_mode(FvpVM *vm, const FvpValue *mode)
{
    long m;

    if (vm == NULL || mode == NULL || mode->type != (uint8_t)FVP_INT) {
        return 0;
    }
    m = (long)mode->i;
    if (m < -1 || m > 6) {
        return 0;
    }
    vm->window_mode_calls += 1u;
    vm->window_mode_last = (int)m;
    if (m == 0 || m == 1) {
        vm->window_state = (int)m;
    }
    return 1;
}

int fvp_vm_exit_mode(FvpVM *vm, const FvpValue *mode)
{
    long m;

    if (vm == NULL || mode == NULL || mode->type != (uint8_t)FVP_INT) {
        return 0;
    }
    m = (long)mode->i;
    if (m < 0 || m > 4) {
        return 0;
    }
    vm->exit_mode_calls += 1u;
    vm->exit_mode_last = (int)m;
    return 1;
}

int fvp_vm_text_buff(FvpVM *vm, const FvpValue *idx, const FvpValue *a,
                     const FvpValue *b)
{
    int wi;
    long w = 8;
    long h = 8;

    if (vm == NULL || idx == NULL) {
        return 0;
    }
    if (idx->type != (uint8_t)FVP_INT) {
        return 0;
    }
    wi = (int)idx->i;
    if (wi < 0 || wi >= FVP_TEXT_WINDOWS) {
        return 0;
    }
    vm->text_buff_calls += 1u;
    if (a != NULL && a->type == (uint8_t)FVP_INT && a->i >= 0) {
        w = (long)a->i;
    }
    if (b != NULL && b->type == (uint8_t)FVP_INT && b->i >= 0) {
        h = (long)b->i;
    }
    /* 0x430ea0：任一分量 > 0x400 就整条放弃（连记录也不做）。 */
    if (w > 0x400 || h > 0x400) {
        vm->text_buff_calls -= 1u;
        return 0;
    }
    vm->text_buff[wi].w = (unsigned short)w;
    vm->text_buff[wi].h = (unsigned short)h;
    vm->text_buff[wi].set = 1u;
    if (getenv("FVP_DEBUG_TEXT") != NULL) {
        fprintf(stderr, "TextBuff: layer=%d w=%ld h=%ld\n", wi, w, h);
    }
    return 1;
}

int fvp_vm_set_proj_folder(FvpVM *vm, const FvpValue *arg)
{
    FvpStr *s;

    if (vm == NULL || arg == NULL) {
        return 0;
    }
    if (arg->type != (uint8_t)FVP_STR || arg->str == NULL) {
        return 0;
    }
    vm->proj_folder_calls += 1u;
    s = fvp_arena_str(&vm->arena, arg->str->data, arg->str->len);
    vm->proj_folder = (char *)s->data;
    return 1;
}

void fvp_vm_control_mask(FvpVM *vm, const FvpValue *arg)
{
    if (vm == NULL) {
        return;
    }
    vm->control_calls[0] += 1u;
    /* 静态分析 0x437440：只比较参数的**类型字节**（cmpb $0x0,(%eax)）然后 sete——
     * 参数是 null（原版 Variant type 0）时 engine+0x458 置 1，否则置 0，
     * 参数字面值不参与。`pushfalse` 现在压的是 FVP_NULL，所以 1 这一支真的
     * 能走到了；同时把参数类型记下来（control_mask_type，诊断用）。 */
    vm->control_mask = fvp_value_is_null(arg) ? 1u : 0u;
    vm->control_mask_type = (arg != NULL) ? arg->type : 0xFFu;
}

void fvp_vm_control_pulse(FvpVM *vm)
{
    if (vm == NULL) {
        return;
    }
    vm->control_calls[1] += 1u;
    vm->pulse = 1u; /* 静态分析 0x437460：movb $0x1,0xfd8(engine) */
}

int fvp_vm_cursor_change(FvpVM *vm, const FvpValue *arg)
{
    if (vm == NULL || arg == NULL) {
        return 0;
    }
    vm->control_calls[2] += 1u;
    /* 静态分析 0x437470：参数必须是 int 且 0..3，否则整条命令什么都不做；
     * 合法时 tail-call 0x441810(engine, id) → engine+0x88 = id（+ 换 Win32 光标）。 */
    if (arg->type != (uint8_t)FVP_INT) {
        return 0;
    }
    if (arg->i < 0 || arg->i > 3) {
        return 0;
    }
    vm->cursor_shape = (int)arg->i;
    return 1;
}

int fvp_vm_cursor_move(FvpVM *vm, const FvpValue *x, const FvpValue *y,
                       const FvpValue *force)
{
    if (vm == NULL || x == NULL || y == NULL) {
        return 0;
    }
    vm->control_calls[3] += 1u;
    /* 静态分析 0x4374a0：鼠标设备可用时直接做；不可用时要求第三个参数的
     * 类型字节非 0（cmpb $0x0,0x10(%ecx); je 返回）才做。本站没有"鼠标设备
     * 表"（一律按可用处理），也没有 null 类型，所以第三参数只记录不判定。 */
    vm->cursor_force_type = (force != NULL) ? force->type : 0xFFu;
    if (x->type != (uint8_t)FVP_INT || y->type != (uint8_t)FVP_INT) {
        return 0;
    }
    vm->cursor_x = (int)x->i;
    vm->cursor_y = (int)y->i;
    vm->cursor_moved = 1;
    return 1;
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
    fvp_history_free(&vm->history);
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

/* 把线程追加到调度队列尾部。 */
static void fvp_thread_enqueue(FvpVM *vm, FvpThread *t)
{
    if (vm->nthreads == vm->thread_cap) {
        vm->thread_cap = vm->thread_cap ? vm->thread_cap * 2u : 8u;
        vm->threads = (FvpThread **)fvp_xrealloc(vm->threads,
                                                 vm->thread_cap * sizeof(FvpThread *));
    }
    vm->threads[vm->nthreads] = t;
    vm->nthreads += 1;
}

void fvp_vm_start(FvpVM *vm)
{
    FvpThread *t = fvp_thread_new(vm->hcb->main_start, "main");
    t->id = vm->thread_ids++;
    fvp_thread_enqueue(vm, t);
    vm->cur = t;
}

void fvp_vm_start_at(FvpVM *vm, unsigned int pc)
{
    FvpThread *t;

    if (vm == NULL || vm->hcb == NULL) {
        return;
    }
    t = fvp_thread_new(pc, "main");
    t->id = vm->thread_ids++;
    fvp_thread_enqueue(vm, t);
    vm->cur = t;
}

/* 时间模型：当所有线程都被定时阻塞时，把虚拟时钟推到最早的
 * 唤醒时刻并叫醒到期线程。返回 1 = 推进成功，0 = 没有可推进的目标（应结束）。 */
static int timeline_advance(FvpVM *vm)
{
    size_t i;
    int have = 0;
    uint64_t next = 0;

    for (i = 0; i < vm->nthreads; ++i) {
        FvpThread *t = vm->threads[i];

        if (!t->alive || !t->timed || t->wake_ms <= vm->virtual_ms) {
            continue;
        }
        if (!have || t->wake_ms < next) {
            next = t->wake_ms;
            have = 1;
        }
    }
    if (!have) {
        return 0;
    }
    vm->virtual_ms = next;
    for (i = 0; i < vm->nthreads; ++i) {
        FvpThread *t = vm->threads[i];

        if (t->alive && t->timed && t->wake_ms <= vm->virtual_ms) {
            t->timed = 0;
            t->blocked = 0;
            t->wait = 0;
        }
    }
    return 1;
}

uint64_t fvp_vm_run(FvpVM *vm, uint64_t max_steps)
{
    vm->max_steps = max_steps;
    {
    int blocked_run = 0;
    while (vm->nthreads > 0 && vm->steps < vm->max_steps) {
        FvpThread *t = vm->threads[0];
        size_t i;
        int stepped = 0;
        /* pop(0) */
        for (i = 0; i + 1u < vm->nthreads; ++i) {
            vm->threads[i] = vm->threads[i + 1u];
        }
        vm->nthreads -= 1u;
        if (!t->alive) {
            /* 槽里还指着它的话一并清掉，避免悬空指针。 */
            if (t->slot >= 0 && t->slot < FVP_THREAD_SLOTS) {
                if (vm->thread_slots[t->slot] == t) {
                    vm->thread_slots[t->slot] = NULL;
                }
                vm->thread_exits += 1u; /* 线程自己 ret 到脚本末尾也算一次退出 */
            }
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
            if (vm->timeline_on && t->timed) {
                /* 这个线程在等虚拟时钟。如果轮了一圈都没人跑，就推进时钟。 */
                blocked_run += 1;
                if (blocked_run >= (int)vm->nthreads) {
                    blocked_run = 0;
                    if (!timeline_advance(vm)) {
                        break;
                    }
                }
                continue;
            }
            t->wait += 1;
            if (t->wait < 2) {
                continue;
            }
            t->blocked = 0;
            t->wait = 0;
        }
        blocked_run = 0;
        stepped = 1;
        (void)stepped;
        fvp_step(vm, t);
        /* 卡死侦测：脚本自旋（见 vm.h 的 stall_* 注释）。放在这里而不是
         * fvp_step 里，是因为 fvp_step 还要被单测/工具直接调用。 */
        if (vm->stall_after != 0u && vm->stall_at == 0u &&
            vm->steps - vm->last_syscall_step > vm->stall_after) {
            vm->stall_span = vm->steps - vm->last_syscall_step;
            vm->stall_at = vm->steps;
            vm->stall_pc = vm->last_pc;
            break;
        }
    }
    }
    return vm->steps;
}

void fvp_vm_set_stall_limit(FvpVM *vm, uint64_t n)
{
    if (vm == NULL) {
        return;
    }
    vm->stall_after = n;
}

int fvp_vm_stalled(const FvpVM *vm)
{
    return vm != NULL && vm->stall_at != 0u;
}

/* ------------------------------------------------------------------ */
/* 协程：ThreadStart(136) / ThreadExit(132)                            */
/*                                                                     */
/* 静态分析结论（docs/vm-commands.md §5）：ThreadStart(threadNo 0..31,     */
/* entryOffset)——threadNo==0 先把 32 个槽全部重置(0x444860)，再用      */
/* 0x4453f0(ctx, offset) 以 offset 为 PC 建线程。所以协程槽只是「按号   */
/* 存一个执行上下文」，入口函数没有参数（新线程从空栈开始）。           */
/*                                                                     */
/* 主线程（fvp_vm_start 建的那个）不在 32 个槽里：否则脚本开头的         */
/* ThreadStart(0, ...) 一重置就把自己弄死了。                          */
/* ------------------------------------------------------------------ */

/* 把一个槽里的线程标记为死；返回 1 表示确实收掉了一个活线程。 */
static int fvp_thread_kill_slot(FvpVM *vm, int slot, FvpThread *keep)
{
    FvpThread *t;

    if (slot < 0 || slot >= FVP_THREAD_SLOTS) {
        return 0;
    }
    t = vm->thread_slots[slot];
    if (t == NULL) {
        return 0;
    }
    vm->thread_slots[slot] = NULL;
    t->slot = -1;
    if (t == keep || !t->alive) {
        return 0; /* 自杀交给 fvp_vm_thread_exit 的调用方；已死的等回收 */
    }
    t->alive = 0;
    vm->thread_exits += 1u;
    return 1;
}

int fvp_vm_thread_start(FvpVM *vm, int no, uint32_t pc)
{
    FvpThread *t;
    int i;

    if (vm == NULL || !vm->threads_on) {
        return 0;
    }
    if (no < 0) {
        no = 0;
    }
    if (no >= FVP_THREAD_SLOTS) {
        no = FVP_THREAD_SLOTS - 1;
    }
    if (no == 0) {
        for (i = 0; i < FVP_THREAD_SLOTS; ++i) {
            fvp_thread_kill_slot(vm, i, vm->cur);
        }
        vm->thread_resets += 1u;
    } else {
        fvp_thread_kill_slot(vm, no, vm->cur);
    }
    t = fvp_thread_new(pc, "thread");
    t->id = vm->thread_ids++;
    t->slot = no;
    vm->thread_slots[no] = t;
    fvp_thread_enqueue(vm, t);
    vm->thread_starts += 1u;
    if (!vm->quiet) {
        fprintf(stdout, "[thread %d] start at 0x%x\n", no, (unsigned)pc);
    }
    return 1;
}

int fvp_vm_thread_exit(FvpVM *vm, FvpThread *self, int which)
{
    int i;
    int n = 0;

    if (vm == NULL || !vm->threads_on) {
        return 0;
    }
    if (which == 0) {
        for (i = 0; i < FVP_THREAD_SLOTS; ++i) {
            if (vm->thread_slots[i] == self) {
                continue;
            }
            n += fvp_thread_kill_slot(vm, i, self);
        }
    } else {
        if (which < 0) {
            which = 0;
        }
        if (which >= FVP_THREAD_SLOTS) {
            which = FVP_THREAD_SLOTS - 1;
        }
        if (vm->thread_slots[which] == self) {
            /* 线程结束自己：交给调度器在下一轮回收。 */
            self->alive = 0;
            self->slot = -1;
            vm->thread_slots[which] = NULL;
            vm->thread_exits += 1u;
            return 1;
        }
        n = fvp_thread_kill_slot(vm, which, self);
    }
    if (!vm->quiet && n > 0) {
        fprintf(stdout, "[thread exit] killed %d thread(s)\n", n);
    }
    return 1;
}

/* ThreadRaise(134)：handler 0x437B80 把 arg1 传给 32 次 0x4447E0(ctx, ms)：
 *   if ((ctx->flags & 4) != 0 && ctx->wait_ms == ms) ctx->flags &= ~4;
 * 即"叫醒睡眠时长正好等于 ms 的那些线程"。注意它只认 sleep（flags bit2 = 4），
 * 不认 ThreadWait 的 wait 位（bit1 = 2）。本站用 blocked + timed 表示在等虚拟时钟，
 * sleep_ms 记 ThreadSleep 的时长（ThreadWait 记 -1，不会被叫醒）。 */
int fvp_vm_thread_raise(FvpVM *vm, const FvpValue *ms)
{
    int slot;
    uint64_t woken = 0;

    if (vm == NULL || ms == NULL || ms->type != (uint8_t)FVP_INT) {
        return -1;
    }
    for (slot = 0; slot < FVP_THREAD_SLOTS; ++slot) {
        FvpThread *t = vm->thread_slots[slot];

        if (t == NULL || !t->blocked || !t->timed) {
            continue;
        }
        if (t->sleep_ms != (int)ms->i) {
            continue;
        }
        t->blocked = 0;
        t->timed = 0;
        t->wait = 0;
        woken += 1u;
    }
    vm->thread_raise_calls += 1u;
    vm->thread_raise_ms = (int)ms->i;
    vm->thread_raise_woken += woken;
    return (int)woken;
}

/* ------------------------------------------------------------------ */
/* 定时器：TimerSet(139) / TimerGet(138) / TimerSuspend(140)           */
/*                                                                    */
/* 静态分析语义见 docs/vm-commands.md §5。这里只维护「起点 + 时长 + 激活」 */
/* 三个字段，已过时间一律从虚拟时钟 vm->virtual_ms 推导；没有 --timeline */
/* 时时钟恒 0，于是 TimerGet 返回 0，与旧的「未实现返回 0」行为一致。   */
/* ------------------------------------------------------------------ */

int fvp_vm_timer_set(FvpVM *vm, int id, int ms)
{
    if (vm == NULL) {
        return 0;
    }
    if (fvp_timer_set(vm->timer, FVP_TIMER_COUNT, id, ms, vm->virtual_ms)) {
        vm->timer_sets += 1u;
        return 1;
    }
    return 0;
}

int fvp_vm_timer_get(FvpVM *vm, int id, int opt)
{
    if (vm == NULL) {
        return 0;
    }
    return fvp_timer_get(vm->timer, FVP_TIMER_COUNT, id, opt, vm->virtual_ms);
}

void fvp_vm_timer_suspend(FvpVM *vm, int on)
{
    if (vm == NULL) {
        return;
    }
    vm->timer_suspended = on ? 1 : 0;
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
    if (vm->cur != NULL) {
        vm->cur->ntext += 1u;
    }
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