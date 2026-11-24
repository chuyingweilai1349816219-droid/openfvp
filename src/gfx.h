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
    int fallback_glyphs; /* 当前图集缺字、从别的图集缩放取过来的字形数 */
    int tofu_glyphs;     /* 所有图集都没有这个字、只能画豆腐块的次数   */
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

/* 诊断：文理缓存占用（字节）与引擎中的图像条数。 */
size_t gfx_tex_bytes(const Gfx *g);
int gfx_tex_count(const Gfx *g);

/* ---- 逐字显示（打字机效果）-------------------------------------------
 * 默认关闭：关闭时 TextPrint 的整行文本一次全部可见，已验收的帧一个像素都不变。
 * 打开后：TextPrint 的新行从 0 个字符开始，调用方每帧 gfx_text_advance(dt_ms)
 * 按 TextSpeed(129) 的“每字毫秒”推进；gfx_render() 只画前 shown_chars 个字符。
 * speed_ms == 0 表示不等待（一次显示完）。 */
void gfx_typewriter_set_enabled(Gfx *g, int on);
int gfx_typewriter_enabled(const Gfx *g);
void gfx_text_set_speed(Gfx *g, int layer, int ms);
void gfx_text_advance(Gfx *g, int dt_ms);
/* 以下三个是给外壳/测试看的诊断值。 */
int gfx_text_shown_chars(const Gfx *g, int layer);
int gfx_text_total_chars(const Gfx *g, int layer);
int gfx_text_busy(const Gfx *g, int layer); /* 1 = 还在逐字显示 */

/* TextShadowDist(125)：**阴影距离 0..12**（静态分析 handler 0x43CBB0 → 0x4248B0
 * 把一字节写进 tw+0x597）。TextSpace(128)：**字间距/行间距**，两个分量各 -32..32
 * （静态分析 handler 0x43CE10 写 tw+0x5A8/+0x5AA）。
 *
 * ⚠️ 2026-11-24 修正：以前把 0x43CE10 当成 TextShadowDist 并按"两个分量偏移"去
 * 渲染，结果把脚本的阴影距离 12 画成了向右 12 px 的实心重影（用户报「残影」）。
 * 现在两条命令都**只记录数值、不参与渲染** —— 原版阴影的形状/颜色/透明度与间距
 * 的真实用法都没有静态分析证据，参考实现 work/render.py 也只存不画。
 * 详见 docs/render-model.md §7.1。 */
void gfx_text_set_shadow(Gfx *g, int layer, int dist);
void gfx_text_get_shadow(const Gfx *g, int layer, int *dist);
void gfx_text_set_space(Gfx *g, int layer, int x, int y);
/* TextFont(113)：两个字体号（静态分析 0x43DA70 → tw+0x590 u16 / tw+0x593 u8）。
 * 只记录、不参与选字体：引擎字体号 -1（正文）/ -2（章节标题）对应哪套字模
 * 没有证据，渲染仍按字号挑图集。 */
void gfx_text_set_font(Gfx *g, int layer, int id, int id2);
/* TextFormat(118)：六个排版参数（静态分析 0x43CC10），只记录。 */
void gfx_text_set_format(Gfx *g, int layer, int a, int b, int c, int d,
                         int e, int f);
/* TextFunction(119)：三个开关（静态分析 0x43CCF0），只记录。 */
void gfx_text_set_function(Gfx *g, int layer, int a, int b, int c);

/* 纹理缓存上限（字节）。默认 512 MiB，见 gfx.c 的 GFX_TEX_CACHE_DEFAULT_BYTES。
 * 超上限时按 LRU 淘汰 refs == 1 的条目（被图元/组/部件引用的永远不会被淘汰，
 * 需要时会重新解码），因此像素输出不变。cap 传 0 = 不限制。
 * gfx_tex_set_cap_bytes() 会立即做一次淘汰，可用于测试与小内存设备。 */
size_t gfx_tex_cap_bytes(const Gfx *g);
void gfx_tex_set_cap_bytes(Gfx *g, size_t cap);
unsigned long gfx_tex_evictions(const Gfx *g); /* 因上限被淘汰的图像数 */
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
/* Motion*Stop：撤掉某个图元的某一类补间（kind < 0 = 全部）。返回撤掉的条数。
 * 实现在 src/motion.c 的 fvp_motion_cancel()。 */
int gfx_motion_cancel(Gfx *g, int prim_id, int kind);

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

/* PrimHit(76)：鼠标命中测试。语义逐条来自静态分析（docs/render-model.md §8）：
 *   handler 0x43B9C0 → 0x43B8D0（沿 parent 链累加 x/y、任一层 draw==0 即未命中、
 *   链必须走到 prim 0）→ 0x43B5F0（按类型判定矩形）。
 * type 4 sprite：矩形 = 图头 x/y + prim x/y + 累加偏移，写过多 WH 时取 min(prim, 图)；
 * type 2 tile：矩形 = prim x/y + 累加偏移，且 alpha 必须非 0；
 * type 1 group：递归任一子图元命中即命中；type 0/3/5：恒不命中。
 * ignore_draw = 原版第二参数非空（跳过起始图元的 draw==0 早退）。
 * 返回 1 = 命中。
 *
 * ⚠️ 已知未实现：矩形+UV 判定之后原版还要查"可点像素掩码"（0x43B750 起的循环
 * 与 0x4259B0），那一段没解出来，所以现在**矩形内即算命中**（被掩码挖空的像素
 * 也会被算作命中）。这是本函数唯一的近似，别处都照抄原版。 */
int gfx_prim_hit(Gfx *g, int prim_id, int ignore_draw, int cx, int cy);

/* ---- graph slots / parts --------------------------------------------- */

/* Load a name into a graph slot (GraphLoad).  name == NULL releases the
 * slot (like the engine's type-0 GraphLoad).  On a load failure the slot is
 * left untouched and -1 is returned (matches the Python prototype). */
int gfx_graph_load(Gfx *g, int slot, const char *name);

/* ---- 外字（GaijiLoad 21）---------------------------------------------
 * 原版 GaijiLoad(字符, 字号, 图名) 的语义是：把这个字符在绘制时换成图名对应的
 * 那张图（实测参数形如 `('ハ', 64, "graph/gaiji_heart68")`）。
 * 本站的做法：syscall 层把图装进一个图形槽，再调 gfx_gaiji_add() 记下
 * 「码点 → 槽 + 原始字号」；文本绘制时命中码点就用槽里的图，按**当前字号**
 * 等比缩放画上去（原始字号只用于记录/诊断）。
 * 返回 1 = 记录成功；码点 0、槽号越界或表满返回 0。 */
#define GFX_MAX_GAIJI 64
int gfx_gaiji_add(Gfx *g, uint32_t codepoint, int size, int slot);
int gfx_gaiji_count(const Gfx *g);
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