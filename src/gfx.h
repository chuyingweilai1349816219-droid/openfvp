/* FVP engine -- gfx: primitive table, layer compositing, text layout.
 *
 * A faithful C port of the proven Python prototype
 * (fvp/work/render.py, spec in fvp/docs/render-model.md).  The frame model:
 *
 *   * a 1920x1080 RGBA8888 back buffer,
 *   * a table of ~4096 primitives with the engine's field layout
 *     (type / draw / alpha / blend / z / xy / wh / uv / angle / scale /
 *      graph slot / text window),
 *   * graph slots that hold a decoded hzc1 image (plus optional parts
 *     overlays composited into the slot, like the engine does),
 *   * text windows 0..31 (0 = dialogue body, 8 = speaker name,
 *     9 = chapter/location title) rendered with a bitmap font.
 *
 * Drawing order is NOT z sorted: the group tree is walked depth first
 * (children in insertion order, hidden subtrees skipped) and sprites that
 * never entered a group are appended afterwards in ascending id order --
 * exactly what the engine's 0x438ab0 type-1 branch does.
 *
 * Canvas pixel layout: bytes R,G,B,A per pixel, top down, no padding
 * (stride = width * 4).  gfx_canvas() exposes the same memory as uint32_t,
 * which on a little-endian host is 0xAABBGGRR.  For Win32 StretchDIBits use
 * BI_BITFIELDS with masks R=0x000000FF, G=0x0000FF00, B=0x00FF0000,
 * A=0xFF000000.
 *
 * The module owns no VM and does no I/O except through GfxHost (image
 * loading) and gfx_font_load() (bitmap font atlas).  src/gfx.c also ships a
 * ready made host that reads the game's graph*.bin through src/bin.c +
 * src/hzc1.c: see gfx_archive_host_create().
 */
#ifndef FVP_GFX_H
#define FVP_GFX_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define GFX_PRIM_MAX 4096   /* prim ids 0..4095 (engine: 1..0xFFF)        */
#define GFX_GRAPH_MAX 4096  /* graph slots (engine: 0..0xFFF)             */
#define GFX_PARTS_MAX 64    /* parts ids 0..0x3F                          */
#define GFX_TEXT_MAX 32     /* text windows 0..31                         */
#define GFX_MAX_W 4096
#define GFX_MAX_H 4096
#define GFX_MAX_PRIM_CHILDREN 256
#define GFX_MAX_OVERLAYS 8
#define GFX_NAME_MAX 192

typedef struct Gfx Gfx;

/* ---- images and the host callback table ------------------------------ */

/* One decoded hzc1 image.  rgba is frames stacked vertically, each frame
 * width*height pixels of 4 bytes in R,G,B,A order (top down).  x/y are the
 * image header anchor (hzc1 +0x18/+0x1A) that the engine adds to the prim
 * offset -- see render-model.md 2.1.  The image stays owned by the host:
 * gfx calls host->free_image() when the last slot referencing it is
 * released (graph reload, or gfx_destroy). */
typedef struct GfxImage {
    int w;
    int h;
    int x;
    int y;
    int frames;      /* >= 1                                            */
    uint8_t *rgba;   /* w*h*4*frames bytes                              */
} GfxImage;

typedef struct GfxHost {
    void *ud;
    /* Load one resource by its script name (UTF-8, e.g. "graph/BG043_000"
     * or "graph_bs/CHR_..."; no extension).  Return 0 on success and fill
     * *out.  Return non-zero on failure (gfx records and skips the slot). */
    int (*load_image)(void *ud, const char *name, GfxImage *out);
    /* Release an image previously returned by load_image. */
    void (*free_image)(void *ud, GfxImage *img);
} GfxHost;

/* ---- primitive fields ------------------------------------------------ */

typedef enum GfxPrimField {
    GFX_F_TYPE = 0, /* 0=none 1=group 2=tile 4=sprite 5=text            */
    GFX_F_DRAW,     /* 0 = hide this prim and its whole subtree          */
    GFX_F_ALPHA,    /* 0..255                                            */
    GFX_F_BLEND,    /* 0 = normal, 1 = additive approximation            */
    GFX_F_Z,        /* 100..10000, recorded but not used for sorting     */
    GFX_F_X,
    GFX_F_Y,
    GFX_F_W,        /* 0 = use the image's own width                     */
    GFX_F_H,        /* 0 = use the image's own height                    */
    GFX_F_U,        /* texture offset, not used by the reference renderer*/
    GFX_F_V,
    GFX_F_OP1,
    GFX_F_OP2,
    GFX_F_ANGLE,    /* 0.1 degree units, %3600                           */
    GFX_F_SCALE_X,  /* *1000 (1000 = 1.0)                                */
    GFX_F_SCALE_Y,
    GFX_F_GRAPH,    /* graph slot (type 4), -1 = none                    */
    GFX_F_TW,       /* text window (type 5), -1 = none                   */
    GFX_F_TILE,
    GFX_F_PARENT,   /* parent group id, -1 = none (same as group_in)      */
    GFX_F_FLAGS,
    GFX_F_COUNT
} GfxPrimField;

