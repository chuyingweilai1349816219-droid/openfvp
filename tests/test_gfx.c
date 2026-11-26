/* test_gfx.c -- drive src/gfx.c from a JSON snapshot and export the frame.
 *
 * The C renderer never re-runs the script VM.  fvp/tests/gfx_dump.py runs the
 * Python reference to a checkpoint and writes gfx-scene.json (prim table,
 * graph slots, parts overlays, text windows, expected draw list).  This
 * program:
 *
 *   1. loads that JSON into a Gfx through the public setters,
 *   2. renders the frame cold (image decode + composite + transforms) and
 *      warm (everything cached), reporting both timings,
 *   3. writes <out>/gfx-frame.png (RGBA, uncompressed deflate: no zlib in
 *      this project) and <out>/gfx-geom.json (the C draw list),
 *   4. diffs its draw list against the reference list inside the JSON and
 *      exits non-zero on any geometry mismatch.
 *
 * Build:  make test_gfx      (or see fvp/tests/verify_gfx.ps1)
 * Usage:  bin/test_gfx.exe fvp/out/c/gfx-scene.json [-o fvp/out/c] [-r N]
 */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "gfx.h"

/* ----------------------------------------------------------------------- */
/* tiny JSON reader (enough for the scene format written by gfx_dump.py)   */
/* ----------------------------------------------------------------------- */

enum { J_NULL = 0, J_BOOL, J_NUM, J_STR, J_ARR, J_OBJ };

typedef struct JVal JVal;
struct JVal {
    int type;
    double num;
    char *str;
    JVal **items; /* J_ARR                                          */
    int nitem;
    char **keys; /* J_OBJ                                           */
    JVal **vals;
    int nmem;
};

typedef struct JParser {
    const char *p;
    int err;
} JParser;

static void *xmalloc(size_t n)
{
    void *p = malloc(n != 0 ? n : 1);
    return p;
}

static JVal *jv_new(int type)
{
    JVal *v = (JVal *)calloc(1, sizeof(JVal));
    if (v != NULL) {
        v->type = type;
    }
    return v;
}

static void jv_free(JVal *v)
{
    int i;
    if (v == NULL) {
        return;
    }
    free(v->str);
    for (i = 0; i < v->nitem; i++) {
        jv_free(v->items[i]);
    }
    for (i = 0; i < v->nmem; i++) {
        free(v->keys[i]);
        jv_free(v->vals[i]);
    }
    free(v->items);
    free(v->keys);
    free(v->vals);
    free(v);
}

static void jskip(JParser *jp)
{
    while (*jp->p == ' ' || *jp->p == '\t' || *jp->p == '\r' ||
           *jp->p == '\n') {
        jp->p++;
    }
}

static char *jparse_string(JParser *jp)
{
    const char *p = jp->p;
    char *out;
    size_t cap, n = 0;

    if (*p != '"') {
        jp->err = 1;
        return NULL;
    }
    p++;
    cap = strlen(p) + 8;
    out = (char *)xmalloc(cap);
    if (out == NULL) {
        jp->err = 1;
        return NULL;
    }
    while (*p != '"') {
        if (*p == '\0') {
            free(out);
            jp->err = 1;
            return NULL;
        }
        if (*p == '\\') {
            p++;
            switch (*p) {
            case '"':
                out[n++] = '"';
                p++;
                break;
            case '\\':
                out[n++] = '\\';
                p++;
                break;
            case '/':
                out[n++] = '/';
                p++;
                break;
            case 'b':
                out[n++] = '\b';
                p++;
                break;
            case 'f':
                out[n++] = '\f';
                p++;
                break;
            case 'n':
                out[n++] = '\n';
                p++;
                break;
            case 'r':
                out[n++] = '\r';
                p++;
                break;
            case 't':
                out[n++] = '\t';
                p++;
                break;
            case 'u': {
                unsigned cp = 0;
                int k;
                for (k = 0; k < 4; k++) {
                    char c = p[1 + k];
                    cp <<= 4;
                    if (c >= '0' && c <= '9') {
                        cp |= (unsigned)(c - '0');
                    } else if (c >= 'a' && c <= 'f') {
                        cp |= (unsigned)(c - 'a' + 10);
                    } else if (c >= 'A' && c <= 'F') {
                        cp |= (unsigned)(c - 'A' + 10);
                    } else {
                        jp->err = 1;
                        free(out);
                        return NULL;
                    }
                }
                p += 5;
                if (cp < 0x80) {
                    out[n++] = (char)cp;
                } else if (cp < 0x800) {
                    out[n++] = (char)(0xC0 | (cp >> 6));
                    out[n++] = (char)(0x80 | (cp & 0x3F));
                } else {
                    out[n++] = (char)(0xE0 | (cp >> 12));
                    out[n++] = (char)(0x80 | ((cp >> 6) & 0x3F));
                    out[n++] = (char)(0x80 | (cp & 0x3F));
                }
                break;
            }
            default:
                jp->err = 1;
                free(out);
                return NULL;
            }
            continue;
        }
        out[n++] = *p++;
    }
    out[n] = '\0';
    jp->p = p + 1;
    return out;
}

