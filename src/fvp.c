/* FVP VM in C -- value helpers, arena allocator, diagnostics. */
#include "fvp.h"

#include <errno.h>
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void fvp_fatal(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    fputs("fvm: ", stderr);
    vfprintf(stderr, fmt, ap);
    fputc('\n', stderr);
    va_end(ap);
    exit(2);
}

void *fvp_xmalloc(size_t n)
{
    void *p = malloc(n ? n : 1);
    if (p == NULL) {
        fvp_fatal("out of memory (%lu bytes)", (unsigned long)n);
    }
    return p;
}

void *fvp_xcalloc(size_t count, size_t size)
{
    void *p = calloc(count ? count : 1, size ? size : 1);
    if (p == NULL) {
        fvp_fatal("out of memory (%lu x %lu bytes)",
                  (unsigned long)count, (unsigned long)size);
    }
    return p;
}

void *fvp_xrealloc(void *p, size_t n)
{
    void *q = realloc(p, n ? n : 1);
    if (q == NULL) {
        fvp_fatal("out of memory (%lu bytes)", (unsigned long)n);
    }
    return q;
}

char *fvp_xstrdup(const char *s)
{
    size_t n = strlen(s) + 1;
    char *p = (char *)fvp_xmalloc(n);
    memcpy(p, s, n);
    return p;
}

size_t fvp_strlen(const char *s)
{
    return strlen(s);
}

/* ------------------------------------------------------------------ */
/* Arena                                                               */
/* ------------------------------------------------------------------ */

#define FVP_ARENA_CHUNK (64u * 1024u)

void fvp_arena_init(FvpArena *a)
{
    a->head = NULL;
}

void fvp_arena_free(FvpArena *a)
{
    FvpArenaChunk *c = a->head;
    while (c != NULL) {
        FvpArenaChunk *next = c->next;
        free(c);
        c = next;
    }
    a->head = NULL;
}

static void *fvp_arena_alloc_raw(FvpArena *a, size_t n)
{
    size_t align = sizeof(void *);
    n = (n + align - 1) & ~(align - 1);
    if (a->head == NULL || a->head->cap - a->head->used < n) {
        size_t cap = n > FVP_ARENA_CHUNK ? n : FVP_ARENA_CHUNK;
        FvpArenaChunk *c = (FvpArenaChunk *)fvp_xmalloc(sizeof(FvpArenaChunk) + cap);
        c->next = a->head;
        c->used = 0;
        c->cap = cap;
        a->head = c;
    }
    {
        void *p = a->head->data + a->head->used;
        a->head->used += n;
        return p;
    }
}

void *fvp_arena_alloc(FvpArena *a, size_t n)
{
    return fvp_arena_alloc_raw(a, n);
}

FvpStr *fvp_arena_str(FvpArena *a, const char *data, size_t len)
{
    FvpStr *s = (FvpStr *)fvp_arena_alloc_raw(a, sizeof(FvpStr) + len + 1);
    s->len = (uint32_t)len;
    if (len != 0) {
        memcpy(s->data, data, len);
    }
    s->data[len] = '\0';
    return s;
}

/* ------------------------------------------------------------------ */
/* Value semantics (Python)                                            */
/* ------------------------------------------------------------------ */

int fvp_value_truth(const FvpValue *v)
{
    switch (v->type) {
    case FVP_INT:
        return v->i != 0;
    case FVP_FLOAT:
        return v->f != 0.0;
    case FVP_STR:
        return v->str != NULL && v->str->len != 0;
    case FVP_NULL:
        return 0; /* 原版 type 0：false / null */
    default:
        return 0;
    }
}

int fvp_value_is_null(const FvpValue *v)
{
    return v != NULL && v->type == (uint8_t)FVP_NULL;
}

