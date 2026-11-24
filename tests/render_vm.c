/* render_vm.c -- end-to-end test: script VM + C renderer.
 *
 * Runs the real HCB script through the C VM (src/vm.c + src/syscall.c) with
 * the verified render layer (src/gfx.c) attached through
 * fvp_vm_attach_gfx(), calls gfx_render() once per "frame" (a fixed number of
 * VM steps), exports the canvas at the checkpoint as PNG and dumps the draw
 * list, so the pixels can be compared with the Python reference frame
 * (fvp/out/render/story_<step>.png).
 *
 * This program is NOT part of bin/fvm.exe: fvm.exe never attaches a renderer
 * and keeps its byte-exact prototype behaviour (see docs/c-gfx.md section 8).
 *
 * Usage (see tests/verify_render_vm.ps1):
 *   render_vm.exe <script.hcb> [options]
 *     --game DIR        directory holding graph*.bin (default: script dir)
 *     --steps N         VM step budget (default 528000)
 *     --click N         synthetic click period (default 4000, same as
 *                       work/run_render.py's story frames)
 *     --enc NAME        cp932 (default) or gbk
 *     --frame-steps N   VM steps per rendered frame (default 1000)
 *     --out DIR         output directory (default out/c)
 *     --hold-text N     render.py --hold-text window in VM steps
 *                       (default 30000, 0 = disabled); this is a snapshot
 *                       convention applied at render time only, it never
 *                       changes VM state
 *     --no-png          skip the PNG exports
 *     --quiet           only print the final summary
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "encoding.h"
#include "fvp.h"
#include "gfx.h"
#include "hcb.h"
#include "vm.h"

#ifdef _WIN32
#include <windows.h>
#endif

/* ----------------------------------------------------------------------- */
/* paths / timing                                                          */
/* ----------------------------------------------------------------------- */

static double now_ms(void)
{
    return (double)clock() * 1000.0 / (double)CLOCKS_PER_SEC;
}

static char *xstrdup(const char *s)
{
    size_t n = strlen(s) + 1u;
    char *p = (char *)malloc(n);
    if (p != NULL) {
        memcpy(p, s, n);
    }
    return p;
}

static int path_join(char *dst, size_t cap, const char *dir, const char *name)
{
    int n;
    if (name != NULL && (name[0] == '/' || name[0] == '\\')) {
        n = snprintf(dst, cap, "%s", name);
    } else {
        n = snprintf(dst, cap, "%s/%s", dir, name);
    }
    return (n > 0 && (size_t)n < cap) ? 0 : -1;
}

/* The VM's hcb loader and gfx's font loader use fopen() (ANSI code page),
 * while gfx_archive_host_create() takes UTF-8.  argv on MinGW is ANSI. */
static int ansi_to_utf8(const char *in, char *out, size_t cap)
{
#ifdef _WIN32
    wchar_t w[2048];
    if (MultiByteToWideChar(CP_ACP, 0, in, -1, w, 2048) <= 0) {
        return -1;
    }
    if (WideCharToMultiByte(CP_UTF8, 0, w, -1, out, (int)cap, NULL, NULL) <= 0) {
        return -1;
    }
    return 0;
#else
    if (strlen(in) >= cap) {
        return -1;
    }
    strcpy(out, in);
    return 0;
#endif
}

static int dir_of(const char *path, char *out, size_t cap)
{
    const char *slash = strrchr(path, '/');
    const char *bslash = strrchr(path, '\\');
    const char *cut = slash;
    size_t n;
    if (bslash != NULL && (cut == NULL || bslash > cut)) {
        cut = bslash;
    }
    if (cut == NULL) {
        if (cap < 2u) {
            return -1;
        }
        strcpy(out, ".");
        return 0;
    }
    n = (size_t)(cut - path);
    if (n >= cap) {
        n = cap - 1u;
    }
    memcpy(out, path, n);
    out[n] = '\0';
    return 0;
}

/* ----------------------------------------------------------------------- */
/* PNG writer (stored deflate; the project has no zlib) -- same code as     */
/* tests/test_gfx.c, kept local so this test is self-contained.             */
/* ----------------------------------------------------------------------- */