static JVal *jparse_value(JParser *jp, int depth);

static JVal *jparse_array(JParser *jp, int depth)
{
    JVal *v = jv_new(J_ARR);
    int cap = 0;

    if (v == NULL) {
        jp->err = 1;
        return NULL;
    }
    jp->p++; /* '[' */
    jskip(jp);
    if (*jp->p == ']') {
        jp->p++;
        return v;
    }
    for (;;) {
        JVal *it;
        jskip(jp);
        it = jparse_value(jp, depth + 1);
        if (it == NULL) {
            jv_free(v);
            return NULL;
        }
        if (v->nitem >= cap) {
            int nc = cap ? cap * 2 : 8;
            JVal **ni = (JVal **)realloc(v->items, (size_t)nc * sizeof(JVal *));
            if (ni == NULL) {
                jv_free(it);
                jv_free(v);
                jp->err = 1;
                return NULL;
            }
            v->items = ni;
            cap = nc;
        }
        v->items[v->nitem++] = it;
        jskip(jp);
        if (*jp->p == ',') {
            jp->p++;
            continue;
        }
        if (*jp->p == ']') {
            jp->p++;
            return v;
        }
        jv_free(v);
        jp->err = 1;
        return NULL;
    }
}

static JVal *jparse_object(JParser *jp, int depth)
{
    JVal *v = jv_new(J_OBJ);
    int cap = 0;

    if (v == NULL) {
        jp->err = 1;
        return NULL;
    }
    jp->p++; /* '{' */
    jskip(jp);
    if (*jp->p == '}') {
        jp->p++;
        return v;
    }
    for (;;) {
        char *key;
        JVal *val;

        jskip(jp);
        key = jparse_string(jp);
        if (key == NULL) {
            jv_free(v);
            return NULL;
        }
        jskip(jp);
        if (*jp->p != ':') {
            free(key);
            jv_free(v);
            jp->err = 1;
            return NULL;
        }
        jp->p++;
        jskip(jp);
        val = jparse_value(jp, depth + 1);
        if (val == NULL) {
            free(key);
            jv_free(v);
            return NULL;
        }
        if (v->nmem >= cap) {
            int nc = cap ? cap * 2 : 8;
            char **nk = (char **)realloc(v->keys, (size_t)nc * sizeof(char *));
            JVal **nv = (JVal **)realloc(v->vals, (size_t)nc * sizeof(JVal *));
            if (nk == NULL || nv == NULL) {
                free(key);
                jv_free(val);
                jv_free(v);
                jp->err = 1;
                return NULL;
            }
            v->keys = nk;
            v->vals = nv;
            cap = nc;
        }
        v->keys[v->nmem] = key;
        v->vals[v->nmem] = val;
        v->nmem++;
        jskip(jp);
        if (*jp->p == ',') {
            jp->p++;
            continue;
        }
        if (*jp->p == '}') {
            jp->p++;
            return v;
        }
        jv_free(v);
        jp->err = 1;
        return NULL;
    }
}

static JVal *jparse_value(JParser *jp, int depth)
{
    JVal *v;

    jskip(jp);
    if (depth > 64) {
        jp->err = 1;
        return NULL;
    }
    switch (*jp->p) {
    case '{':
        return jparse_object(jp, depth);
    case '[':
        return jparse_array(jp, depth);
    case '"': {
        char *s = jparse_string(jp);
        if (s == NULL) {
            return NULL;
        }
        v = jv_new(J_STR);
        if (v == NULL) {
            free(s);
            return NULL;
        }
        v->str = s;
        return v;
    }
    case 't':
        if (strncmp(jp->p, "true", 4) == 0) {
            jp->p += 4;
            v = jv_new(J_BOOL);
            if (v != NULL) {
                v->num = 1;
            }
            return v;
        }
        break;
    case 'f':
        if (strncmp(jp->p, "false", 5) == 0) {
            jp->p += 5;
            v = jv_new(J_BOOL);
            if (v != NULL) {
                v->num = 0;
            }
            return v;
        }
        break;
    case 'n':
        if (strncmp(jp->p, "null", 4) == 0) {
            jp->p += 4;
            return jv_new(J_NULL);
        }
        break;
    default:
        break;
    }
    {
        char *end = NULL;
        double d = strtod(jp->p, &end);
        if (end == NULL || end == jp->p) {
            jp->err = 1;
            return NULL;
        }
        jp->p = end;
        v = jv_new(J_NUM);
        if (v != NULL) {
            v->num = d;
        }
        return v;
    }
}

