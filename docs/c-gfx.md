# C 渲染层 `gfx`（src/gfx.c / src/gfx.h）

> 以下内容由 DeepSeek 生成,不一定代表作者个人意愿

把已经逐像素验证过的 Python 渲染原型（`work/render.py`，规格见
`docs/render-model.md`）移植成 C 引擎的渲染模块，并用**像素级比对**证明移植正确。

一句话结论（2026-11-23，checkpoint 528000 与 534000）：

| 指标 | 结果 |
|---|---|
| 图元位置误差 | **0 px**（24/24、35/35 个 sprite 的 x/y/w/h/mode/z/alpha 与参考逐字段相同） |
| 像素差异（RGB 任一通道） | **289 / 2,073,600 = 0.0139%**（534000 帧：292） |
| PSNR（RGB） | **49.95 dB** |
| SSIM（luma） | **0.99971** |
| 热帧合成（全缓存命中） | **≈10.4 ms**（1920×1080） |
| 冷帧（含 80 张图解码 + 全部合成） | **≈400 ms**，其中解码 ≈315 ms、首帧合成 ≈87 ms |

差异的 289 个像素里：109 个在文字包围盒内（PIL 抗锯齿舍入 ±1），184 个是对话框
圆角/描边的栅格化差异（PIL 用 `rectangle+pieslice/arc`，本实现用圆角距离判定）。
文字之外的 sprite 区域只有 46 个像素不同且**最大通道差 = 1**。

---

## 1. 文件与构建

| 文件 | 作用 |
|---|---|
| `src/gfx.h` | 对外接口（画布、图元表、graph/parts/text、字体、统计、归档 host） |
| `src/gfx.c` | 实现：图元表、group 树深度优先遍历、图像缓存、缩放/旋转/alpha/裁切、文本排版、PIL 兼容合成、`graph*.bin` 归档 host |
| `tests/test_gfx.c` | 场景 JSON 播放器：读 `gfx-scene.json` → 调 setter → `gfx_render` → 导出 PNG/几何清单 + 自检 + 计时 |
| `tests/gfx_dump.py` | 跑 Python 参考到指定步数，dump「完整图元状态 + graph/parts + 文本窗口 + 期望绘制清单」；顺带生成位图字体图集 |
| `tests/gfx_compare.py` | 像素比对（差异数/最大通道差/PSNR/SSIM/分区域统计/差异图） |
| `tests/verify_gfx.ps1` | 一键验证：dump → 构建 → C 渲染 → 比对 → PASS/FAIL |
| `out/c/gfx-frame.png` | C 渲染的 1920×1080 对照帧（8.3 MB，PNG 用 stored deflate，项目无 zlib） |
| `out/c/gfx-diff.json` / `gfx-diff.png` | 差异统计报告 / 放大 8 倍的差异热图 |
| `out/c/gfx-geom.json` | C 侧绘制清单（与 `gfx-scene.json` 的 `expect_draw` 对比即位置证明） |

构建（`PATH` 里先放 `tools\w64devkit\w64devkit\bin`）：

```bash
cd fvp
make gfx          # bin/libfvp_gfx.a  = gfx.o + bin.o + hzc1.o + inflate.o
make test_gfx     # bin/test_gfx.exe
bin/test_gfx.exe out/c/gfx-scene.json -o out/c

# 一键（含 Python 侧 dump 与像素比对）
powershell -ExecutionPolicy Bypass -File fvp\tests\verify_gfx.ps1
```

`fvp/Makefile` 只**追加**了 `gfx` / `test_gfx` / `clean-gfx` 三个目标，`all`、
`lib`、`test`、`win32` 的语义未改。`gfx.c` 依赖：`bin.c`/`hzc1.c`/`inflate.c`
（仅归档 host 用）和 `-lm`。桌面壳链接 `bin/libfvp_gfx.a` 即可。

---

## 2. 接口

### 2.1 画布与生命周期

```c
typedef struct Gfx Gfx;
Gfx *gfx_create(int w, int h, const GfxHost *host);  /* host 可为 NULL */
void gfx_destroy(Gfx *g);
void gfx_set_background(Gfx *g, int r, int g_, int b, int a); /* 默认 0,0,0,255 */
void gfx_render(Gfx *g);
const uint32_t *gfx_canvas(Gfx *g, int *w, int *h);       /* 泄漏到下次 render */
const uint8_t  *gfx_canvas_rgba(Gfx *g, int *w, int *h);  /* 同一块内存的字节视图 */
```

画布布局：**1920×1080（默认）、32bpp、top-down、stride = w*4、内存字节序 R,G,B,A**。
`gfx_canvas()` 返回 `uint32_t*`，小端机上数值 = `0xAABBGGRR`。
Win32 `StretchDIBits` 请用 `BI_BITFIELDS`，掩码
`R=0x000000FF, G=0x0000FF00, B=0x00FF0000, A=0xFF000000`（不需要拷贝/换色）。

### 2.2 资源回调

渲染层不碰文件系统/VM，纹理由 host 提供：

```c
typedef struct GfxImage {
    int w, h;        /* 单帧尺寸 */
    int x, y;        /* hzc1 头里的图锚点（render-model.md §2.1 的 graph.x/y） */
    int frames;      /* 多帧图（fmt=2）帧数 */
    uint8_t *rgba;   /* w*h*4*frames，帧垂直叠放，top-down RGBA */
} GfxImage;

typedef struct GfxHost {
    void *ud;
    int  (*load_image)(void *ud, const char *name, GfxImage *out); /* 0=成功 */
    void (*free_image)(void *ud, GfxImage *img);
} GfxHost;
```

现成实现（内部用 `bin.c`+`hzc1.c`，按 `work/render_assets.py` 的规则建全局名字表并
按需解码）：

```c
GfxArchiveHost *gfx_archive_host_create(const char *game_dir);
GfxHost        *gfx_archive_host_iface(GfxArchiveHost *h);
void            gfx_archive_host_destroy(GfxArchiveHost *h);
int             gfx_archive_host_image_count(const GfxArchiveHost *h);
```

`load_image` 名字解析与 Python 一致：先按原名查（6 个包零重名，全局表
`<包名>/<条目名>`），再按 `graph_bg/`、`graph_bs/`、`graph_vis*`、`graph/` 前缀回退
（脚本常写 `graph/BG…` 而实际在 `graph_bg.bin`）。解码结果的 `x/y` 就是几何锚点，
`frames` 供 `MotionAnim`/`PartsSelect` 选帧。

> Windows 注意：`bin.c` 用 `fopen()`，只接受 ANSI 代码页路径。`gfx_archive_host_create`
> 内部用 `MultiByteToWideChar(CP_UTF8)→CP_ACP` 转换，所以 UTF-8 的游戏目录可以直接传。

### 2.3 图元表（id 0..4095，字段布局同引擎）

字段偏移与语义见 `render-model.md §1.1`。C 侧统一用字段枚举：

```c
typedef enum GfxPrimField {
    GFX_F_TYPE, GFX_F_DRAW, GFX_F_ALPHA, GFX_F_BLEND, GFX_F_Z,
    GFX_F_X, GFX_F_Y, GFX_F_W, GFX_F_H, GFX_F_U, GFX_F_V,
    GFX_F_OP1, GFX_F_OP2, GFX_F_ANGLE, GFX_F_SCALE_X, GFX_F_SCALE_Y,
    GFX_F_GRAPH, GFX_F_TW, GFX_F_TILE, GFX_F_PARENT, GFX_F_FLAGS
} GfxPrimField;

int gfx_set_prim(Gfx *g, int id, int field, int v);   /* 引擎钳制规则 */
int gfx_prim_reset(Gfx *g, int id);                   /* PrimSetNull，保留 group 链 */
int gfx_prim_group_in(Gfx *g, int child, int parent);
int gfx_prim_group_out(Gfx *g, int child);
int gfx_prim_set_clip(Gfx *g, int id, int x, int y, int w, int h); /* 扩展 */
```

钳制规则与 handler 一致：alpha 0..255、z 100..10000、scale 100..10000、
angle `%3600`、x/y 按 16 位有符号、w/h/u/v/tile 按 16 位、parent 等价 group_in。

### 2.4 graph 槽 / parts

```c
int  gfx_graph_load(Gfx *g, int slot, const char *name);  /* NULL = 释放槽 */
int  gfx_graph_set_frame(Gfx *g, int slot, int frame);
int  gfx_graph_add_overlay(Gfx *g, int slot, const char *name,
                           int x, int y, int frame);      /* parts 补丁 */

int  gfx_parts_load(Gfx *g, int parts_id, const char *name);
int  gfx_parts_assign(Gfx *g, int parts_id, int slot);
int  gfx_parts_select(Gfx *g, int parts_id, int frame);
void gfx_parts_release(Gfx *g, int parts_id);
```

