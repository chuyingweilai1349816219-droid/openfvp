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

### 3.1 更正：iroseka 的 `title_start*` 是「New Game」的状态变体，不是 5 个菜单项

上一版把 `title_start` / `title_start1..5` 当成 5 个菜单项的贴图，这是错的。
2026-11-24 把 6 张图都看了一遍：

| 素材 | 内容 |
|---|---|
| `title_start`(340×80) | `New Game` 正常态（白边 + 深蓝字） |
| `title_start1`(340×91) | `New Game` **选中态**（青色发光 + 下面带「はじめから」注音） |
| `title_start2..5`(340×80) | `New Game` 的其它变体（不同发光/淡出） |

也就是说 iroseka 只有"New Game"这一项有贴图，`Continue/Album/Option/Exit` 是引擎
**用字体画的**（这也解释了为什么 `font/` 里的字体、以及标题上那套衬线体只出现在
文本项上）。星空的记忆则相反：`title_start`(275×40) 就是文字本身，
`title_start_ray` 是发光层，两项分开贴。

因此 profile 里 **iroseka 的 asset 全部留空 → 走文字模式**，
hoshimemo 的 asset 指向各自贴图 → 走贴图模式。

### 3.2 外壳现在真的会贴图（`gfx_draw_image_at`）

新增 `gfx_draw_image_at(gfx, slot, x, y)`：把图形槽里的图按
`slot_compose() + blit_sprite()` 直接贴到画布（和精灵渲染同一条路径），
在 `gfx_render()` **之后**调用（和文字覆盖层同一时机）。

`--title-menu` 的流程变成：

1. 按 profile 找当前作品 → 把 `asset` 名装进图形槽 `0xF00+i`；
2. 装上了 → 覆盖层逐项 `gfx_draw_image_at()`（星空的记忆：真贴图）；
3. 一张都没装上（五彩斑斓的世界：没有菜单贴图）→ 退回文字模式（内建字体）。

实测：星空的记忆 `title: 5/5 menu textures loaded (slots 0xF00..0xF04)`，
截图出来就是原版那套"白色描边 + 青色发光"的字样、居中、行距 90；
五彩斑斓的世界 `title: no usable texture, falling back to text`，右对齐 1769 不变。

## 4. 每作品 profile：**两部作品的标题菜单布局不一样**

2026-11-24 用同样的方法量了《星空的记忆 HD》的原版标题画面
（`work/measure_hoshi.py`，原始帧 `out/orig-hoshi2/frame-004.png`），结果和 iroseka
差别很大：

| | 五彩斑斓的世界 HD | 星空的记忆 HD |
|---|---|---|
| 对齐 | **右对齐**到 x=1769 | **居中**于 x=960 |
| 行距 | 100 | **90** |
| 第 1 项墨迹上沿 | 511 | 581 |
| 墨迹高 | 40..42（字号 56） | **33..34**（字号约 45） |
| 菜单项 | New Game / Continue / Album / Option / Exit | **GAME START** / CONTINUE / ALBUM / OPTION / **EXIT** |
| 贴图 | `title_start`(340×80) 等，**字 + 发光烘焙在同一张图** | `title_start`(275×40) + `title_start_ray`(1920×20)，**字与发光分开**；其余项同理 `title_continue`/`title_album`/`title_option`/`title_end` |

星空的记忆实测墨迹（客户区 1920×1080）：

| 项目 | 墨迹 y | 高 | 墨迹 x | 中心 |
|---|---|---|---|---|
| GAME START | 581..614 | 34 | 830..1088 | (959, 597) |
| CONTINUE | 671..704 | 34 | 856..1064 | (960, 687) |
| ALBUM | 761..793 | 33 | 891..1028 | (959, 777) |
| OPTION | 851..884 | 34 | 884..1036 | (960, 867) |
| EXIT | 941..973 | 33 | 914..1005 | (959, 957) |

所以 `titlemenu` 模块把几何抽成 **profile**（`FvpTitleProfile`）：
`fvp_title_profile_find("Hoshimemo_HD.hcb")` → `fvp_title_profile_hoshimemo`，
`("iroseka.hcb")` → `fvp_title_profile_iroseka`；每个 profile 里带
对齐方式、行距、首行墨迹上沿、每一项的标签 / 贴图素材名 / 贴图尺寸 / 实测墨迹矩形。
外壳按游戏目录自动选 profile，`fvp_title_apply_profile()` 灌几何，
命中用 `fvp_title_profile_hit()`（直接用实测墨迹矩形，不需要字体）。