static JVal *jload(const char *path)
{
    FILE *fp = fopen(path, "rb");
    long n;
    char *buf;
    JParser jp;
    JVal *v;

    if (fp == NULL) {
        fprintf(stderr, "test_gfx: cannot open %s\n", path);
        return NULL;
    }
    if (fseek(fp, 0, SEEK_END) != 0) {
        fclose(fp);
        return NULL;
    }
    n = ftell(fp);
    rewind(fp);
    if (n <= 0) {
        fclose(fp);
        return NULL;
    }
    buf = (char *)xmalloc((size_t)n + 1);
    if (buf == NULL) {
        fclose(fp);
        return NULL;
    }
    if (fread(buf, 1, (size_t)n, fp) != (size_t)n) {
        free(buf);
        fclose(fp);
        return NULL;
    }
    fclose(fp);
    buf[n] = '\0';
    jp.p = buf;
    jp.err = 0;
    v = jparse_value(&jp, 0);
    free(buf);
    if (v == NULL || jp.err) {
        fprintf(stderr, "test_gfx: JSON parse error in %s\n", path);
        jv_free(v);
        return NULL;
    }
    return v;
}

static JVal *jget(const JVal *obj, const char *key)
{
    int i;
    if (obj == NULL || obj->type != J_OBJ) {
        return NULL;
    }
    for (i = 0; i < obj->nmem; i++) {
        if (strcmp(obj->keys[i], key) == 0) {
            return obj->vals[i];
        }
    }
    return NULL;
}

static int jint(const JVal *v, int def)
{
    return (v != NULL && v->type == J_NUM) ? (int)v->num : def;
}

static const char *jstr(const JVal *v)
{
    return (v != NULL && v->type == J_STR) ? v->str : NULL;
}

/* ----------------------------------------------------------------------- */
/* PNG writer (no zlib in this project: stored deflate blocks)             */
/* ----------------------------------------------------------------------- */

typedef struct Buf {
    unsigned char *p;
    size_t n, cap;
    int oom;
} Buf;

static void buf_reserve(Buf *b, size_t extra)
{
    if (b->oom) {
        return;
    }
    if (b->n + extra > b->cap) {
        size_t nc = b->cap ? b->cap : 65536;
        unsigned char *np;
        while (nc < b->n + extra) {
            nc *= 2;
        }
        np = (unsigned char *)realloc(b->p, nc);
        if (np == NULL) {
            b->oom = 1;
            return;
        }
        b->p = np;
        b->cap = nc;
    }
}

static void buf_put(Buf *b, const void *data, size_t n)
{
    buf_reserve(b, n);
    if (b->oom) {
        return;
    }
    memcpy(b->p + b->n, data, n);
    b->n += n;
}

static void buf_u8(Buf *b, unsigned v)
{
    unsigned char c = (unsigned char)v;
    buf_put(b, &c, 1);
}

static void buf_be32(Buf *b, unsigned long v)
{
    unsigned char c[4];
    c[0] = (unsigned char)((v >> 24) & 0xFF);
    c[1] = (unsigned char)((v >> 16) & 0xFF);
    c[2] = (unsigned char)((v >> 8) & 0xFF);
    c[3] = (unsigned char)(v & 0xFF);
    buf_put(b, c, 4);
}

static unsigned long crc_table[256];
static int crc_ready;

static void crc_init(void)
{
    unsigned long c;
    int n, k;
    for (n = 0; n < 256; n++) {
        c = (unsigned long)n;
        for (k = 0; k < 8; k++) {
            c = (c & 1) ? 0xEDB88320ul ^ (c >> 1) : (c >> 1);
        }
        crc_table[n] = c;
    }
    crc_ready = 1;
}

static unsigned long crc32_of(const unsigned char *p, size_t n)
{
    unsigned long c = 0xFFFFFFFFul;
    size_t i;
    if (!crc_ready) {
        crc_init();
    }
    for (i = 0; i < n; i++) {
        c = crc_table[(c ^ p[i]) & 0xFF] ^ (c >> 8);
    }
    return c ^ 0xFFFFFFFFul;
}

static unsigned long adler32_of(const unsigned char *p, size_t n)
{
    unsigned long a = 1, b = 0;
    size_t i;
    for (i = 0; i < n; i++) {
        a = (a + p[i]) % 65521ul;
        b = (b + a) % 65521ul;
    }
    return (b << 16) | a;
}

static void png_chunk(Buf *out, const char *type, const unsigned char *data,
                      size_t n)
{
    unsigned char *tmp = (unsigned char *)xmalloc(n + 4);
    unsigned long crc;

    if (tmp == NULL) {
        out->oom = 1;
        return;
    }
    memcpy(tmp, type, 4);
    if (n != 0) {
        memcpy(tmp + 4, data, n);
    }
    crc = crc32_of(tmp, n + 4);
    buf_be32(out, (unsigned long)n);
    buf_put(out, tmp, n + 4);
    buf_be32(out, crc);
    free(tmp);
}