typedef struct Buf {
    unsigned char *p;
    size_t n;
    size_t cap;
    int oom;
} Buf;

static void buf_reserve(Buf *b, size_t extra)
{
    size_t need;
    size_t cap;
    unsigned char *np;
    if (b->oom) {
        return;
    }
    need = b->n + extra;
    if (need <= b->cap) {
        return;
    }
    cap = b->cap ? b->cap : 4096u;
    while (cap < need) {
        cap *= 2u;
    }
    np = (unsigned char *)realloc(b->p, cap);
    if (np == NULL) {
        b->oom = 1;
        return;
    }
    b->p = np;
    b->cap = cap;
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
    unsigned char c = (unsigned char)(v & 0xFFu);
    buf_put(b, &c, 1);
}

static void buf_be32(Buf *b, unsigned long v)
{
    unsigned char c[4];
    c[0] = (unsigned char)((v >> 24) & 0xFFu);
    c[1] = (unsigned char)((v >> 16) & 0xFFu);
    c[2] = (unsigned char)((v >> 8) & 0xFFu);
    c[3] = (unsigned char)(v & 0xFFu);
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
            c = (c & 1u) ? (0xEDB88320UL ^ (c >> 1)) : (c >> 1);
        }
        crc_table[n] = c;
    }
    crc_ready = 1;
}

static unsigned long crc32_of(const unsigned char *p, size_t n)
{
    unsigned long c = 0xFFFFFFFFUL;
    size_t i;
    if (!crc_ready) {
        crc_init();
    }
    for (i = 0; i < n; i++) {
        c = crc_table[(c ^ p[i]) & 0xFFu] ^ (c >> 8);
    }
    return c ^ 0xFFFFFFFFUL;
}

static unsigned long adler32_of(const unsigned char *p, size_t n)
{
    unsigned long a = 1, b = 0;
    size_t i;
    for (i = 0; i < n; i++) {
        a = (a + p[i]) % 65521UL;
        b = (b + a) % 65521UL;
    }
    return (b << 16) | a;
}

static void png_chunk(Buf *out, const char *type, const unsigned char *data,
                      size_t n)
{
    unsigned long crc;
    unsigned char t[4];
    t[0] = (unsigned char)type[0];
    t[1] = (unsigned char)type[1];
    t[2] = (unsigned char)type[2];
    t[3] = (unsigned char)type[3];
    buf_be32(out, (unsigned long)n);
    buf_put(out, t, 4);
    if (n != 0) {
        buf_put(out, data, n);
    }
    /* CRC covers type + data */
    {
        Buf tmp;
        memset(&tmp, 0, sizeof tmp);
        buf_put(&tmp, t, 4);
        if (n != 0) {
            buf_put(&tmp, data, n);
        }
        crc = crc32_of(tmp.p, tmp.n);
        free(tmp.p);
    }
    buf_be32(out, crc);
}

static int write_png(const char *path, const unsigned char *rgba, int w, int h)
{
    Buf out;
    Buf raw;
    Buf z;
    unsigned char ihdr[13];
    size_t stride = (size_t)w * 4u;
    size_t y, off;
    int rc = 0;

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
        if (fwrite(out.p, 1, out.n, fp) != out.n) {
            rc = -1;
        }
        fclose(fp);
    }
    free(out.p);
    return rc;
}

/* ----------------------------------------------------------------------- */
/* draw-list JSON (same field names as tests/gfx_geom.json)                */
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

static void json_puts(FILE *f, const char *s)
{
    const unsigned char *p = (const unsigned char *)s;
    fputc('"', f);
    for (; *p != 0; p++) {
        if (*p == '"' || *p == '\\') {
            fputc('\\', f);
            fputc(*p, f);
        } else if (*p < 0x20u) {
            fprintf(f, "\\u%04x", (unsigned)*p);
        } else {
            fputc(*p, f);
        }
    }
    fputc('"', f);
}