graph 槽缓存「解码后的整张图 + 当前帧号 + parts 覆盖层列表」。绘制某槽时按需把
「基准帧 + 覆盖层」合成到槽自己的 RGBA 缓冲（`compo_version` 脏标记），
等价于 Python 的 `Graph.image()`。overlay 的去重/追加顺序与
`render.py::_apply_parts_overlay` 一致（本帧 graph 槽 1 同时存了表情帧 0 和 13，
C 侧也照原样合成两层）。

### 2.5 文本

```c
void gfx_text_append(Gfx *g, int layer, const char *utf8); /* == gfx_draw_text */
void gfx_draw_text(Gfx *g, int layer, const char *utf8);
void gfx_text_clear(Gfx *g, int layer);
void gfx_text_set_color(Gfx *g, int layer, int r, int g_, int b);
void gfx_text_set_size(Gfx *g, int layer, int w, int h);
void gfx_text_set_pos(Gfx *g, int layer, int x, int y);
void gfx_text_set_out_size(Gfx *g, int layer, int w, int h);
int  gfx_text_set_layout(Gfx *g, int layer, const GfxTextLayout *lay);
```

* `[读音|正文]` 在 `gfx_text_append` 内解析，只保留正文（`[・|X]` 同规则，不画上方小字）。
* 全角空格保留（首行缩进），不做 trim。
* 按 `max_w` 逐字符换行（`wrap_text()` 的同款规则：不切英文单词，首字符超宽也保留）。
* 层 0 = 正文、8 = 人名、9 = 章节标题；默认版式 = `render.py::TEXT_LAYOUT`
  （0: x150 y905 maxw1620 size34 lines3；8: x150 y845 maxw700 size34 lines1；
  9: x0 y380 maxw1920 size56 lines2；其它层 x120 y880 maxw1600 size30 lines3）。
* 层 0 的字号还会按 `TextSize` 覆盖：`clamp(size_h>8 ? size_h : 34, 18, 40)`。
* 单个窗口最多 16 行（Python 无上限，实际每帧 ≤3 行）。
* 对话框底框/人名板在层 0 或层 8 有内容时绘制，几何/颜色是 Python 的常量近似。

### 2.6 位图字体

```c
typedef struct GfxFont GfxFont;
GfxFont *gfx_font_load(const char *path);   /* GFXF v2 图集，见附录 A */
void gfx_font_free(GfxFont *f);
int  gfx_add_font(Gfx *g, GfxFont *f);      /* 借用，不接管所有权 */
int  gfx_font_pixel_size(const GfxFont *f);
```

`gfx.c` 内置一套 **8×16 ASCII 点阵字体**（由 msgothic 生成后嵌入源码），没有任何
图集时也能出字；非 ASCII 字符退化为「缺字框」。验证时由 `gfx_dump.py` 生成
`gfx-font-<size>.bin`（同一 TTF、同一像素尺寸、含 glyph / stroke1 / stroke2 三层
覆盖率），`test_gfx` 加载后文字可做到与参考逐像素近似（见 §4）。

### 2.7 诊断

```c
typedef struct GfxStats {   /* 最近一次 gfx_render 的计数（image_loads 累计） */
    int sprites, texts, image_loads, cache_hits, compo_builds,
        resize_cache_hits;
    long long pixels;
} GfxStats;
const GfxStats *gfx_stats(const Gfx *g);
int gfx_draw_count(const Gfx *g);
const GfxDrawItem *gfx_draw_item(const Gfx *g, int index); /* 绘制清单 */
const char *gfx_last_error(const Gfx *g);
```

`GfxDrawItem = {prim, name, x, y, w, h, mode, z, alpha}`，`mode ∈ ui/char/bg`。
这份清单就是「位置误差」的可审计证据：test_gfx 导出它，并与 Python 侧 `info`
逐字段比对。

### 2.8 group 变换（相对 Python 原型的扩展）

`gfx_set_group_transform(g, 1)`（默认开）会把祖先 group 的 x/y/scale/alpha/angle
累加到子节点。Python 原型只按树序绘制、不传播 group 变换；本验证帧所有被绘制
图元的祖先都是单位变换，所以两者 draw list 完全一致（已实测 24/24、35/35）。
如果将来出现非单位的 group 变换，C 侧行为会更接近引擎 `+0x0c` 链的语义，届时
需要单独核对。

---

## 3. 与 Python 参考的对应关系

| Python（`work/render.py`） | C（`src/gfx.c`） | 说明 |
|---|---|---|
| `Prim` + `reset()` | `GfxPrim` + `gfx_prim_reset()` | 0x34 字段布局；reset 保留 parent/children |
| `_set_parent` / `_detach_child` | `gfx_prim_group_in/out` | 移动到子链表尾（插入顺序） |
| `load_graph()` / `Graph` | `gfx_graph_load` + `GfxSlot` | 槽缓存 `compo`（基准帧+覆盖层） |
| `Graph.image()` / `Part` | `slot_compose()` / `gfx_parts_*` | overlay 追加到列表尾 |
| `visible_sprites()` | `walk_tree()` + `mark_tree()` | 深度优先；不在树里的 sprite 按 id 升序补画 |
| `_sprite_geometry()` | `render_sprite()` 的 mode 分支 | 三种锚点，见 §3.1 |
| `Image.resize(BILINEAR)` | `resize_rgba()` | 预乘 alpha→三角滤波两遍→反预乘（Pillow 12 语义） |
| `img.rotate(-angle/10, expand)` | `rotate_rgba()` | 双线性；仅 UI 类且 angle≠0 |
| `getchannel('A').point(v*a//255)` | `alpha_eff < 255` 分支 | 整数向下取整 |
| `_paste_clipped()` | `blit_sprite()` + `over_px_opaque()` | 不透明目标用精确等价快速路径 |
| `additive` 分支 | `blit_sprite(additive=1)` | 加色近似 |
| `_draw_dialog_box()` / 人名板 | `draw_dialog_box()` / `draw_name_plate()` | 单一 overlay 语义；**只在 `--text-fixed-layout` 旧路径里画**，默认（图元驱动）不画 |
| `_draw_text()` | `draw_text_all()` / `draw_text_window()` | 版式常量与字号规则 1:1 |
| `parse_ruby()` | `ruby_strip()` | `[读音|正文]`→正文 |
| `wrap_text()` | `text_wrap()` | 逐字符换行 |
| `d.text(..., stroke_width=w)` | `draw_glyph_atlas()` 的 stroke 层 | 用图集里 FreeType 描边覆盖率 |
| `ImageDraw` 文字 ink 混合 | `ink_px()` | 四通道都按覆盖率混合（alpha 也混），重实现参考 PNG 文字半透明像素 |

### 3.1 三种锚点（`render-model.md §2.2` 的唯一自洽解）

```
ax = graph.x + prim.x ;  ay = graph.y + prim.y
h > w                → char: sc = scale_x/1000
                             rw = round(w*sc), rh = round(h*sc)
                             (ax - rw//2, ay - rh)
w > W*1.15 或 h > H*1.15 → bg: ((W-w)//2 + ax, (H-h)//2 + ay)，1:1，超画布裁掉
其余                  → ui:  (ax, ay)，rw = round((prim.w or w)*sx)，
                             rh = round((prim.h or h)*sy)
```

注意 `prompt1..22` 这类 36×38 的小图因为 `h > w` 会走 **char** 分支，位置是
`ax - rw//2, ay - rh`——C 侧照搬整数除法/`round()`（Python 的
`round()` 是 half-to-even，C 里用 `py_round()` 重实现）。

### 3.2 合成数学（实测对齐 Pillow）

* 不透明目标（画布）：`out = (s*sa + d*(255-sa) + 127) / 255`，对随机 20000 组
  像素与 `Image.alpha_composite` **完全一致**（0 差异）。
* 非不透明目标（graph 槽上贴 parts 覆盖层）：用通用 over 公式，个别像素与
  Pillow 有 ±1 差异（只影响表情补丁内部，实测本帧 sprite 区域最大通道差 = 1）。
* 缩放：Pillow 12 对 RGBA 会先转 `RGBa`（MULDIV255 预乘）→ 两遍三角滤波
  （水平后垂直，各自量化到 8 位）→ 反预乘 `floor(c*255/a)`。C 侧同法；
  对真实立绘 1366×1905→820×1143 的 numpy 对照中 99.56% 像素完全一致。