/* 原版 Variant 的 type 0（null/false）在数值运算里按 int 0 处理。
 *
 * 为什么：本项目的逐字节基准（ref/ref-900k.txt，900k 步输出的每一行台词）是拿
 * Python 参考实现生成的，那份参考实现里 pushfalse 就是 0；若这里让 null 在
 * 比较/算术里"不可比"，脚本里高频的 `syscall X; pushtemp; pushfalse; eq` 模式
 * 会整片改变结果，冻结基准就守不住了。所以数值/字符串上下文保持与 int 0 等价，
 * 只在 handler 需要区分"省略"与"显式 0"时用 fvp_value_is_null()。 */
static void fvp_as_number(const FvpValue *v, FvpValue *out)
{
    *out = *v;
    if (out->type == (uint8_t)FVP_NULL) {
        out->type = (uint8_t)FVP_INT;
        out->i = 0;
        out->f = 0.0;
        out->str = NULL;
    }
}

static int fvp_str_eq(const FvpStr *a, const FvpStr *b)
{
    if (a == b) {
        return 1;
    }
    if (a == NULL || b == NULL) {
        return a == b;
    }
    return a->len == b->len && memcmp(a->data, b->data, a->len) == 0;
}

static int fvp_num_eq(const FvpValue *a, const FvpValue *b)
{
    if (a->type == (uint8_t)FVP_INT && b->type == (uint8_t)FVP_INT) {
        return a->i == b->i;
    }
    if (a->type == (uint8_t)FVP_FLOAT && b->type == (uint8_t)FVP_FLOAT) {
        return a->f == b->f;
    }
    if (a->type == (uint8_t)FVP_INT) {
        return (double)a->i == b->f;
    }
    return a->f == (double)b->i;
}

int fvp_value_eq(const FvpValue *a, const FvpValue *b)
{
    FvpValue na;
    FvpValue nb;
    int an;
    int bn;

    fvp_as_number(a, &na);
    fvp_as_number(b, &nb);
    an = na.type != (uint8_t)FVP_STR;
    bn = nb.type != (uint8_t)FVP_STR;
    if (an && bn) {
        return fvp_num_eq(&na, &nb);
    }
    if (!an && !bn) {
        return fvp_str_eq(na.str, nb.str);
    }
    return 0;
}

int fvp_value_cmp(const FvpValue *a, const FvpValue *b, int *ok)
{
    FvpValue na;
    FvpValue nb;
    int an;
    int bn;

    fvp_as_number(a, &na);
    fvp_as_number(b, &nb);
    an = na.type != (uint8_t)FVP_STR;
    bn = nb.type != (uint8_t)FVP_STR;
    *ok = 1;
    if (an && bn) {
        if (na.type == (uint8_t)FVP_INT && nb.type == (uint8_t)FVP_INT) {
            if (na.i < nb.i) {
                return -1;
            }
            return na.i > nb.i;
        }
        {
            double x = na.type == (uint8_t)FVP_INT ? (double)na.i : na.f;
            double y = nb.type == (uint8_t)FVP_INT ? (double)nb.i : nb.f;
            if (x < y) {
                return -1;
            }
            return x > y;
        }
    }
    if (!an && !bn) {
        const FvpStr *s = na.str;
        const FvpStr *t = nb.str;
        uint32_t n = s->len < t->len ? s->len : t->len;
        int c = n ? memcmp(s->data, t->data, n) : 0;
        if (c != 0) {
            return c < 0 ? -1 : 1;
        }
        if (s->len == t->len) {
            return 0;
        }
        return s->len < t->len ? -1 : 1;
    }
    *ok = 0;
    return 0;
}

/* ------------------------------------------------------------------ */
/* str(value)                                                          */
/* ------------------------------------------------------------------ */

static int fvp_roundtrips(const char *s, double d)
{
    char *end = NULL;
    double x;
    uint64_t ux;
    uint64_t ud;
    errno = 0;
    x = strtod(s, &end);
    if (end == NULL || *end != '\0') {
        return 0;
    }
    memcpy(&ux, &x, sizeof(ux));
    memcpy(&ud, &d, sizeof(ud));
    return ux == ud;
}