static int write_geom(Gfx *g, const char *path, uint64_t step)
{
    FILE *f = fopen(path, "w");
    int i, n = gfx_draw_count(g);
    if (f == NULL) {
        return -1;
    }
    fprintf(f, "{\n \"frame\": \"vm\",\n \"step\": %llu,\n \"draw\": [\n",
            (unsigned long long)step);
    for (i = 0; i < n; i++) {
        const GfxDrawItem *it = gfx_draw_item(g, i);
        fprintf(f,
                "  {\"prim\": %d, \"name\": ", it->prim);
        json_puts(f, it->name);
        fprintf(f,
                ", \"x\": %d, \"y\": %d, \"w\": %d, \"h\": %d, "
                "\"mode\": \"%s\", \"z\": %d, \"alpha\": %d}%s\n",
                it->x, it->y, it->w, it->h, mode_name(it->mode), it->z,
                it->alpha, (i + 1 < n) ? "," : "");
    }
    fprintf(f, " ]\n}\n");
    fclose(f);
    return 0;
}

/* ----------------------------------------------------------------------- */
/* text observer: reproduces render.py --hold-text at render time only     */
/* ----------------------------------------------------------------------- */

typedef struct TextShadow {
    FvpVM *vm;
    int hold_window;
    int layer_lines[GFX_TEXT_MAX];
    char *lines[GFX_TEXT_MAX][16];
    char *last_text[GFX_TEXT_MAX]; /* survives TextClear (render.py keeps it) */
    uint64_t last_step[GFX_TEXT_MAX];
} TextShadow;

static void shadow_clear_layer(TextShadow *s, int layer)
{
    int i;
    for (i = 0; i < s->layer_lines[layer]; i++) {
        free(s->lines[layer][i]);
        s->lines[layer][i] = NULL;
    }
    s->layer_lines[layer] = 0;
}

static const char *shadow_last_text(const TextShadow *s, int layer)
{
    return s->last_text[layer];
}

static void shadow_on_text(void *ud, int layer, const char *text, int clear)
{
    TextShadow *s = (TextShadow *)ud;
    if (layer < 0 || layer >= GFX_TEXT_MAX) {
        return;
    }
    if (clear) {
        shadow_clear_layer(s, layer);
        return;
    }
    if (text == NULL) {
        return;
    }
    if (s->layer_lines[layer] < 16) {
        s->lines[layer][s->layer_lines[layer]] = xstrdup(text);
        s->layer_lines[layer] += 1;
    }
    free(s->last_text[layer]);
    s->last_text[layer] = xstrdup(text);
    s->last_step[layer] = s->vm->steps;
}

/* Apply render.py's _visible_text() convention: an empty window whose last
 * real line is still recent shows that line.  Returns the layers touched. */
static int shadow_hold_apply(TextShadow *s, int *held)
{
    int i, n = 0;
    if (s->hold_window <= 0) {
        return 0;
    }
    for (i = 0; i < GFX_TEXT_MAX; i++) {
        const char *last = shadow_last_text(s, i);
        if (s->layer_lines[i] != 0 || last == NULL) {
            continue;
        }
        if (s->vm->steps - s->last_step[i] > (uint64_t)s->hold_window) {
            continue;
        }
        gfx_text_clear(s->vm->gfx_bridge->gfx, i);
        gfx_text_append(s->vm->gfx_bridge->gfx, i, last);
        held[n++] = i;
    }
    return n;
}

static void shadow_hold_restore(TextShadow *s, const int *held, int n)
{
    int i;
    for (i = 0; i < n; i++) {
        gfx_text_clear(s->vm->gfx_bridge->gfx, held[i]);
    }
}

static void shadow_free(TextShadow *s)
{
    int i;
    for (i = 0; i < GFX_TEXT_MAX; i++) {
        shadow_clear_layer(s, i);
        free(s->last_text[i]);
        s->last_text[i] = NULL;
    }
    s->hold_window = 0;
}

