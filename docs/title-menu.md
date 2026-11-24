# 标题菜单（引擎侧 UI）

> 以下内容由 DeepSeek 生成,不一定代表作者个人意愿

原版的标题菜单**不是脚本画的**：2026-11-24 用原版抓帧确认，`New Game / Continue /
Album / Option / Exit` 在脚本执行之前就出现在屏幕上并等待输入
（`docs/original-verification.md` §4.1）。本引擎目前从脚本第 0 条指令跑起，所以缺的
就是这块引擎侧 UI。

本文记录：**已经量到的几何**、**已经落地的布局/命中模块**、**还差什么**。

---

## 1. 实测几何（ground truth）

来源：`out/orig-title/orig-title-005.png`，1926×1109 全窗位图，客户区 1920×1080，
客户区起点偏移 (3, 29)。度量工具：`work/measure_title.py`（墨迹 = 客户区右 34% 内
`R+G+B < 260` 且 `B ≥ R` 的像素）。

| 项目 | 墨迹 y | 高 | 墨迹 x | 宽 | 中心 |
|---|---|---|---|---|---|
| `New Game` | 511..552 | 42 | 1461..1769 | 309 | (1615, 531) |
| `Continue` | 611..652 | 42 | 1514..1769 | 256 | (1641, 631) |
| `Album` | 712..753 | 42 | 1580..1768 | 189 | (1674, 732) |
| `Option` | 811..852 | 42 | 1575..1768 | 194 | (1671, 831) |
| `Exit` | 912..951 | 40 | 1662..1768 | 107 | (1715, 931) |

三条硬指标：

* **右对齐**到 x = 1769（右边距 151 px）；
* **行距 100 ± 1 px**（中心 y 531/631/732/831/931）；
* **墨迹高 40..42 px**，按拉丁字号的常见比例反推 **56 px**；字体是**衬线体**，
  带浅色描边/阴影。

版权行 `Copyright 2011-2022 FAVORITE All Rights Reserved.` 在客户区底部 y ≈ 1026+ 的右半侧
（检测到 1958 个暗色采样点）。

---

## 2. 已落地：`src/titlemenu.c` 布局 + 命中（与渲染无关）

```c
FvpTitleMenu m;
fvp_title_init(&m);                       /* 几何默认值 = 上表实测值 */
int idx = fvp_title_hit(&m, mx, my, measure, ctx);   /* 0..4 或 -1 */
fvp_title_move(&m, mx, my, measure, ctx);            /* 更新 hover */
int picked = fvp_title_click(&m, mx, my, measure, ctx); /* 命中且 enabled → 0..4 */
```

设计要点：

* **右对齐靠测宽**：模块本身不碰字体，只收一个 `FvpTitleMeasure` 回调
  （`int (*)(void *ctx, const char *utf8, int font_px, int *width)`）。
  有字体就用真宽度，没字体回落到 `字符数 × 字号/2` 的等宽估计——**两种情况下右边界
  都严格是 1769**。
* **墨迹框 ≠ 点击带**：`fvp_title_item_rect()` 返回墨迹框（高 = 字号 3/4 = 42，
  与实测 40..42 吻合）；`fvp_title_hit()` 在此基础上横向各放宽 24 px、纵向补足到
  整行 100 px——原版悬停/点击的行高就是 100。
* `enabled[]` 用来表达"没存档时 Continue 是灰的"这类状态（数据来源是 `save/` 目录，
  由外壳填）。

测试 `tests/test_titlemenu.c`（**90 项**，不需要游戏数据）：用"假测宽"复现上表的实测
宽度，逐项断言右边界 = 1769、左边界 ±1 px、墨迹中心 y ±2 px、墨迹高 ±2 px，以及
命中/悬停/点击/禁用项/无字体回落/NULL 安全。

### 2.1 像素级渲染验证（`tests/test_titlemenu_draw.c`，68 项）

光有布局对是不够的，还得证明"画出来真的落在那个位置"。为此给 gfx 加了两个
引擎侧 UI 用的 API：

```c
int gfx_text_width(Gfx *g, const char *utf8, int px);          /* 量宽 */
int gfx_draw_text_line(Gfx *g, const char *utf8, int x, int y, int px,
                       int r, int gr, int b, int alpha);       /* 画一行 */
```

它们不带文本窗口的排版/换行/对话框，只按字号挑字体逐字画；**字号和字体自带字号
不同时按比例缩放**（内建 8x16 位图放大到 56 px → 一个字 28 px 宽、墨迹高约 42，
正好对上原版实测的 40..42）。注意顺序：`gfx_render()` 会按图元/文本窗口重建画布，
所以引擎侧 UI 的叠加文字必须在 render **之后**画（外壳里也是这个顺序）。

测试在 1920×1080 画布上按布局画 5 项，再扫墨迹包围盒断言：

| 断言 | 结果 |
|---|---|
| 5 项墨迹右边界 | 全部落在 1766..1769（原版 1768/1769） |
| 行距 | 每两项中心差 100 ± 2 px |
| 墨迹高 | 30..60 px（内建字体实测 ≈42，与原版 40..42 一致） |
| 墨迹上沿 | 布局 y 到 y+10 之间（内建位图字形自带约 7 px bearing，属字体度量差异） |
| 命中一致性 | 每项中心点的 `fvp_title_hit()` 都返回该项 |

`make check` 现在跑 23 个单测。

