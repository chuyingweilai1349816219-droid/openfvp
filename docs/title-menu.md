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

### 4.1 还差什么

1. **主标题背景**：**星空的记忆已解决** —— `title_bg1`（graph.bin 索引 987，
   1920×1080，晴天海景）就是标题背景，外壳现在把它铺在最底层（日志
   `title: background 'title_bg1' loaded`）。原版在它上面还叠了一层**提亮/光晕**
   （原版截图整体更白），那层参数还没解出来，所以我们的配色比原版更饱和。
   五彩斑斓的世界**有强候选**：`title_bg1_A`（graph.bin 索引 1025，2400×1350）——
   把它的**中心 1920×1080 裁剪**和原版标题帧对比，平均差 17（整帧平均 48），
   构图一致；差 17 的原因是原版在那张图上做了**缓慢平移/缩放动画 + 整体提亮**。
   还没接进外壳（需要"裁剪后贴"的画法），见下一条。
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