**本引擎渲染实测对照**（`--title-menu` + 截图 + `work/measure_ours_box.py`）：

| | 原版 | 本引擎 | 差 |
|---|---|---|---|
| iroseka 墨迹右边界 | 1769 / 1769 / 1768 / 1768 / 1768 | 1767 / 1767 / 1768 / 1766 / 1762 | ≤ 6 px |
| iroseka 行距 | 100 | 100 / 101 / 102 / 100 | ±2 |
| hoshimemo 中心 x | 959 / 960 / 959 / 960 / 959 | 957 / 958 / 957 / 958 / 957 | ≤ 3 px |
| hoshimemo 行距 | 90 | 90 / 89 / 91 / 89 | ±1 |

（字宽/墨迹高不同是字体差异——原版是衬线贴图，本引擎退化时用内建位图字体；
**对齐基准与行距是对齐的**，这正是 profile 要保证的东西。）

新增第三部作品时只需要：量出它的墨迹表 → 在 `titlemenu.c` 里加一个 profile →
`fvp_title_profile_find()` 加一个关键字。缺 profile 时退回 iroseka 的通用布局并打日志。

### 4.2 Continue 的存档列表（只读）

点 Continue 会打开存档列表：`src/savelist.c` 把 `<game_dir>\save\sNNN.bin` 逐个
解析成一条记录（时间戳 7 字节、章节名 cp932→UTF-8、当前行、缩略图标记），
按 `x=120, y=150, row_h=64, width=1680` 的带画出来，行文本形如
`No.481   2023/03/19 23:43   プロローグ`。命中判定按行带算（`fvp_savelist_hit`）。

- Windows 下 CJK 路径只能走宽字符 API，所以**目录枚举与读文件在外壳里做**
  （`FindFirstFileW` + `_wfopen`），`savelist.c` 只吃"槽文件字节"
  （`fvp_savelist_add_slot_bytes`），保持可移植；
- 槽尾的字符串池（`src/savetail.c`，见 `docs/save-format.md` §2.5.1）里能解出
  当时的**背景图名**（例如 s905 的 `BG063_000b`），行文本会显示成
  `No.905   2023/03/19 23:43   [BG063_000b] プロローグ`；解不出来就不显示；
- 选中一行只记日志（`save list: selected No.481 ...`）并**停在列表里**——
  槽尾 500–597 KB 的引擎状态快照还没解，恢复不了，所以不启动脚本（如实标注）；
- 调试/截图入口：`--title-save-list`（等价于启动即打开 Continue 列表）；
- 实测：五彩斑斓的世界 6 个可读槽（第 7 个 `save.bin` 是全局状态不是槽），
  星空的记忆 0 个（没有 `save/` 目录）；这两条都进了多游戏门禁。

### 4.3 键盘导航

`fvp_title_nav(m, +1/-1)` 上下移动 hover（**跳过 `enabled[] == 0` 的项**、到边界环绕、
全禁用时返回 -1），`fvp_title_activate(m)` 确认当前项（没有 hover 时退到第一个可用项）。
外壳把 ↑/↓/Enter/Esc 做成窗口输入里的 `nav` 位掩码（`FVP_NAV_UP/DOWN/LEFT/RIGHT/
ENTER/ESC`），**只有菜单打开时**才让窗口捕获取消 ESC 的"直接退出"语义
（`input.capture_nav`），菜单关掉后 ESC 仍是退出。

单测（`test_titlemenu`，共 227 项）覆盖：从无 hover 开始、连续上下、反向环绕、
禁用项跳过、全禁用、激活回退、NULL。

### 4.4 hover 态（选中状态贴图）

星空的记忆的菜单素材是"文字 + 发光"两张图：`title_start`（文字，275×40）与
`title_start_ray`（整行光带，1920×20），其余四项同理。外壳现在把**发光层也装进
图形槽**（`0xF08+i`），鼠标悬停到第 i 项时先把光带贴上去、再贴文字：

- 光带横向居中、竖向对准该行墨迹中心（`gfx_image_size` 取真实尺寸后算）；
- 五彩斑斓的世界没有发光素材（`asset_glow` 为空）→ 只走文字模式的高亮色；
- `--title-hover N` 是**截图/门禁用的钩子**：把第 N 项锁定为 hover（不随鼠标变化），
  这样"hover 态"可以被像素级验证。