static int write_png(const char *path, const unsigned char *rgba, int w, int h)
{
    Buf out;
    Buf raw;
    Buf z;
    unsigned char ihdr[13];
    size_t stride = (size_t)w * 4u;
    size_t y, off;
    int i;

    memset(&out, 0, sizeof out);
    memset(&raw, 0, sizeof raw);
    memset(&z, 0, sizeof z);
    /* scanlines with filter byte 0 */
    buf_reserve(&raw, (stride + 1) * (size_t)h);
    for (y = 0; y < (size_t)h; y++) {
        unsigned char zero = 0;
        buf_put(&raw, &zero, 1);
        buf_put(&raw, rgba + y * stride, stride);
    }
    if (raw.oom) {
        free(raw.p);
        return -1;
    }
    /* zlib stream with stored (uncompressed) deflate blocks */
    buf_u8(&z, 0x78);
    buf_u8(&z, 0x01);
    off = 0;
    while (off < raw.n) {
        size_t chunk = raw.n - off;
        int final;
        unsigned char hdr[5];
        if (chunk > 65535u) {
            chunk = 65535u;
        }
        final = (off + chunk >= raw.n) ? 1 : 0;
        hdr[0] = (unsigned char)final;
        hdr[1] = (unsigned char)(chunk & 0xFF);
        hdr[2] = (unsigned char)((chunk >> 8) & 0xFF);
        hdr[3] = (unsigned char)(~chunk & 0xFF);
        hdr[4] = (unsigned char)((~chunk >> 8) & 0xFF);
        buf_put(&z, hdr, 5);
        buf_put(&z, raw.p + off, chunk);
        off += chunk;
    }
    buf_be32(&z, adler32_of(raw.p, raw.n));
    free(raw.p);
    if (z.oom) {
        free(z.p);
        return -1;
    }
    /* PNG signature + IHDR + IDAT + IEND */
    {
        static const unsigned char sig[8] = {0x89, 'P', 'N', 'G', '\r', '\n',
                                             0x1A, '\n'};
        buf_put(&out, sig, 8);
    }
    ihdr[0] = (unsigned char)((w >> 24) & 0xFF);
    ihdr[1] = (unsigned char)((w >> 16) & 0xFF);
    ihdr[2] = (unsigned char)((w >> 8) & 0xFF);
    ihdr[3] = (unsigned char)(w & 0xFF);
    ihdr[4] = (unsigned char)((h >> 24) & 0xFF);
    ihdr[5] = (unsigned char)((h >> 16) & 0xFF);
    ihdr[6] = (unsigned char)((h >> 8) & 0xFF);
    ihdr[7] = (unsigned char)(h & 0xFF);
    ihdr[8] = 8;  /* bit depth */
    ihdr[9] = 6;  /* colour type RGBA */
    ihdr[10] = 0; /* compression */
    ihdr[11] = 0; /* filter */
    ihdr[12] = 0; /* interlace */
    png_chunk(&out, "IHDR", ihdr, sizeof ihdr);
    /* IDAT may be split, but a single chunk is legal */
    png_chunk(&out, "IDAT", z.p, z.n);
    png_chunk(&out, "IEND", NULL, 0);
    free(z.p);
    if (out.oom) {
        free(out.p);
        return -1;
    }
    {
        FILE *fp = fopen(path, "wb");
        if (fp == NULL) {
            free(out.p);
            return -1;
        }
        i = (fwrite(out.p, 1, out.n, fp) == out.n) ? 0 : -1;
        fclose(fp);
    }
    free(out.p);
    return i;
}

/* ----------------------------------------------------------------------- */
/* scene loading                                                           */
/* ----------------------------------------------------------------------- */

static int path_join(char *dst, size_t cap, const char *dir, const char *name)
{
    int n;
    if (name == NULL || name[0] == '/') {
        n = snprintf(dst, cap, "%s", name != NULL ? name : "");
    } else {
        n = snprintf(dst, cap, "%s/%s", dir, name);
    }
    return (n > 0 && (size_t)n < cap) ? 0 : -1;
}

static int load_fonts(Gfx *g, JVal *root, const char *dir)
{
    JVal *fonts = jget(root, "fonts");
    int i;

    if (fonts == NULL || fonts->type != J_ARR) {
        return 0;
    }
    for (i = 0; i < fonts->nitem; i++) {
        const char *name = jstr(jget(fonts->items[i], "path"));
        char path[1024];
        GfxFont *f;
        if (name == NULL || path_join(path, sizeof path, dir, name) != 0) {
            continue;
        }
        f = gfx_font_load(path);
        if (f == NULL) {
            fprintf(stderr, "[font] cannot load %s (built-in fallback)\n", path);
            continue;
        }
        gfx_add_font(g, f);
    }
    return 0;
}