* 文字 ink：PIL `ImageDraw` 对 RGBA 是「把 ink 按覆盖率混入四个通道（含 alpha）」，
  所以参考 PNG 的文字描边像素 alpha = 200/120 而不是 255。`ink_px()` 重实现后，
  文字区域差异从约 1.4 万像素降到 109 像素。

---

## 4. 验证方法与结果

### 4.1 流程

```
gfx_dump.py --at N
   ├─ 用 work/render.py 跑到第 N 步（hold-text=30000，与发布 PNG 相同参数）
   ├─ 把完整状态写 gfx-scene.json（prims/graphs/parts overlays/texts/expect_draw）
   ├─ 生成 gfx-font-<size>.bin（本次帧用到的字符）
   └─ 顺手保存 gfx-frame-py.png；与 out/render/story_<N>.png 比对，必须逐字节一致
test_gfx gfx-scene.json
   ├─ 读 JSON → 调 gfx_* setter 复现场景（不重跑 VM、不跑脚本）
   ├─ 冷渲染 + 热渲染计时；导出 gfx-frame.png / gfx-geom.json
   └─ 自检：C draw list vs expect_draw（不同则 exit 2）
gfx_compare.py
   └─ 像素统计 + 分区域 + SSIM → gfx-diff.json / gfx-diff.png
```

`gfx_dump.py` 会打印
`[check] python re-render vs story_00528000.png: 0 differing px`，即 dump 与已发布
参考帧**逐字节一致**（SHA256 相同），保证 C 侧输入就是参考图对应的那一帧。

### 4.2 结果

| checkpoint | sprites | 位置误差 | 差异像素 | PSNR | SSIM | 文字内/外差异 |
|---|---:|---:|---:|---:|---:|---|
| 528000（story_00528000） | 24 | **0 px** | 289 (0.0139%) | 49.95 dB | 0.99971 | 105 / 184 |
| 534000（story_00534000）\* | 35 | **0 px** | 292 (0.0141%) | 49.95 dB | 0.99971 | 108 / 184 |

> \* 534000 这一行仍是 2026-11-23 的参考帧数字（**pushtrue 修复前**生成的），本轮只重冻结了
> 528000 那一帧，没有重跑 534000；下次重跑时数字会变。

> **2026-11-26 参考帧重冻结**：参考 VM（`work/fvm.py` / `ref/fvm_ref.py`）此前把
> `pushtrue` 实现成 push int 1，而原版 `0x444960` 只写 Variant 类型字节 `0x01`（bool，无 int 载荷）。
> 后果是脚本里"传 true = 保持原值"的位置类实参被当成 1 写进去：528000 那一帧的
> `prim 551` 被记成 (1,1)、`prim 552` 被记成 (1,852)，参考帧其实是**带着这个 bug 合成**的。
> 修掉之后 `551 → (0,27)`、`552 → (375,852)`——后者与 C 引擎侧先前实测的 (375,852) 完全一致，
> 两条实现互相印证。参考帧 `out/render/story_00528000.png` 已用修好的模型重生成
> （`gfx_dump.py` 自检 `0 differing px`），因此本表数字从 293/49.94/0.0141% 变为
> 289/49.95/0.0139%；C 与参考的吻合度没有下降。

剩余差异逐项定位：

* `graph_bs/CHR_真紅_基_白衣制服` 区域：46 px，**最大通道差 1**（缩放/合成舍入）。
* 对话框/人名板圆角+描边：184 px（PIL 用 `rectangle + pieslice/arc` 的整数栅格化，
  本实现用圆角距离判定，差 1 像素宽）。
* 文字包围盒内：105 px，主要是 AA 边缘 ink 舍入 ±1；C 侧已按 PIL 的
  「四通道按覆盖率混合」实现，所以参考图里半透明的描边像素也能对上。
* 其余（背景 1:1 贴图、22 个 prompt 小图标）：**0 px** 差异。

---

## 4.3 场景导出与回放（FVPS v2，2026-11-26）

`--scene` 回放需要一份"某一帧的渲染状态"。两种来源：

| 来源 | 导出方式 | 状态是否带 motion |
|---|---|---|
| Python 参考模型 | `tests/gfx_dump.py` → `work/scene_to_bin.py`（JSON → FVPS） | **否**：参考模型不跑 motion，prim 的 alpha/位置停在脚本初始化值 |
| C 引擎 | `bin/fvp-headless.exe ... --dump-scene out/c/scene.bin`（`gfx_scene_export()`） | **是**：C 有 motion 实现，导出的是"引擎真跑到这一步"的状态 |

FVPS v2 布局（小端，`src/gfx.c: gfx_scene_export()` 写、`src/platform/win32/win32_scene.c` 读）：

```
"FVPS" | u32 version=2 | i32 canvas_w, canvas_h | i32 bg r,g,b,a
graphs : u32 n; { i32 slot; u32 namelen + bytes; i32 w,h,x,y,frames,frame }
prims  : u32 n; { i32 id,type,draw,alpha,blend,z,x,y,w,h,u,v,op1,op2,
                  angle,sx,sy,graph,tw,parent }   (20 × i32)
texts  : u32 n; { i32 idx,r,g,b,size0,size1,out0,out1,panel_w,panel_h,
                  has_pos,pos0,pos1,nlines; 每行 u32 len + bytes }
layout : u32 n; { i32 idx,x,y,max_w,size,max_lines }
```

v2 比 v1 多的是 `panel_w/panel_h`（`TextBuff(110)` 的离屏面板尺寸）：
图元驱动模式下它就是换行宽度与裁剪高度，没有它回放出来的正文会按版面表换行，
和引擎不一致。

**诊断开关**：`fvp-headless.exe --text-out-steps FILE` 把每句台词连同**VM 步数**
写出来（`<step>\t[layer] text`），用来定位"某句台词出现在第几步"。
`--text-out` 的格式不变。

**已知限制（本轮实测）**：

1. **两个引擎的步数不可比**：参考模型与 C 的输入模型不同（点击周期、协程开关、
   `hold_text`），同样是 step 528000，C 侧导出的是 135 个图元、0 个 type-5 文本图元、
   文本窗口全空；参考模型那一步却是"对话框 + 台词"。所以拿 Python 的 step 号
   去 C 侧 `--steps` 取帧是错的。
2. **headless + gfx 到不了正文**：带 gfx 时 `PrimHit` 走图元查询、永远不命中，
   脚本卡在标题菜单（`primhit=0/0`、`--click 600` 也 0 行文本）；不带 gfx 的
   `bin/fvm.exe` 同样参数能跑出 195 行（`ref/ref-900k.txt`）。因此"导出正文帧"
   还缺一步：让 headless 的 `PrimHit` 与 gfx 解耦（诊断模式），或者用
   `FVP_START_PC` 配上真实的栈/协程上下文。

## 5. 性能

测量条件：Windows + w64devkit gcc 16.2 `-O2`，单线程，`clock()`（CPU 时间），
`bin/test_gfx.exe out/c/gfx-scene.json -o out/c -r 20`（冷帧 1 次 + 热帧 20 次平均）。
图元 282、graph 槽 80、绘制 sprite 24、画布 1920×1080。

| 阶段 | 耗时 |
|---|---:|
| 场景装入 + 80 张 hzc1 解码（96 MB RGBA） | 311–336 ms（≈315 ms） |
| 首帧合成（24 个槽合成 + 立绘 0.6× 缩放 + alpha + 全部 blit + 文本） | 85–89 ms（≈87 ms） |
| **冷启动一帧（解码 + 合成）** | **396–425 ms（≈405 ms）** |
| **热帧（纹理/槽合成/缩放结果全缓存命中）** | **10.25–10.65 ms（≈10.4 ms，≈96 fps）** |
| PNG 导出（stored deflate，8.3 MB） | 96–134 ms（不计入合成耗时） |

热帧里 touched 像素 2,957,264；主要开销是背景 2400×1350→裁到 1920×1080 与立绘
820×1143 的 alpha 合成。冷帧相对热帧多出的 ≈77 ms 主要是：立绘预乘缩放
（1366×1905→820×1143 两遍滤波）与 24 个 graph 槽的基准帧+覆盖层合成。

内存（本帧实测量级）：解码纹理会常驻 95.7 MB（80 个槽），被绘制槽的合成图
≈23.4 MB（背景 13.0 MB + 立绘 10.4 MB 等），画布 8.3 MB，立绘变换缓存 3.7 MB。
纹理由 host 引用计数管理：槽释放/重载后自动 `free_image`。

> 说明：`image_loads=80` 是照 Python `GraphLoad` 的语义 eager 解码；真正被绘制
> 的只有 24 个槽。若想进一步压冷启动，可把 host 的 `load_image` 改成惰性
> （第一次用到才解码），接口不需要变。

---