### 2.2 外壳接线（`--title-menu`）与实测对照

桌面外壳新增 `--title-menu`：启动后**不**立刻跑脚本，而是画标题菜单并等输入；
点 `New Game`（或回车）才 `fvp_vm_start()`，点 `Exit` 关窗口，
`Continue`/`Album`/`Option` 目前只记录（还没有存档界面/相册/设置页）。
鼠标位置用外壳已有的 `canvas_x/canvas_y`（1920×1080 客户坐标），
命中判定直接复用 `fvp_title_move/hit/click`。

```powershell
fvp.exe --game <游戏目录> --script iroseka.hcb --title-menu --run-seconds 3 `
        --no-audio --scale 1 --screenshot out\title-ours.bmp
python work\measure_ours.py out\title-ours.png
```

同一套度量方法量我们自己画的标题菜单（1920×1080 画布，白字黑底）：

| 项目 | 原版实测 | 本引擎实测 | 差 |
|---|---|---|---|
| 墨迹右边界 | 1768 / 1769 | **1766..1768** | ≤ 3 px（右对齐列一致） |
| 行距（相邻项中心差） | 100 ± 1 | **100**（538→638→738→938） | 0 |
| 墨迹带 y（首项） | 511..552 | 516..560 | 上沿 +5（内建字形 bearing） |
| 墨迹高 | 40..42 | 45（`Option` 含降部 52） | 字体不同：内建位图 vs 原版衬线体 |
| 左边界 | 与字宽相关（1461..1662） | 1546..1659 | 字体不同必然不同，**右对齐才是硬指标** |

注意：`gfx_render()` 会按图元/文本窗口重建画布，所以菜单覆盖层必须在
**render 之后**画；截图路径同理（`--screenshot` 里也要补画一次，否则截出来是黑的）。

---

## 3. 菜单项是**预渲染贴图**（2026-11-24 查清，取代"字体未定"的说法）

对 `graph.bin` 逐项 dump 之后找到了标题素材，并用"文字掩码模板匹配"在原始标题帧里
逐一验证（`work/match_title_buttons.py`）：

| graph.bin index | 名字 | 尺寸 | 是什么 |
|---|---|---|---|
| 1049 | `title_start` | 340×80 | **「New Game」整张图**（衬线字 + 白色外发光都烘焙在图里） |
| 1050–1054 | `title_start1..5` | 340×80/91 | 其余菜单项贴图（含状态变体） |
| 1042 | `title_logo1` | 1150×283 | 作品 logo |
| 744 | `logo_bg` | 1920×1080 | 近乎纯白（FAVORITE splash 用） |
| 745 | `logo_favo` | 868×133 | FAVORITE logo |
| 284 | `a_top_s1` | 1920×1080 | 标题画面的一版背景（画面上写着 `ALBUM MODE`，夜空） |
| 282/283 | `a_top_p1/p2` | 1760×632 / 775×632 | 标题画面的局部零件 |

模板匹配结果（`title_start` 对 `out/orig-title/orig-title-005.png` 客户区）：

```
text bbox in tile: x=15..324  y=19..60   (2434 个暗色像素)
best match: 2243/2434 = 92.2%   文字落在 (1460, 511)
→ 贴图左上角 = (1445, 492)，墨迹右边界 = 1769
```

和 §1 的实测表逐项都能对上：

* 贴图 y + 19 = 511 = 实测「New Game」墨迹上沿；
* 贴图 x + 324 = 1769 = 实测 5 项墨迹右边界；
* 行距 100 → 第 2 张图 y = 592 → 墨迹上沿 611 = 实测「Continue」611。

所以原版标题菜单就是**几个贴图按固定位置贴上去**，字号/字体/阴影全在贴图里——
"菜单字体"根本不存在。`titlemenu` 模块因此新增**图像模式**：
`fvp_title_set_images(m, 1)` 之后 `fvp_title_item_rect()` 返回贴图矩形
（340×80 @ `(1445, 492 + i*100)`）、`fvp_title_image_pos()` 给贴图左上角，
命中带仍是整行 100 px；文字模式保留作为"没有素材时的退化方案"。

---

## 4. 还差什么（按顺序）

1. **主标题背景是哪个 graph 还没指认**：`a_top_s1` 是 ALBUM MODE 那版（夜空），
   白天天空那版还没在 graph.bin 里找到（`a_top_p1/p2` 尺寸对不上 1920×1080）。
2. **外壳改成画贴图**：现在 `--title-menu` 用文字模式（内建字体 56 px）画，
   几何对得上但字形不同；接上 `gfx_graph_load` + 图元之后改成贴 `title_start*`。
3. **Continue 的可用性**：取决于 `save/` 目录里有没有槽（目前一律可用）。
4. **过渡动画**：原版点 `New Game` 后是白屏过渡再进 OP 影片，这一步依赖 WMV（还没做）。

---

## 5. 复现方式

```powershell
# 量原版（需要玩家自己的游戏；见 docs/original-verification.md）
python fvp\work\measure_title.py out\orig-title\orig-title-005.png

# 跑本模块的单测
cd fvp
mingw32-make -f Makefile test_titlemenu
bin\test_titlemenu.exe
```

> 本项目由 **6amm** 辅助 DeepSeek 完成，其中所有代码由 DeepSeek 生成。
> 赞助：<https://afdian.com/a/6ammm>