static int write_text_json(const TextShadow *s, const char *path, uint64_t step)
{
    FILE *f = fopen(path, "w");
    int i, j, first = 1;
    if (f == NULL) {
        return -1;
    }
    fprintf(f, "{\n \"step\": %llu,\n \"hold_text\": %d,\n \"layers\": [\n",
            (unsigned long long)step, s->hold_window);
    for (i = 0; i < GFX_TEXT_MAX; i++) {
        const char *last = shadow_last_text(s, i);
        int hold = 0;
        if (s->layer_lines[i] == 0 && last != NULL && s->hold_window > 0 &&
            s->vm->steps - s->last_step[i] <= (uint64_t)s->hold_window) {
            hold = 1;
        }
        if (s->layer_lines[i] == 0 && !hold) {
            continue;
        }
        if (!first) {
            fprintf(f, ",\n");
        }
        first = 0;
        fprintf(f, "  {\"layer\": %d, \"held\": %d, \"lines\": [", i, hold);
        if (hold) {
            json_puts(f, last != NULL ? last : "");
        } else {
            for (j = 0; j < s->layer_lines[i]; j++) {
                if (j != 0) {
                    fprintf(f, ", ");
                }
                json_puts(f, s->lines[i][j]);
            }
        }
        fprintf(f, "]}");
    }
    fprintf(f, "\n ]\n}\n");
    fclose(f);
    return 0;
}

/* ----------------------------------------------------------------------- */
/* fonts                                                                   */
/* ----------------------------------------------------------------------- */

static int load_fonts(Gfx *g, const char *dir, int quiet)
{
    int size, n = 0;
    for (size = 8; size <= 72; size++) {
        char name[64];
        char path[1024];
        GfxFont *f;
        snprintf(name, sizeof name, "gfx-font-%d.bin", size);
        if (path_join(path, sizeof path, dir, name) != 0) {
            continue;
        }
        f = gfx_font_load(path);
        if (f == NULL) {
            continue;
        }
        if (gfx_add_font(g, f) == 0) {
            n += 1;
            if (!quiet) {
                printf("[font] %s size=%d glyphs=%d\n", name,
                       gfx_font_pixel_size(f), gfx_font_glyph_count(f));
            }
            /* gfx borrows the atlas; keep it alive for the whole run */
        } else {
            gfx_font_free(f);
        }
    }
    return n;
}

/* ----------------------------------------------------------------------- */
/* main                                                                    */
/* ----------------------------------------------------------------------- */

static void usage(const char *prog)
{
    fprintf(stderr,
            "usage: %s <script.hcb> [--game DIR] [--steps N] [--click N]\n"
            "       [--enc cp932|gbk] [--frame-steps N] [--out DIR]\n"
            "       [--hold-text N] [--motion] [--motion-dt MS] [--typewriter]\n"
            "       [--typewriter-dt MS] [--no-png] [--quiet]\n",
            prog);
}