## 6. 偏差清单（与 Python 参考 / 引擎真实行为）

1. **group 变换**：C 默认把祖先 group 的 x/y/scale/alpha 累加给子节点；Python 原型
   不传播。验证帧所有被绘制图元的祖先都是单位变换，故 draw list 一致；未来出现
   非单位 group 变换时需要复核（本项是「更接近引擎」的选择，不是 bug）。
2. **缩放实现**：按 Pillow 12 的「预乘 alpha + 三角滤波两遍 + 反预乘」实现，
   与 Pillow 输出约 99.6% 像素完全一致，其余为 ±1；不是 GPU 语义。
3. **非不透明目标的 over 公式**：graph 槽内贴 parts 覆盖层时，与 Pillow 的整数
   舍入有 ±1 差异（不影响外部结果：sprite 区域最大通道差 = 1）。
4. **圆角矩形**：`rounded_rectangle` 的 fill/outline 用像素中心距离判定，
   PIL 用 `rectangle + pieslice/arc` 的整数栅格化；圆角/描边边缘差 1 像素
   （本帧 184 px）。框体位置/尺寸/颜色/透明度一致。
5. **文字 ink 模型**：已按 PIL 的「四通道按覆盖率混合」重实现（含描边 alpha
   200/120 导致参考 PNG 文字半透明这一反直觉行为）。剩余 109 px 是 AA 舍入 ±1。
6. **字体**：模块内置 8×16 ASCII 点阵；验证用的 `gfx-font-*.bin` 是
   `gfx_dump.py` 用 msgothic 在**与参考相同的像素尺寸**下生成的覆盖率图集
   （glyph + FreeType 描边宽度 1/2）。这不是游戏字体，只是让像素比对可信的
   测试资产；真实引擎接字体时替换 `gfx_add_font()` 即可。
7. **旋转**：只对 UI 类生效（同 Python），`expand=True` 的 bbox 与采样为近似实现；
   本帧没有旋转图元被绘制。
8. **UV**：`PrimSetUV` 只记录，不参与取样（Python 同样忽略，`W/H` 被当作目标尺寸）。
9. **blend**：`PrimSetBlend=1` 用加色近似（同 Python）。
10. **tile（type=2）**：只记录，不绘制（Python 也是 `pass`）。
11. **Z 不排序**：严格按 group 树深度优先 + 树外 id 升序（同引擎/同 Python）。
12. **hold-text**：`--hold-text` 是 Python 快照技巧，不是引擎行为；C 模块不实现
    hold，由 `gfx_dump.py` 把「可见行」算好后写进 JSON（`lines` 字段）。
13. **PNG 输出**：无 zlib，用 stored deflate，文件 8.3 MB；仅测试工具需要，
    模块本身不写文件。
14. **字体图集/场景 JSON 是测试格式**，不是游戏运行时格式；真实链路里
    `GraphLoad`/`PrimSet*` 由 syscall 层直接调 setter。

---

## 7. 集成说明（给 syscall 层 / 桌面壳）

`gfx` 不持有 VM、不做 I/O、没有全局状态（可多实例）。syscall 层按
`render.py` 的 handler 表转发即可：

| 命令 id | 命令 | gfx 调用 |
|---:|---|---|
| 22 | GraphLoad | `gfx_graph_load(g, slot, name)`（name 为 NULL 时释放） |
| 64/65/71 | PartsAssign/Load/Select | `gfx_parts_assign/load/select` |
| 73/75 | PrimGroupIn/Out | `gfx_prim_group_in/out` |
| 80 | PrimSetNull | `gfx_prim_reset` |
| 77/78/79/81/87/88/89/90/91 | PrimSetAlpha/Blend/Draw/OP/Tile/UV/WH/XY/Z | `gfx_set_prim(id, GFX_F_* , v)` |
| 82/83 | PrimSetRS/RS2 | `GFX_F_ANGLE` + `GFX_F_SCALE_X/Y` |
| 85 | PrimSetSprt | `gfx_prim_reset` + type=4 + graph/x/y |
| 86 | PrimSetText | `gfx_prim_reset` + type=5 + tw |
| 42 | MotionAlpha | `GFX_F_ALPHA`（直接取 to，同原型） |
| 45 | MotionAnim | `gfx_graph_set_frame(prim.graph, animId)` |
| 48/49/51/54/57 | MotionMove* | x/y/z/scale 直写 |
| 111/112/120/122/123/126 | TextClear/Color/OutSize/Pos/Print/Size | `gfx_text_*` |
| 23/66/14/15/… | GraphRGB/PartsMotion/Dissolve/*Test | 同原型：空实现或返回 1 |

每帧循环：`update_animations` → `gfx_render(g)` → 读 `gfx_canvas()` 上屏。
桌面壳只需要 `gfx_create / gfx_render / gfx_canvas / gfx_destroy` 四个函数。

---

## 8. 还没做的

* `Dissolve`(14)、`GraphRGB`(23)、`PartsMotion`(66)、`V3D*`、`Movie`、`Snow*`：
  与 Python 原型一致地未实现（都不影响本帧）。
* 纹理 UV/裁切：`PrimSetUV` 未参与取样；C 侧提供了 `gfx_prim_set_clip()` 但没有
  对应命令。
* 圆角与 Pillow 的逐像素一致性：如需绝对 0 差异，需要移植 `draw.c` 的
  `pieslice/arc` 整数算法。
* 文字排版：`TextFormat/TextFunction/TextShadowDist/TextSpace` 的真实参数化
  （现在用固定版式，与 Python 相同）。
* 桌面壳（`src/platform/win32`）是队友的工作，不在本模块内。

---

## 附录 A：位图字体格式 GFXF v2

`tests/gfx_dump.py::write_font()` 生成，`gfx_font_load()` 读取。

```
0x00  4  'GFXF'
0x04  u32 version (2)
0x08  u32 size       像素 em size（与 PIL 的 font size 相同）
0x0C  u32 margin     单元四周留白（描边用，当前 4）
0x10  u32 ascent
0x14  u32 descent
0x18  u32 count
0x1C  count * { u32 codepoint, u32 advance, u32 w }   /* 按 codepoint 升序 */
      count 组像素块，每组 3 层（glyph / stroke_w=1 / stroke_w=2），
      每层 w * (ascent+descent+2*margin) 字节，8-bit 覆盖率，顺序拼接
```

绘制时：笔位 `(pen, y)` → 单元左上角 `(pen - margin, y - margin)`，
先按覆盖率用 ink 颜色混合 stroke 层，再用 glyph 层覆盖；`advance` 为笔进量。
内置字体是 `layers=1` 的特例（8×16 1bpp，advance 8，非 ASCII 走缺字框）。

## 附录 B：gfx-scene.json 字段

`test_gfx.c` 只读以下字段（`gfx_dump.py` 写出）：

```jsonc
{
  "version": 1, "step": 528000, "game": "C:/.../五彩斑斓的世界HD",
  "canvas": [1920, 1080], "background": [0, 0, 0, 255],
  "prims": [{ "id":100, "type":4, "draw":1, "alpha":225, "blend":0, "z":700,
              "x":0, "y":464, "w":0, "h":0, "u":0, "v":0, "op1":0, "op2":0,
              "angle":0, "sx":1000, "sy":1000, "graph":100, "tw":-1,
              "tile":0, "flags":3, "parent":7, "children":[] }],
  "graphs": [{ "slot":100, "name":"graph_bs/CHR_真紅_基_白衣制服",
               "w":1366, "h":1905, "x":884, "y":709, "frames":1, "frame":0,
               "overlays":[{ "x":508, "y":161, "name":"…_表情", "frame":13 }] }],
  "texts": [{ "idx":0, "lines":["……"], "color":[10,51,100], "size":[64,32],
              "pos":null, "out_size":[64,16] }],
  "fonts": [{ "path":"gfx-font-32.bin", "size":32, "…":"…" }],
  "text_layout": { "0": {"x":150,"y":905,"maxw":1620,"size":34,"lines":3} },
  "expect_draw": [{ "prim":186, "name":"graph/BG043_000", "x":-239, "y":-51,
                    "w":2400, "h":1350, "mode":"bg", "z":100, "alpha":255 }]
}
```

`expect_draw` 是 Python 侧 `render_frame()` 的 `info` 列表，也就是位置真值；
`test_gfx` 的几何自检与 `gfx_compare.py` 的 `geometry` 段都对比它。

---

## 9. syscall 接线与端到端验证

> 任务书里这一节编号为 §8；本文原有 §8「还没做的」保留，故新增节编为 §9。

把 `src/gfx.c` 通过一个**可选的** VM 上下文接到 `src/syscall.c` 的命令分发上，让「引擎边跑脚本边出画面」成立，同时保证 `bin\fvm.exe` 的行为一字不变。

### 9.1 接线方式（只加不改）

| 文件 | 改动 |
|---|---|
| `src/vm.h` | `struct Gfx;` 前置声明；`FvpGfxBridge`（gfx 指针 + `prim_graph` 影子表 + 可选文本观察回调）；`FvpVM.gfx_bridge`；`void fvp_vm_attach_gfx(FvpVM *vm, struct Gfx *gfx)` |
| `src/vm.c` | `fvp_vm_attach_gfx()` 实现（`gfx == NULL` = 脱离并释放 bridge）；`fvp_vm_free()` 释放 bridge |
| `src/syscall.c` | 弹参之后、原 `switch` 之前：`gfx_bridge != NULL` 时按 §7 表把 30 条渲染命令转发给 setter；所有 gfx 符号用 `#pragma weak` 声明 |