实测（星空的记忆，hover 第 2 项 CONTINUE）：与无 hover 截图相比 **34,128 个像素不同**（逐像素），
且**全部**落在第 2 项那一行（y≈681–686），正是原版选中时那条横贯全屏的青色光带。

### 4.5 标题背景（星空的记忆 `title_bg1`）

`title_bg1`（graph.bin 索引 987，1920×1080）就是星空的记忆的标题背景：晴天海景。
外壳把它装进 `0xFF0` 槽，在贴图模式下**最先**画（铺满整屏），再画发光层、菜单文字。
实测 `--title-menu` 截图和原版标题帧的构图一致（同一张图、同一比例），差别只是原版
额外叠了一层整体提亮/光晕（未解）。

五彩斑斓的世界没有对应材质（白天那版背景仍未在 graph.bin 里找到），所以它仍是
黑底 + 文字菜单；`docs/title-menu.md` §4.1 有记录。

### 4.6 关键发现：**素材头里自带摆放坐标**

HZC1 图片头除了宽高，还有 `x/y` 两个字段；引擎渲染时 `img.x/img.y` 直接进坐标计算
（`render-model.md` §2.1）。所以标题 logo、版权行、菜单按钮的**摆放位置本来就存在
游戏素材里**，不用从截图反推。实测（星空的记忆）：

| 素材 | 尺寸 | 头里的 x/y | 和之前从截图量的墨迹框对比 |
|---|---|---|---|
| `title_bg1` | 1920×1080 | (0,0) | — |
| `title_logo1` | 1082×418 | (419,72) | — |
| `title_continue` | 230×40 | (845,670) | 墨迹 856..1064 / 671..704 → 正好差 11/1 px（贴图内留白） |
| `title_end` | 110×40 | (905,940) | 墨迹 914..1005 / 941..973 → 同样差 11/1 px |
| `title_copyright` | 445×24 | (737,1043) | 屏幕最底部 |

所以外壳现在**优先用素材头的 x/y** 摆放（`gfx_image_offset()`），只有算不出来时才退回
profile 里按墨迹框反推的位置。五彩斑斓的世界同理：`title_logo1` 1150×283 @ (701,149)，
现在也画在正确位置了。

**标题画面现在的画法**（顺序即原版层次）：背景 → logo → 菜单项（hover 时先垫发光层）
→ 版权行。

### 4.7 两作标题画面现状（2026-11-25）

| | 五彩斑斓的世界 | 星空的记忆 |
|---|---|---|
| 背景 | `title_bg1_A` 2400×1350，**中心裁剪** (240,135) → 1920×1080 | `title_bg1` 1920×1080，整屏 |
| logo | `title_logo1` 1150×283 @ (701,149) | `title_logo1` 1082×418 @ (419,72) |
| 菜单 | 文字模式（没有菜单贴图） | 5 张原版贴图 + 发光层，位置取自素材头 |
| 版权行 | 素材未指认 | `title_copyright` 445×24 @ (737,1043) |
| 与原版整帧平均差 | **14.9**（未接背景时 48） | 25 左右（差在提亮层） |

"裁剪贴图"为此新增了 `gfx_draw_image_cropped()`：从图形槽里的图取一块贴到画布，
越界自动裁掉。背景比屏幕大时外壳自动走裁剪路径（profile 的 `bg_src_x/bg_src_y`
给出裁剪起点）。

**仍未解**：原版背景上那层整体提亮/光晕（两作都有，原版明显更白）。这条直接影响
整帧差异（iroseka 的 14.9 主要来自它），需要找到引擎的调色/叠加参数才能继续收敛。

### 4.8 开场序列（`--opening`，2026-11-25）

原版启动后先是一串 logo 淡入淡出（`docs/original-verification.md` §6 的抓帧时间线），
然后标题画面从白色里淡入。实测素材：`logo_bg` 是**纯白 1920×1080**，
`logo_favo` 是带 alpha 的 FAVORITE logo（868×133 @ (526,407)）。

实现：
- `src/openseq.c/.h`：纯时间轴（不碰 gfx）——白底 300 ms → logo 淡入 1200 →
  停留 2100 → 淡出 1200 → 标题从白色淡入（白色遮罩 255→0）600，合计 **5400 ms**；