int main(int argc, char **argv)
{
    const char *script = NULL;
    const char *game_arg = NULL;
    const char *encname = "cp932";
    const char *outdir = "out/c";
    uint64_t steps = 528000;
    uint64_t frame_steps = 1000;
    int64_t click = 4000;
    int hold_window = 30000;
    int motion = 0;
    int motion_dt = 16;
    int typewriter = 0;   /* --typewriter: 逐字显示（确定性验证用） */
    int tw_dt = 16;       /* --typewriter-dt: 每帧推进的毫秒数 */
    int no_png = 0;
    int quiet = 0;
    int i;
    FvpEncoding enc;
    FvpHcb hcb;
    FvpVM vm;
    FvpVM bench;
    char err[256];
    char game_ansi[1024];
    char game_utf8[1024];
    char script_dir[1024];
    GfxArchiveHost *ah = NULL;
    GfxHost host;
    Gfx *g = NULL;
    TextShadow shadow;
    uint64_t target;
    int frames = 0;
    int held[GFX_TEXT_MAX];
    int nheld = 0;
    double t_all0, t_all1;
    double t_bench0, t_bench1;
    double t_setup0, t_setup1;
    double render_sum = 0.0;
    double cold_ms = 0.0;
    double hot_sum = 0.0;
    int hot_frames = 0;
    double max_render_ms = 0.0;
    int rc = 0;
    int font_count = 0;
    GfxStats st;
    char png_path[1024];
    char png_nohold_path[1024];
    char geom_path[1024];
    char text_path[1024];
    char perf_path[1024];

    for (i = 1; i < argc; i++) {
        const char *a = argv[i];
        if (strcmp(a, "--game") == 0 && i + 1 < argc) {
            game_arg = argv[++i];
        } else if (strcmp(a, "--steps") == 0 && i + 1 < argc) {
            steps = (uint64_t)strtoull(argv[++i], NULL, 10);
        } else if (strcmp(a, "--click") == 0 && i + 1 < argc) {
            click = (int64_t)strtoll(argv[++i], NULL, 10);
        } else if (strcmp(a, "--enc") == 0 && i + 1 < argc) {
            encname = argv[++i];
        } else if (strcmp(a, "--frame-steps") == 0 && i + 1 < argc) {
            frame_steps = (uint64_t)strtoull(argv[++i], NULL, 10);
        } else if (strcmp(a, "--out") == 0 && i + 1 < argc) {
            outdir = argv[++i];
        } else if (strcmp(a, "--hold-text") == 0 && i + 1 < argc) {
            hold_window = (int)strtol(argv[++i], NULL, 10);
        } else if (strcmp(a, "--no-png") == 0) {
            no_png = 1;
        } else if (strcmp(a, "--motion") == 0) {
            motion = 1;
        } else if (strcmp(a, "--typewriter") == 0) {
            typewriter = 1;
        } else if (strcmp(a, "--typewriter-dt") == 0 && i + 1 < argc) {
            tw_dt = (int)strtol(argv[++i], NULL, 10);
} else if (strcmp(a, "--motion-dt") == 0 && i + 1 < argc) {
            motion_dt = (int)strtol(argv[++i], NULL, 10);
        } else if (strcmp(a, "--quiet") == 0) {
            quiet = 1;
        } else if (strcmp(a, "--help") == 0 || strcmp(a, "-h") == 0) {
            usage(argv[0]);
            return 0;
        } else if (a[0] == '-') {
            usage(argv[0]);
            return 2;
        } else if (script == NULL) {
            script = a;
        } else {
            fprintf(stderr, "render_vm: unexpected argument %s\n", a);
            return 2;
        }
    }
    if (script == NULL) {
        usage(argv[0]);
        return 2;
    }
    if (frame_steps < 1) {
        frame_steps = 1;
    }
    if (click < 0) {
        click = 0;
    }
    if (!fvp_encoding_from_name(encname, &enc)) {
        fprintf(stderr, "render_vm: unknown encoding %s\n", encname);
        return 2;
    }

    /* --game defaults to the script's directory (the .hcb lives next to the
     * graph*.bin archives).  gfx wants UTF-8, the C runtime argv is ANSI. */
    if (game_arg == NULL) {
        if (dir_of(script, script_dir, sizeof script_dir) != 0) {
            strcpy(script_dir, ".");
        }
        game_arg = script_dir;
    }
    if (ansi_to_utf8(game_arg, game_utf8, sizeof game_utf8) != 0) {
        /* the C runtime may already hand us UTF-8 (UTF-8 ACP systems) */
        if (strlen(game_arg) >= sizeof game_utf8) {
            fprintf(stderr, "render_vm: game path too long\n");
            return 2;
        }
        strcpy(game_utf8, game_arg);
    }
    if (strlen(game_arg) >= sizeof game_ansi) {
        fprintf(stderr, "render_vm: game path too long\n");
        return 2;
    }
    strcpy(game_ansi, game_arg);

    if (!fvp_hcb_load(&hcb, script, enc, err, sizeof err)) {
        fprintf(stderr, "render_vm: %s\n", err);
        fvp_hcb_free(&hcb);
        return 3;
    }

    /* ---- baseline: VM only, no renderer -------------------------------- */
    memset(&bench, 0, sizeof bench);
    fvp_vm_init(&bench, &hcb, 1);
    bench.click_period = click;
    fvp_vm_start(&bench);
    t_bench0 = now_ms();
    (void)fvp_vm_run(&bench, steps);
    t_bench1 = now_ms();
    if (!quiet) {
        printf("[bench] VM only: %llu steps in %.2f ms (%.0f ksteps/s), "
               "text lines %llu\n",
               (unsigned long long)bench.steps, t_bench1 - t_bench0,
               bench.steps / (t_bench1 - t_bench0 + 1e-9),
               (unsigned long long)bench.ntext);
    }
    fvp_vm_free(&bench);

    /* ---- renderer setup ------------------------------------------------ */
    t_setup0 = now_ms();
    ah = gfx_archive_host_create(game_utf8);
    if (ah == NULL) {
        fprintf(stderr,
                "render_vm: cannot open the graph archives under '%s'\n",
                game_ansi);
        fvp_hcb_free(&hcb);
        return 1;
    }
    host = *gfx_archive_host_iface(ah);
    g = gfx_create(1920, 1080, &host);
    if (g == NULL) {
        fprintf(stderr, "render_vm: gfx_create failed\n");
        gfx_archive_host_destroy(ah);
        fvp_hcb_free(&hcb);
        return 1;
    }
    font_count = load_fonts(g, outdir, quiet);
    t_setup1 = now_ms();
    if (!quiet) {
        printf("[gfx] archive: %d named images, %d font atlas(es), "
               "setup %.1f ms\n",
               gfx_archive_host_image_count(ah), font_count,
               t_setup1 - t_setup0);
    }

    /* ---- VM + renderer ------------------------------------------------- */
    memset(&shadow, 0, sizeof shadow);
    shadow.hold_window = hold_window;
    memset(&vm, 0, sizeof vm);
    fvp_vm_init(&vm, &hcb, 1);
    vm.click_period = click;
    fvp_vm_attach_gfx(&vm, g);
    gfx_motion_set_enabled(g, motion);
    gfx_typewriter_set_enabled(g, typewriter);
    shadow.vm = &vm;
    if (vm.gfx_bridge != NULL) {
        vm.gfx_bridge->on_text = shadow_on_text;
        vm.gfx_bridge->text_ud = &shadow;
    }
    fvp_vm_start(&vm);

    target = steps;
    t_all0 = now_ms();
    while (vm.steps < target) {
        uint64_t before = vm.steps;
        uint64_t next = before + frame_steps;
        double r0, r1;
        if (next > target || next < before) {
            next = target;
        }
        (void)fvp_vm_run(&vm, next);
        if (vm.steps == before) {
            break; /* no runnable thread left */
        }
        gfx_motion_advance(g, motion_dt);
        gfx_text_advance(g, tw_dt);
        nheld = shadow_hold_apply(&shadow, held);
        r0 = now_ms();
        gfx_render(g);
        r1 = now_ms();
        if (frames == 0) {
            cold_ms = r1 - r0;
        } else {
            hot_sum += r1 - r0;
            hot_frames += 1;
        }
        if (r1 - r0 > max_render_ms) {
            max_render_ms = r1 - r0;
        }
        render_sum += r1 - r0;
        shadow_hold_restore(&shadow, held, nheld);
        nheld = 0;
        frames += 1;
    }
    t_all1 = now_ms();

    /* the canvas currently holds the last render (with the hold-text
     * snapshot convention applied, exactly like gfx_dump.py draws it) */
    if (!no_png) {
        const uint8_t *rgba;
        int w = 0, h = 0;
        rgba = gfx_canvas_rgba(g, &w, &h);
        if (path_join(png_path, sizeof png_path, outdir, "vm-frame.png") != 0 ||
            write_png(png_path, rgba, w, h) != 0) {
            fprintf(stderr, "render_vm: cannot write the frame PNG\n");
            rc = 4;
        }
    }

    st = *gfx_stats(g);
    {
        int draw = gfx_draw_count(g);
        if (path_join(geom_path, sizeof geom_path, outdir, "vm-geom.json") !=
                0 ||
            write_geom(g, geom_path, vm.steps) != 0) {
            fprintf(stderr, "render_vm: cannot write %s\n", geom_path);
        }
        if (path_join(text_path, sizeof text_path, outdir, "vm-text.json") !=
                0 ||
            write_text_json(&shadow, text_path, vm.steps) != 0) {
            fprintf(stderr, "render_vm: cannot write %s\n", text_path);
        }
        if (!quiet) {
            printf("[frame] step=%llu sprites=%d texts=%d image_loads=%d "
                   "compo_builds=%d pixels=%lld\n",
                   (unsigned long long)vm.steps, draw, st.texts,
                   st.image_loads, st.compo_builds, st.pixels);
        }
    }

    /* no-hold variant, for an honest "what does hold-text add" number */
    if (!no_png) {
        const uint8_t *rgba;
        int w = 0, h = 0;
        gfx_render(g);
        rgba = gfx_canvas_rgba(g, &w, &h);
        if (path_join(png_nohold_path, sizeof png_nohold_path, outdir,
                      "vm-frame-nohold.png") != 0 ||
            write_png(png_nohold_path, rgba, w, h) != 0) {
            fprintf(stderr, "render_vm: cannot write the no-hold PNG\n");
            rc = 4;
        }
    }

    if (path_join(perf_path, sizeof perf_path, outdir,
                  "vm-render-perf.json") == 0) {
        FILE *f = fopen(perf_path, "w");
        if (f != NULL) {
            char script_utf8[2048];
            double total = t_all1 - t_all0;
            double hot_avg = hot_frames > 0 ? hot_sum / (double)hot_frames
                                            : 0.0;
            if (ansi_to_utf8(script, script_utf8, sizeof script_utf8) != 0) {
                snprintf(script_utf8, sizeof script_utf8, "%s", script);
            }
            fprintf(f,
                    "{\n"
                    " \"script\": ");
            json_puts(f, script_utf8);
            fprintf(f,
                    ",\n"
                    " \"steps\": %llu,\n"
                    " \"click\": %lld,\n"
                    " \"frame_steps\": %llu,\n"
                    " \"frames\": %d,\n"
                    " \"hold_text\": %d,\n"
                    " \"vm_only_ms\": %.3f,\n"
                    " \"vm_plus_render_ms\": %.3f,\n"
                    " \"avg_frame_ms\": %.3f,\n"
                    " \"render_total_ms\": %.3f,\n"
                    " \"render_avg_ms\": %.3f,\n"
                    " \"render_cold_ms\": %.3f,\n"
                    " \"render_hot_avg_ms\": %.3f,\n"
                    " \"render_max_ms\": %.3f,\n"
                    " \"font_atlases\": %d,\n"
                    " \"gfx_setup_ms\": %.3f,\n"
                    " \"draw_items\": %d,\n"
                    " \"sprites\": %d,\n"
                    " \"texts\": %d,\n"
                    " \"image_loads\": %d\n"
                    "}\n",
                    (unsigned long long)vm.steps, (long long)click,
                    (unsigned long long)frame_steps, frames, hold_window,
                    t_bench1 - t_bench0, total,
                    frames > 0 ? total / (double)frames : 0.0, render_sum,
                    frames > 0 ? render_sum / (double)frames : 0.0, cold_ms,
                    hot_avg, max_render_ms, font_count, t_setup1 - t_setup0,
                    gfx_draw_count(g), st.sprites, st.texts,
                    st.image_loads);
            fclose(f);
        }
    }

    if (!quiet) {
        double total = t_all1 - t_all0;
        double hot_avg = hot_frames > 0 ? hot_sum / (double)hot_frames : 0.0;
        printf("\n=== render_vm summary ===\n");
        printf("steps           %llu (target %llu)\n",
               (unsigned long long)vm.steps, (unsigned long long)target);
        printf("frames          %d (one gfx_render per %llu VM steps)\n",
               frames, (unsigned long long)frame_steps);
        printf("vm only         %.2f ms\n", t_bench1 - t_bench0);
        printf("vm + render     %.2f ms  (%.2f ms per frame)\n", total,
               frames > 0 ? total / (double)frames : 0.0);
        printf("  gfx_render    total %.2f ms, avg %.3f ms/frame "
               "(cold %.2f, hot avg %.3f, max %.2f)\n",
               render_sum, frames > 0 ? render_sum / (double)frames : 0.0,
               cold_ms, hot_avg, max_render_ms);
        printf("draw list       %d sprites, %d text windows\n",
               gfx_draw_count(g), st.texts);
        printf("artifacts       %s, %s, %s, %s\n",
               no_png ? "(no png)" : png_path, geom_path, text_path,
               perf_path);
    }

    fvp_vm_free(&vm);
    shadow_free(&shadow);
    gfx_destroy(g);
    gfx_archive_host_destroy(ah);
    fvp_hcb_free(&hcb);
    return rc;
}
