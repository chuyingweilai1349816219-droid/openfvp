/* FVP VM in C -- common types and helpers.
 *
 * Mirrors the value model of the Python reference implementation
 * (fvp/ref/fvm_ref.py): a stack machine whose values are Python ints,
 * floats and unicode strings.  int64/double stand in for Python's
 * arbitrary-precision int; see fvp/docs/c-vm.md for the differences.
 */
#ifndef FVP_FVP_H
#define FVP_FVP_H

#include <stddef.h>
#include <stdint.h>

#define FVP_GLOBAL_COUNT 8192
#define FVP_MAX_ARGC 32
#define FVP_LOCALS_LIMIT 256

/* Immutable UTF-8 string.  Strings are allocated from an arena and never
 * freed individually (the arena is released when the VM is destroyed), which
 * matches the reference implementation's GC'd semantics closely enough for
 * the string operations the scripts actually perform. */
typedef struct FvpStr {
    uint32_t len;      /* byte length, excluding the NUL terminator */
    char data[];       /* UTF-8 bytes, always NUL terminated */
} FvpStr;

typedef enum FvpType {
    FVP_INT = 0,
    FVP_FLOAT = 1,
    FVP_STR = 2,
    /* 原版 Variant 的 type 0：false / null /「可选参数省略」。
     * pushfalse(0x08) 压出来的是它（原版 0x444940 `movb $0x0,...`：
     * 整段函数只写类型字节，载荷不动）。
     * 在数值/字符串上下文里与 int 0 等价（见 fvp.c 的说明），
     * 但 handler 可以用 fvp_value_is_null() 区分"省略"与"显式 0"——
     * LipAnim(38)/LipSync(39)/ControlMask(8)/CursorShow 都靠这个区别。 */
    FVP_NULL = 3,
    /* 原版 Variant 的 **type 1**：`true`。pushtrue(0x09) 压出来的是它
     * （原版 0x444960：`c6 44 c1 08 01` = 只写类型字节 0x1，载荷不动）。
     *
     * 为什么必须与 int 1 区分：原版每个 PrimSet / Motion 类 handler 都做
     * `cmpb $0x2,(%edx)` / `cmpb $0x2,0x8(%edx)` 之类的**逐实参类型检查**，
     * 只有 Variant type 2（int）才写字段；脚本用 `true` 当哨兵表示"这一维不改"。
     * 我们以前把 pushtrue 压成 int 1，于是 `PrimSetXY(551, true, true)` 把
     * x/y 写成 1 —— 正文窗口因此落在 (1,1)（用户报的"正文位置不对"根因）。
     * 数值上下文里它仍等于 1（fvp_as_number），所以比较/算术行为不变。 */
    FVP_BOOL = 4
} FvpType;

typedef struct FvpValue {
    uint8_t type;
    int64_t i;
    double f;
    FvpStr *str;
} FvpValue;

static inline FvpValue fvp_make_int(int64_t i)
{
    FvpValue v;
    v.type = (uint8_t)FVP_INT;
    v.i = i;
    v.f = 0.0;
    v.str = NULL;
    return v;
}

static inline FvpValue fvp_make_null(void)
{
    FvpValue v;
    v.type = (uint8_t)FVP_NULL;
    v.i = 0;
    v.f = 0.0;
    v.str = NULL;
    return v;
}

static inline FvpValue fvp_make_float(double d)
{
    FvpValue v;
    v.type = (uint8_t)FVP_FLOAT;
    v.i = 0;
    v.f = d;
    v.str = NULL;
    return v;
}

static inline FvpValue fvp_make_str(FvpStr *s)
{
    FvpValue v;
    v.type = (uint8_t)FVP_STR;
    v.i = 0;
    v.f = 0.0;
    v.str = s;
    return v;
}

/* 原版 Variant type 1 = true（pushtrue 压的值）。
 * 数值上下文里等于 1，但类型不是 int —— 这正是各 handler 的
 * `cmpb $0x2` 类型检查要区分的东西。 */
static inline FvpValue fvp_value_bool(void)
{
    FvpValue v;
    v.type = (uint8_t)FVP_BOOL;
    v.i = 1;
    v.f = 1.0;
    v.str = NULL;
    return v;
}

/* Python truthiness: 0 / 0.0 / "" are false, everything else true.
 * FVP_NULL (原版 type 0) is false. */
int fvp_value_truth(const FvpValue *v);

/* 1 = 原版 Variant 的 type 0（false / null / 省略），即 pushfalse 压出来的值。
 * 参数指针本身为 NULL 时返回 0（"没有这个参数" 由各 handler 自己判 argc）。 */
int fvp_value_is_null(const FvpValue *v);

/* 1 = 原版 Variant 的 type 1（true），即 pushtrue 压出来的值。 */
int fvp_value_is_bool(const FvpValue *v);

/* Python `a == b` (never raises; cross-type number comparison). */
int fvp_value_eq(const FvpValue *a, const FvpValue *b);

/* Python ordering for `a < b` style operators.  Returns -1/0/1 and sets
 * *ok = 0 when Python would raise TypeError (string vs number). */
int fvp_value_cmp(const FvpValue *a, const FvpValue *b, int *ok);

void fvp_fatal(const char *fmt, ...);
void *fvp_xmalloc(size_t n);
void *fvp_xcalloc(size_t count, size_t size);
void *fvp_xrealloc(void *p, size_t n);
char *fvp_xstrdup(const char *s);

/* Bump allocator for immutable strings. */
typedef struct FvpArenaChunk {
    struct FvpArenaChunk *next;
    size_t used;
    size_t cap;
    unsigned char data[];
} FvpArenaChunk;

typedef struct FvpArena {
    FvpArenaChunk *head;
} FvpArena;

void fvp_arena_init(FvpArena *a);
void fvp_arena_free(FvpArena *a);
void *fvp_arena_alloc(FvpArena *a, size_t n);
FvpStr *fvp_arena_str(FvpArena *a, const char *data, size_t len);

/* str(value) exactly as Python would render it (int -> decimal,
 * float -> repr, str -> itself). */
FvpStr *fvp_value_to_str(FvpArena *a, const FvpValue *v);
/* Python repr(x) for a float, best effort (shortest round-trip). */
FvpStr *fvp_float_repr(FvpArena *a, double d);

/* Convenience: length of a C string capped at UINT32_MAX. */
size_t fvp_strlen(const char *s);

#endif /* FVP_FVP_H */