要点：

* **fvm.exe 不链接 gfx.c 也能链接**：`gfx_graph_load` / `gfx_set_prim` / `gfx_text_*` 等 15 个入口都是 weak 符号，`bin/fvm.exe` 里解析为 0；`gfx_bridge == NULL` 时一行 gfx 代码都不会执行，原 `unknown cmds` 计数、`temp` 语义、`--text-out` 全部不变（见 §9.4）。
* **参数顺序**：栈上先压的是第 1 个参数，`fvp_syscall()` 的 `args[0]` 就是它；转发直接用同一份 `args`。
* **类型规则照 `work/render.py`**：只有整数（`_type()==2`）才写 `PrimSet*/Motion*` 的数值分量；字符串才进 `GraphLoad` / `PartsLoad` / `TextPrint`；`TextPrint` 还跳过单字节数字句柄（渲染原型的「未解析文本变量」启发式）。
* **`argc` 差异**：逐字节 VM 用的是 `cmd_argc.json`（`gen_cmd_table.h`），渲染原型 `render.py` 用 `cmd_table.json`，两者有 6 条不同：49/51/52/54/94/95。gfx 转发时按 `cmd_table` 的 argc **从操作数栈顶非破坏性取参**（`fvp_gfx_argc()`），VM 的弹栈行为完全不动，因此渲染侧拿到的是参考原型同样的参数，而 VM 仍是逐字节验证过的那一套。
* **`FloatToInt`(20)**：fvm/C VM 没实现（不影响文本契约），但画面坐标要靠它（脚本里 `FloatToInt(84.375)` 的结果会进 `PrimSetXY`）。观察到 gfx 上下文时按 `render.py` 语义返回 `int(v)`；不挂 gfx 时保持原样。实测不接这一步时，draw list 24 项里有 2 项位置错：背景 `BG043_000` 的 y 从 −51 变 −134、立绘从 (474,30) 变 (475,−433)——即坐标停在占位值 1。
* **`MotionAnim` 的 prim.graph**：`gfx.h` 没有 prim 字段 getter，`FvpGfxBridge.prim_graph[4096]` 由 `PrimSetSprt` 写入、`PrimSetNull/Sprt/Text/Tile` 复位为 −1，供 `gfx_graph_set_frame()` 使用。
* **无法接的命令**（gfx 没有对应 setter，保持空实现）：`ColorSet`(7，渲染层不取调色板)、`TextSpeed`(129，文本是静态排版)、`PrimExitGroup`(72，参考原型只记录不参与合成)、`TextFormat/TextFunction/TextFont*` 等排版类。
* **`GraphLoad` 的 null 释放**：C 值模型没有 null；`render.py` 对 int/bool 占位符也是直接忽略，所以这里只接受非空字符串（与参考帧一致）。`gfx_graph_load(NULL)` 的能力保留给桌面壳。
* **文本观察回调**：`GfxBridge.on_text` 仅在测试里设置（用于重实现 `--hold-text` 快照约定），`fvm.exe` 恒为 NULL。

### 9.2 端到端测试 `tests/render_vm.c`

`bin/render_vm.exe` = `src/fvp.c + encoding.c + hcb.c + vm.c + syscall.c + gfx.c + bin.c + hzc1.c + inflate.c`：

1. 读 `iroseka.hcb`，用游戏目录建 `gfx_archive_host`，`gfx_create(1920,1080)`，加载 `out/c/gfx-font-*.bin`；
2. `fvp_vm_init` → `fvp_vm_attach_gfx` → 每 **1000 VM 步**为一帧调一次 `gfx_render()`（`--frame-steps` 可调；528000 正好是帧边界）；
3. 在目标步导出 `gfx_canvas_rgba()` 为 `out/c/vm-frame.png`，同时导出 draw list（`vm-geom.json`）和文本窗口状态（`vm-text.json`）；
4. `--hold-text 30000` 只在**渲染前**把「已 TextClear 但上一句还在窗口内」的行临时补回，渲染后立即清掉，不改变 VM 状态（默认值与 `gfx_dump.py` 相同；`0` 关闭）；
5. 另存一张不做 hold 的 `vm-frame-nohold.png`，用于量化快照约定本身的影响；
6. 计时写 `vm-render-perf.json`：先跑一遍「只跑 VM 不渲染」的基线，再跑带渲染的版本。

### 9.3 验证结果（528000 步，`--click 4000`，与 `story_00528000.png` 同一个快照）

| 指标 | 结果 |
|---|---|
| 绘制清单 | **24/24 sprite** 与 `gfx-scene.json` 的 `expect_draw` 逐字段相同（prim/name/x/y/w/h/mode/z/alpha），**位置误差 0 px** |
| 差异像素（RGB 任一通道） | **289 / 2,073,600 = 0.0139%**，最大通道差 153，mean abs 0.007612 |
| PSNR（RGB / on black） | **49.95 dB / 49.95 dB** |
| SSIM（luma） | **0.99971** |
| 文字包围盒内/外差异 | **109 / 184 px** |
| 文本窗口 | 层 0/8/9 与参考完全一致（层 0 是 hold-text 补回的台词） |
| 不做 hold-text | 9303 px（0.4486%），PSNR 30.42 dB；差异几乎全部在对话框文字区（9119 px） |
| 字节验收 | `compare.py --run` **PASS**、`--run --steps 2000000` **PASS** |

这个结果与 §4.2 的 gfx 层基线**每一个数字都相同**，因为 C VM 跑到 528000 步时的最终渲染状态（24 个 sprite + 3 个文本窗口）与 `gfx-scene.json` 描述的参考状态逐字段一致——同一份渲染层输入，自然得到同一帧。也就是说，这次端到端跑通没有引入任何新的渲染误差。

### 9.4 `bin\fvm.exe` 行为不变的证据

* `python fvp\tests\compare.py --run`：PASS，6496 B / 195 行，sha256 `7f9837ffaf3b7b5759581c625698c9554e83f2cdbdeadf635dca6fc9c01b3ea2`；
* `python fvp\tests\compare.py --run --steps 2000000`：PASS，15378 B / 450 行，sha256 `32d7d4e2dce441738a79828d57ec6a7522cb639204141b5571bb42bf9bb3856e`；
* 改动前后各跑一次 `fvm.exe --steps 900000 --click 600 --quiet --text-out ...`：**stdout 494 B 逐字节相同、stderr 0 B、text-out sha256 相同**；
* 代码层：所有 gfx 调用都在 `vm->gfx_bridge != NULL && gfx_set_prim != NULL` 分支内，`fvm.exe` 既没有 gfx 上下文也没有强 gfx 符号。

### 9.5 性能（528000 步 + 每 1000 步渲染一次）

测量条件同 §5：Windows + w64devkit gcc 16.2 `-O2`，单线程，`clock()` CPU 时间，画布 1920×1080。同机还有其它构建任务在跑，数字有 ±10% 波动，下表给的是多次运行的区间。

| 项目 | 结果 |
|---|---|
| 只跑 VM（无渲染） | **8–11 ms**（528000 步，约 5–7×10⁷ steps/s） |
| 跑 VM + 528 帧渲染（总） | **≈5.1–5.5 s**，**平均 9.8–10.3 ms/帧** |
| `gfx_render()` 合计 / 平均 | ≈4780–5030 ms / **9.0–9.5 ms/帧** |
| 首帧（空场景） | 2–3 ms |
| 热帧平均 | **9.0–9.5 ms/帧**（≈105–110 fps） |
| 最慢一帧 | **350–420 ms**（首次加载大图/解码的尖峰，发生在图像开始出现的那一帧） |
| 纹理解码 | 92 次 `load_image`（`GraphLoad` 语义是 eager 解码，发生在 VM 运行期间，不在 `gfx_render` 里） |
| gfx 初始化 | 4–7 ms（归档索引 2653 张 + 4 个字体图集） |