static int load_prims(Gfx *g, JVal *root)
{
    JVal *prims = jget(root, "prims");
    int i;

    if (prims == NULL || prims->type != J_ARR) {
        return -1;
    }
    /* pass 1: scalar fields */
    for (i = 0; i < prims->nitem; i++) {
        JVal *p = prims->items[i];
        int id = jint(jget(p, "id"), -1);
        if (id < 0) {
            continue;
        }
        gfx_prim_reset(g, id);
        gfx_set_prim(g, id, GFX_F_TYPE, jint(jget(p, "type"), 0));
        gfx_set_prim(g, id, GFX_F_DRAW, jint(jget(p, "draw"), 1));
        gfx_set_prim(g, id, GFX_F_ALPHA, jint(jget(p, "alpha"), 255));
        gfx_set_prim(g, id, GFX_F_BLEND, jint(jget(p, "blend"), 0));
        gfx_set_prim(g, id, GFX_F_Z, jint(jget(p, "z"), 1000));
        gfx_set_prim(g, id, GFX_F_X, jint(jget(p, "x"), 0));
        gfx_set_prim(g, id, GFX_F_Y, jint(jget(p, "y"), 0));
        gfx_set_prim(g, id, GFX_F_W, jint(jget(p, "w"), 0));
        gfx_set_prim(g, id, GFX_F_H, jint(jget(p, "h"), 0));
        gfx_set_prim(g, id, GFX_F_U, jint(jget(p, "u"), 0));
        gfx_set_prim(g, id, GFX_F_V, jint(jget(p, "v"), 0));
        gfx_set_prim(g, id, GFX_F_OP1, jint(jget(p, "op1"), 0));
        gfx_set_prim(g, id, GFX_F_OP2, jint(jget(p, "op2"), 0));
        gfx_set_prim(g, id, GFX_F_ANGLE, jint(jget(p, "angle"), 0));
        gfx_set_prim(g, id, GFX_F_SCALE_X, jint(jget(p, "sx"), 1000));
        gfx_set_prim(g, id, GFX_F_SCALE_Y, jint(jget(p, "sy"), 1000));
        gfx_set_prim(g, id, GFX_F_GRAPH, jint(jget(p, "graph"), -1));
        gfx_set_prim(g, id, GFX_F_TW, jint(jget(p, "tw"), -1));
        gfx_set_prim(g, id, GFX_F_TILE, jint(jget(p, "tile"), 0));
        gfx_set_prim(g, id, GFX_F_FLAGS, jint(jget(p, "flags"), 0));
    }
    /* pass 2: group links (children order == engine insertion order) */
    for (i = 0; i < prims->nitem; i++) {
        JVal *p = prims->items[i];
        JVal *ch = jget(p, "children");
        int id = jint(jget(p, "id"), -1);
        int k;
        if (id < 0 || ch == NULL || ch->type != J_ARR) {
            continue;
        }
        for (k = 0; k < ch->nitem; k++) {
            int cid = jint(ch->items[k], -1);
            if (cid >= 0) {
                if (gfx_prim_group_in(g, cid, id) != 0) {
                    fprintf(stderr, "[prim] group_in(%d,%d) failed\n", cid, id);
                }
            }
        }
    }
    return 0;
}

static int load_graphs(Gfx *g, JVal *root)
{
    JVal *graphs = jget(root, "graphs");
    int i, loaded = 0, failed = 0;

    if (graphs == NULL || graphs->type != J_ARR) {
        return -1;
    }
    for (i = 0; i < graphs->nitem; i++) {
        JVal *o = graphs->items[i];
        int slot = jint(jget(o, "slot"), -1);
        const char *name = jstr(jget(o, "name"));
        JVal *ov;
        int k;
        if (slot < 0 || name == NULL) {
            continue;
        }
        if (gfx_graph_load(g, slot, name) != 0) {
            fprintf(stderr, "[graph] slot %d: %s -> %s\n", slot, name,
                    gfx_last_error(g));
            failed++;
            continue;
        }
        loaded++;
        gfx_graph_set_frame(g, slot, jint(jget(o, "frame"), 0));
        ov = jget(o, "overlays");
        if (ov != NULL && ov->type == J_ARR) {
            for (k = 0; k < ov->nitem; k++) {
                JVal *e = ov->items[k];
                const char *on = jstr(jget(e, "name"));
                if (on == NULL) {
                    continue;
                }
                gfx_graph_add_overlay(g, slot, on, jint(jget(e, "x"), 0),
                                      jint(jget(e, "y"), 0),
                                      jint(jget(e, "frame"), 0));
            }
        }
    }
    printf("[load] graphs: %d loaded, %d failed\n", loaded, failed);
    return failed != 0 ? -1 : 0;
}

