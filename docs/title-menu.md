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

## 3. 还差什么（按顺序）

1. **字体来源未定**：游戏 `font/` 里只有 Sarasa Gothic（cjk），而菜单是**衬线拉丁体**，
   说明来自系统字体或引擎内建资源——这一条还没查清（`docs/original-verification.md` §5.1）。
   在查清前，外壳里先用内建位图字体按 56 px 缩放画出即可，几何仍然对得上。
2. **标题背景图是哪个 graph id 未定位**：原版标题背景是天空 CG + logo，属于引擎侧资源
   引用，脚本里没有对应命令；需要在 exe 里找常量。
3. **外壳接线未做**：桌面外壳目前直接跑脚本；接上以后需要
   `TITLE → （New Game / Continue）→ 脚本` 的状态机、鼠标移动/点击喂给
   `fvp_title_move/click`、以及 `Continue` 的存档可用性。
4. **过渡动画**：原版点 `New Game` 后是白屏过渡再进 OP 影片，这一步依赖 WMV（还没做）。

---

## 4. 复现方式

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