typedef enum GfxDrawMode {
    GFX_MODE_UI = 0,   /* top-left anchored sprite / UI patch          */
    GFX_MODE_CHAR = 1, /* standing portrait: bottom-centre anchor      */
    GFX_MODE_BG = 2,   /* oversized scenery: centred, 1:1              */
    GFX_MODE_TEXT = 3  /* text primitive (no texture)                  */
} GfxDrawMode;

/* One entry of the draw list produced by gfx_render().  test_gfx dumps this
 * and compares it field by field with the Python reference draw list, which
 * is how the "0 pixel position error" is proven. */
typedef struct GfxDrawItem {
    int prim;
    char name[GFX_NAME_MAX];
    int x, y, w, h;  /* final destination rectangle                    */
    int mode;        /* GfxDrawMode                                    */
    int z;
    int alpha;
} GfxDrawItem;

typedef struct GfxStats {
    int sprites;      /* sprites composited in the last gfx_render()     */
    int texts;        /* text windows drawn in the last gfx_render()     */
    int image_loads;  /* host->load_image calls since gfx_create()       */
    int cache_hits;   /* graph slots that reused a decoded image         */
    int compo_builds; /* graph slot composites rebuilt (base+overlays)   */
    int resize_cache_hits; /* per-prim transformed images reused         */
    long long pixels; /* destination pixels touched by the last render   */
} GfxStats;

/* ---- create / destroy / canvas --------------------------------------- */

/* host may be NULL (no textures can be loaded; text and prim bookkeeping
 * still work).  The GfxHost contents are copied; host->ud must outlive gf. */
Gfx *gfx_create(int w, int h, const GfxHost *host);
void gfx_destroy(Gfx *g);

int gfx_width(const Gfx *g);
int gfx_height(const Gfx *g);

/* Fill the canvas (gfx_render() does this first with the background colour,
 * default opaque black, same as the Python reference). */
void gfx_set_background(Gfx *g, int r, int gr, int b, int a);

void gfx_render(Gfx *g);

/* The composed canvas, valid until the next gfx_render()/gfx_destroy().
 * Do not free.  Returns NULL for w == NULL misuse.  Either w/h may be NULL. */
const uint32_t *gfx_canvas(Gfx *g, int *w, int *h);
const uint8_t *gfx_canvas_rgba(Gfx *g, int *w, int *h);

/* ---- primitive table ------------------------------------------------- */

/* Reset one prim to the engine's initial state (PrimSetNull): type 0,
 * draw 1, alpha 255, blend 0, z 1000, xy 0, wh/uv 0, angle 0, scale 1000,
 * graph/tw -1.  Group links are preserved, like the engine does. */
int gfx_prim_reset(Gfx *g, int id);

/* Generic field setter (the task's "gfx_set_prim"): one call per field.
 * Values are clamped/wrapped exactly like the engine handlers:
 *   ALPHA 0..255, Z 100..10000, SCALE 100..10000, ANGLE %3600,
 *   X/Y are 16-bit signed, W/H/U/V/TILE are 16-bit, TYPE is 0..255.
 * Returns 0 on success, -1 for an id/field out of range. */
int gfx_set_prim(Gfx *g, int id, int field, int v);

/* 读回一个图元字段（主要给测试和动画断言用）。返回 0 成功、-1 越界。 */
int gfx_get_prim(const Gfx *g, int id, int field, int *out);

/* ---- 动画补间（Motion* 的 gfx 侧）------------------------------------
 *
 * 语义与 src/motion.c 一致：只做时间轴 + 线性插值，mode（1..5）原样保存但
 * 不影响输出（缓动曲线未确证）。默认**不启用**：只有调用
 * gfx_motion_set_enabled(g, 1) 之后，gfx_motion_advance() 才会真正改图元字段，
 * 这样既有的像素级验收（参考帧）在动画关闭时逐字节不变。
 */
int gfx_motion_add(Gfx *g, int prim_id, int kind, int from_x, int from_y,
                   int to_x, int to_y, int dur_ms, int mode);
void gfx_motion_set_enabled(Gfx *g, int on);
int gfx_motion_enabled(const Gfx *g);
/* 推进 dt_ms；启用时把每条活跃补间的当前值写回图元字段。 */
void gfx_motion_advance(Gfx *g, int dt_ms);
int gfx_motion_active(const Gfx *g);
void gfx_motion_clear(Gfx *g);

/* Attachment: parent becomes a group (type 1), child is moved to the end of
 * the parent's child list (PrimGroupIn / PrimGroupOut). */
int gfx_prim_group_in(Gfx *g, int child, int parent);
int gfx_prim_group_out(Gfx *g, int child);

/* Per-prim clip rectangle in canvas space (x,y,w,h); w<=0 disables it.
 * This is an extension: the Python reference only clips to the canvas. */