与 §5 的「热帧 ≈10.4 ms」一致（本测试每帧多算一次 8.3 MB 画布清零/合成，数字在 9.1–10.4 ms 之间波动）。PNG 导出（stored deflate，8.3 MB）不计入上面的运行时间。

### 9.6 复现

```bash
# 一键：构建 → 跑 VM+渲染 → 像素比对 → 两条字节验收
powershell -ExecutionPolicy Bypass -File fvp\tests\verify_render_vm.ps1

# 只重跑比对（已构建/已导出）
python fvp\tests\vm_compare.py
python fvp\tests\gfx_compare.py          # 旧的 gfx 层基线（未受影响）

# 手工
bin\render_vm.exe <game>\iroseka.hcb --steps 528000 --click 4000 \
    --frame-steps 1000 --hold-text 30000 --out out\c
python fvp\tests\vm_compare.py --frame fvp\out\c\vm-frame.png
```

产物（`fvp/out/c/`）：`vm-frame.png`（8.3 MB）、`vm-frame-nohold.png`、`vm-geom.json`、`vm-text.json`、`vm-render-perf.json`、`vm-diff.json`、`vm-diff.png`（差异热图 ×8）、`vm-diff-nohold.json/.png`、`verify-render-vm.log`（一键脚本完整输出）。

文件：`src/syscall.c`、`src/vm.c`、`src/vm.h`（只加不破坏）、`tests/render_vm.c`、`tests/vm_compare.py`、`tests/verify_render_vm.ps1`。

### 9.7 已知差异 / 限制

1. **hold-text 只是快照约定**：`gfx.c` 和 VM 都不实现它；`render_vm` 在渲染前临时补行、渲染后清掉。不做 hold 时是 9303 px / PSNR 30.42 dB（见 §9.3），这就是「参考帧的台词其实已经 TextClear」带来的差异。
2. **`FloatToInt` 只在挂 gfx 时生效**：它改变 `temp`，若不接会看到 §9.1 的 2 个 sprite 位置错；接上后 C VM 的轨迹才与 `render.py` 在该步一致。`fvm.exe` 不挂 gfx，不受影响。
3. **`argc` 双表**：渲染转发按 `cmd_table` 的 argc 取参，VM 仍按 `cmd_argc` 弹栈——两条路径各自保持被验证过的行为，代价是「gfx 看到的实参」与「VM 弹掉的实参个数」不同，这一点仅在本节记录。
4. **`MotionAnim` 负帧号**：`render.py` 用 Python 取模（−1 % n = n−1），`gfx_graph_set_frame()` 把负帧钳到 0；脚本实际只传非负 animId，未观察到差异。
5. **`GraphLoad` 的 null 释放**没有从脚本侧接通（C 值模型没有 null）；参考原型同样忽略非字符串参数。
6. `TextSpeed` / `ColorSet` / `PrimExitGroup` 无 gfx setter，保持空实现（与参考原型的合成结果一致）。
7. **帧的粒度是测试约定**：引擎没有固定「每 N 步一帧」；`--frame-steps 1000` 只是基准测试的采样方式，528000 是它的整数倍。真正的桌面壳按 60 Hz 时间片驱动（`src/platform/win32`）。
8. **单线程语义**：与 `render.py` 默认（`enable_threads=False`）一致；脚本协程文本那部分差异沿用 `render-model.md §7` 的记录，本测试不涉及。
9. 只对 **528000** 这一个已发布 checkpoint 做了端到端像素比对；其它步数没有参考 PNG 可比（534000 只有历史产物，未纳入一键验证）。
10. 字体仍用 `gfx_dump.py` 生成的 msgothic 覆盖图集（`out/c/gfx-font-*.bin`）；真实引擎字体替换不在本任务范围。

## 缺字回退：图集没有的码点用 GDI 现场栅格化（2026-11-25）

桌面外壳用预生成的位图图集（`bin/gfx-font-<size>.bin`）画字，图集按目标作品的
**日文**脚本生成。汉化补丁的文本（例如存档里的 `　谁这样说道。`）会出现图集没有的
汉字，之前统一渲染成豆腐块。

现在加了一条**缺字回退**通路，思路和原版一致（原版本来就调用 GDI 的
`CreateFontA` / `GetGlyphOutlineA`）：

1. `src/gfx.c`：`draw_line_font()` 画之前先检查"这一串里有没有图集缺字"，
   有且平台装了回退 → 走回退；
2. `src/gfx.h` 定义 `GfxGlyphFallback`，平台层只需返回一份**8 位覆盖率掩码**
   （配色/混合由 gfx.c 做，像素格式不外泄）；
3. 新增 `src/platform/win32/win32_gdi_text.c`：`CreateFontW(-px, ...,
   L"Microsoft YaHei")` → 32bpp DIB → `TextOutW` → 取覆盖率；
4. 外壳启动时安装回退；退出前日志报 `gfx: glyph fallback used N time(s)`。

**基线安全**：冻结基线里的文本没有缺字，回退不会触发 —— `verify_gfx` 仍是
289 px（0.0139%）、PSNR 49.95、SSIM 0.99971；`compare.py` 逐字节一致。

实测（存档 905 的部分恢复视图）：回退触发 **13 次**，`　谁这样说道。` 正常显示。

**逐码点回退（2026-11-25 改进）**：最初是"整串一次栅格化"，混排时字距和图集对不齐。
现在改成**逐码点**：`draw_text_run()` 遇到图集缺字时，只把这**一个码点**交给平台回退，
推进宽度取 `advance_of()`（GDI `GetTextExtentPoint32W` 的结果），所以
"图集字 + 回退字"混排时字距是连续的。`GfxGlyphFallback.advance_of` 是可选项，
不提供时退回用掩码宽度。

单测覆盖（`tests/test_glyph_fallback.c`，18 项）：装一个**假的平台回退**（4×4 掩码 +
固定推进 12），断言缺字不再计入 `tofu_glyphs`、`glyph_fb_runs` 计到 1、卸载回退后
恢复豆腐块行为——不依赖 Windows GDI，跨平台可跑。

**已知限制**：全角空格（U+3000）走回退时，首字与空格的字距仍略有偏差（GDI 的
空格推进与图集给的宽度不同）；要彻底一致得把回退字形**并入图集**并复用图集的
推进表。

## 与原版同协议的缺字回退：cp932 + GGO_BITMAP（2026-11-25）

上面那条回退用的是**抗锯齿**路径（`TextOutW` + 32bpp DIB + `ANTIALIASED_QUALITY`），
它好看，但不是原版的画法。`docs/original-verification.md` §8 从原版 exe 静态提取出
真实协议（`ref/exe-gdi-params.json`）：

| 参数 | 原版的值 |
|---|---|
| 建字体 | `CreateFontA`，`fdwCharSet=0x80`(`SHIFTJIS_CHARSET`)，字重/质量/间距全 0 |
| 取字形 | `GetGlyphOutlineA`：`GGO_METRICS(0)` 取进位、`GGO_BITMAP(1)` 取 **1bpp 位图** |
| 变换矩阵 | 三次调用共用一个 `MAT2` = **单位矩阵** |
| 文本输出 API | **没有**（全 exe 无 `TextOut`/`ExtTextOut`）——字形位图由引擎自己贴 |

于是 `src/platform/win32/win32_gdi_text.c` 里现在有**两条实现**：

| | 默认（抗锯齿） | `--glyph-original`（原版协议） |
|---|---|---|
| API | `CreateFontW` + `TextOutW` → 32bpp DIB | `CreateFontA` + `GetGlyphOutlineA` |
| 掩码取值 | 0..255 灰度 | **只有 0 / 255** |
| 进位 | `GetTextExtentPoint32W` | `GLYPHMETRICS.gmCellIncX` |
| 字符集 | `DEFAULT_CHARSET` | `SHIFTJIS_CHARSET` |
| 族名 | `Microsoft YaHei` | exe 内置注册表第 0 项 `ＭＳ ゴシック`（`--glyph-face` 可换） |

实现要点：

1. UTF-8 → **cp932** 编码（`WC_NO_BEST_FIT_CHARS`，禁止"猜"替代字）；cp932 表示不了
   的字符返回 0 —— 原版是 ANSI 程序，它自己也画不出来；
2. cp932 字节串按 **DBCS** 拆成 `uChar` 序列（前导字节 `0x81..0x9F`/`0xE0..0xFC`
   与后继字节拼成一个 `WORD`），这正是原版 `GetGlyphOutlineA` 的调用形态；
3. `GGO_BITMAP` 的行按 **4 字节对齐**（`((gmBlackBoxX+31)/32)*4`），逐位贴到画布，
   画布只有 0/255；