- 外壳 `--opening`：按相位画"白底 + logo（按 alpha）"，最后一段先画标题再压白色遮罩；
- `--shot-at N`：跑到 N 毫秒时存一张截图（中间态验证用；结尾不再覆盖同一路径）；
- 单测 `test_openseq`：**388 项**（相位边界、alpha 单调/有界、白底阶段两个 alpha 为 0、
  钳制、NULL 安全）；
- 门禁：`verify_multigame` 新增开场阶段，抓 200 / 1500 / 6200 ms 三帧，断言
  "白场没有 logo 像素 → 1500 ms 有 logo → 序列跑完进标题"。

**如实说明**：时长与缓动是**近似值**（原版整段约 10 s，含第二个 logo 与
`logo_favo_view` 分件动画）；这里先把"白底 + 主 logo + 白场进标题"这条主干做通，
分件与第二个 logo 留给后续轮次。

### 4.9 背景叠加层与菜单配色（2026-11-25）

iroseka 的 `title_bg1_A` 只是"云"，原版标题里的**风车、飘落花瓣、光束**在
`title_bg1_C / D / B` 三层里（实测三种叠加顺序的平均差只差 0.6，这里用测到最好的
C→D→B）。合成结果与原版帧的整帧平均差从 **17.5 降到 13.7**；目视差异（风车/花瓣）
比这个数字的意义更大——平均差对"细线条叠加在相近底色上"不敏感。

星空的记忆则相反：它的"整体提亮"是 `title_frame2`（纯白 RGBA，alpha 均值 72），
叠上后与原版帧的平均差从 48.0 降到 34.6，`title_frame` 再补一点暗角。

- profile 新增 `bg_overlay[4]` + `bg_overlay_count`；外壳在背景之后、logo 之前按序
  叠加（坐标取素材头里的 x/y，再减去背景的裁剪起点 `bg_src_x/y`）；
- `--title-no-bg` 现在会**整组跳过**背景与叠加层（几何测量用）。

**菜单配色**：iroseka 的菜单是深蓝衬线字，实测暗部均值 **RGB(0,19,143)**。profile
新增 `menu_color[3]`，文字模式按它上色（我们用 (10,30,140)，门禁实测渲染出的
最暗像素是 (140,30,10) 的 BGR 即 RGB(10,30,140)，与原版同色相）；hover 用同色相
提亮 30%（原版 hover 样式未实测，这条已在文档里标注为近似）。

### 4.10 存档预览（选中槽的"当时画面"）

存档列表选中某一槽时，外壳把槽尾字符串池里记的**背景图名**（例如 `BG063_000b`）
装进预览槽、等比缩到 640×360 画在右下角，并在底部写出该槽的台词。

- 它**不是读档**：不恢复任何游戏状态，只把"这个存档当时停在哪张图、哪句话"
  还原成一帧；文档与日志都写 preview，`Load(40)` 仍是"未实现"；
- 内容全部来自存档数据本身（图名来自槽尾字符串池），没有猜测；
- `--preview-slot N`：预选第 N 号槽（截图/门禁用）；
- 门禁：选中记有图名的槽（iroseka 的 s905）时必须出现
  `save list: preview graph 'BG063_000b' loaded`，且预览区域确实有非黑像素
  （实测 3600 个采样点亮）。

### 4.11 标题菜单的字体：量出来了（Palatino Linotype Bold ≈61px）

iroseka 的菜单是深蓝衬线字。用 `scripts/identify-title-font.py` 把本机 11 个
候选字体按"渲染 → 质心对齐 → ±8 px 平移搜索 → 归一化相关"与原版截图的
"New Game" 墨迹框（309×42）比对：

| 字体 | 字号 | 相关系数 | 渲染墨迹框 |
|---|---:|---:|---|
| **Palatino Linotype Bold** | **61** | **0.745** | **309×44** |
| Palatino Linotype | 63 | 0.700 | — |
| Palatino Linotype Bold | 60 | 0.599 | — |
| Georgia Bold | 58 | 0.580 | — |
| Bookman Old Style | 59 | 0.541 | — |
| Times New Roman | 69 | 0.613 | — |

两条独立证据指向同一结论：**Palatino 家族、粗体、约 61–62 px**——

1. 相关系数 0.745，明显高于其它候选；
2. **墨迹宽度 309 px 与原版完全一致**（高度 44 vs 42，差 2 px 来自抗锯齿阈值）。