static int load_texts(Gfx *g, JVal *root)
{
    JVal *texts = jget(root, "texts");
    int i;

    if (texts != NULL && texts->type == J_ARR) {
        for (i = 0; i < texts->nitem; i++) {
            JVal *o = texts->items[i];
            int idx = jint(jget(o, "idx"), -1);
            JVal *lines = jget(o, "lines");
            JVal *color = jget(o, "color");
            JVal *size = jget(o, "size");
            JVal *pos = jget(o, "pos");
            JVal *outsz = jget(o, "out_size");
            int k;
            if (idx < 0 || idx >= 32) {
                continue;
            }
            if (lines != NULL && lines->type == J_ARR) {
                for (k = 0; k < lines->nitem; k++) {
                    if (lines->items[k]->type == J_STR) {
                        gfx_text_append(g, idx, lines->items[k]->str);
                    }
                }
            }
            if (color != NULL && color->nitem >= 3) {
                gfx_text_set_color(g, idx, jint(color->items[0], 255),
                                   jint(color->items[1], 255),
                                   jint(color->items[2], 255));
            }
            if (size != NULL && size->nitem >= 2) {
                gfx_text_set_size(g, idx, jint(size->items[0], 32),
                                  jint(size->items[1], 32));
            }
            if (pos != NULL && pos->type == J_ARR && pos->nitem >= 2) {
                gfx_text_set_pos(g, idx, jint(pos->items[0], 0),
                                 jint(pos->items[1], 0));
            }
            if (outsz != NULL && outsz->nitem >= 2) {
                gfx_text_set_out_size(g, idx, jint(outsz->items[0], 64),
                                      jint(outsz->items[1], 16));
            }
        }
    }
    {
        JVal *lay = jget(root, "text_layout");
        if (lay != NULL && lay->type == J_OBJ) {
            int k;
            for (k = 0; k < lay->nmem; k++) {
                int idx = atoi(lay->keys[k]);
                JVal *o = lay->vals[k];
                GfxTextLayout l;
                if (idx < 0 || idx >= 32 || o->type != J_OBJ) {
                    continue;
                }
                l.x = jint(jget(o, "x"), 0);
                l.y = jint(jget(o, "y"), 0);
                l.max_w = jint(jget(o, "maxw"), 1600);
                l.size = jint(jget(o, "size"), 30);
                l.max_lines = jint(jget(o, "lines"), 3);
                gfx_text_set_layout(g, idx, &l);
            }
        }
    }
    return 0;
}

/* ----------------------------------------------------------------------- */
/* geometry comparison                                                     */
/* ----------------------------------------------------------------------- */

static const char *mode_name(int mode)
{
    switch (mode) {
    case GFX_MODE_CHAR:
        return "char";
    case GFX_MODE_BG:
        return "bg";
    case GFX_MODE_TEXT:
        return "text";
    default:
        return "ui";
    }
}

static int mode_from_name(const char *s)
{
    if (s == NULL) {
        return GFX_MODE_UI;
    }
    if (strcmp(s, "char") == 0) {
        return GFX_MODE_CHAR;
    }
    if (strcmp(s, "bg") == 0) {
        return GFX_MODE_BG;
    }
    return GFX_MODE_UI;
}

static int compare_draw(Gfx *g, JVal *root)
{
    JVal *exp = jget(root, "expect_draw");
    int n = gfx_draw_count(g);
    int i, bad = 0;

    if (exp == NULL || exp->type != J_ARR) {
        printf("[geom] no expect_draw in scene json\n");
        return 0;
    }
    printf("[geom] C draw list: %d, reference: %d\n", n, exp->nitem);
    if (n != exp->nitem) {
        printf("[geom] COUNT MISMATCH\n");
        bad++;
    }
    for (i = 0; i < exp->nitem && i < n; i++) {
        const GfxDrawItem *it = gfx_draw_item(g, i);
        JVal *e = exp->items[i];
        const char *ename = jstr(jget(e, "name"));
        int eprim = jint(jget(e, "prim"), -1);
        int ex = jint(jget(e, "x"), 0), ey = jint(jget(e, "y"), 0);
        int ew = jint(jget(e, "w"), 0), eh = jint(jget(e, "h"), 0);
        int emode = mode_from_name(jstr(jget(e, "mode")));
        int ez = jint(jget(e, "z"), 0);
        int ea = jint(jget(e, "alpha"), 0);
        if (it == NULL) {
            bad++;
            continue;
        }
        if (it->prim != eprim || it->x != ex || it->y != ey || it->w != ew ||
            it->h != eh || it->mode != emode || it->z != ez ||
            it->alpha != ea || ename == NULL || strcmp(it->name, ename) != 0) {
            printf("[geom] #%d MISMATCH prim=%d/%d pos=%d,%d/%d,%d size=%dx%d/"
                   "%dx%d mode=%s/%s z=%d/%d a=%d/%d name=%s/%s\n",
                   i, it->prim, eprim, it->x, it->y, ex, ey, it->w, it->h, ew,
                   eh, mode_name(it->mode), mode_name(emode), it->z, ez,
                   it->alpha, ea, it->name, ename != NULL ? ename : "?");
            bad++;
        }
    }
    if (bad == 0) {
        printf("[geom] exact match: %d/%d sprites (0 px position error)\n", n,
               exp->nitem);
    }
    return bad;
}