4. 基线安全：默认仍是抗锯齿路径，`verify_gfx` / `compare.py` 的数字不受影响；
   `--glyph-original` 只用于"和原版对齐"的对照与门禁。

单测 `tests/test_gdi_ggo.c`（28 项，`make test_gdi_ggo`，走 `run_all.py`）锁住这些性质：
二值掩码、`gmCellIncX` 的 N 倍进位、cp932 之外的字符返回 0、默认族名与可换可复位、
NULL/越界安全。实测抗锯齿路径「日」@32 有 **54 / 1224 ≈ 4.4%** 的像素是中间值——
这些像素在原版协议里**根本不会出现**，这就是"协议差"的量化形式。

## 原版的 D3D9 混合状态：从 exe 抄出的 `SetRenderState` 常量（2026-11-25）

第 15 轮的"背景四层怎么合成"是用**拟合**回答的：把 54 个候选（顺序 × 混合 × 底色 ×
缩放）在原版帧上各比一遍，取误差最小的"直通 alpha over"。按项目方法论（先结构分析、
抓帧只做验证），这个结论应该有 exe 里的依据。本轮补上：
`scripts/extract-exe-d3d-states.py` → 基线 `ref/exe-d3d-states.json` → 门禁
`tests/verify_exe_d3d_states.py`（`run_all` 53 → 54 步）。

### 方法：D3D9 是 COM，状态走 vtable

`SetRenderState` 在设备 vtable 上，调用形态固定：

```asm
mov  0x8(%esi),%eax      ; eax = 设备对象（HGraph 的成员 +8）
mov  (%eax),%ecx         ; ecx = vtable
mov  0xe4(%ecx),%edx     ; edx = vtable[0xE4/4]
push $value              ; 第 3 个实参（__stdcall 右到左）
push $state              ; 第 2 个实参
push %eax                ; 第 1 个实参 = this
call *%edx
```

脚本就按这个形态扫：`call *%reg` → 往前找加载同一寄存器的 `mov 0xOFF(...),%reg`
（得到 vtable 偏移 0xE4）→ 再往回收立即数 `push $imm`（依次是 value、state）。
**值可能来自分支**（同一个调用点有两个 `push`，例如模式字节 0 走 `INVSRCALPHA`、
1 走 `ONE`），这种站点收多个候选值并标 `conditional`，不假装只有一个值。

覆盖率是硬的：asm 里 `mov 0xe4(...)` 一共 **36 处**，本脚本解析出 (state, value) 的
**31 处**，剩下 **5 处**实参来自寄存器或更远的分支（`0x00425501`、`0x0042ab0d`、
`0x0042b5d1`、`0x0042bd24`、`0x0042c3aa`），记进 `unresolved_sites`，不推测。

### 抄出来的状态

| 状态 | 值（D3D 枚举名） | 说明 |
|---|---|---|
| `ALPHABLENDENABLE`(27) | `TRUE` | 混合打开（全 exe 只设过一次 TRUE） |
| `SRCBLEND`(19) | `SRCALPHA`(5)、`ONE`(2) | 主路径恒为 `SRCALPHA` |
| `DESTBLEND`(20) | `INVSRCALPHA`(6)、`ONE`(2)、`ZERO`(1) | 6 = over，2 = 加色，1 = 覆盖 |
| `BLENDOP`(171) | **从未设置** | ⇒ D3D9 默认 `ADD`（不需要 SUBTRACT/MIN/MAX） |
| `ALPHATESTENABLE`(15) / `ALPHAREF`(24) / `ALPHAFUNC`(25) | TRUE→FALSE / 255 / `GREATEREQUAL` | 一次性开 alpha test、改完关掉 |
| `ZENABLE`(7) | `FALSE` | 2D，无深度测试 |
| `CULLMODE`(22) | `NONE` | 双面都画 |
| `LIGHTING`(137) | `FALSE` | 不做光照 |
| `COLORVERTEX`(141) / `DIFFUSEMATERIALSOURCE`(145) | TRUE / `COLOR1` | 用顶点色（我们渲染器的 per-prim 颜色） |
| `DITHERENABLE`(26) | `FALSE` | 无抖动 |

### 三种混合配置

| 配置 | SRCBLEND | DESTBLEND | 公式 | 出现位置 |
|---|---|---|---|---|
| **straight-alpha over** | `SRCALPHA` | `INVSRCALPHA` | `dst = src.rgb*src.a + dst.rgb*(1-src.a)` | 主路径（5 个固定调用点 + 4 个条件调用点的默认分支） |
| **additive（alpha 加色）** | `SRCALPHA` | `ONE` | `dst = src.rgb*src.a + dst.rgb` | 4 个条件调用点（模式字节 = 1） |
| **overwrite（不混合）** | `ONE` | `ZERO` | `dst = src.rgb` | 只出现在 `Clear` 之前的覆盖写（2 处） |

### 交叉验证：这个"模式字节"就是 prim+8

条件调用点的形态是（`0x00425534` 一处为例）：

```asm
425515  mov  0x8(%edi),%al      ; 模式字节
425518  test %al,%al
42551a  jne  0x425520
42551c  push $0x6               ; 0 → DESTBLEND = INVSRCALPHA
42551e  jmp  0x425526
425520  cmp  $0x1,%al
425522  jne  0x425536           ; 其它值 → 干脆不设 dest blend
425524  push $0x2               ; 1 → DESTBLEND = ONE
```

而第 133 轮从 `PrimSetBlend`(78) 的 handler 抄出来的事实是：它**只接受 0..1**、
直接写 **prim+8**。两边是同一个域，含义一一对应：

```text
prim+8 = 0  →  SRCALPHA / INVSRCALPHA  →  over
prim+8 = 1  →  SRCALPHA / ONE          →  加色
```

所以引擎里的"混合字节"不是近似，就是 D3D9 的 `DESTBLEND` 选择器；我们的
`GFX_F_BLEND`（`src/gfx.h`：0 = normal、1 = 加色）在语义上与原版一致，
**加色模式是原版真实用到的（4 处调用点）**，不是我们为了效果自己加的。

### 结论

1. **"直通 alpha over" 现在有结构分析依据**：`SRCBLEND=SRCALPHA` + `DESTBLEND=INVSRCALPHA`
   + `ALPHABLENDENABLE=TRUE` + `BLENDOP` 默认 `ADD`。第 15 轮的 54 候选拟合结论与此
   一致，但它现在只算**验证**，不是依据。
2. **不需要实现 `BLENDOP` 的其它取值**（SUBTRACT/MIN/MAX）：exe 从不设置。
3. **加色混合要按原版实现**（`SRCALPHA`/`ONE`），而不是"加色近似"；它在原版里由
   prim+8 的 1 触发。
4. 覆盖写（`ONE`/`ZERO`）只服务于 `Clear` 那条路径，渲染图元时不会用到。

## 原版的 D3D9 设备创建参数：后台缓冲到底有没有 alpha（2026-11-25）

上一节把混合状态从 exe 抄出来了（`SRCALPHA`/`INVSRCALPHA`、`ALPHABLENDENABLE=TRUE`、
`BLENDOP` 从未设置）。还剩一个真问题：**后台缓冲有没有 alpha 通道**？这决定
"加色混合时目标 alpha 怎么算"到底会不会影响画面。答案在 `CreateDevice` 的
`D3DPRESENT_PARAMETERS` 里，抓帧永远拿不到。

`scripts/extract-exe-d3d-present.py` → 基线 `ref/exe-d3d-present.json` →
门禁 `tests/verify_exe_d3d_present.py`（`run_all` 54 → 55 步）。

### 方法

1. 找 `call 0x44672c`（`d3d9.dll!Direct3DCreate9` 的 IAT thunk）→ 它所在函数就是
   D3D 初始化；返回的 `IDirect3D9*` 存进 `this+0x4`；
2. 同一函数里找 `push $0x38; push 0; push %ptr; call 0x447b70`（memset）——
   **0x38 = 56 字节 = `sizeof(D3DPRESENT_PARAMETERS)`**，由此确定两套参数的成员基址
   （窗口模式 `this+0x14c`、全屏模式 `this+0x1bc`）；
3. 用极简寄存器跟踪（`xor %ebx,%ebx` / `mov $imm,%reg` / `mov %reg,%reg` /
   `mov 0xNN(%reg),%reg`）把每个 `mov …,0xOFF(%esi)` 还原成 `字段 = 值`
   （字段下标 = `OFF - 基址`）；
4. 找 `mov 0x40(%ebx),%eax; call *%eax`（`IDirect3D9::CreateDevice`，vtable 0x40）
   并把 7 个 push 按 `__stdcall` 顺序还原成实参。