（Palatino 的度量孪生字体 Book Antiqua 也值得试；原版确切字体名仍未知，只能说
"最接近 Palatino Linotype Bold"，这是**近似**而不是定论。）

复现：`python scripts/identify-title-font.py --sizes 58:72`（需要
`out/orig-title/orig-title-005.png`，由 `scripts/capture-original.ps1` 抓的原版帧）。

**下一步**：用 Palatino Bold 61 生成字库图集（`scripts/make-font-atlas.py`），
给标题菜单文本一条"指定字体"的通路，然后把我们渲染的 "New Game" 与原版做同样的
相关比对，作为字体还原的验收指标。

### 4.12 标题菜单接上专用字库（Palatino Bold 61）：相关度 0.27 → 0.73

按 §4.11 的结论生成了专用字库并接上：

1. **生成**（Pillow，用系统字体；图集不入库，属构建产物）：
   ```
   set FVP_FONT_TTF=C:\Windows\Fonts\palab.ttf
   python scripts/make-font-atlas.py --hcb <iroseka.hcb> --hcb <Hoshimemo_HD.hcb> \
       --size 61 --out bin        # → bin/gfx-font-61.bin（2764 字形）
   ```
   `tests/gfx_dump.py` 的 TTF 选择新增 `FVP_FONT_TTF` 环境变量覆盖（默认不变，
   所以冻结基线 `compare.py`/`verify_gfx` 完全不受影响）；
2. **接线**：`gfx` 新增 `gfx_draw_text_line_font()` / `gfx_text_width_font()`
   （指定字体绘制，脚本正文仍用按字号挑的位图图集）；外壳新增 `--title-font <图集>`，
   标题菜单的文字项用它画（字号取图集自带 61，不缩放）；
3. **验收**（同一把尺子：与原版 "New Game" 墨迹框做归一化相关）：

| 渲染方式 | 墨迹框 | 相关度 |
|---|---|---|
| 内建位图字体（之前） | 219×43 | **0.266** |
| **Palatino Bold 61 图集** | 311×44 | **0.726** |
| 原版（参考） | 309×42 | — |

墨迹宽度从 219 → 311（原版 309），相关度 **0.27 → 0.73**。剩下的差距主要来自：
我们仍然带 1 px 描边（原版是柔和阴影，阴影参数未测），以及抗锯齿阈值差异。

复现：`python scripts/identify-title-font.py --check-frame out/iro-serif.bmp`
（`--check-frame` 会把我们自己渲染的那一帧与原版做同样的相关比对）。

### 4.13 New Game 的白场过渡（`--title-fade`，默认 800 ms）

原版点 New Game 后是"白屏过渡再进正文/OP"（`docs/original-verification.md` §6）。
现在外壳这么做：

1. 选中 New Game → 白场 alpha 0→255（前半段，默认 400 ms）；
2. **白场峰值处**才 `fvp_vm_start()`（原版也是遮住画面时才切换）；
3. 后半段 alpha 255→0，露出脚本第一帧。

实现只是一个帧内状态（`title_fade_active` + 已过毫秒），白场用 `gfx_fill_canvas()`；
`--title-fade N` 可改时长（0 = 直接进，用于 A/B）；`--pick N` 是截图/门禁钩子，
把第 N 项当成"已被选中"。

实测（iroseka，`--pick 0 --shot-at 450/1600`）：450 ms 帧均值 **223**（接近全白）、
1600 ms 帧均值 **0.4**（已是脚本画面）；日志给出
`start transition -> vm started at 400 ms` / `start transition done (800 ms)`。
门禁对这两点都有断言（白场峰值亮度 + 脚本确实在过渡中启动）。

### 4.14 存档列表 → "部分恢复"视图（Load 的诚实中间态）

存档列表里按 **Enter**（或左键点同一行）会进入"部分恢复"视图：

1. 把该槽字符串池里记的**背景图**整屏铺开（`gfx_draw_image_scaled` 到 1920×1080）；
2. 在下方画出该槽记录的**台词**；
3. 左上角常驻标注 `[PARTIAL RESTORE] visual + text only`；
4. 日志写 `load: partial restore slot=N graph=... (visual+text only, game state NOT restored)`。

**它不是读档**：不恢复全局变量、文本窗口、脚本位置——按 §4.2 的静态分析结论，存档里
本来就没有 pc/栈，真正的读档需要"场景/行号重放"。这个视图的意义是把"存档里确实能
还原的东西（画面+文本）"先做出来，并且**明确标注部分恢复**，不误导。

