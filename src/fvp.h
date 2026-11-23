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
    FVP_STR = 2
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

/* Python truthiness: 0 / 0.0 / "" are false, everything else true. */
int fvp_value_truth(const FvpValue *v);

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