顺带一个**交叉验证**：同一个 `IDirect3D9` 对象上还出现过 0x18/0x1c/0x20 三个 vtable
调用，实参形态分别是 `(Adapter, Format)`、`(Adapter, Format, Mode, &mode)`、
`(Adapter, &mode)` —— 正好对上 `GetAdapterModeCount` / `EnumAdapterModes` /
`GetAdapterDisplayMode`（第 6/7/8 号方法），0x40 = 第 16 号 = `CreateDevice`。
这组三对一的自洽性是"vtable 偏移没读错"的直接证据。

### 抄出来的参数

| 字段 | 窗口模式（`this+0x14c`） | 全屏模式（`this+0x1bc`） |
|---|---|---|
| `BackBufferWidth/Height` | 0（之后由设置对象的 `+0x68/+0x6c` 填） | 设置对象 `+0x60/+0x64` |
| **`BackBufferFormat`** | **`D3DFMT_UNKNOWN`(0)** | **`D3DFMT_A8R8G8B8`(21)** |
| `BackBufferCount` | 1 | 1 |
| `MultiSampleType/Quality` | 0 / 0 | 0 / 0 |
| `SwapEffect` | **`COPY`(3)** | **`COPY`(3)** |
| `hDeviceWindow` | 0 | 0 |
| `Windowed` | **TRUE** | **FALSE** |
| `EnableAutoDepthStencil` | FALSE | FALSE |
| `AutoDepthStencilFormat` | D16（字段写了但没启用） | D16（同） |
| `Flags` | 0 | 0 |
| `FullScreen_RefreshRateInHz` | 0 | 0 |
| `PresentationInterval` | `DEFAULT`(0) | `DEFAULT`(0) |

`CreateDevice`（`0x42f218`，vtable 0x40，6 个实参）：

```text
Adapter            = 0            (D3DADAPTER_DEFAULT)
DeviceType         = 1            (D3DDEVTYPE_HAL)
hFocusWindow       = 来自设置对象
BehaviorFlags      = 0x24         (MULTITHREADED | SOFTWARE_VERTEXPROCESSING)
pPresentationParameters = this+0x14c（窗口）或 this+0x1bc（全屏，显示模式 == 2 时）
ppDevice           = this+0x8
```

### 结论：后台缓冲的 alpha 不参与显示

* **窗口模式**用 `D3DFMT_UNKNOWN` —— 设备跟随当前桌面显示模式（通常 `X8R8G8B8`），
  没有 alpha 语义；
* **全屏模式**虽然是 `A8R8G8B8`，但 `Present` 把后台缓冲送显示器时只取 RGB，
  alpha 不参与显示。

所以：**混合公式里"目标 alpha 怎么算"不影响可见结果**。我们的渲染器在加色路径里
把目标 alpha 直接写 255（`src/gfx.c` 的 `blit_sprite`），与"原版后台缓冲没有 alpha
语义"并不冲突 —— 这一条现在有结构分析依据，不是"我们没做"。

顺带也解释了两件小事：`SwapEffect=COPY` + `BackBufferCount=1` 说明它是最朴素的双缓冲
（不是 FLIP 链），`PresentationInterval=DEFAULT` 说明**没有显式开/关垂直同步**
（窗口模式下 D3D9 默认按驱动行为，抓帧时"相位不同"、偶发撕裂就来自这里）。

## 逐层仿射合成 `gfx_draw_affine_layers()`（2026-11-26）

标题背景动画用。接口：`layers[]`（每层给出 slot / 图头 xy / OP / 锚点 / 缩放 per-mille /
角度单位 0.05° / alpha），`div` = 内部分辨率除数。

变换（与 `docs/opening.md` §3 一致）：

```
屏幕 = 锚点 + S·R·(图头 + 源像素 − OP)
```

实现要点：

* **逆映射 + 双线性**：屏幕点反算源坐标再取 4 邻域（比正映射无孔洞）；
* **16.16 定点增量**：每行算一次 u/v，行内只做整数加法（`u += du`），
  不再每像素 4 次浮点乘加；
* **`div=1` = 全分辨率**（默认，直接写画布，没有二次放大）；`div>1` 先合成到
  `(w/div)×(h/div)` 缓冲再 `zoom_warp_rgba()` 放大（`--title-bg-div` 可调）；
* **每层两个缓存**（都按 `compo_version` 失效，`slot_drop_compo()` 里一起释放）：
  `compo_opaque`（alpha 全 255 → 跳过 alpha 采样与混合除法）、
  `compo_mask`（8×8 覆盖掩码 → 稀疏的羽毛/风车层整屏扫描时先粗剔除）；
* **锚点是画面坐标**：`anchor = OP − 背景裁剪原点`（OP 在设计画布坐标系里）。
  否则整层恒定平移；自检不变式 = "缩放 1000‰/角度 0 时与静态直贴逐字节相同"；
* **像素中心约定**：屏幕像素中心反算到源坐标后要减 0.5（源像素序号 + 权重），
  末行/末列把基址退到 `cw-2`/`ch-2` 并给满权重，保证恒等映射逐字节复制；
* **合成缓冲初值透明**：没有被任何层覆盖的像素保持 `a=0`，`div>1` 的放大路径会把
  它写成不透明——所以调用方必须保证最底层（不透明背景）真的盖住整屏，
  否则会出现黑边（五彩的 A 层 2400×1350、1300‰→900‰ 绕中心缩放，始终覆盖屏幕）。

## 文本窗口的几何接口（2026-11-26）

| 命令 | 原版 handler | gfx 接口 | 语义 |
|---|---|---|---|
| `TextBuff(110)` | `0x43C830` → `0x430EA0` | `gfx_text_set_buff()` / `gfx_text_get_buff()` | 建/改**离屏面板**：`w*h*4` RGBA 缓冲（tw+0x520），尺寸写 tw+0x5DC/+0x5DE；缺省 8、上限 0x400，任一分量越界整条放弃 |
| `TextSize(126)` | `0x43CD60` | `gfx_text_set_size_ex()` | **字模格**宽/高（tw+0x592/+0x594，12..64 / 8..32），两个分量各自要求 int + 范围，不满足的那个保持原值 |
| `TextPos(122)` | `0x43CA40` | `gfx_text_set_pos_ex()` | **面板内光标**（tw+0x5E8/+0x5EA），逐轴要求 int；两款作品**一次都没调用** |
| `TextOutSize(120)` | `0x43C960` | `gfx_text_set_out_size()` | 装饰参数 a(0..12)/b(0..8) → tw+0x595/+0x596，逐分量校验 |

图元驱动模式（默认）下：换行宽度 = 面板宽、行数上限 = 面板高 / 行高、字号 = 字模格高
（只在该层真的被 `TextSize` 写过时；`size_h_set` 标志）。旧路径
`--text-fixed-layout` 不读这些，冻结像素基线不变。

## 图元父链（A 类基线）

`ref/exe-prim-hierarchy.json`（`scripts/extract-exe-prim-hierarchy.py` 生成，
`tests/verify_exe_prim_hierarchy.py` 门禁）冻结了引擎的图元结构与派发：
基址 `engine+0x61DD64`、步长 `0x34`、4096 项；`+0x0C` 父 id、`+0x10` 兄弟 id、
`+0x2A` 第一个子图元；派发 `0x438AB0` 把**父链上各级 x/y 相加**（组迭代时
`x = 组.x + 传入x`）。渲染端 `prim_abs_xy()` 实现的就是这条不变式；
type-5 文本的层号在 `+0x32`，窗口内嵌 surface 在窗口 `+4`。

### 文本笔位与对象表（2026-11-26 本轮）

排版用的笔位是 `tw+0x5E8/+0x5EA`（字形落位 `0x424550` 直接读），排版起点
`pen_x = tw+0x5AC`、`pen_y = 0`（`0x4337A9..0x4337BE`），每段前再做一次
`pen_x += (右边界 − 左边界)/2`（`0x433037..0x433048`）——**每行水平居中、垂直顶端
对齐**。`0x433A91..0x433AFC` 会从一块数据一次性填面板宽高/笔位，是"窗口版式记录"
的加载点（下一轮查这块数据的格式）。

引擎自建对象表（`ref/exe-engine-objects.json`）：`0x448833` 是
`Construct(base, elemsize, count, ctor)`；初始化里共 14 张表，其中
`sprite`（`0x101064`，`0x51C`×4096）、`prim_alt`（`0x61D064`，`0x34`×64）、
`prim`（`0x61DD64`，`0x34`×4096）三张**首尾相接**，`prim` 的三元组与
`ref/exe-prim-hierarchy.json` 互为独立佐证。