`--load-slot N` 可直接进入该视图（截图/门禁用）；`ESC` 退回存档列表。

**顺带修的一个真实 bug**：同一存档里章节名是 CP932、汉化台词是 GBK，之前按单一编码
解码会把台词解成半角片假名乱码。现在 `src/savelist.c` 增加了"看着不对就换另一种编码"
的启发式（半角片假名/替换字符占比 > 25% 时重试），并有单测覆盖。

### 4.15 游戏内菜单（引擎侧 UI，覆盖在脚本画面上）

原版没有"画菜单"的脚本命令（命令表里只有 `MenuMessSkip` 与 `TitleMenu` 两条相关项），
游戏内菜单由 exe 自己画。所以这里和标题菜单一样属于**外壳实现**：

- 纯逻辑在 `src/gamemenu.{h,c}`：初始化（标签/行距）、居中布局、命中测试、
  键盘导航（跳过不可用项、循环）、激活/点击、`selected` 记录；
- 单元测试 `tests/test_gamemenu.c`（**50 项**）：坐标边界（左/右/行间/末项之下）、
  跳过不可用项、循环导航、打开时 hover 落在第一项、关闭后不再响应、NULL 与上限安全；
- 外壳（`src/platform/win32/main_win32.c`）：游戏中按 **ESC** 打开/关闭，
  ↑/↓ 或鼠标移动选行，Enter/左键确认；画法是**压暗背景 + 居中列出 5 项 + hover 高亮**，
  文字复用标题菜单字库（`--title-font`，没有就退回位图字体），左上角常驻 `ESC: close`；
- 动作（2026-11-25 起都是**真动作**）：
  * `SAVE`：用模板槽 + 当前画面写一份**真实存档**，但只写到 `--save-preview`
    指定的路径，**绝不写进游戏自己的 `save/`**（日志里明说 game save dir untouched）；
  * `LOAD`：先扫描 `save/sNNN.bin` 再打开存档列表（实测 6 个槽与磁盘一致）；
    真正的"读档"仍是部分恢复，见 `docs/save-snapshot.md` §4.6；
  * `OPTION`：设置面板——**主音量可调**（面板打开时 ↑/↓ 每次 ±10%，通过
    `fvp_snd_bridge_set_master()` 立即作用到正在播放的通道并读回显示；
    文字速度/跳过模式仍只显示当前值）。`--menu-adjust N` 可连调 N 步（门禁用）；
  * `TITLE` 回标题画面、`QUIT` 退出进程；
- `--menu-at N` 是截图/门禁钩子：脚本开始跑 N 毫秒后自动打开菜单。

门禁 `tests/verify_gamemenu.py`（`run_all` 第 6.5 步）：断言日志
`menu: auto-opened at <t> ms (5 item(s))` 且 t 落在请求时间附近（实测 407 ms vs 400 ms）、
截图 1920×1080×32、菜单区域有文字墨迹（实测 446 个采样点）。

**注意**：ESC 现在**不会再直接退出窗口**（`capture_nav` 常开），游戏中 ESC 变成"开关菜单"，
退出请用菜单里的 `QUIT`。这个行为变化写进了 `docs/win32-shell.md`。

### 4.1 还差什么

1. **主标题背景：两作都已接上**（见 §4.7）——
   - 星空的记忆：`title_bg1`（索引 987，1920×1080）整屏铺底；
   - 五彩斑斓的世界：`title_bg1_A`（索引 1025，2400×1350）**中心裁剪** 1920×1080。
   两作都还差原版那层**整体提亮/光晕**（原版截图明显更白），参数未解，所以我们的
   配色比原版更饱和——这是目前标题画面与原版的主要差异（iroseka 整帧平均差 14.9）。
2. **外壳改成画贴图**：现在 `--title-menu` 用文字模式（内建位图字体，字号由实测墨迹高反推）
   画，几何对得上但字形不同；接上 `gfx_graph_load` + 图元后改成贴 `title_start*`。
3. **Continue 的可用性**：已按 `<game_dir>\save\sNNN.bin` 的个数启用/禁用
   （0 个 → 置灰；点它只记日志，读档界面还没做）。实测五彩斑斓的世界 7 个槽 →
   可用，星空的记忆没有 save 目录 → 置灰。
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