int gfx_prim_set_clip(Gfx *g, int id, int x, int y, int w, int h);

/* ---- graph slots / parts --------------------------------------------- */

/* Load a name into a graph slot (GraphLoad).  name == NULL releases the
 * slot (like the engine's type-0 GraphLoad).  On a load failure the slot is
 * left untouched and -1 is returned (matches the Python prototype). */
int gfx_graph_load(Gfx *g, int slot, const char *name);
void gfx_graph_release(Gfx *g, int slot);
/* Multi-frame image: choose the base frame (MotionAnim). */
int gfx_graph_set_frame(Gfx *g, int slot, int frame);
/* Append a patches overlay (parts frame) to a slot at image-space (x,y).
 * An identical (name, frame) entry is moved to the end instead of being
 * duplicated, mirroring render.py's Graph.overlays bookkeeping. */
int gfx_graph_add_overlay(Gfx *g, int slot, const char *name, int x, int y,
                          int frame);

/* parts (expression patches): load a multi-frame image, assign it to a
 * graph slot and select a frame; the selected frame is composited into the
 * slot's image at the parts header x/y, exactly like PartsSelect does. */
int gfx_parts_load(Gfx *g, int parts_id, const char *name);
int gfx_parts_assign(Gfx *g, int parts_id, int slot);
int gfx_parts_select(Gfx *g, int parts_id, int frame);
void gfx_parts_release(Gfx *g, int parts_id);

/* ---- text windows ---------------------------------------------------- */

/* Append one line to a text window (TextPrint).  "[reading|body]" ruby
 * markup is reduced to "body" (see docs/02-text-format.md).  The string is
 * UTF-8; layer 0 = body, 8 = name, 9 = title. */
void gfx_draw_text(Gfx *g, int layer, const char *utf8); /* alias: append */
void gfx_text_append(Gfx *g, int layer, const char *utf8);
void gfx_text_clear(Gfx *g, int layer);
void gfx_text_set_color(Gfx *g, int layer, int r, int gr, int b);
void gfx_text_set_size(Gfx *g, int layer, int w, int h);
void gfx_text_set_pos(Gfx *g, int layer, int x, int y);
void gfx_text_set_out_size(Gfx *g, int layer, int w, int h);

/* Override the built-in layout table for one layer (defaults are the ones
 * proven in the Python renderer; see gfx.c gfx_text_layout_default). */
typedef struct GfxTextLayout {
    int x, y;      /* first line origin (layer 9 ignores x: it centres) */
    int max_w;     /* wrap width                                       */
    int size;      /* font pixel size                                  */
    int max_lines; /* lines kept                                       */
} GfxTextLayout;
int gfx_text_set_layout(Gfx *g, int layer, const GfxTextLayout *lay);
const GfxTextLayout *gfx_text_get_layout(Gfx *g, int layer);

/* ---- bitmap fonts ---------------------------------------------------- */

/* GFXF atlas written by fvp/tests/gfx_dump.py (see docs/c-gfx.md).  A
 * built-in 8x16 ASCII font is always available as a fallback. */
typedef struct GfxFont GfxFont;
GfxFont *gfx_font_load(const char *path);
void gfx_font_free(GfxFont *f);
int gfx_font_pixel_size(const GfxFont *f);
int gfx_font_glyph_count(const GfxFont *f);

/* Register a font with a Gfx (borrowed, not owned).  Drawing picks the
 * font whose pixel size equals the layer's layout size. */
int gfx_add_font(Gfx *g, GfxFont *f);
void gfx_clear_fonts(Gfx *g);

/* ---- group transforms (extension over the Python prototype) ---------- */

/* When enabled (default), a group prim's x/y/scale/alpha is accumulated
 * down the tree: children are translated by (x,y), scaled, and their alpha
 * multiplied.  The Python prototype derives group order from the tree but
 * ignores group transforms; every ancestor of a drawn primitive in the
 * verification frame is identity, so both agree there. */
void gfx_set_group_transform(Gfx *g, int enabled);

/* ---- diagnostics ----------------------------------------------------- */

const GfxStats *gfx_stats(const Gfx *g);
int gfx_draw_count(const Gfx *g);
const GfxDrawItem *gfx_draw_item(const Gfx *g, int index);
const char *gfx_last_error(const Gfx *g);

/* ---- convenience host: graph*.bin via bin.c + hzc1.c ----------------- */

typedef struct GfxArchiveHost GfxArchiveHost;
/* game_dir is the directory holding graph.bin / graph_bg.bin / ... .
 * Returns NULL if none of the packages could be opened. */
GfxArchiveHost *gfx_archive_host_create(const char *game_dir);
GfxHost *gfx_archive_host_iface(GfxArchiveHost *h);
void gfx_archive_host_destroy(GfxArchiveHost *h);
int gfx_archive_host_image_count(const GfxArchiveHost *h);

#ifdef __cplusplus
}
#endif

#endif /* FVP_GFX_H */