static void dump_geom(Gfx *g, const char *path)
{
    FILE *fp = fopen(path, "w");
    int i, n = gfx_draw_count(g);

    if (fp == NULL) {
        fprintf(stderr, "test_gfx: cannot write %s\n", path);
        return;
    }
    fprintf(fp, "{\n \"frame\": \"gfx\",\n \"draw\": [\n");
    for (i = 0; i < n; i++) {
        const GfxDrawItem *it = gfx_draw_item(g, i);
        fprintf(fp,
                "  {\"prim\": %d, \"name\": \"%s\", \"x\": %d, \"y\": %d, "
                "\"w\": %d, \"h\": %d, \"mode\": \"%s\", \"z\": %d, "
                "\"alpha\": %d}%s\n",
                it->prim, it->name, it->x, it->y, it->w, it->h,
                mode_name(it->mode), it->z, it->alpha,
                (i + 1 < n) ? "," : "");
    }
    fprintf(fp, " ]\n}\n");
    fclose(fp);
}

static double now_ms(void)
{
    return (double)clock() * 1000.0 / (double)CLOCKS_PER_SEC;
}

static void usage(void)
{
    printf("usage: test_gfx <gfx-scene.json> [-o outdir] [-r repeat] "
           "[--no-png]\n");
}

int main(int argc, char **argv)
{
    const char *scene = NULL;
    const char *outdir = "out/c";
    int repeat = 3;
    int no_png = 0;
    JVal *root;
    Gfx *g;
    GfxHost host;
    GfxArchiveHost *ah;
    GfxStats cold;
    const char *game = NULL;
    int i;
    int canvas_w = 1920, canvas_h = 1080;
    double t_load0, t_load1, t_cold, t_warm = 0.0;
    int bad;
    FILE *log;
    char logpath[1024];
    char pngpath[1024];
    char geomp[1024];
    char dir[1024];

    for (i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-o") == 0 && i + 1 < argc) {
            outdir = argv[++i];
        } else if (strcmp(argv[i], "-r") == 0 && i + 1 < argc) {
            repeat = atoi(argv[++i]);
        } else if (strcmp(argv[i], "--no-png") == 0) {
            no_png = 1;
        } else if (strcmp(argv[i], "--game") == 0 && i + 1 < argc) {
            game = argv[++i];
        } else if (argv[i][0] == '-') {
            usage();
            return 1;
        } else {
            scene = argv[i];
        }
    }
    if (scene == NULL) {
        usage();
        return 1;
    }
    if (repeat < 1) {
        repeat = 1;
    }
    /* scene directory: everything (fonts) resolves relative to it */
    {
        const char *slash = strrchr(scene, '/');
        const char *bslash = strrchr(scene, '\\');
        const char *cut = slash;
        if (bslash != NULL && (cut == NULL || bslash > cut)) {
            cut = bslash;
        }
        if (cut != NULL) {
            size_t n = (size_t)(cut - scene);
            if (n >= sizeof dir) {
                n = sizeof dir - 1;
            }
            memcpy(dir, scene, n);
            dir[n] = '\0';
        } else {
            strcpy(dir, ".");
        }
    }
    t_load0 = now_ms();
    root = jload(scene);
    if (root == NULL) {
        return 1;
    }
    {
        JVal *cv = jget(root, "canvas");
        if (cv != NULL && cv->type == J_ARR && cv->nitem >= 2) {
            canvas_w = jint(cv->items[0], 1920);
            canvas_h = jint(cv->items[1], 1080);
        }
    }
    if (game == NULL) {
        game = jstr(jget(root, "game"));
    }
    if (game == NULL) {
        game = "C:/Users/Mechrevo/Downloads/\xe4\xba\x94\xe5\xbd\xa9\xe6\x96\x91\xe6\x96\x93\xe7\x9a\x84\xe4\xb8\x96\xe7\x95\x8cHD";
    }
    ah = gfx_archive_host_create(game);
    if (ah == NULL) {
        fprintf(stderr,
                "test_gfx: cannot open the graph archives under '%s'\n"
                "          pass --game <game dir> if this copy moved\n",
                game);
        jv_free(root);
        return 1;
    }
    printf("[load] archive host: %d named images\n",
           gfx_archive_host_image_count(ah));
    host = *gfx_archive_host_iface(ah);
    g = gfx_create(canvas_w, canvas_h, &host);
    /* 这个 harness 是 verify_gfx 的“冻结像素基线”一侧：显式用旧路径
     * （合成对话框 + 每层按版面表画）。引擎默认已经改成图元驱动（原版做法），
     * 但基线是旧路径冻结的，所以这里必须钉住 old path，不能跟着默认走。 */
    gfx_set_text_fixed_layout(g, 1);
    if (g == NULL) {
        fprintf(stderr, "test_gfx: gfx_create failed\n");
        return 1;
    }
    {
        JVal *bg = jget(root, "background");
        if (bg != NULL && bg->type == J_ARR && bg->nitem >= 4) {
            gfx_set_background(g, jint(bg->items[0], 0), jint(bg->items[1], 0),
                               jint(bg->items[2], 0), jint(bg->items[3], 255));
        }
    }
    load_fonts(g, root, dir);
    if (load_prims(g, root) != 0) {
        fprintf(stderr, "test_gfx: scene has no prims\n");
        return 1;
    }
    if (load_graphs(g, root) != 0) {
        fprintf(stderr, "test_gfx: not every graph slot could be loaded\n");
    }
    load_texts(g, root);
    t_load1 = now_ms();

    gfx_render(g);
    t_cold = now_ms();
    cold = *gfx_stats(g);
    for (i = 0; i < repeat; i++) {
        double a = now_ms();
        gfx_render(g);
        t_warm += now_ms() - a;
    }
    t_warm /= (double)repeat;

    printf("[time] scene load + %d image decodes : %8.2f ms\n",
           gfx_stats(g)->image_loads, t_load1 - t_load0);
    printf("[time] first render (cold, incl. slot composites): %8.2f ms\n",
           t_cold - t_load1);
    printf("[time] cold end-to-end (load+render)  : %8.2f ms\n",
           t_cold - t_load0);
    printf("[time] warm render (all cached), avg of %d: %8.2f ms\n", repeat,
           t_warm);
    printf("[stats] cold: sprites=%d texts=%d slot_composites=%d "
           "resize_cache_hits=%d px=%lld\n",
           cold.sprites, cold.texts, cold.compo_builds,
           cold.resize_cache_hits, cold.pixels);
    printf("[stats] images: loaded=%d cache_hits=%d\n", cold.image_loads,
           cold.cache_hits);
    printf("[stats] warm: sprites=%d texts=%d slot_composites=%d px=%lld\n",
           gfx_stats(g)->sprites, gfx_stats(g)->texts,
           gfx_stats(g)->compo_builds, gfx_stats(g)->pixels);

    bad = compare_draw(g, root);

    snprintf(pngpath, sizeof pngpath, "%s/gfx-frame.png", outdir);
    snprintf(geomp, sizeof geomp, "%s/gfx-geom.json", outdir);
    snprintf(logpath, sizeof logpath, "%s/gfx-test.log", outdir);
    if (!no_png) {
        const uint8_t *rgba;
        int w = 0, h = 0;
        double a = now_ms();
        rgba = gfx_canvas_rgba(g, &w, &h);
        if (write_png(pngpath, rgba, w, h) != 0) {
            fprintf(stderr, "test_gfx: failed to write %s\n", pngpath);
            bad++;
        } else {
            printf("[out] %s (%dx%d, %.1f MB, %.1f ms)\n", pngpath, w, h,
                   (double)((size_t)w * h * 4 + h) / 1e6, now_ms() - a);
        }
    }
    dump_geom(g, geomp);
    printf("[out] %s\n", geomp);
    log = fopen(logpath, "w");
    if (log != NULL) {
        fprintf(log, "scene=%s\n", scene);
        fprintf(log, "sprites=%d texts=%d\n", gfx_stats(g)->sprites,
                gfx_stats(g)->texts);
        fprintf(log, "load_ms=%.3f cold_render_ms=%.3f warm_render_ms=%.3f\n",
                t_load1 - t_load0, t_cold - t_load1, t_warm);
        fprintf(log, "image_loads=%d tex_cache_hits=%d compo_builds=%d "
                     "resize_cache_hits=%d pixels=%lld\n",
                gfx_stats(g)->image_loads, gfx_stats(g)->cache_hits,
                gfx_stats(g)->compo_builds, gfx_stats(g)->resize_cache_hits,
                gfx_stats(g)->pixels);
        fprintf(log, "geom_mismatches=%d\n", bad);
        fclose(log);
    }
    gfx_destroy(g);
    gfx_archive_host_destroy(ah);
    jv_free(root);
    printf("[result] %s\n", bad == 0 ? "PASS" : "FAIL");
    return bad == 0 ? 0 : 2;
}