FvpStr *fvp_float_repr(FvpArena *a, double d)
{
    char out[160];
    char mant[32];
    char ebuf[16];
    int o = 0;
    int nd = 0;
    int exp10 = 0;
    int neg;
    double ad;
    int p;

    if (isnan(d)) {
        return fvp_arena_str(a, "nan", 3);
    }
    if (isinf(d)) {
        return d < 0.0 ? fvp_arena_str(a, "-inf", 4) : fvp_arena_str(a, "inf", 3);
    }
    if (d == 0.0) {
        return signbit(d) ? fvp_arena_str(a, "-0.0", 4) : fvp_arena_str(a, "0.0", 3);
    }
    neg = d < 0.0;
    ad = neg ? -d : d;

    for (p = 1; p <= 17; ++p) {
        char tmp[64];
        const char *e;
        const char *c;
        int k = 0;
        snprintf(tmp, sizeof(tmp), "%.*e", p - 1, ad);
        if (!fvp_roundtrips(tmp, ad)) {
            continue;
        }
        e = strchr(tmp, 'e');
        if (e == NULL) {
            continue;
        }
        for (c = tmp; c < e; ++c) {
            if (*c != '.' && k < (int)sizeof(mant) - 1) {
                mant[k++] = *c;
            }
        }
        mant[k] = '\0';
        nd = k;
        exp10 = atoi(e + 1);
        break;
    }
    if (nd == 0) {
        /* should not happen: 17 significant digits always round-trip */
        snprintf(out, sizeof(out), "%.17g", d);
        return fvp_arena_str(a, out, strlen(out));
    }
    while (nd > 1 && mant[nd - 1] == '0') {
        nd--;
        mant[nd] = '\0';
    }

    if (neg) {
        out[o++] = '-';
    }
    if (exp10 >= -4 && exp10 < 16) {
        int intdigits = exp10 + 1;
        if (intdigits <= 0) {
            int i;
            out[o++] = '0';
            out[o++] = '.';
            for (i = 0; i < -intdigits; ++i) {
                out[o++] = '0';
            }
            memcpy(out + o, mant, (size_t)nd);
            o += nd;
        } else if (nd <= intdigits) {
            int i;
            memcpy(out + o, mant, (size_t)nd);
            o += nd;
            for (i = 0; i < intdigits - nd; ++i) {
                out[o++] = '0';
            }
            out[o++] = '.';
            out[o++] = '0';
        } else {
            memcpy(out + o, mant, (size_t)intdigits);
            o += intdigits;
            out[o++] = '.';
            memcpy(out + o, mant + intdigits, (size_t)(nd - intdigits));
            o += nd - intdigits;
        }
    } else {
        int ae = exp10 < 0 ? -exp10 : exp10;
        int en;
        out[o++] = mant[0];
        if (nd > 1) {
            out[o++] = '.';
            memcpy(out + o, mant + 1, (size_t)(nd - 1));
            o += nd - 1;
        }
        out[o++] = 'e';
        out[o++] = exp10 < 0 ? '-' : '+';
        en = snprintf(ebuf, sizeof(ebuf), "%d", ae);
        if (en < 2) {
            out[o++] = '0';
        }
        memcpy(out + o, ebuf, (size_t)en);
        o += en;
    }
    out[o] = '\0';
    return fvp_arena_str(a, out, (size_t)o);
}

FvpStr *fvp_value_to_str(FvpArena *a, const FvpValue *v)
{
    switch (v->type) {
    case FVP_INT: {
        char buf[32];
        int n = snprintf(buf, sizeof(buf), "%lld", (long long)v->i);
        return fvp_arena_str(a, buf, (size_t)n);
    }
    case FVP_FLOAT:
        return fvp_float_repr(a, v->f);
    case FVP_STR:
        return v->str;
    case FVP_NULL:
        /* 与 int 0 的字符串形式保持一致（见 fvp_as_number 的说明）。 */
        return fvp_arena_str(a, "0", 1);
    default:
        return fvp_arena_str(a, "", 0);
    }
}