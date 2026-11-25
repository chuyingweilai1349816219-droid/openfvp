# 变更记录

> 以下内容由 DeepSeek 生成,不一定代表作者个人意愿

格式参考 [Keep a Changelog](https://keepachangelog.com/zh-CN/1.1.0/)，版本号语义化。

## [1.0.0] — 2026-11-24：第一个正式版（引擎本体 + 工程完整）

引擎本体的第一个正式版。**定位是"引擎与工程完整、已知限制写清楚"，不是"能完整通关"**，
所以下面把「做了什么」和「明确不做什么」并列写。逐条可核对，不含宣传词。

### 这一版里落地的（相对 0.8.0）

- **32 个协程槽**（`ThreadStart`/`ThreadExit`/`ThreadNext`，默认开启）+ 修掉栈底帧自循环的
  VM bug；900k / 2M 两档开关文本逐字节相同（`tests/verify_threads.py` 门禁）
- **16 个定时器**（`TimerSet`/`TimerGet`/`TimerSuspend`，新模块 `src/timer.c`）
- **逐字显示（打字机）**：`TextSpeed`(129) + `TextSuspendChr`(130) 的渲染侧，
  默认关闭（`--typewriter`），UTF-8 按码点边界截断
- **纹理缓存 LRU 上限**（默认 256 MiB）+ 诊断计数
- **周期性能报告修 bug**：fps/slow 之前用全程累计值除以 5 s 窗口（135 s 会报 1606.9 fps）
- **未实现命令统计口径修正**：外壳日志从 52 种降到 25 种，剩下的是文本显示细节
- **`SaveData`(95) 记录**：查明它只打三个已读标记，不是状态摊平入口
- **测试从 20 步扩到 23 步**（新增 `verify_threads`、`test_timer`、`test_typewriter`、`test_tex_cache`）

### 1.0.0 明确不做（已知限制）

| 限制 | 影响 | 依据 |
|---|---|---|
| **游戏内存档/读档** | 游戏里点保存/读取不会真的生效 | 槽尾 500–597 KB 的引擎状态快照由 `0x43DE90` 直接序列化内部结构，字段语义未解（`docs/save-format.md` §2.5） |
| 动画缓动曲线（`mode 1..5`） | 演出是线性插值，和原版不同 | 没有静态分析证据，也未拿到原版实机对照 |
| 游戏内菜单/设置/存档界面 | 没有这些界面 | `Menu*`/`TitleMenu` 是桩 |
| WMV 视频 | 开场动画被跳过 | `Movie*` 返回"已播放完毕" |
| 语音播放 | 只有 BGM/SE 出声 | voice id 依赖存档层，未接 |
| 文本显示细节 | `TextFont`/`TextShadowDist`/`TextFormat`/`TextFunction`/`TextSkip` 是桩 | 外壳实测 25 种未实现命令里的高频项 |
| 长跑内存 | 随游程阶梯上升 | 脚本把 79 个槽（含整套 `fc_*` UI 图）一次性装载且从不释放；LRU 只能回收被覆盖过的图 |
| Android 后端 | 只有文档与设计 | 非本阶段目标 |
| 与**原版**逐像素对照 | 1.0.0 时未做 | 所有"一致"都是相对我们自己的 Python 参考实现。**2026-11-24 起已能抓到原版真值画面**（标题菜单已量出坐标，见 `docs/original-verification.md`）；正文对话框的对照仍待做 |
| 老版本 FVP 作品 | 未测试 | 只验证过两部（`iroseka` / 星空のメモリア） |

### 验收（1.0.0 发布前实跑）

| 项 | 结果 |
|---|---|
| `tests/run_all.py` | **23 项全绿**（含 VM 逐字节 / 资源 / 音频 / 存档 / flag / textattr / thumb / encode / wheel / motion / tex_cache / timer / typewriter / 协程 / fuzz 27,300 次变异） |
| `tests/compare.py --run` | PASS：6,496 B / 195 行 / `7f9837ff…b3ea2` |
| `verify_gfx.ps1 -SkipDump` | PASS：24/24 图元 0 px、293 px 差（0.0141%）、PSNR 49.94、SSIM 0.99971 |
| `verify_motion.ps1` | PASS：关两次一致；1,000 步开=关；60,000 步开≠关 |
| `verify_threads.py` | PASS：`start == exit`、开/关同字节、900k 与 2M 都对冻结基准 |
| `check-docs.py` | PASS：29 篇文档、0 缺失路径、DeepSeek 声明齐全 |
| 外壳长跑 | 15 分钟级采样：59.7–59.9 fps、`slow=0`、`underruns=0`、`exit=run-seconds` |

## [未发布] — 文本呈现命令（1.0.0 之后）

### 新增

- **标题背景叠加层 + 菜单实测配色；`run_all` 失败自动重试一次**
  - iroseka 的风车/花瓣/光束在 `title_bg1_C/D/B` 三层里（A 层只有云）：
    按 C→D→B 叠加后与原版帧整帧平均差 17.5 → **13.7**（目视差异远大于数字，
    因为细线条叠在相近底色上平均差几乎不动）
  - profile 新增 `bg_overlay[4]/bg_overlay_count`；外壳在背景后、logo 前按序叠加，
    坐标用素材头的 x/y 减去背景裁剪起点；`--title-no-bg` 整组跳过
  - 星空的记忆叠 `title_frame2 + title_frame`（纯白 RGBA 提亮 + 暗角）：
    与原版帧平均差 48.0 → 34.6
  - 菜单配色：实测原版深蓝 **RGB(0,19,143)**；profile 新增 `menu_color[3]`，
    文字模式按它上色（渲染出来是 RGB(10,30,140)），hover 同色相提亮 30%
    （原版 hover 未实测，文档标注为近似）
  - 门禁：新增"背景叠加层必须装上"与"文字菜单必须是实测深蓝"两条断言
  - `run_all.py`：每一步失败自动**重试一次**，记录 `attempts` /
    `first_attempt_failed`，重试通过时标 `FLAKY`——偶发失败不再被当噪声

- **开场序列（刪除：白底 FAVORITE logo 淡入淡出 + 标题从白色淡入）**
  - 实测澄清：`logo_bg` 是**纯白 1920×1080**，`logo_favo` 是带 alpha 的 logo
    （868×133 @ (526,407)）——所以原版开场是**白底**，不是黑底
  - 新增 `src/openseq.c/.h`：纯时间轴（白底 300 → logo 淡入 1200 → 停留 2100 →
    淡出 1200 → 标题从白色淡入 600 = 5400 ms），带 smoothstep 缓动
  - 外壳 `--opening` 按相位画；新增 `--shot-at N` 抓中间态截图
  - `gfx` 新增 `gfx_draw_image_alpha()`（整张图乘 alpha）与 `gfx_fill_canvas()`（全屏色）
  - 单测 `test_openseq` **388 项**（相位边界/单调有界/白底阶段 alpha=0/钳制/NULL）
  - 门禁：抓 200 / 1500 / 6200 ms 三帧，断言白场无 logo 像素 → 1500 ms 有 logo → 序列跑完进标题
  - 如实标注：时长/缓动是近似值（原版约 10 s、还有第二个 logo 与分件动画）

- **五彩斑斓世界的白天标题背景接上了（刪除：`title_bg1_A` 中心裁剪），两作标题画面基本完整**
  - 新增 `gfx_draw_image_cropped()`：从一个图形槽取 (sx,sy) 起的一块贴到画布，
    越界自动裁掉；背景比屏幕大时外壳自动走这条路径（profile 的 `bg_src_x/y`）
  - iroseka：`title_bg1_A`（2400×1350）中心裁剪 (240,135) → 1920×1080；
    与原版标题帧整帧平均差 **14.9**（未接背景时 48）
  - 两作标题画面现在的元素：背景 + logo + 菜单（星空是原版贴图+hover 发光，
    iroseka 是文字）+ 版权行（星空）；位置全部来自素材头的 x/y
  - 仍差原版那层**整体提亮/光晕**（两作都有，原版明显更白），参数未解，
    如实写进 `docs/title-menu.md` §4.7
  - 门禁：iroseka 现在也要求"背景装上 + 整帧过半点采样是亮的"

- **素材头里自带摆放坐标：标题 logo / 版权行 / 菜单项都按引擎自己的数据摆放**
  - 发现 HZC1 图片头的 `x/y` 就是原版摆放位置（`render-model.md` §2.1 的
    `img.x/img.y`），不用再从截图反推：`title_logo1` 1082×418 @ (419,72)、
    `title_copyright` 445×24 @ (737,1043)、`title_continue` 230×40 @ (845,670)、
    `title_end` 110×40 @ (905,940)（和之前量的墨迹框正好差 11/1 px 的贴图留白）
  - 新增 `gfx_image_offset()`；外壳优先用它摆放，算不出来才退回 profile 的
    墨迹框反推位置
  - profile 新增 `logo_asset` / `copyright_asset`；标题画面按原版层次画：
    背景 → logo → 菜单项（hover 先垫发光层）→ 版权行
  - 五彩斑斓的世界：`title_logo1` 1150×283 @ (701,149) 现在也画在正确位置
    （背景仍是黑底，因为白天背景还没接）
  - 五彩斑斓的世界白天背景**找到强候选**：`title_bg1_A`（1025，2400×1350），
    中心 1920×1080 裁剪与原版帧平均差 17（整帧 48），差在"平移动画 + 提亮"
  - `verify_multigame.py` 新增 logo / 版权行加载断言；`docs/title-menu.md`
    新增 §4.6 与 §4.1 的候选记录

- **星空的记忆的标题背景找到了：`title_bg1`，标题画面现在是一整张真背景**
  - 用 `c_assets dump/png` 把 `title_bg1/2/3`（graph.bin 索引 987/988/989）
    导出来和原版标题帧比对，**`title_bg1`（1920×1080 晴天海景）就是标题背景**
  - profile 新增 `bg_asset`；外壳把背景装进 `0xFF0` 槽、在贴图模式下**最先**
    铺满整屏，再画发光层与菜单文字；日志 `title: background 'title_bg1' loaded`
  - 五彩斑斓的世界没有对应素材（白天版背景仍未在 graph.bin 里找到）→ 保持
    黑底 + 文字菜单，`docs/title-menu.md` §4.1 如实记录
  - 原版在背景上还叠了一层整体提亮/光晕（原版截图更白），那层参数未解，
    我们的配色因此比原版更饱和——如实写进 §4.5
  - `verify_multigame.py` 新增断言：有背景素材的作品必须真的铺上（日志 + 整帧
    "过半点采样是亮的"），没有素材的作品整帧必须仍是黑的

- **标题菜单的 hover 态：贴原版发光层（星空的记忆 `title_*_ray`）**
  - 新增 `gfx_image_size()`：取图形槽里图片的真实像素尺寸（UI 排发光层要用）
  - 外壳把 `asset_glow` 素材装进 `0xF08+i`；hover 到第 i 项时先贴整行光带
    （横向居中、竖向对准该行墨迹中心）再贴文字；iroska 没有发光素材 → 仍走
    文字高亮
  - 新增 `--title-hover N` 钩子：锁定第 N 项为 hover（不随鼠标变化），让 hover 态
    可被截图/像素级验证
  - 实测（星空的记忆 hover 第 2 项）：与无 hover 截图相比 **34,128 个像素不同**，
    集中在第 2 项那一行（y≈681–686）——就是原版选中时横贯全屏的青色光带
  - `verify_multigame.py` 标题菜单阶段新增第三次跑：hover 态必须改变像素，
    且**过半差异像素落在被 hover 的那一行**
  - `docs/title-menu.md` 新增 §4.4

- **Load(40) / SaveWrite(96) 的请求链路在外壳里打通（含"不写坏档"红线）**
  - 以前 VM 只记录请求、外壳从不消费，等于两个命令是空桩。现在外壳在执行帧里
    取走请求：
  - `SaveWrite(slot)` + `--save-template`：用模板当底，替换**当前画布缩略图**、
    脚本 `SaveCreate` 暂存的三条字符串、**当前时间**，写
    `<game_dir>\save\sNNN.bin`；没有模板则**拒绝写**（槽尾语义未解，硬造只会写坏档）
  - `Load(slot)`：只记日志（快照恢复仍未实现，如实标注）
  - 新增调试钩子 `--debug-save-request N` / `--debug-load-request N`，用来在
    脚本自己调这些命令之前端到端验证链路
  - 实测：模板 + 请求 12 → 写出 616,572 B 的 `s012.bin`（时间戳=当前、缩略图=当前
    画面、尾段与模板一致，`probe_slot` 可读回）；不带模板 → 只打拒绝日志、不落文件
  - `verify_multigame.py` 新增断言：不带模板的存档请求必须被拒绝，且**游戏
    `save/` 目录条目数不变**（防写坏用户目录）
  - `docs/win32-shell.md` 新增"存档/读档请求怎么走"一节与选项表

- **把槽尾字符串池接进研究工具链并回填文档（`tailstat.py` + `save-snapshot.md`）**
  - `tools/tailstat.py` 新增 `parse_pool()` / `looks_like_graph()`（与
    `src/savetail.c` 同一规则），输出新增 `== 槽尾字符串池 @+0x19408 ==` 一节：
    s481/s485 = 1 条对话行、**s905 = 2 条含 `BG063_000b` 图名**、
    s800/s901/s906 = 没有
  - `docs/save-snapshot.md` 新增 §1.4（字符串池实测表 + 落地位置 + 复现命令），
    §4 第 1 条从"变长 DENSE 里是什么"改成"字符串池已解，其余子结构未解"，
    工具表补上 `tailstat.py --pool` 与 `scripts/analyze-save-tail.py`
  - 这样"槽尾研究"的两个文档（`save-format.md` §2.5.1 / `save-snapshot.md`
    §1.4）与两个工具口径一致、可互相复现

- **槽尾字符串池解析落地（`src/savetail.c`），存档列表显示当时的背景图名**
  - `fvp_savetail_parse()`：校验 `+0x19408` 的标签 `0x20`、读个数（≤8）、
    逐条 u16 长度 + NUL 结尾字符串、跳过条目间的 0 填充，cp932→UTF-8，
    并用"大写字母开头 + 含下划线"判定图名
  - 6 个真实槽实测：s481/s485 = 1 条（当前对话行）；**s905 = 2 条，含
    `BG063_000b` 图名** + 对话行；s800/s901/s906 = 个数 0（系统/自动槽），
    解析器如实返回"没有"而不是猜
  - 存档列表行文本变成 `No.905 … [BG063_000b] プロローグ`；解不出就不显示
  - `tests/test_savelist` 66 → **83 项**（合成字符串池的正/负例：坏标签、
    个数 0/超限、过短尾段、NULL、条目间填充、图名进入行文本）
  - 多游戏门禁新增断言：可读槽 > 0 时首行必须是 `No.NNN + 时间戳`
  - `docs/save-format.md` §2.5.1、`docs/title-menu.md` §4.2 同步

- **槽尾"引擎状态"结构研究（只读）：确认是固定版式 + 找到字符串池**
  - 新增只读工具 `scripts/analyze-save-tail.py`：解析槽头、跳过缩略图，
    统计尾段大小/非零范围/4 KiB 块相似度，并定位当前对话行在尾段里的偏移
  - 6 个真实槽实测：尾段 502,020–598,984 字节；相邻两个 4 KiB 块逐字节
    相同率 67–85%（符合结构体数组序列化）；str0 不在尾段里、str2 固定在
    +0x19412 附近
  - **+0x19408 起是一段字符串池**（u32 0x20 + u32 计数 + u16 长度前缀字符串）：
    s481/s485 是当前对话行，s905 是 `BG063_000b`（图名）+ 对话行——也就是
    槽尾里直接存了当时的背景图名；条目间对齐规则尚未钉死，工具只打印原始字节
  - 结论写进 `docs/save-format.md` §2.5.1：真 Continue 仍需要映射字符串池
    之外的段（多轮结构分析），当前列表保持只读并如实标注

- **标题菜单的键盘导航（↑/↓/Enter/Esc）+ 窗口输入加 nav 位掩码**
  - `fvp_title_nav(m, ±1)`：上下移动 hover，**跳过置灰项**、边界环绕、
    全禁用时返回 -1；`fvp_title_activate(m)`：确认当前项（无 hover 时退到
    第一个可用项）
  - `FvpWin32Input` 新增 `nav`（`FVP_NAV_UP/DOWN/LEFT/RIGHT/ENTER/ESC` 按下沿
    位掩码）与 `capture_nav`：只有菜单打开时才把 ESC 交给菜单，菜单关掉后
    ESC 仍是退出
  - 外壳：标题菜单 ↑/↓ 选择、Enter 确认；存档列表 ↑/↓ 选槽、Enter 确认、
    Esc 回标题菜单
  - `test_titlemenu` 新增 20 项导航断言（共 **227 项**）
  - `docs/win32-shell.md` 按键表、`docs/title-menu.md` §4.3 同步

- **Continue 的存档列表（只读）：新模块 `src/savelist.c` + `--title-save-list`**
  - `fvp_savelist_*`：把 `<game_dir>\save\sNNN.bin` 解析成记录（7 字节时间戳、
    章节名 cp932→UTF-8、当前行、缩略图标记、文件大小），提供行文本格式化与
    行带命中判定；布局 `x=120 y=150 row_h=64 width=1680`
  - Windows 下 CJK 路径只能走宽字符 API，所以目录枚举/读文件在外壳里做
    （`FindFirstFileW` + `_wfopen`），模块本身只吃"槽字节"，保持可移植
  - 点 Continue → 打开列表；选中一行只记日志（快照恢复还没做，如实标注），
    点空白处回标题菜单；`--title-save-list` 可直接打开列表（截图/调试用）
  - 实测：五彩斑斓的世界 **6 个可读槽**（第 7 个 save.bin 是全局状态不是槽）、
    星空的记忆 **0 个**；汇总行补 `continue=enabled (6 slot(s), 6 readable)`
  - `tests/test_savelist`（**66 项**）：合成真实格式槽文件 → 解析字段、
    行文本、命中、上限、坏文件/NULL；`make check` 现 24 个单测、run_all 39 步
  - 多游戏门禁第二次跑 `--title-menu --title-save-list`，断言两部作品的可读槽数
    与汇总行一致

- **标题菜单的 Continue 改成按存档可用性启用/禁用（进多游戏门禁）**
  - 新增 `title_count_save_slots()`：数 `<game_dir>\save\sNNN.bin`；
    0 个槽 → Continue 置灰、点它只记日志（读档界面还没做）、不启动脚本；
    有槽 → 正常可点
  - 修掉一个重复初始化 bug：第一帧还会再 `fvp_title_init()` 一次，把刚设好的
    enabled[] 冲回全可用（表象是"没有存档也显示 Continue 可用"）。现在几何/
    可用性只在启动时设一次
  - 汇总行补 `continue=enabled/disabled (N slot(s))`，实测：
    五彩斑斓的世界（`save/` 里 7 个槽）→ enabled；星空的记忆（没有 save 目录）
    → disabled
  - `verify_multigame.py` 的标题菜单阶段新增断言：
    `continue is enabled/disabled in the title summary`（按作品期望值）

- **多游戏门禁新增"标题菜单"阶段：两部作品的 profile 与菜单几何都成了硬断言**
  - `tests/verify_multigame.py` 新增 `stage_title_menu()`：两部目标作品各跑一次
    `--title-menu --screenshot`，断言外壳退出码、汇总行里的 profile/align、
    走贴图还是文字模式，然后用**纯 Python 读我们自己写的 32bpp BMP**
    （不依赖 Pillow）量菜单墨迹：
  - 五彩斑斓的世界：5 项墨迹右边界 1767/1767/1767/1766/1762（目标 1769 ±8）、
    行距 100/101/102/94（目标 100 ±6）、日志出现 "falls back to text"
  - 星空的记忆：5 项中心 961/960/959/961/959（目标 960 ±10）、
    行距 90/90/90/90（目标 90 ±6）、日志出现 "5/5 menu textures loaded"
  - 这一步让"标题菜单只对一款游戏调过"变成会失败的断言（对齐/行距/贴图模式都对不上
    就直接 FAIL）

- **标题菜单真的会贴原版贴图了（新增 `gfx_draw_image_at`）+ 更正 iroseka 的素材判断**
  - 新增 `gfx_draw_image_at(gfx, slot, x, y)`：把图形槽里的图按 `slot_compose()`
    + `blit_sprite()` 直接贴到画布（与精灵渲染同一条路径），在 `gfx_render()` 之后调用
  - 外壳 `--title-menu`：按 profile 把素材名装进图形槽 `0xF00+i`，装上就贴图、
    贴不上就退回文字模式；星空的记忆实测 `5/5 menu textures loaded`，截图出来就是
    原版那套"白描边 + 青色发光"字样、居中、行距 90
  - **更正**：iroseka 的 `title_start` / `title_start1..5` 是 **「New Game」的状态变体**
    （正常 / 选中带「はじめから」注音 / 淡出…），不是 5 个菜单项；其余项是引擎用
    字体画的。所以 iroseka profile 的 asset 全部留空 → 走文字模式，右对齐 1769 不变
  - `tests/test_titlemenu` 调整：iroseka 不要求有贴图，hoshimemo 每项必须有
    独立贴图 + 发光层素材

- **标题菜单改成"每作品 profile"：量了第二部作品，证明两作布局不一样**
  - 用同一套方法量了《星空的记忆 HD》的原版标题画面（`work/measure_hoshi.py`）：
    **居中于 x=960**（iroseka 是右对齐 1769）、**行距 90**（iroseka 100）、
    首行墨迹上沿 581、墨迹高 33..34；菜单项文字也不同
    （`GAME START / CONTINUE / ALBUM / OPTION / EXIT`）；
    素材是 `title_start`(275×40) + `title_start_ray`(1920×20) 分离式，
    iroseka 则是 `title_start`(340×80) 字+发光烘焙在一张图
  - `titlemenu` 新增 `FvpTitleProfile`（对齐方式/行距/首行上沿/每项标签/贴图素材名/
    贴图尺寸/实测墨迹矩形）+ 两份 profile（iroseka / hoshimemo）+
    `fvp_title_profile_find()`（按游戏目录或脚本名关键字，大小写不敏感）、
    `fvp_title_apply_profile()`、`fvp_title_profile_hit()`（直接用实测墨迹矩形命中，
    不需要字体）、`fvp_title_profile_image_pos()`
  - 桌面外壳 `--title-menu` 自动选 profile 并按它的对齐方式落笔（右对齐贴墨迹右边界 /
    居中按墨迹中心），汇总行打印 `profile=..., align=...`
  - 实测对照：iroseka 墨迹右边界 1767/1767/1768/1766/1762（原版 1769/1769/1768/1768/1768，
    ≤6 px）、行距 100/101/102/100；hoshimemo 中心 957/958/957/958/957
    （原版 959/960/959/960/959，≤3 px）、行距 90/89/91/89
  - `tests/test_titlemenu` 136 → **208 项**（两份 profile 的几何/命中/差异断言：
    行距、对齐方式、首个标签必须真不一样，防止"只对一款调"）
  - `docs/title-menu.md` §4 记录两作实测表、profile 机制与"加第三部作品"的步骤

- **查清标题菜单的真实实现：菜单项是预渲染贴图（不是文字）+ 模块补图像模式**
  - 逐项 dump `graph.bin` 找到标题素材：`title_start`(340×80) 就是「New Game」整张图、
    `title_start1..5` 是其余菜单项、`title_logo1`(1150×283) 是作品 logo、
    `a_top_s1`(1920×1080) 是 ALBUM MODE 那版标题背景、`a_top_p1/p2` 是零件、
    `logo_bg`/`logo_favo` 是 FAVORITE splash 素材
  - 用"文字掩码模板匹配"在原始标题帧里验证 `title_start`：**92.2% 吻合，文字落在
    (1460,511)，贴图左上角 = (1445,492)，墨迹右边界 1769**——和上一轮实测表逐项对上
    （贴图 y+19=511、x+324=1769、行距 100 推出第 2 行墨迹上沿 611）
  - 结论：**"菜单字体"不存在**，衬线字 + 白色外发光都烘焙在贴图里（这也解释了为什么
    `font/` 里只有 Sarasa、为什么量出的字高和常见字体比例对不上）
  - `titlemenu` 模块补**图像模式**：`fvp_title_set_images()` /
    `fvp_title_image_pos()`，`fvp_title_item_rect()` 在图像模式返回贴图矩形
    （340×80 @ `(1445, 492+i*100)`），命中带仍是整行 100 px；文字模式保留作退化方案
  - `tests/test_titlemenu` 90 → **136 项**（新增图像模式的几何/命中断言）
  - `docs/title-menu.md` §3 记录素材表、模板匹配结果与推导；`work/match_title_buttons.py`
    是这次用的匹配脚本（本地，不入库）

- **标题菜单接进桌面外壳（`--title-menu`）**
  - 启动后先画引擎侧标题菜单并等输入：点 `New Game`（或回车）才 `fvp_vm_start()`，
    `Exit` 关窗口，`Continue`/`Album`/`Option` 先只记录（还没有存档界面/相册/设置页）
  - 鼠标用外壳已有的 `canvas_x/canvas_y`（1920×1080 客户坐标），命中复用
    `fvp_title_move/hit/click`；悬停项高亮、不可用项压暗
  - **顺序坑**：`gfx_render()` 会按图元/文本窗口重建画布，菜单覆盖层必须在 render
    **之后**画；截图路径也要补画一次（否则 `--screenshot` 截出来是全黑）
  - 实测对照（同一套度量方法，原版 vs 本引擎）：墨迹右边界 **1766..1768 vs 1768/1769**
    （差 ≤3 px）、行距 **100 vs 100**、首项墨迹带上沿 516 vs 511（内建字形 bearing）、
    墨迹高 45（`Option` 含降部 52）vs 40..42（字体不同：内建位图 vs 原版衬线体）；
    左边界因字宽不同必然不同，**右对齐才是硬指标**
  - 汇总多一行 `title menu enabled (5 item(s) right-aligned to x=1769, pitch 100, picked ...)`
  - `docs/title-menu.md` §2.2 + `docs/win32-shell.md` 选项表已更新

- **标题菜单的像素级渲染验证**（`gfx_text_width`/`gfx_draw_text_line` +
  `tests/test_titlemenu_draw.c`）
  - gfx 新增两个引擎侧 UI 用的 API：任意位置/任意字号画一行、量一行宽；
    **字号与字体自带字号不同时按比例缩放**（内建 8x16 放大到 56 px → 每字 28 px 宽、
    墨迹高约 42，正好对上原版实测 40..42）；`gfx_render()` 会重建画布，
    所以叠加文字必须在 render 之后画（测试与外壳同序）
  - `tests/test_titlemenu_draw.c`（**68 项**）：1920×1080 画布上按布局画 5 项，
    扫墨迹包围盒断言右边界 1766..1769、行距 100±2、墨迹高 30..60、墨迹上沿
    （内建字形约 7 px bearing，属字体度量差异）、命中与布局一致
  - `make check` 现为 **23 个单测**，`run_all` 现为 **38 步**
  - `docs/title-menu.md` 新增 §2.1 记录这两个 API 与验证结果

- **原版抓帧补上"周期点击推进" + 序章时序数据**（`scripts/capture-original.ps1 -ClickEvery`）
  - 用户授权后可自动跑原版：`-ClickEvery N` 每 N 秒点一次客户区中心推进剧情
    （点之前临时把窗口挪回屏幕、点完再挪出桌面）
  - 抓到的序章时序：影片段 → 第一次白屏（≈(500,490) 一小段乱码，对应「空」标题卡）
    → 天空背景正文段（≈15 个不同文本帧）→ 第二次白屏（第二张「空」）。
    与 `ref/ref-900k.txt` 里 `[0] … → [9] 空 → 20 行 [0] → [9] 空` 的结构一致，
    下一步可拿"两次「空」之间的点击次数"与 trace 的行数（21 行）对齐
  - 脚本重新改成**纯 ASCII**（PowerShell 5.1 按 ANSI 读文件，CJK 注释会让解析器报错）

- **标题菜单（引擎侧 UI）的布局 + 命中模块**（`src/titlemenu.c`、`tests/test_titlemenu.c`）
  - 原版标题菜单不是脚本画的（抓帧确认），本引擎从脚本第 0 条指令跑起，缺的就是这块
  - 几何全部来自原版实测：5 项**右对齐到 x = 1769**、**行距 100**、**墨迹高 40..42
    （字号 56）**、中心 y 531/631/732/831/931；模块默认值与之一致
  - `fvp_title_init/move/hit/click`：右对齐靠外部测宽回调（无字体时等宽回落，
    两种情况下右边界都严格 1769）；墨迹框高 = 字号 3/4 = 42，点击带补足到整行 100 px，
    横向各留 24 px 余量；`enabled[]` 表达"没存档时 Continue 灰掉"
  - `tests/test_titlemenu`：**90 项**，用假测宽复现实测宽度，逐项断言右/左边界、
    墨迹中心与墨迹高（±1~2 px），以及命中/悬停/点击/禁用/无字体回落/NULL 安全
  - 文档 `docs/title-menu.md`：实测表、模块 API、还差什么（字体来源未定、标题背景
    graph id 未定位、外壳状态机未接、过渡依赖 WMV）
  - `make check` 现为 **22 个单测**，`run_all` 现为 **37 步**

- **opcode 分发表读全 + 比较 opcode 的"表 + setcc"钉死 + 第三次 A/B**
  - 从 `0x4815E0`（`0x4463E0` 初始化，40 项 x 4 字节）读出全部 opcode 的 handler，
    列进 `docs/vm-commands.md` §1.2.1
  - 六个比较 handler 全部读死：eq=C/`setne`、neq=C/`sete`、gt=B/`setne`、
    **le(0x25)=A/`sete` 即 `a >= b`**、lt=A/`setne`、**ge(0x27)=B/`sete` 即 `a <= b`**；
    参数顺序也读死了（`push &b; push &a` → cdecl arg1 = 先压入的 a）。也就是说
    本站 0x25/0x27 的**名字与语义和静态分析相反**——这条矛盾以前只写"改了会变 0 行"，
    现在把证据链和实现都补齐了
  - 把这条真实语义 + "表外组合下 0x25/0x27 为真"实现进 `--bool-type`（默认关）后第三次 A/B：
    iroseka 900k 步仍是 **0 行**，但表现变了——**thread 0 一直跑，`PrimGroupIn(73)`
    被调用 31,154 次**（默认模型 2,399 次），即忠实模型是卡在粒子/动画组循环里，
    而不是跑飞。已写进 `docs/roadmap.md`「三次 A/B 的经过与结论」
  - `tests/test_nullvalue` 77 → **80 项**：新增 `pushint8 1; pushfalse; le` 的最小 `.hcb`，
    钉死"默认模型按数值得假、忠实模型按表 A+sete 得真"
  - 默认路径未动：`compare.py` 仍逐字节一致（195 行）

- **7×7 比较表读全了（更正一条旧结论）+ `--bool-type` 第二次 A/B**
  - 把初始化器 `0x446580` 完整读了一遍：三张表（`0x481388`/`0x481450`/`0x481518`，
    各 49 项）先全填 `0x444f50`（**返回 0**），再把
    (int,int)/(int,float)/(float,int)/(float,float)/(str,str) 这些对换成专用比较器；
    表 C 还把 `(null,null)` 和"和 type 1 配对"的 11 项换成 `0x444f70`（**返回 1**）
  - **比较器返回的是布尔 0/1，不是三态** → 旧文档"异类型默认相等、ge 因此在异类型上
    为真"**是错的**，已改正：默认项返回 0 = **比较结果为假**；`eq(int,null)` 为假、
    `eq(null,null)` 为真。这也解释清了脚本里最高频的
    `…Test; pushtemp; pushfalse; eq` 原来是在问**"返回值是不是 null"**
  - 开关内再补两件忠实语义：异类型/null/true 参与的比较一律为假；未赋值的 global
    读成 `FVP_NULL`（新增 `FvpVM.globals_set[]` 位图）
  - **第二次 A/B：iroseka 900k 步仍然 0 行**（默认仍是 195 行逐字节一致）。
    两次实验都指向"布尔 + 比较表 + 未赋值 global"这三件改不动那 195 行的成因，
    流程在 `pc 0x05aaf5` 一进函数体就走偏；下一步先钉死 opcode→表/`setcc` 映射
    （0x22/0x23 的 handler 还没读），详见 `docs/roadmap.md`
  - `tests/test_nullvalue` 74 → **77 项**：新增 `pushint8 0; pushfalse; eq` 的
    最小 `.hcb`，钉死"默认模型得真（int 1）/ 忠实模型得假（FVP_NULL）"

- **`fvm --bool-type` 实验开关 + 第二次 A/B 的前半（结论：不能只改布尔表示）**
  - 开关打开后逐字照 handler 0x445a30：比较/逻辑/位测试类 opcode 为假压
    `FVP_NULL`、为真压 int 1，`jmpcond` 只在 `type == FVP_NULL` 时跳；
    **默认关闭**，冻结基准仍在"数值真假"模型下逐字节一致
  - 实测：打开后 **iroseka 900k 步的 195 行文本输出变成 0 行**；第一个分歧点在
    step 12,645 / pc `0x05aaf5`，正是 `initstack[0,4]; pushint8[0]; copystack[3];
    pushint8[0]; jmpcond` 这类"常量 0 条件"（数值模型跳去 ret，类型模型继续执行
    函数体）。执行函数体后流程立刻走偏（`unknown cmds` 分布完全不同），说明
    7×7 比较表、未赋值 global = null 这两件必须一起改——已写进 `docs/roadmap.md`
  - 静态统计（官方解码器，`work/jmpcond_pred.py`）：jmpcond 的条件 81–85% 来自
    比较类、13–17% 来自 logand/logor、~1% 来自 `test`、0.7–1.2% 来自 `pushint8`
    （其中 iroseka 17 / hoshimemo 51 / sakura 65 处是 `pushint8 0`，即两种语义
    真正分叉的地方）
  - `tests/test_nullvalue` 66 → **74 项**：新增两个最小 `.hcb` 把"int 0 在两种模型
    下跳不跳"钉死（默认 pc=13 跳、类型模型 pc=11 不跳；`pushfalse` 两种都跳到 pc=12）

- **值模型补上 null（原版 Variant type 0）：`pushfalse` 不再压成 int 0**
  - 新增 `FVP_NULL` + `fvp_make_null()` + `fvp_value_is_null()`；opcode 0x09
    `pushfalse` 压 `FVP_NULL`（依据：原版 0x444940 写的是 `Variant.type = 0`）。
    `pushtrue`(0x08) 仍是 int 1 —— 没有证据表明原版对 type 1 的算术/比较有特殊规则，
    不硬造第二个类型
  - **null 分支第一次真的能走到**：`LipAnim(38)` 的「arg2 为 null → 只停」
    （新增 `lip_anim_stop_calls`，外壳汇总打印 `stop N`）、`LipSync(39)` 的
    `on = 0`、`ControlMask(8)` 的"1 分支"、`CursorShow(12)` 的 null → 0。
    `GraphLoad` 本来就是"非字符串不加载"，null 落在既有规则里无需改
  - **冻结基准不动**：`FVP_NULL` 在数值/字符串上下文里与 int 0 等价
    （`truth`/`eq`/`cmp`/`to_str` 都按 0 处理），脚本里高频的
    `syscall; pushtemp; pushfalse; eq` 结果不变。实测 `compare.py` 仍逐字节一致、
    `verify_gfx.ps1 -SkipDump` 仍 293 px / PSNR 49.94 / SSIM 0.99971
  - 新增 `tests/test_nullvalue.c`（**66 项**）：null 的类型/真值/比较/字符串化、
    LipSync 的 null 与显式 0 之别、LipAnim 只停分支不写 mode/arg、ControlMask、
    CursorShow，以及**端到端**跑一个只含 `pushfalse` 的最小 `.hcb`
    （9 字节：`u32 script_len=5 | 0x09 | u32 main_start=4`）证明栈顶类型是 `FVP_NULL`；
    `make check` 现为 **21 个单测**

- **原版真值抓帧工具 + 原版启动序列查清**（`scripts/capture-original.ps1`、
  `docs/original-verification.md`）
  - 查清"原版启动几秒就退出、截图全黑"的真因：引擎启动时要往
    `Documents\FAVORITE\<作品名>\save\s901.bin` 写系统存档，目录缺失或父进程无写权限时
    弹 `#32770` 消息框「…の作成に失敗しました」后**以退出码 0 正常退出**；
    目录建好并给足权限后原版能一路跑到标题菜单并进正文
  - 抓帧改用 `PrintWindow(PW_RENDERFULLCONTENT)`（`CopyFromScreen` 抓 D3D9 只会得到全黑）；
    点击用 `GetClientRect`+`ClientToScreen` 从游戏空间坐标现场换算（窗口会被 DPI 变成
    1707×1067 / 1926×1109，硬编码坐标必点空）
  - **结论：标题菜单是引擎侧 UI**，不是脚本画的——我们的引擎从脚本第 0 条指令跑起，
    所以会"跳过标题直接进序章"；这补上了目标② TitleMenu 缺口的证据
  - 量出标题菜单几何（1920×1080 客户区）：5 项右对齐于 x=1769、右边距 **151 px**、
    行距 **100±1 px**、墨迹高 **40–42 px**，中心 y 依次 531/631/732/831/931
  - 该任务按用户要求**暂停**，剩余步骤（影片段结束后引擎为何退出、正文对话框对照、
    TitleMenu 实现）写在文档 §6
  - **原版窗口缩不小，已改为"挪出桌面"**：引擎把客户区钉死在 1920×1080、窗口样式没有
    `WS_THICKFRAME`，`SetWindowPos` 设尺寸无效（连换样式再设也无效），注册表与
    `Documents\FAVORITE\` 下也没有分辨率配置；实测只有**移出桌面**可行且
    `PrintWindow` 仍能抓到 99.2% 非黑像素（最小化后只能抓到 237×39 图标位图）。
    抓帧脚本因此默认 `-Park offscreen`：一出现就搬到桌面外，点 `New Game` 时临时挪回、
    点完再挪走

- **文本图元的 alpha 乘进文字（近似实现）+ 新增像素级单测 `test_textprims`（20 项）**
  - `PrimSetText(86)` 建的文本图元带 `prim+0x07 = alpha`（原版用它做淡入淡出）；
    现在 `draw_text_window()` 收一个 `alpha` 参数：正文层描边按 `120·alpha/255`、
    标题层按 `200·alpha/255` 缩放，颜色按 `c·alpha/255` 压暗
  - **原理性近似，照实写**：本引擎的字形覆盖是"一次性写屏"——`ink_px()` 在
    `cov == 255` 时直接写 RGB、不参与 DST 混合，所以做不到物理正确的 src-over。
    要正确就得改造 `ink_px`（会连带改变现有描边像素 → 动 293 px 冻结基线），
    因此 `alpha < 255` 时只做"按比例压暗"，文档与 `docs/render-model.md` §1.3 都写明
  - `alpha == 255`（默认路径与全部冻结基线）**一个像素都不变**：`compare.py` 仍字节一致、
    `verify_gfx` 仍 293 px / PSNR 49.94 / SSIM 0.99971
  - 新增单测 `tests/test_textprims.c`（**20 项**，`make check` 现在 **20 个单测**）：
    用内建 8×16 字体 + `gfx_canvas_rgba` 的区域亮度和做像素级断言 ——
    默认位置 / 图元位置 A-B 切换、不再重复画、连渲两次一致、`alpha=128` 比 255 暗、
    `draw=0` 不画、`tw<0` 不算绑定、NULL 安全
  - `Makefile` 新增 `test_textprims` 目标并接进 `UNIT_TESTS`；`README` 的单测数 9 → 20，
    `docs/testing.md` 补这一行

- **文本窗口的屏幕位置来源查清并实现：`PrimSetText`(86) 把文本挂到图元上（`--text-prims`）**
  - handler **0x43BDC0**：把图元重建成 **type 5 = 文本图元**——
    `prim+0x32` = 文本窗口号、`prim+0x14/0x16` = x/y、`prim+0x07` = alpha 255、
    `prim+0x08` = blend 0。**位置由图元（含父链）决定**，不是脚本给窗口设坐标
  - 佐证：`TextPos`(122) 在两部目标作品里 **0 次调用**；而《星空的记忆 HD》的层 6
    （「丘の上」）就来自一个绑定 prim 553 的文本图元 → 之前"层 6 压在正文上"的真正原因
  - 本站新增 `--text-prims` 开关（默认关）：打开后被绑定的文本窗口画在
    **图元绝对坐标**（父链 x/y 累加）上，并且不再按版面表画第二遍；关着时保持冻结基线
  - 实测（8 s 抓帧）：《星空的记忆 HD》「丘の上」从左下正文位置移到海面上方，
    与正文不再重叠；《五彩斑斓的世界 HD》与原来基本一致（其图元位置与固定版式接近）
  - 顺带补上 `PrimSetText` 的 alpha：原版建图元时写 `prim+0x07 = 0xFF`，
    所以这类图元的 alpha 不该是 0（我们的实现在 86 里显式置 255）
  - 新增 `gfx_set_text_prims()` / `gfx_text_prim_bindings()` 与汇总行
    `text prims N drawn, M bound`；`FVP_DEBUG_TEXT=1` 会打印每个文本图元的
    tw/draw/alpha/visible
  - `tests/verify_multigame.py` 的桌面阶段多一条：`--text-prims` 短跑 4 s，
    断言 `text prims … drawn` > 0（两作实测都是 1 drawn / 2 bound）
  - **冻结基线不变**：开关默认关，`verify_gfx` 仍是 293 px / PSNR 49.94 / SSIM 0.99971；
    `compare.py` 字节一致；`make check` 全绿
  - 已知遗留：图元 alpha 乘进文字颜色还没做（文本图元目前恒 255）；`--text-prims`
    目前只在桌面上暴露，`render_vm` 的像素门仍走默认路径

- **更正 `TextOutSize`(120) 的语义：它不是宽高（顺带解答"层 6 版面重叠"的真实成因）**
  - handler **0x43C960** 写的是 `tw+0x595` / `tw+0x596`，校验区间 **0..0xC** / **0..0x8**
    （两个分量各自独立，越界只跳过那一个），**不是** 宽/高
  - 0x595/0x596 与 TextSize 的 0x592/0x593/0x594、TextShadowDist 的 0x597 同属
    **tw+0x590 起 0x28 字节的 text-style 块**；绘制时
    `0x43396F: memcpy(stack, tw+0x590, 0x28)` 整块拷进绘制结构 → 它们是真正会被消费的
    文字装饰参数（之前怀疑"死存储"是错的，记在这里避免以后重复走弯路）
  - **实测两作都传越界值**：iroseka `(0,64,32) (8,64,32) (9,64,1)`、
    hoshimemo `(0,64,32) (5,64,16) (6,64,1)` → 按原版逻辑整对跳过，
    **这条命令在目标两作里实际是空操作**（本站照此实现：逐分量钳制 + 越界保留旧值）
  - **顺带确认"版面位置不是脚本设的"**：`TextPos`(122) 在两作里 **0 次调用**
    （静态统计 `use=0`）→ 层位置只能来自引擎内部初始化，上一轮报的"层 6 与正文重叠"
    是缺这张内部表，不是漏接命令；这条记为下一轮的靶子
  - 新增 `gfx_text_get_out_size()`（返回"是否作用过"）与外壳汇总行
    `text out layer N: a=.. b=.. (TextOutSize)`；`FVP_DEBUG_TEXT=1` 时 stderr 打原始参数
  - `tests/test_typewriter.c` 38 → **46 项**（上界、越界整对跳过、分量独立、getter 语义、NULL）
  - `docs/vm-commands.md` / `docs/render-model.md` 的 120 行按静态分析改写

- **第二部作品的桌面外壳验证 + 字体图集覆盖修复（tofu 19 → 0）**
  - 新工具 `scripts/make-font-atlas.py`：用 `ref/fvm_ref.py` 精确解出 HCB 里所有
    `pushstring` 字符串（不是"把二进制当 cp932 硬解"），取目标作品并集，再调
    `tests/gfx_dump.py::write_font()`（同一 TTF / margin / glyph+stroke1+stroke2 三层）
    生成 `bin\gfx-font-<size>.bin`
  - 关键发现：图集必须覆盖渲染端**真正会请求的每一档字号** —— `gfx_layout_default()`
    的默认层字号是 **30**，而早期只生成了 32/34/56，于是《星空的记忆 HD》的层 6
    （地名标签）整层退回内置 8×16 字体，开场 8 个豆腐块
  - 修 `str.isprintable()` 的坑：它对**全角空格 U+3000** 返回 False，早期过滤器把
    台词里的全角空格全丢掉 → 每次出现都画豆腐块
  - 实测（8 s 抓帧，`--no-audio`）：**iroseka 1 tofu → 0**、**hoshimemo 19 tofu → 0**；
    两作都 `unimpl cmds none`、1920×1080 帧正常写出
  - **诊断能力**：`GfxStats` 新增 `tofu_cp[8]/ntofu_cp`，汇总多一行
    `glyphs missing U+XXXX …`；设 `FVP_DEBUG_TOFU=1` 还能在 stderr 看到每处豆腐块用的
    是哪套字体（`builtin=1` = 整层退回内置字体）——这次正是靠它一次定位到"层 6 + 30 px"
  - `tests/verify_multigame.py` 新增**桌面外壳阶段**：两部目标作品各跑 8 s 抓帧，
    断言退出码 0、写出 1920×1080 帧、`vm … steps` > 0、`unimpl cmds none`、
    `glyphs 0 tofu`、日志里有 `screenshot: … ok`；`--no-shell` 可跳过
  - 图集 2764 glyph / 12–34 MB 一个，落在 `bin\`（gitignore，不进仓库），
    生成命令写进 `docs/win32-shell.md` §5
  - 已知遗留：层 6（地名标签）的**版面位置**仍按通用默认（x=120,y=880,size=30），
    在《星空的记忆 HD》里会和正文层重叠 —— 层位置需要按层逆出来，另开一轮

- **动画缓动 `mode 1..5` 从静态分析确认并落地（第 ④ 项优先级）**
  - 补间条目（每槽 28 字节，6 个管理器共用一套代码）里 `+4` = mode 字节、
    `+0x14` = 已过时间、`+0x18` = 时长；推进函数 **0x422C10** 的
    `jmp *0x423154(,%eax,4)` 是一个 **5 项跳表**（索引 = mode−1）：
    mode 1→`0x422CB6`、2→`0x422D25`、3→`0x422DAF`、4→`0x422E37`、5→`0x422F8F`；
    mode 0 或 >5 走 `0x423112` 的特殊分支
  - 5 个例程都用 64 位整数乘除（`__allmul` **0x447DD0** / `__alldiv` **0x447D20**，
    向零截断）：**1 = 线性、2 = 缓入 `t²`、3 = 缓出 `1−(1−t)²`、
    4 = 缓入-缓出（以 d/2 分段）、5 = 先缓出后缓入（同样分段）**
  - 本站 `fvp_motion_ease(mode, p)`（`src/motion.c`）实现这 5 条曲线，
    `fvp_motion_value()` 用它插值 → 位置 / 透明度 / 缩放三条 Motion* 共用
  - **证据级别照实写**：mode 1/2/3 逐条指令确证；mode 4/5 的分段点、首段公式、
    端点与中点确证，**第二半段**只做到"按同一分段结构对称推导"（x/y 共用代码 +
    寄存器别名），因此不声称与原版逐位一致；原版 i16 + 整数除法，本站 double，
    中间帧最多差 1 个坐标单位 —— 两点都写进 `docs/animation-baseline.md` §2.3
  - 冻结基线不动：`compare.py` / `run_all.py` / `verify_gfx` 都在动画关闭下跑
    （`motion: disabled (static prims, pixel baseline kept)`），像素差仍是 293 px
  - `tests/test_motion.c` 47 → **85 项**（端点 / 越界钳制 / 中点 / 101 点单调性 /
    形态特征值 / mode 0 与表外值 / 端到端 25% 时间点坐标）

- **`LipAnim`(38) / `LipSync`(39) 落地 —— 静态命令缺口归零（2 → 0，148/148）**
  - `engine+0x683E74` 是**口型表**：4 个槽，每槽 0x20 字节，`+4` = 状态字节
    （**0 = 空 / 1 = 停 / 2 = 播放中**）、`+8` = 槽号（释放时写回 `engine+0x84` 的空闲栈）、
    `+0xa` = i16 的 id
  - `LipAnim(38)` handler **0x43A6E0**：arg1 int **1..0xFFF**；arg2 为 **null → 只停**
    （`0x42D700(id)`：清状态、槽号进空闲栈、活跃计数 `engine+0x8C` 减一）；否则
    arg2 int **0..3**（模式）、arg3 int **1..0xFFF**、arg4 int **1..300000**、
    arg5 int **1..0xFFF**（必填）、arg6 int **1..300000**（必填）、
    arg7 int **1..0xFFF** / arg8 int **1..300000**（**可选**，`0x43A7FE` 分支 push 0,0）；
    全过才 `0x42EFA0(表, id, mode, …)`：先停旧的，再从空闲栈取槽、状态置 1、
    8 个参数按 u16 写进槽
  - `LipSync(39)` handler **0x43A830**：arg1 int **1..0xFFF**；`cmpb $0x0,0x8` 只看
    第二参数**是不是 null**（null → 0、其它 → 1），再 `0x428870(表, id, on)`：
    on → 状态字节 = 2，off → `0x4283A0` 停 + 状态字节 = 1
  - 本站两条都只记录参数：`lip_anim_id/mode/arg[8]`、`lip_sync_id/on` + 两个计数；
    外壳与 `fvm` 汇总多一行 `lip anim N call(s) id=… mode=… ; sync N call(s) id=… on=…`
  - **已知限制（照实写）**：两条的 null 分支都依赖"值类型 0 = null"，本站值模型是
    `FVP_INT=0 / FVP_FLOAT=1 / FVP_STR=2`（没有 null，`pushfalse` 也压 int 0），
    所以 `LipAnim` 只能走"启动"、`LipSync` 只能走 `on = 1` —— 与 `ControlMask` /
    `CursorShow` / `GraphLoad` 的 null 限制同类
  - **`check-commands.py` 的 `MISSING_BASELINE` 从 2 改到 0，`--strict` 首次全绿**：
    148 条命令里 147 条有显式处理分支、1 条落在通用 `*Test` 规则里、0 条缺口
  - 单测 `tests/test_vmmisc.c` 2264 → **2290 项**（LipAnim 6/8 参数、每个必填参数越界、
    可选参数缺省；LipSync 两个参数、缺第二参数按 0 记、越界与 NULL）；
    `test_vm_edge.py` 51 → **53 例**（`syscall-38-lipanim`、`syscall-39-lipsync`，
    断言汇总行）
  - 实测 90 万步两条都 **0 次调用**（`use = 0`），验证来自单测 + 合成脚本

- **`ThreadRaise`(134) 落地（缺口 3 → 2）**
  - handler **0x437B80**：arg1 **必须 int**，否则整条丢弃；然后对
    `engine+0x68DFC0 + i*0xB28`（i = 0..31，32 个协程槽）逐个调 **0x4447E0(ctx, ms)**
  - 0x4447E0 的有效代码只有三行：`if ((flags & 4) && ctx->wait_ms == ms) flags &= ~4;`
    —— 语义是**叫醒"睡眠时长正好等于 ms"的线程**，而且只认 `ThreadSleep` 的 sleep 位
    (`0x4`)，不认 `ThreadWait` 的 wait 位 (`0x2`)
  - 本站实现 `fvp_vm_thread_raise()`（`src/vm.c`）：`FvpThread` 新增 `sleep_ms`
    （ThreadSleep 记时长、ThreadWait 记 -1），命中就清 `blocked/timed/wait`；
    返回叫醒个数，参数非法返回 -1
  - 外壳与 `fvm` 汇总多一行 `threads raise N call(s), last ms=…, woken M`
  - 这是**真的会改执行状态**的一条（不是纯记录）：被叫醒的线程在同一轮调度里就会继续跑，
    所以它和 `--timeline` 的虚拟时钟是配套的
  - 单测 `tests/test_vmmisc.c` 2251 → **2264 项**（手工往 32 个槽里放三个线程：
    匹配的 sleep、不匹配的 sleep、ThreadWait，逐个验证唤醒/不唤醒/计数）；
    `test_vm_edge.py` 50 → **51 例**（新增 `syscall-134-threadraise`，断言汇总行）
  - 棘轮 3 → **2**；实测 90 万步 `ThreadRaise` **0 次调用**，所以只能靠单测与合成脚本验证

- **`Snow`(97) 落地（缺口 4 → 3）**
  - 18 参数大命令，handler **0x43C230**：arg1..arg14 **必填**（逐段 `cmp` 区间见
    `docs/vm-commands.md`），另有 **arg8 ≤ arg9** 的交叉约束（`0x43C356 cmp %eax,%ebp; jg`）；
    arg15..arg18 **可选**（`0x43C41A/432/44F/46D` 处失败只跳过赋值，不拒整条命令）
  - 反复出现的 `0x4381B0(v, lo, hi)` 是纯判定辅助函数：`type==2 && lo<=value<=hi`
  - 全部通过 → 16 个整数压栈调 **0x431DB0(层结构, …)**；层结构基址
    `engine+0x683F1C + arg1*0x5048`，比 `SnowStart` 的 `0x688F1C` 早 `0x5000`
    （同一块 0x5048 层结构里的不同子结构）
  - 本站没有雪花渲染，只记录：`snow_calls` / `snow_layer` / `snow_vals[18]` /
    `snow_opt_mask`；外壳与 `fvm` 在调用过时多打印一行
    `snow full  N call(s), layer=.., arg2..14 = … , opt mask=0x…`
  - 脚本侧证据：iroseka 里这条命令**静态出现 1 次**（pc `0x035BC7`），压了
    16 个表达式 + `arg17=0` + `arg18=0`；`arg17=0` 落在 10..0x2710 之外 → 按可选参数
    缺省处理，**说明编译器用常量 0 表示"这个可选参数不指定"**。前 90 万步没执行到它，
    所以改用合成脚本做端到端验证
  - `tests/test_vm_edge.py` 新增 `syscall-snow-97` 用例（18 个 push + `syscall 97` +
    `ret`，断言 20 步、退出码 0），并给这个测试框架加了 `expect_stdout_contains`
    断言能力，从 `fvm` 汇总里确认
    `snow full  1 call(s), layer=0, arg2..14 = 100 200 300 2 2 1 10 100 1 …`
  - 单测 `tests/test_vmmisc.c` 2225 → **2251 项**（14 必填参数逐个越界 + 交叉约束 +
    可选参数位图 + 失败不计数）；`test_vm_edge.py` 49 → **50 例**；棘轮 4 → **3**

- **外壳验收恢复 + 45 s 长跑复测（环境自愈，未改任何代码）**
  - `bin\fvp.exe --game <iroseka> --run-seconds 6 --no-audio --screenshot` 退出码 0，
    写出 1920×1080 帧（8,294,400 B），`bin\fvp-win32.log` 79 行完整
  - `tests/verify_soak.ps1 -Seconds 45` **PASS**：退出码 0、工作集 140.1 → 196.1 MiB
    （增长 56.0 MiB ≤ 96）、8 个 5 s 窗口 fps 全 ≥ 55、0 underrun、0 错误行
  - 汇总实测：**45.01 s / 2,694 帧 / 59.86 fps**、frame avg 16.70 ms（max 87.90）、
    gdi present avg 1.98 ms（p95 3.00 / max 6.25 / slow 0）、VM **240,201 步**
    （与历史基线同一数值）、43 行台词、audio submitted 3,378 / completed 3,375 /
    underruns 0、prim hit 55 次 0 命中、`unimpl cmds none`
  - **`unimpl cmds none` 从此是外壳实测值**：之前 GUI 起不来时只能按控制台侧推断，
    现在 148 条命令里"实测命中过的"全部有处理分支（与静态审计 143/148 互相印证）
  - 已知问题里「shell 在 WinMain 之前卡死」条目改为**已解决**，并记下排查教训

- **`PartsMotion`(66) / `PartsMotionPause`(67) / `PartsMotionStop`(68) / `PartsRGB`(70) /
  `PrimGroupMove`(74) 落地（缺口 9 → 4）**
  - `PartsMotion(partsId 0..0x3F, kind 0..0xFF, timeMs 1..300000)`：handler **0x43B3E0**，
    三段校验任一失败就整条丢弃（`0x43B433 ret $0x4`）；全过才
    `0x42D400(&tween_mgr[0x651D70], partsId, kind, timeMs)` 起补间。本站没有 parts 渲染层，
    记录 `parts_motion[id]` / `parts_motion_time[id]` + 计数，不建 tween
  - `PartsMotionPause(partsId, on 0..1)`：handler **0x43B440**，写 parts 表
    `0x61D064 + id*0x34 + 0x33` 的暂停字节；`on` 非 int 或越界整条丢弃
  - `PartsMotionStop(partsId 1..0x3F)`：handler **0x43B480** 尾调用
    `0x4264C0(&tween_mgr[0x651D70], partsId)`
  - `PartsRGB(partsId, r, g, b)`：handler **0x43B500**，每个分量「是 int 且 0..200 才采用，
    否则回落 100」，`rgb_component()` 逐字重实现；再 `0x424500(&parts[id], r, g, b)`
  - `PrimGroupMove(group 1..0xFFF, target 1..0xFFF)`：handler **0x43B980**，两个参数都校验后
    `0x438E20(group, target)`
  - 单测 `tests/test_vmmisc.c` 2183 → **2225 项**（逐条参数边界 + 「计数只算成功」）
  - 棘轮 9 → **4**；门禁全绿（`compare` 字节一致、`verify_gfx` 293 px / PSNR 49.94 / SSIM 0.99971 不变）
  - 附：parts 表 `0x61D064 + id*0x34`（64 项 = 0xD00 字节）正好接在 prim 数组基址
    `0x61DD64` 前；`0x651D70` 也正是存档快照里 6 个 tween manager 的第一个
    （`docs/save-format.md` §2.4.4）——以后真做 parts 动画可直接复用 `src/motion.c`

- **`SnowStart`(98) / `SnowStop`(99) / `PrimSetSnow`(84) 落地（缺口 12 → 9）**
  - `SnowStart(layer 0..1, …)`：handler **0x43C4F0**，写 `engine+0x688F1C + layer*0x5048`
    处的开关字节 = 1（两层雪花系统）；`SnowStop` 写 0（handler **0x43C520**）
  - `PrimSetSnow(primId 1..0xFFF, on 0..1, …)`：handler **0x43BC40**，校验后写该图元的
    积雪参数；本站只记录 on
  - 本站没有粒子/积雪渲染 → 三条只记录（`snow_on[2]` / `prim_snow[4096]` + 计数），
    两个外壳汇总打印 `snow layer=…`
  - 单测 `tests/test_vmmisc.c` 2159 → **2183 项**；棘轮 12 → **9**；门禁全绿
  - **附：快照对齐尝试 4** —— 6 个槽的"最长连续零段"是 89,960–90,117 B（= 32×0xB28 的
    98.4%），但位置在距文件尾 43,276–45,002 B 处，落在 §2.4.1 的稀疏块一带，说明
    "零区 = 协程上下文"不能按顺序直接对齐；副产品是
    `tail_len − zero_run_end ≈ 43,300 ± 1,700` 这个可用的自检不变量，已写进文档

- **`CursorShow`(12) / `ExitDialog`(16) / `MovieStop`(63) 落地（缺口 15 → 12）**
  - `CursorShow(v)`：handler **0x4374F0**，**只看参数类型**（null→0、其它→1）再 tail-call
    `0x441860(engine,值)`；本站没有 null 值模型 → 恒为 1（与 ControlMask 同一处限制）
  - `ExitDialog()`：handler **0x437560**，条件成立时置线程 ctx 的 `+0x68E7D0 = 1` → 只计数
  - `MovieStop()`：handler **0x43B300**，`call 0x4220F0(engine+0x8C->0x2D4)` 停 WMV →
    本站没有视频在放，空操作 + 计数（语义上正好成立）
  - 单测 `tests/test_vmmisc.c` 2149 → **2159 项**；两个外壳汇总多一行 `cursor show=…`
  - 棘轮 15 → **12**；门禁全绿
  - **附：快照锚点尝试 3 也失败**（协程上下文的 type/pc/sp 三元判据太弱，命中 28.6 万个
    位置，0x34 周期），已把"判据必须要求连续多条记录成立 + 用 0x16500 块长度做联合
    校验"的教训写进 `docs/save-format.md`

- **`InputFlash`(27) / `InputGetRepeat`(33) / `InputSetClick`(37) 落地（缺口 18 → 15）**
  - `InputFlash()`：handler **0x4376A0**，`call 0x442400(engine+0x120)`，无返回值 → 只计数
  - `InputGetRepeat()`：handler **0x437860**，**返回 int**（`engine+0x448`）→ 返回记录值（默认 0）
  - `InputSetClick(v)`：handler **0x437920**，v 必须 int 且只能是 0/1，写 `engine+0x438`
  - 单测 `tests/test_vmmisc.c` 2135 → **2149 项**；两个外壳汇总多一行 `input click=… repeat=…`
  - `scripts/check-commands.py` 棘轮 18 → **15**；门禁全绿（`compare` 字节一致、
    `verify_gfx` 293 px / PSNR 49.94 / SSIM 0.99971 不变）
  - **附：存档快照的第二个锚点也失败了** —— 用 prim 数组（4096×0x34）当锚点，
    numpy 稀疏采样筛出 103,163 个候选、全量校验后**没有**一个能凑出 ≥3 条"活的" prim。
    已把失败原因与下一个锚点（32 个协程上下文，`0x68DFC0..0x6A44C0` 正好 0x16500 字节，
    与调色板首尾相接）写进 `docs/save-format.md`

- **`MotionPause`(60) / `TextPause`(121) 落地（缺口 20 → 18）**
  - `MotionPause(primId 0..0xFFF, on 0..1)`：handler **0x43AB60**，写 `prim+9`（暂停字节）
  - `TextPause(twIdx 0..31, on 0..1)`：handler **0x43C9C0**，写 `tw+0x5DA`
  - 两条的 `on` 都只收 0..1；参数非 int / 越界时走"等价于 0"的分支（照静态分析写）
  - 实现：`FvpVM` 新增 `prim_pause[4096]` / `tw_pause[32]` + 两个计数；
    **TextPause 会真的生效**：`gfx_text_set_pause()` → `gfx_text_advance()` 跳过暂停窗口，
    所以 `--typewriter` 下"暂停期间不逐字推进、取消后继续"是可见行为；
    MotionPause 只记录（补间按 prim_id 组织，暂停位还没接进 tween 推进，文档已注明）
  - 单测：`tests/test_vmmisc.c` 2114 → **2135 项**；`tests/test_typewriter.c`
    31 → **38 项**（暂停/恢复/越界/NULL）
  - `scripts/check-commands.py` 棘轮 20 → **18**；门禁：`make check` 19 个单测 PASS、
    `compare.py` 字节一致、`run_all.py --fast` 34 PASS、
    `verify_gfx` 293 px / PSNR 49.94 / SSIM 0.99971 全不变
  - 附：存档快照这轮想用"调色板签名"当锚点定位真实槽里的分段偏移，**6 个槽全部落空**
    （调色板会被 ColorSet 覆盖），已把这次失败记录进 `docs/save-format.md`，下轮改用
    prim 数组/协程上下文当锚点

- **存档快照 `0x43DE90` 的分段结构摸清（本轮，唯一阻断级项目开工）**
  - 确认 `0x43DE90` 不是"只写时间戳"，而是**整个 `sNNN.bin` 的序列化器**：
    `0x43DE90..0x43E67B` 共 **685 条指令 / 62 次 call**，`this` 就是引擎对象，
    缓冲区在 `engine+0x00`（数据）/`+0x04`（容量）/`+0x08`（长度）/`+0x0C`（粒度）
  - 按写入顺序整理出 19 段（`docs/save-format.md` §2.4.4）：时间戳 → 3 条字符串 →
    缩略图（`engine+0x8C` 窗口对象）→ **32 个协程上下文**（`0x68DFC0`，0xB28×0x20）
    → **字符串句柄表**（`0x6A49EC`，128 条）→ **调色板**（`0x6A44C0`，0x400 B）→
    全局变量/flags（`0x6A49C0`）→ 外字表（`0x6A49CC`）→ **prim 数组**
    （`0x61DD64`，**0x34000 B** = 4096×0x34）→ 6 组补间管理器（`0x651D70` /
    `0x651E04` / `0x65380C` / `0x65BA54` / `0x679A5C` / `0x67CE64`）→ 子对象若干
  - **量级自检**：prim 212,992 B + 协程 91,648 B ≈ **304 KB**，再加调色板/字符串表/
    补间管理器，正好对上实测槽文件 500–597 KB → 说明分段表没读歪（独立证据）
  - 这两大块（prim 0x34 字节结构、协程上下文 0xB28 字节）的字段偏移我们**已经在
    渲染/VM 侧确认过**，读档时可以直接复用，不必再解一遍；剩下要确认的是各段的
    长度前缀顺序与"读回来后哪些字段要重建（纹理、字符串句柄）"
  - 本轮纯结构分析文档（无代码改动）：`check-docs.py` PASS；控制台门禁沿用上一轮同代码状态

- **存档槽路径助手 `fvp_save_slot_path()` + 存档请求消费流程复核（本轮）**
  - `src/save.c` 新增 `fvp_save_slot_path(buf, cap, game_dir, slot)`：照抄引擎的
    格式串 **`"%s/save/s%03d.bin"`**（iroseka_HD.exe `0x45DA64`），槽号只收 0..999，
    缓冲不够/参数非法返回 0；`tests/test_save.c` 的 selftest 加 **10 项**（0/999 槽、
    1000/-1 拒绝、NULL、缓冲太小），单测数 58 → **68**
  - 复核了"请求 → 落盘"的完整链路（`docs/save-format.md` §1.1）：帧更新 `0x43F7C2` 起
    检查 `engine+0xfc8`（模板就绪）→ 调 `0x43DE90` 写头部 → 置 `+0xfc9`（快照就绪）；
    然后再看 `+0xfca`（= `SaveWrite` 请求）与 `+0xfcc`（槽号），
    `wsprintfA` 拼 `%s/save/s%03d.bin` → `0x433FB0` 写文件 → `0x438300`/`0x4396F0`
    收尾 save.bin
  - 由此把"游戏内保存不生效"拆成两个明确缺口：① 外壳没消费请求（现在有了拼路径的
    助手，接上模板写入即可）② 内存快照（`0x43DE90` 之后约 500–597 KB）仍未解
  - 门禁：`make check` 19 个单测 PASS、`compare.py` 字节一致、`run_all.py --fast`
    34 PASS、`verify_gfx` 293 px / PSNR 49.94 / SSIM 0.99971 全不变

- **字体表四条命令落地：`TextFontCount/Get/Name/Set`(114/115/116/117)（缺口 24 → 20）**
  - 静态分析确认原版 `engine+0x94` 是 `vector<font>`：Count 返回条数、Get 返回
    `engine+0x6a66c4`（当前字体号）、Name 收 `0..count-1`、**Set 的下界是 -5**
    （原版行为，照抄）
  - 名字来源：原版是 exe 的字体注册表；本站改成**外壳把实际加载的位图字集注册进来**
    （`gfx-font-32/34/56` → `fvp_vm_font_register("gfx-font-<size>")`），所以运行时
    `TextFontCount` 给的是真实字集数；`fvm` 不注册 → 0 条（预期）
  - 实现：`FvpVM` 新增 `font_name[16] / nfont_names / cur_font` +
    4 个 `fvp_vm_font_*` API，syscall `case 114/115/116/117`；两个外壳汇总多一行
    `fonts N name(s), current=M`
  - 单测 `tests/test_vmmisc.c` 扩到 **2114 项**；合成脚本验证接线：
    `fonts 0 name(s), current=-1`（fvm 不注册字体，Set(2) 在空表上被拒）
  - `scripts/check-commands.py` 棘轮 24 → **20**；`make check` 19 个单测、
    `compare.py` 字节一致、`verify_gfx` 293 px / PSNR 49.94 / SSIM 0.99971 全不变

- **`Load`(40) / `SaveWrite`(96) 落地（本轮，缺口 26 → 24）—— 交互闭环的第一环**
  - `Load(slot 0..999)`：handler **0x43a870**，置 `engine+0xfcb = 1` 与
    `engine+0xfcc = slot`，再置当前线程 ctx 的 `+0x68e7d0 = 1`
  - `SaveWrite(slot 0..999)`：handler **0x43c1e0**，置 `engine+0xfca = 1` 与
    `engine+0xfcc = slot`；若 `engine+0xfc9 != 0` 还要 `call 0x438540(engine+0x14, slot)`
    立即落盘
  - **关键结论**：这两条命令本身**不读写文件**，只把"请求 + 槽号"写进引擎字段，
    真正的读写在界面/外壳一侧 —— 所以"游戏内保存不生效"的根因不是这两条没实现，
    而是**外壳没有消费请求 + 内存快照（0x43DE90）未解**
  - 实现：`FvpVM` 新增 `load_request / save_request / request_slot` +
    `fvp_vm_take_load_request()/take_save_request()`（取走语义），两个外壳汇总打印
    `save/load request slot=...`；单测 `tests/test_vmmisc.c` 扩到 **2090 项**
  - 实测：90 万 / 400 万步 headless 跑都没触发（在更后面的交互流程里）；用合成脚本
    `pushint8 5; syscall 40; pushint8 7; syscall 96` 验证接线成功：
    `save/load request slot=7 (load 1 call(s), save 1 call(s))`
  - `scripts/check-commands.py` 棘轮 26 → **24**；`make check` 19 个单测、
    `compare.py` 字节一致、`verify_gfx` 293 px / PSNR 49.94 / SSIM 0.99971 全不变

- **`render_vm --watch-gaiji`：按文本内容找外字帧（本轮，工具就位但没找到帧）**
  - 每帧检查文本窗口内容，命中已注册外字码点就停在那一帧，存
    `gaiji-hit-on.png` / `gaiji-hit-off.png`（关外字再渲一次）供逐像素 A/B；
    命中之前**不渲染**，因此可以深跑
  - 用它跑出来的**关键发现**：headless 跑到 **20,000,000 步**，iroseka 的文本只推进到
    **1,170 行就停住**（`--click` 4000 → 200 也没用），且这 1,170 行里**一次都没有**
    `怒/汗/汁/ハ`（外字字符）→ 表情符号出现在更后面的场景/分支，headless 直跑进不去
  - 结论：**真实帧的外字验证要等交互闭环（菜单/存档/选择支）能推进剧情之后**才能做；
    工具已就位。外字绘制路径本身由 `tests/test_gaiji_render.c` 的 17 项覆盖
  - 门禁：`make check` 19 个单测、`compare.py` 字节一致、`verify_gfx`
    293 px / PSNR 49.94 / SSIM 0.99971 全部不变

- **外字渲染的真实数据复核 + A/B 开关（本轮，把上一轮"还没验"补掉一半）**
  - 绕开起不来的外壳，用控制台渲染器 `render_vm` 跑真实脚本：到 60,000 步时
    **注册了 4 条外字** —— `U+6012 怒`、`U+6C57 汗`、`U+6C41 汁`、`U+30CF ハ`，
    size 全 64，**四条图形槽的图都加载成功**
  - **重要发现**：章节标题里的「空」**不是外字**（不在那 4 条里，仍走跨图集兜底），
    而外字字符在脚本里都是**单独一个字符的字符串**（iroseka：`怒` 98 次、`ハ` 80 次、
    `汗` 32 次、`汁` 4 次）→ 外字是**表情/符号贴图**，不是标题字形。这也解释了
    60,000 步那帧开/关外字 **0 px 差异**
  - 新增 A/B 验证设施：`gfx_gaiji_set_enabled()/gfx_gaiji_enabled()`、
    `gfx_gaiji_entry()`（诊断访问器）、`render_vm --no-gaiji`，并且 `render_vm`
    汇总会逐条打印外字表（码点/字号/槽/图是否加载）
  - 顺手清掉 `src/gfx.h` 里前几轮补丁留下的重复声明（3 处 `gfx_gaiji_*` 块重复）
  - 门禁：`make check` **19 个单测**、`compare.py` 字节一致、`run_all.py --fast`
    34 PASS、`verify_gfx` **293 px / PSNR 49.94 / SSIM 0.99971**（一个像素都没动）
  - **还没验**：一张真正带外字（表情符号）的帧 —— 这类符号是一次性打印的，
    `--hold-text` 抓不到，下一轮要写"按文本内容找帧"的小工具

- **`Rand`(92) / `TextRepaint`(124) 落地（本轮，缺口 28 → 26）**
  - `Rand()`：handler **0x437a20** —— 把 retval 标成 int 后 `call 0x4483da`（MSVC CRT
    的 `rand()`），返回值即 **0..RAND_MAX(0x7FFF)**。本站用固定种子的 xorshift32
    （每次 `& 0x7FFF`），**同一进程内可复现**——基准对比依赖确定性，这点不能丢
  - `TextRepaint()`：handler **0x437a50** 是 `call 0x441790(engine)`（让文本重画）。
    本站每帧整张画布重画，所以它是**真空操作**，只记调用次数
  - 单测 `tests/test_vmmisc.c` **2069 项**（范围、非恒定、可复现、NULL 安全、计数器）
  - `scripts/check-commands.py` 的棘轮从 28 收到 **26**；`make check` 现在 **19 个单测**、
    `run_all.py` **35 步**；两个外壳汇总各多一行 `rand state=... ; text repaint N call(s)`

- **外字渲染落地：`GaijiLoad`(21) 不只是记录了 —— 字符会被真的替换成图（本轮）**
  - 数据侧：`gfx_gaiji_add(cp, size, slot)` / `gfx_gaiji_count()`，表上限 64 条，
    同码点重复登记=覆盖；syscall `case 21` 把图装进高号图形槽 `0x0F00+`（避开脚本自己的
    图形槽）再登记「码点 → 槽 + 原始字号」
  - 绘制侧：`draw_text_run` 命中码点时用 `blit_rgba_scaled()`（最近邻 + alpha 混合）
    按**当前字号**等比缩放画图，并按缩放后的宽度推进笔位（排版/居中仍然对）；
    槽里没图就退回原来的字形路径。外字优先于内建字形与跨图集兜底
  - 单测 `tests/test_gaiji_render.c` **17 项**：假 host 造 8×8 纯红图，验证
    「登记前没有纯红 → 登记后同一位置出现纯红」、重复登记覆盖、参数校验、空槽不画
  - 门禁：`make check` **18 个单测**、`run_all.py` **34 步**；`verify_gfx` 仍是
    293 px / PSNR 49.94 / SSIM 0.99971（528k 帧里 `tw9` 没有文字，静态基准本来也
    覆盖不到外字，这一点在 roadmap 里写明了）
  - **还没验**：真实游戏里的章节标题（GUI 起不来，等环境恢复后抓帧对照）；
    「原始字号」目前只记录、绘制一律按当前字号缩放（推断，实机可能要改）

- **`IntToText`(24) 落地 + 命令覆盖率静态审计（本轮）**
  - `IntToText(value, opt?)`：handler **0x437646**（`wsprintfA` 格式串 `"%d"` →
    分配字符串句柄 → 返回类型标 5）。本站用 arena 造 `FvpStr` 当句柄；
    第 2 个参数（文档记作 opt 1..10）没有证据说明影响格式，先忽略
  - 新增 `scripts/check-commands.py`：静态扫 `src/syscall.c` 的四个 `switch (cid)`，
    把 148 条 id 分成"有处理分支 / 通用 `*Test` / 已确证空操作 / **缺口**"，
    并带 `MISSING_BASELINE` 棘轮（缺口变大就报 REGRESSION），已接进 `run_all.py`
  - **更正经的口径**（本轮自查发现）：之前说"148 条里 146 条已实现"是把"实测调用过的
    命令"当成了全集。真实数字是 **119 条有处理分支 + 1 条走通用 `*Test` 规则 +
    28 条缺口**，那 28 条在本作 90 万步里一次都没被调用（所以运行时直方图看不见）。
    完整名单与棘轮说明写进 `docs/roadmap.md` 的"命令覆盖率的真实数字"
  - 回归测试：`tests/test_vm_edge.py` 新增 2 个 `IntToText` 用例（`42` / `-7`），
    现在 **49 个用例** 全 PASS

- **（口径修正）上一轮"脚本命中的命令全部有实现"仍然成立**，但请连着上面的静态审计一起看：
  它只覆盖实测路径，不代表 148 条全实现

- **`GaijiLoad`(21) / `V3DMotion`(142) 落地（本轮，只记录参数）—— 到此脚本命中的命令全部有实现**
  - `GaijiLoad(char, size 12..0x40, image)`：handler **0x43f4f0**，两个字符串 + 一个
    int 尺寸，然后 `call 0x43e9c0(engine+0x6a49cc, str1, size, str2)`
    - 实测参数形态很有信息量：**`size=64, "ハ", "graph/gaiji_heart68"`** —— 这条命令
      是"外字"：把某个字符替换成 graph 包里的一张图。它同时解释了为什么 56px 位图字集
      只有 5 个字形、以及 `tw9` 的 `TextFont(9,-2,1)` 到底在指什么
  - `V3DMotion(x?, y?, z?, timeMs 1..300000, mode 1..5, flag?)`：handler **0x43cfe0**，
    arg4/arg5 的取值区间与 `MotionMove` 完全一致；本作没有 3D 场景
  - 实现：只记录（`gaiji_calls/size/name1/name2`、`v3d_calls/time_ms/mode`），
    表壳与 `fvm` 汇总各打印一行
  - 实测（`fvm --steps 900000`）：`gaiji 24 call(s), size=64 name=ハ / graph/gaiji_heart68`、
    `v3d 20 call(s), time=1 ms mode=1`，文本仍是 **195 行**、`stall none`
  - 单测 `tests/test_gaiji.c` **33 项**（`make check` 现在 **17 个单测**，
    `run_all.py` **32 步**）

- **`WindowMode`(147) / `ExitMode`(17) 落地（本轮，只记录模式值）**
  - `WindowMode(mode -1..6)`：handler **0x43d0d0**。模式 0/1 把窗口状态置 0/1 并作为
    返回值，模式 2 返回当前值（当前值 ==2 时返回 -1），-1 走"切回"分支
  - `ExitMode(mode 0..4)`：handler **0x43a1a0**。0 分支清 `engine+0x82` 并置当前
    线程 ctx 的 `+0x68e7d4 = 1`，1..4 各有分支
  - 两者真正改的是窗口/线程层字段，本站没有对应结构 → 只记录
    （`window_mode_last / window_state / exit_mode_last` + 计数），外壳与 `fvm`
    汇总打印 `window mode=.. state=.. ; exit mode=..`
  - 实测（iroseka 90 万步）：`WindowMode(0)` 1 次、`ExitMode(2)` 1 次，
    文本仍是 **195 行**、`stall none`

- **外壳新增 `--startup-trace FILE` 启动追踪（诊断用）**
  - 日志是在**解析完参数之后**才打开的，所以"进程起来但还没开日志就卡住"这种情况下
    日志里什么都没有。这个开关把启动分成 9 个阶段写文件并立刻 flush，卡在哪一步一眼可见
  - 启动点：entry / before timing+dpi / dpi / com / 命令行切分 / 日志+选项+脚本+VM /
    音频 / gfx（含字体）/ 窗口 / ready

- **单测 `tests/test_textbuff.c` 扩到 44 项**（`TextBuff` 21 项 + `WindowMode` /
  `ExitMode` 23 项），`make check` 仍是 **16 个单测**、`run_all.py` 31 步

- **`TextBuff`(110) 落地（本轮，只记录尺寸）**
  - handler **0x43c830**：`TextBuff(twIdx, a, b)`，twIdx 必须 int 且在 0..31；
    a/b 各自"是 int 且 >= 0"才采用，否则默认 **8**；随后 `call 0x430ea0(tw, a, b)`
  - setter **0x430ea0**：a/b 任一 **> 0x400** 时整条命令放弃；否则写 u16
    `tw+0x5dc = a` / `tw+0x5de = b` 并重建缓冲（释放 `tw+0x520` 的旧块）——
    这是"文本窗口缓冲尺寸"，不是排版参数
  - 实现：`FvpVM` 新增 `text_buff[32]`（w/h/set）与 `text_buff_calls`，两个外壳汇总
    打印 `text buff N call(s)`；不重建缓冲（我们的文本是按行存的动态串）
  - 实测（控制台）：`fvm --steps 900000` 汇总出现 `text buff 6 call(s)`，文本仍是
    **195 行**、`stall none`；此前它在 `unknown cmds` 里
  - 单测 `tests/test_textbuff.c` **21 项**（`make check` 现在 **16 个单测**，
    `run_all.py` **31 步**）

- **`GraphRGB`(23) / `SysProjFolder`(109) 落地（本轮，只记录状态）**
  - `GraphRGB(graphId, r, g, b)`：handler **0x43a3d0**。槽号必须 int 0..0xFFF；
    三个分量**各自**"是 int 且在 0..200"才采用，否则默认 **100**；随后
    `call 0x428910(engine+0x101064+id*0x51c, r,g,b)` + tail-call
    `0x437330(engine+0x45c, id)`（与 GraphLoad 同一个"槽已更新"回调）
  - `SysProjFolder(folder)`：handler **0x43c7f0**。只接受字符串参数（原版 type 4/5），
    然后 tail-call `0x433c60(engine+0x6a7e78, str)`
  - 实现：`FvpVM` 新增 `graph_rgb[4096][3] / graph_rgb_set[] / graph_rgb_calls` 与
    `proj_folder / proj_folder_calls`；两个外壳汇总打印
    `graph rgb N call(s), folder=...`。**不消费**：渲染端没有"100 是否等于 1.0 倍、
    200 是过曝还是叠色"的证据，工程目录影响哪条资源路径也没解出（同 TextFont 的处理）
  - 实测：iroseka 45 s 外壳 `GraphRGB` 4 次、`SysProjFolder` 1 次（空串），
    `unimpl cmds` 从 **7 种降到 5 种**（`147×1 110×6 21×24 17×1 142×7`）
  - 单测 `tests/test_graphcolor.c` **24 项**（`make check` 现在 **15 个单测**，
    `run_all.py` **30 步**）

- **`ControlMask`(8) / `ControlPulse`(9) / `CursorChange`(10) / `CursorMove`(11) 落地（本轮，只记录状态）**
  - handler 依次 **0x437440 / 0x437460 / 0x437470 / 0x4374a0**（同 `work/vm_id_handler.json`）：
    `ControlMask` 只看参数的**类型字节**（null → engine+0x458=1）、`ControlPulse` 置
    engine+0xfd8=1、`CursorChange` 收 int 0..3 后写 engine+0x88（并换 Win32 光标）、
    `CursorMove(x,y,force)` 校验 x/y 是 int 后调 0x4417c0 设鼠标位置
  - 实现：`FvpVM` 新增 `control_mask / pulse / cursor_shape / cursor_x,y` 与 4 个调用计数，
    `src/vm.c` 里 4 个状态函数，`syscall.c` 的 `case 8/9/10/11` 分派；两个外壳的汇总都
    打印 `control mask=.. pulse=.. cursor=.. pos=.. (mask N / pulse N / change N / move N)`
  - **已知边界（照实写）**：原版用 `type==0` 表示 null，本站的值模型没有 null
    （`FVP_INT == 0`，未赋值 global 读出来是 int 0），所以 `ControlMask` 的 1 分支
    **当前不可达**，`CursorMove` 第三参数退化成只记录类型 —— 文档里标为"已知不可达"，
    不是猜出来的语义；光标形状/屏蔽位的下游消费（没有证据）也没有实现
  - 单测 `tests/test_control.c` **42 项**（`make check` 现在 **14 个单测**）
  - 实测：iroseka 45 s 外壳 `ControlMask ×38 / ControlPulse ×35 / CursorChange ×1 /
    CursorMove ×1`，`unimpl cmds` 从 **11 种降到 7 种**
    （`109×1 147×1 110×6 21×24 17×1 142×7 23×4`）；`compare.py` 仍逐字节一致、
    `verify_gfx` 仍是 293 px / PSNR 49.94 / SSIM 0.99971

- **第三部作品交叉验证 + 引擎级"卡死侦测"（本轮）**
  - 用户提供《樱花，萌放。（さくら、もゆ。）》：`Sakura.hcb` 5,002,852 B 能被完整解码
    （966,284 条指令，`main_start` 0x36a79），且它的**内嵌 148 条命令表与另外两部
    逐项相同** —— 这是"syscall 立即数跨作品稳定"的硬证据
  - 该作品跑到第 2,179 步会进入**脚本自带的死循环**（pc 0x8ef82..0x8ef9b：
    `while (i >= g[1711]) garr[1733][i] = 0`，计数器为 0 时恒真）。为了不再静默
    烧掉整个步数预算，VM 新增**卡死侦测**：连续 N 步没有执行任何 syscall 就判定
    自旋并停下
    - `fvm`: `--stall-after N`（默认 2,000,000，0 关闭）、`--stall-exit`（卡死时
      退出码 **6**）；汇总新增 `stall pc=0x... after N steps without a syscall`
    - 外壳: `--stall-after N`，汇总新增 `stall` 行，卡死后不再往 VM 里塞步数
    - 实测：`Sakura` 1 s 内停下并报 `stall pc=0x8ef8d after 2000001 steps`；
      `iroseka` 90 万步 / 45 s 长跑均 `stall none`（不误报）
  - `tests/verify_multigame.py` 扩到 **三部作品**：
    - 新增跨作品不变量：三部 HCB 的内嵌命令表逐项相同（148 条，名字+argc+顺序）
    - `sakura` 标为 **KNOWN BLOCKER**：要求两次运行都以退出码 6 结束、卡死 pc 仍落在
      那个循环窗口、两次输出逐字节相同；**它哪天跑过去了这条检查就会失败**
  - 顺带确证并记录到 `docs/vm-commands.md` §1.2：`jmpcond`(0x445a30) 的判据是**值的
    类型字节 == 0**（不是数值 == 0），比较类 opcode 把结果写进类型字节；比较运算走
    **7×7 类型对表** `0x481450`（索引 `a.type*7+b.type`，默认项返回"相等"）

- **`Motion\*Stop`(43 / 46 / 55 / 58) 落地：按图元撤补间（本轮）**
  - 四条命令 argc 全为 1、参数是 `primId`，handler 依次 **0x43a990 / 0x43aae0 /
    0x43b0a0 / 0x43add0**（`work/vm_id_handler.json`）：`MotionAlphaStop`(43) /
    `MotionAnimStop`(46) / `MotionMoveS2Stop`(55) / `MotionMoveZStop`(58)。
    实测调用量（45 s 外壳）43×4、46×25、55×7、58×6 —— 都是"动画半路被打断"时才走
  - 实现：补间表统一在 `src/motion.c`（`MOVE_XY` / `ALPHA` / `SCALE` 三类），新增
    **`fvp_motion_cancel(m, prim_id, kind)`**（`kind < 0` = 所有种类），
    `gfx_motion_cancel()` 只做薄封装；`src/syscall.c` 的
    `case 43 / 46 / 55 / 58` 分派：43 → ALPHA、55 → SCALE、**46 / 58 → 本引擎没有
    帧动画 / Z 补间 → 空操作**（原版也是在空表上撤，等价）。没有 gfx 上下文
    （`fvm.exe`）时仍落回"未实现"分支，与以前一致
  - 实测：外壳 45 s 的 `unimpl cmds` 从 **15 种降到 11 种**（43 / 46 / 55 / 58 全部
    消失），剩 `9×57 8×46 21×24 142×8 110×6 23×4 109×1 147×1 10×1 11×1 17×1`
  - 单测：`tests/test_motion.c` +16 项（共 **47** 项）；`run_all.py` 27 PASS / 0 FAIL

### 已知问题

- **（已解决）外壳 exe 曾有一段时间起不来**：2026-11-24 15:00 之后的一段窗口里，
  `bin\fvp.exe` 在**进入 `WinMain` 之前**就卡住 —— 进程存在但既不写日志、也不建窗口，
  `--startup-trace` 也拿不到第一行（卡在 CRT/加载器阶段）。当时的交叉排查记录：
  `notepad` 能起（551 ms）、`fvm.exe` 等控制台程序全正常、链接同一组
  `-mwindows -lgdi32 -luser32 -lwinmm -lshell32 -lole32` 的 20 行探针 exe 同样卡住，
  判断是第三方注入（NVIDIA Overlay / Nahimic / League Akari 之类）在装载阶段死锁。
  **2026-11-24 19:03 复测已恢复正常**（机器环境变化后自愈，未改任何代码）：
  `--game <iroseka> --run-seconds 6 --no-audio --screenshot` 退出码 0，写出
  1920×1080 帧（8,294,400 B），日志 `bin\fvp-win32.log` 完整，
  `unimpl cmds  none`。教训写在这里：**这类"进程起来但不写日志"的现象要先怀疑注入，
  而不是先改代码**；同时 `--startup-trace` 的价值被证实（它能区分"卡在 CRT"和
  "卡在 WinMain 之后"）。

- **《樱花，萌放。》启动流程卡在脚本死循环**（pc 0x8ef82..0x8ef9b）。计数器
  `global 1711` 由 0x8eef3 初始化、0x8f5d0 递增、被这个循环自身清零；我们的运行到
  这里时它是 0，说明 0x8eef3 之后的某一步和原版分了叉。已由门禁固定成"确定性卡死"，
  下一步要顺着 0x8eef3 的调用链找分叉点（见 `docs/roadmap.md` 的交叉验证一节）

### 修复

- **修正 opcode `0x1f` 的语义：它是「位测试」，不是「取真值」**
  - handler **0x444e40**：两个操作数都是 int 时算 `(a & (1 << (b & 31))) != 0`
    （x86 的 `shl` 只取移位数低 5 位），任一操作数不是 int 就写 0
  - 以前按 `truth(a)` 实现：对 `(4, 1)` 给 1，原版是 0（4 的 bit1 为 0）——语义确实错了
  - **冻结基准不受影响**：iroseka 的前 90 万步里 `0x1f` 一次都没执行（实测 pctrace 直方图
    为 0），所以 `compare.py` 仍然逐字节一致；`src/vm.c` 与 `ref/fvm_ref.py` 同步修正
  - 顺带把四条比较 opcode 的 handler / 用哪张 7×7 表 / `sete` 还是 `setne` 写进
    `docs/vm-commands.md` §1.2。**但 a/b 的先后顺序这一步没能钉死**：按 cdecl 读出来
    0x25 应为 `>=`、0x27 应为 `<=`，可真按这个对调之后 iroseka 90 万步输出直接变成
    0 行（这两条在 90 万步里被执行 **88,325 次**）。所以现有实现不改，文档里如实
    记下这个矛盾，等更多证据
  - 回归测试：`tests/test_vm_edge.py` 新增 4 个用例（bit 置位 / bit 清零 / 33 的移位
    掩码 / 非 int 操作数），现在 **47 个用例** 全 PASS

- **两处测试体系缺口（第 11 轮顺手补上）**
  - `tests/test_motion.c` 一直**没有 Makefile 规则**：`run_all.py` 里有它的步骤，
    但 `make test_motion` 直接 "No rule to make target"，实际跑的是手编的旧二进制
    （31 项，落后于源码）。现在补上 `test_motion` 目标并进 `make check`
    —— `UNIT_TESTS` 从 12 个变 **13 个**
  - `test_glyph_fallback` 把临时图集写死到相对路径 `out/fuzz/`，只有 cwd = `fvp/`
    时才写得出来；`run_all.py` 是从仓库根跑的，于是稳定 3 项 FAIL
    （"write 34px atlas" / "load atlases"）。改用 `%TEMP%`（回落到 `.`）后两个
    工作目录都 PASS

- **窗口正中的"黑方块"＝豆腐块：新增跨图集字形兜底**（用户问"中间那个方块是什么" →
  定位 → 修 → 抓帧复验）
  - 现象：章节标题位置出现白边黑方块 —— `draw_tofu()` 的缺字占位符
  - 根因：位图字集是**按实际用到的字抠出来的**，覆盖面很窄：32px/34px 各 228 个字形，
    而 **56px 只有 5 个**（`・上学屋校`）；章节标题层 `tw9` 的排版字号正好是 56，
    于是「空」落在那张 5 字图集里画不出来。原版靠 `TextFont(9,-2,1)` 选字模，
    但 `-2` 对应哪套没有证据（见下一条），只能把**取字**做稳
  - 修法：目标图集缺字时，在其它已注册图集里找同一个码点（优先字号最接近），按
    `目标字号/源字号` 最近邻缩放画上去，推进量同比例缩放（排版/居中仍对）；
    全都找不到才画豆腐块
  - 诊断：`GfxStats.fallback_glyphs / tofu_glyphs`（每次 render 归零），外壳汇总打印
    `glyphs N tofu, M from fallback atlas`
  - 实测：8 s 抓帧章节标题「空」正常显示（`out/title-zoom.png`），汇总
    **`0 tofu, 1 from fallback atlas`**；单测 `tests/test_glyph_fallback.c` **16 项**
  - `verify_gfx` 仍是 293 px / PSNR 49.94 / SSIM 0.99971（528k 帧上 tw9 无文字）

- **对话框文字「残影」：撤回阴影绘制 + 修正 `TextShadowDist`/`TextSpace` 的语义读错**
  （用户实跑报障 → 定位 → 修 → 复验）
  - 现象：外壳实跑时对话框里每个字右边都拖着一份深色重影；抓帧放大 4× 确认是**向右偏移的
    实心深色文字副本**。修复后用户确认「现在没有了」
  - **根因①（画错了）**：把命令参数当"阴影偏移"直接渲染 —— 在 `(x+dx, y+dy)` 先用
    `(16,20,30)` alpha=255 画一遍实心副本再画正文。`draw_text_window()` 里这一层已删除
  - **根因②（读错了 handler）**：`0x43CE10` 其实是 **TextSpace(128)**（两个分量各 -32..32 →
    `tw+0x5A8`/`tw+0x5AA`），而 `TextShadowDist(125)` 的 handler 是 **0x43CBB0**：
    `(twIdx, dist 0..12)`，`<0` 当 0、`>12` 当 12，转 `0x4248B0` 只写 `tw+0x597` 一字节。
    所以脚本传的 **`dist=12`** 是"阴影距离拉满"，不是 `dx=12` —— 被我们画成了向右 12 px 的重影
  - 修复：`FvpTextAttr` 拆成 `shadow_dist`(0..12) / `space_x` / `space_y`(-32..32)（掩码新增
    `FVP_TEXTATTR_SPACE`）；`case 125` 改成单参数钳制、**新增 `case 128`**（逐分量判定：超范围
    的那个分量跳过、另一个仍生效）；`gfx_text_set_shadow()` 只存距离，新增
    `gfx_text_set_space()` 存间距，**两者都不参与绘制**（原版阴影的形状/颜色/透明度、以及间距
    在排版里的用法都没有证据；参考实现 `work/render.py` 同样只存不画）
  - **实测（900k 步，`fvm --dump-textattr`）**：`tw0 shadow=12 space=0,0 skip=3`、
    `tw8 shadow=12 space=0,0`、**`tw9 shadow=12 space=32,1`**（章节标题字间距拉满 32、行间距 1）
    —— 这套解释正好对上"章节标题字拉得很开"的观感，说明字段没读反
  - 测试：`tests/test_textattr.c` **56 项**（125 的钳制语义 + 128 的逐分量跳过）
  - 验证：`verify_gfx.ps1 -SkipDump` 仍是 **293 px / PSNR 49.94 / SSIM 0.99971 / 24/24 图元
    0 px**；外壳 45 s 的 `unimpl cmds` 从 20 种降到 **18 种**（128 消失）
  - `fvm --dump-textattr` 现在打印 `shadow=dist space=x,y skip=mode`

### 新增

- **`TextFormat`(118) / `TextFunction`(119) 落地：只记录、不参与排版**
  - `TextFormat` handler **0x43CC10**（argc 7）逐字段判范围写六个偏移：
    a/b → `tw+0x5A8`/`tw+0x5AA`（-32..32，**与 TextSpace 是同一对字段**）、
    c → `tw+0x5AE`（0..64）、d → `tw+0x5AC`（0..64，且把 `tw+0x5E8` 抬到 ≥d）、
    e → `tw+0x5B4`（-16..16）、f → `tw+0x5B0`（-16..16）
  - `TextFunction` handler **0x43CCF0**（argc 4）三个开关：a → `tw+0x5A5`（0..1）、
    b → `tw+0x5A4`（0..2）、c → `tw+0x5A6`（0..2），各自独立判定
  - **实测（900k 步）**：`tw0 fmt=64,64,16,16,32,32 func=1,2,2`、
    `tw8 fmt=1,1,1,1,32,1`、`tw9 fmt=0,0,0,0,32,1`（tw9 只收到过 TextSpace，
    所以 fmt 全 0 而 space=32,1）—— 这正好交叉验证了「TextFormat 的前两个参数就是
    TextSpace 那两个字段」
  - 只记录不参与排版：这九个值（边距/间距/开关）在原版排版端怎么消费没有证据，
    参考实现也只声明不使用
  - `FvpTextAttr.fmt_pad_a/fmt_pad_b/fmt_off_a/fmt_off_b` + `func_a/func_b/func_c`
    （掩码 `FVP_TEXTATTR_FORMAT` / `FVP_TEXTATTR_FUNCTION`）+
    `fvp_textattr_set_format()` / `fvp_textattr_set_function()`；
    gfx 侧 `gfx_text_set_format()` / `gfx_text_set_function()` 只存不画；
    `fvm --dump-textattr` 多打一行 `fmt=... func=...`
  - 测试 `test_textattr` **65 → 82 项**（逐字段跳过、全越界拒绝、开关范围）
  - 外壳 45 s 实测 `unimpl cmds` 从 17 种降到 **15 种**

- **`TextFont`(113) 落地：只记录、不换字模**
  - handler **0x43DA70**：arg0 必须是 int 0..0x1F；arg1（int，**-5 <= v <= 引擎字体数-1**）
    → `0x4247F0` 写 `tw+0x590`（**u16**）；arg2（同一区间）→ `0x424860` 写
    `tw+0x593`（**u8**）；两个分量各自独立判定，超范围的跳过
  - **实测（900k 步，两部作品）**：`(0,-1,1)` 正文、`(8,-1,1)` 人名、
    **`(9,-2,1)` 章节标题**、`(10,-1,1)` —— 字体号是**负数**（-1 / -2），
    正是 `-5` 那个下界给的特殊值留的空间；第二字段恒 1
  - 只记录不换字模：负数字体号对应哪套字模没有证据（我们只有 3 个从游戏数据里解出来的
    图集），渲染仍按**字号**挑字集 —— 所以像素基线不变
  - `FvpTextAttr.font_id / font_id2`（掩码 `FVP_TEXTATTR_FONT`）+
    `fvp_textattr_set_font()` + `gfx_text_set_font()`；`fvm --dump-textattr` 打印
    `font=id,id2`
  - 测试 `tests/test_textattr.c` **56 → 65 项**（-5 下界、逐分量跳过、越界拒绝）
  - 外壳 45 s 实测 `unimpl cmds` 从 18 种降到 **17 种**（113 消失）

- **台词历史 `HistorySet`(26) / `HistoryGet`(25) 落地 + `fvm --dump-history`**
  - 新模块 `src/history.c` / `src/history.h`：100 条环形记录，每条 = {人物名, 台词,
    行 id}（12 字节，`engine+0x6a49c0` = {count, pos, base}）
  - 语义逐条来自静态分析：写入接口 **0x43A5E0**（null → 开新一条：`count=min(count+1,100)`、
    `pos=(pos+1)%100`、清槽；kind 0/1 只收字符串；kind 2 只收 ≥0 的整数），
    读取接口 **0x43A490** + 取字段 **0x438090 / 0x4380C0 / 0x4380F0**
    （`idx` 必须 `< count`，槽位 `pos − idx` 负数 +100 回绕，字段为空时返回**空串**）
  - **实测（900k 步，两部作品）**：`HistorySet` 480 / 425 次，形态固定为
    `(0, 人名) (1, 台词) (2, 行 id)` 加 `(0|1, 整数)` 的无操作调用；`HistoryGet` 动态
    调用 **0** 次；**从不发 `HistorySet(null)`** → `count` 恒 0。这是原版行为，不是移植
    缺陷（原版程序里能让 count 增长的唯一路径就是这条 null 调用）
  - `fvm --dump-history` 导出的槽 0 与真实台词一致：
    `value=2000130 text1="　指をさす。" text0="　　 澪　　 "`
  - 测试 `tests/test_history.c` **164 项**（已并入 `make check` 与 `run_all`）：
    环回绕 / count 上限 / 槽位映射 / 类型与 kind 过滤 / 截断 / NULL 安全
- **`TitleMenu`(141) / `MenuMessSkip`(41) / `SysAtSkipName`(108) 定性为「原版即空操作」**
  - 三者注册的 handler 都是 **0x437950**，而该函数体只有一条 `ret $0x4`
    ——不读参数、不写状态、不设返回值。所以这三个命令以前被算进「未实现」是统计口径
    问题，现在按原版语义显式空转（不是补上了功能，而是证明原版也没有功能）
  - 外壳 90 s / 480,942 步实测 `unimpl cmds` 现在是 **20 种**（同一脚本、同一时长的历史
    记录是 25 种；差额里 125/127/130/95 是前几轮落地的，本轮 26 不再出现在列表里）
- **多作品兼容自动化门禁 `tests/verify_multigame.py`**（第二部作品的硬门禁）
  - 对两部目标作品各跑两遍 `fvm --steps 900000`：`iroseka.hcb` 与 `Hoshimemo_HD.hcb`
  - 断言：① 两个 HCB 都能解码（`script_len` / 指令数 / `main_start` 合理）
    ② 同一部作品两次运行输出**逐字节相同** ③ 900k 步时文本行数不漂移
    （iroseka **195 行 / 6,496 B**、星空のメモリア **164 行 / 6,644 B**）
    ④ 0 个 U+FFFD、日文字符占比 > 50%
  - 实测（900k，两部各两遍）：iroseka `script_len` 4,273,221 / 948,219 条指令 /
    `main_start` 0x22f48 / 195 行 / JP 59.3%；星空のメモリア `script_len` 4,616,534 /
    1,135,159 条指令 / `main_start` 0x36a93 / 164 行 / JP 69.9%；**两部都是 0 个替换字符**
  - `--steps 300000` 是冒烟档（此时只断言"有文本产出"，不比对 900k 的行数基线）
  - 已并入 `tests/run_all.py`（`--fast` 走 300k 档），`run_all` 现有 **25 个步骤**
  - 意义：之前"第二部作品也跑得通"只有一次性手工记录，现在有可复跑的门禁

- **`TextShadowDist`(125) 与 `TextSkip`(127) 落地**（静态分析逐条确认，不是猜的）
  - `TextShadowDist(twIdx, dx, dy)`：两个分量各 **-32..32**，分别写 `tw+0x5a8` / `tw+0x5aa`；
    超范围的分量单独被忽略（另一分量仍生效）。出处 **0x43CE10**
  - `TextSkip(twIdx, mode 0..3)`：字节写 `tw+0x5d7`，超范围整条命令忽略。出处 **0x43CDD0**
  - 渲染侧：`gfx_text_set_shadow()`，偏移非 0 时先在偏移处压一层暗色再画正文；
    **偏移为 0 时完全不画阴影层，像素基线与之前一致**（`verify_gfx` 仍是 293 px / PSNR 49.94 / SSIM 0.99971）
    > ⚠️ 这一条已在同一天**被推翻并撤回**：脚本给的偏移是 (12,0)，画出来是向右 12 px 的
    > 实心重影（用户报「残影」）。现在只记录数值不渲染，见本文件顶部的「修复」。
  - 顺带修正 `TextSpeed`(129) 的取值语义：静态分析 0x43CE70 是 `cmp $-1; jl ret` +
    `cmp $300000; jg ret`，即**超范围整条命令被忽略**，而且 **-1 是合法值**
    （引擎里表示"用设置里的文字速度"）—— 之前把它钳成 0 是错的
  - 测试：`tests/test_textattr.c` 扩到 **47 项**（含 -1 / -7 / 500000 的拒绝语义、
    阴影两分量独立判定、skip 0..3 边界）
- **`SaveData`(95) 记录 + `fvm --dump-savedata`**：查明脚本到底想存什么
  - 实测（90 万 / 200 万步都是 3 次）：`kind=2 idx=900 value=1`、`903`、`904`
    —— 只用来打三个"已读/进度"标记，**不是**把游戏状态摊平的入口
  - 推论：槽尾那 500–597 KB 引擎状态由 `0x43DE90` 直接序列化内部结构，所以
    "解 `SaveData` 的 kind 语义"并不能换来"能写存档"（已写进 `docs/save-format.md` §2.5）
  - 实现：`FvpVM.save_data[256]` + 溢出计数，`fvm --dump-savedata` 打印，
    外壳退出总结打印 `savedata N record(s) of M call(s)`
- **逐字显示（打字机效果）：`TextSpeed`(129) + `TextSuspendChr`(130) 的渲染侧落地**
  - `gfx` 侧：文本窗口新增 `speed_ms` / `shown_chars` / `total_chars` / `accum_ms`；
    `gfx_text_advance(g, dt_ms)` 按"每字毫秒"推进，`gfx_render()` 只画前 N 个字符
    （UTF-8 码点边界安全，2/3/4 字节序列不会切半个字）
  - `TextSuspendChr(130)`：窗口还在逐字显示时把线程挂起（走现有的 yield 机制）
  - **默认关闭**（外壳 `--typewriter` 打开）：关闭时 `shown_chars == total_chars`，
    绘制逻辑不看这些字段，`gfx_text_advance()` 直接返回 —— **已验收的帧一个像素都不变**
  - 实测：外壳 `--typewriter` 跑 12 s / 57.42 fps 正常，`--screenshot` 导出帧里台词完整
    显示（1920×1080，见 `out/tw-shell-on.png`）；未实现命令从 25 种降到 23 种（130 消失）
  - 测试 `tests/test_typewriter.c` **31 项**（已并入 `run_all`）：默认关闭不推进、
    按 ms 逐字、余数不丢、speed 0 立刻显示、UTF-8 混排计数、关掉时拉满、多窗口独立、NULL 安全
  - `render_vm` 新增 `--typewriter` / `--typewriter-dt`，用于确定性回放
- **引擎定时器 `TimerSet`(139) / `TimerGet`(138) / `TimerSuspend`(140)**
  - 语义来自静态分析（`docs/vm-commands.md` §5）：16 个定时器，set 记 `{active, start, duration}`，
    get 返回已过毫秒、给了第二参数则返回 `elapsed*opt/duration`
  - 新模块 `src/timer.c` + `src/timer.h`，VM 只把 `vm.virtual_ms` 当 `now_ms` 传下去；
    **默认没开 `--timeline` 时时钟恒 0 → `TimerGet` 返回 0**，与旧的「未实现返回 0」行为一致
  - 实测：900k 步 unknown 直方图里 `139`（259 次）消失，文本仍是 195 行、协程 5/5，
    字节基线不动
  - 测试 `tests/test_timer.c` **37 项**（已并入 `run_all`）：起点/elapsed/百分比换算与取整/
    INT32 夹紧/ms 钳制/重设重置/16 个互不干扰/NULL 安全
- **纹理缓存 LRU 上限（默认 256 MiB）**：长跑 RSS 阶梯式增长的根因
  - 采样证据：0–95 s 稳定 194 MiB，之后随换场景阶跃 222.9 / 209.5 / 221.7 MiB，
    每一跳都精确对应缓存 75→78→80→81→83 张（108.7 MiB）的阶跃 —— **不是泄漏，是只进不出**
  - 语义：`tex_unref()` 到 0 引用不再立即释放，图像留在缓存等复用；新加 `tex_free()`
    只由淘汰和 `gfx_destroy` 调用。`refs` 只数外部持有者，**refs == 0 = 可淘汰**
  - 淘汰只挑 refs == 0 的条目（被图元/组槽/部件引用的永不动），因此像素输出不变：
    `verify_gfx.ps1 -SkipDump` 仍是 293 px / PSNR 49.94 / SSIM 0.99971
  - 接口：`gfx_tex_cap_bytes` / `gfx_tex_set_cap_bytes` / `gfx_tex_evictions`；
    外壳 `--tex-cache-mb N`（0 = 不限制），`tex:` 行打印上限与累计淘汰数
  - 测试：`tests/test_tex_cache.c` **26 项**（已并入 `run_all`，共 22 步）
  - ⚠️ **实测限制（2026-11-24，`--tex-cache-mb 64` 跑 150 s）**：上限设 64 MiB，缓存仍到
    113.8 MiB、**只有 4 张被淘汰**。原因是本作脚本把整套 UI 贴图（`main_window_mask` + 24 个
    `fc_*` 按钮图）和背景/立绘一次性 `GraphLoad` 到 79 个槽里、**从不释放**，这些图 refs>0
    因而受保护。所以这个上限目前只能回收「被覆盖过的旧图」，**不能压住被引用图的内存**；
    长跑 RSS 依旧随场景阶梯上升（194 → 223 MiB 与不设上限时相同）。彻底解决需要引擎的
    资源卸载策略（未结构分析），已记入 `docs/roadmap.md` 的已知限制。
- **32 个协程槽进 C，默认开启**（`ThreadStart`/`ThreadExit`/`ThreadNext`）
  - `src/vm.h`：`FVP_THREAD_SLOTS 32`、`thread_slots[32]`、`threads_on`、诊断计数
  - `src/vm.c`：`fvp_vm_thread_start()` / `fvp_vm_thread_exit()`，死线程回收时清槽
  - 语义按静态分析：`ThreadStart(0, entry)` 先重置 32 槽再在 0 号槽建线程；`ThreadExit(0)` 收掉
    除自己外所有槽；**主线程不在槽里**（否则 `ThreadStart(0, …)` 会把自己重置掉）
  - **实测行为中立**：900k / 2M 两档，开关两种模式的文本**逐字节相同**，且都等于冻结基准
    （`7f9837ff…` / `32d7d4e2…`）。原因见下一条修正
  - 新增门禁 `tests/verify_threads.py`（已并入 `run_all.py`）：`start == exit`、开/关同字节、
    900k 与 2M 都对冻结基准
  - 新增 `--no-threads`（fvm 与 Win32 外壳），用于 A/B 与回归

### 修复

- **栈底帧的返回地址指回了自己，导致 `ret` 无限自循环**
  `initstack` 在没有 `call` 建立帧时（线程/函数入口）会新建"栈底帧"；旧实现把返回地址写成
  `ins->pc`（`initstack` 自己），于是 `ret` 跳回入口再执行一次 —— 这正是 2026-11 Python 原型
  "58,908 次空转"和"开线程后主线程被饿死"的真因。改为指向 `script_len`（脚本末尾），
  `ret` 即结束线程。主线程不会从栈底帧返回，所以关闭协程时基线不变。
- **周期性能报告的 fps/slow/frame_avg 用了全程累计计数**（除以 5 s 窗口），数值随运行时间
  线性增长（实测 5 s 报 58.7、135 s 报 **1606.9**），不能当门禁。改为独立的窗口统计
  `wperf`，报告后清零；退出总结仍用全程累计。修好后连测四窗：**57.2 / 59.3 / 60.0 / 60.0 fps**。

### 诊断

- `tex:` 行（`gfx_tex_count()` / `gfx_tex_bytes()`）：把 RSS 增长归因到纹理缓存。
  20 分钟采样（每 10 s 记 RSS）结论：**不是内存泄漏，是纹理缓存只进不出**
  —— RSS 在 0–95 s 稳定 194 MiB，之后随换场景阶跃到 222.9 / 209.5 / 221.7 MiB，
  每一跳都精确对应缓存 75→78→80→81→83 张（108.7 MiB）的阶跃。
  → 已由上面的 LRU 上限解决。

### 修正（推翻旧结论）

- **「约 50 句台词缺失」不成立**。旧文档认为台词在协程里拼字符串、协程没实现所以缺失，
  且认为协程入口卡在等时间条件的循环里。实测（`work/probe_threads3.py` + `docs/05-threads.md` §10）：
  本作每个协程入口都是 `initstack; pushint8 0; jmpcond <ret>`，条件是**编译期常量 0**，
  协程建起来就退出 —— 原版引擎里这些协程体也不执行。900k 步 195 行、2M 步 450 行线性推进，
  没有断流。`jmpcond` 方向也做了反证：把方向反过来同一脚本只出 **0 行**。
- 相应改写 `README.md` / `README.en.md` / `docs/05-threads.md` / `docs/render-model.md` /
  `docs/roadmap.md` 里的旧说法。

## [0.8.0] — 未发布：完整测试版（动画/时间模型起步，目标版本）

### 新增

- **最小时间模型（`--timeline`，默认关闭）**：虚拟时钟 + `ThreadSleep`/`ThreadWait` 定时唤醒
  - 调度器在一轮全部定时阻塞时推到**最早** `wake_ms` 并叫醒到期线程；无目标则结束
  - 实测 900k 步：legacy `virtual ms 0` / `--timeline` `virtual ms 2000`，**两者文本逐字节相同**（6,496 B = `ref-900k.txt`）
  - → 诚实结论：时钟确实在走，但**没有**解决「约 50 句台词缺失」；那个问题在别的机制里
  - `tests/verify_timeline.ps1`：7 项断言（默认==冻结基准、timeline 不挂死、时钟前进、
    当前与 legacy 逐字节相同 —— 将来分歧会立刻报错）
- **`src/motion.c` / `src/motion.h`：补间核心数学**（`Motion*` 的时间轴 + 线性插值）
  - 只实现**已确证**部分：开始/持续/完成、线性插值、零时长、完成回收；
    `mode 1..5` 的缓动曲线**没有证据**，原样存下但不影响输出（文档标明未实现）
  - 未接 gfx / syscall，因此现有所有验收数字不变
  - 验收：`tests/test_motion.c` **31 项**（起点/中点/终点、超时钳制、零时长、
    负时长拒绝、多补间并存、reap、clear 保留时钟、NULL 安全）
- **`Motion*` 命令接入补间表**（`src/syscall.c`）：`MotionMove`(48) / `MotionAlpha`(42) / `MotionMoveS2`(54)
  在**动画开启**时改为「当前值 → 目标值 + 时长 + mode」的补间，关闭时仍是原来的静态赋值
  外壳新增 `--motion`（默认关）+ 每帧 `gfx_motion_advance()` + 日志 `motion: N active tween(s)`
  实测：开场 6 s / 15 s 运行里观察到 **1–2 条活跃补间**
  ⚠️ 像素 A/B 对比**不能作数**：默认模式两次 15 s 运行本身就不同（墙钟驱动的文本节奏），
  下一步需要用**步数确定**的 `render_vm` harness 做对照（已记入待办）
- **确定性动画 A/B（`tests/verify_motion.ps1`，新增）**：`render_vm` 加 `--motion` / `--motion-dt`，
  固定步数 + 固定 dt 做对照（外壳壁钟驱动不能做 A/B）。实测：
  关动画两次 60k 步完全一致 `76C16B46…`；**1,000 步开=关** `E46C17E3…`；
  **60,000 步开≠关**（`F3CA8F6F…`）—— 动画在真开场里确实改了像素
- **动画双门制已落实**：静态门 `verify_gfx.ps1 -SkipDump` 仍 **PASS**（图元 24/24
  位置误差 **0 px**、像素差 **293 (0.0141%)**、PSNR **49.94**、SSIM **0.99971**），
  证明动画集成没有动到静态渲染路径；动态门交给 `verify_motion.ps1`。两条写进 `docs/testing.md` 矩阵
- **`docs/animation-baseline.md`**：动画/时间模型的验收基线设计 ——
  哪些旧数字会失效（528k 参考帧、端到端帧）、改用哪三组自洽断言
  （t=0 不动 / t=END 到位 / 单调有界），以及「拿到原版实机对照前不默认开时间模型」的理由


### 新增

- **诊断能力**：`fvm --unknown-all`（完整未知命令直方图）与每线程台词计数；
  实测 900k 步：**195 行全部来自主线程**，`ThreadStart`(136) 调用 **5 次**、
  `ThreadExit`(132) 19 次、`TextSuspendChr`(130) 124 次、`TimerSet`(139) 259 次，全是桩
  → 台词缺失主因定位为 **协程线程未实现**（非时间模型）；
  `docs/05-threads.md` §9 记录了为何不能「顺手实现」（Python 原型加线程后 711 行但大量 `[0] 0`）
- **长跑验收 `tests/verify_soak.ps1`**：真跑外壳 N 秒，期间采样工作集，断言退出码、
  工作集增长、每 5 s 窗口 fps/slow、`underruns=0`、脚本有产出、日志无 error
  - 首次 120 s 实测：退出码 0、工作集 140.9 → 209.5 MiB（增长 68.6 MiB）、23 个窗口
    全部 fps ≥ 55（slow 均 0）、`underruns=0`、127 行台词、0 error
  - ⚠️ 工作集前 120 s 增长 68.6 MiB，**尚未判定**是资源缓存填充还是泄漏，
    需要更长时间的曲线（已安排 10 分钟对照）

### 验收（0.8.0 发布前实跑）

| 项 | 结果 |
|---|---|
| `tests/run_all.py` | **20/20 PASS**（含 VM 逐字节 / 资源 / 音频 / 存档 / flag / textattr / thumb / encode / wheel / motion / fuzz） |
| `verify_gfx.ps1 -SkipDump`（静态门） | PASS：24/24 图元 0 px、293 px 差 (0.0141%)、PSNR 49.94、SSIM 0.99971 |
| `verify_motion.ps1`（动态门） | PASS：关两次一致；1,000 步 开=关；60,000 步 开≠关 |
| `verify_timeline.ps1` | PASS：legacy `virtual ms 0` 且 == `ref-900k.txt`；`--timeline` 2000 ms 且文本相同 |
| `verify_save_preview.ps1` | PASS：真跑外壳 → 4 个槽全部读回，尾段 598,950 B 不变 |
| `verify_save_encoding.ps1` | PASS：CP932 `83 76 …` / GBK `a5 d7 …` 与 Python codecs 逐字节一致 |
| `verify_render_vm.ps1` / `verify_audio.ps1` | PASS：端到端帧一致；BGM 出声、混音非静音 |
| `compare.py --run` | **逐字节一致** 6,496 B / 195 行 / `7F9837FF…` |
| 编译 | `make -B all lib test … win32` **0 警告 0 错误** |

## [0.3.0] — 已打 tag（v0.3.0 → 55b8e6f）：存档写入闭环（模板路径）+ 结构分析文档

0.3 的主线只有一条：**把「存档写入」做成能重复验证的闭环**，并把槽内快照的结构知识固化成一篇文章。
不做的事：从零生成存档（缺字段语义）、游戏内 `SaveWrite`/`Load` 接线。

### 新增

- **`--save-encoding cp932|gbk`：存档字符串编码可选**（默认跟随脚本编码）
  - 实跑对照（同一句 `プロローグ`）：CP932 → `83 76 83 8d 83 8d 81 5b 83 4f`，
    GBK → `a5 d7 a5 ed a5 ed a9 60 a5 b0`，与 Python codecs 的输出**逐字节相同**
  - `test_save --dump-strs` 新开关：打印槽内三条字符串的 hex，便于离线比对
  - 新增 `tests/verify_save_encoding.ps1`：真跑外壳两次（默认 / `--save-encoding gbk`）+
    逐字节断言，**PASS**；这条补上了 0.3 测试盘里「GBK 路径端到端」的空缺
- **存档序列化的字节布局回归钉**：一份 52 字节合成槽（时间戳 + 三条字符串 + 2×2 缩略图 +
  5 字节尾段）序列化后 FNV-1a 64 固定为 `0x55bc2d006a29f2bf`，`make test_save` 会校验
  —— 以后改序列化顺序 / 字节序 / 漏字段会立刻失败（`test_save` 自检 50 → **58 项**）
- **鼠标滚轮接入 VM（`InputGetWheel`，命令 36）**：外壳把窗口滚轮增量累加进 VM，
  脚本取走即清零；120 = 一格，正负方向都支持
  - 顺手修掉一处 id 错位：原代码把 `case 36` 标成 `InputGetUp`，而表里 35 = `InputGetUp`、
    36 = `InputGetWheel` —— 现在两个 id 都按表处理
  - 验收：`tests/test_wheel.c` **8 项**（首帧为 0、单格、同帧累加 120+240=360、负方向、
    加 0 是 no-op、帧间无残留、NULL 安全）；`run_all.py` → **18/18 PASS / 76.7 s**
  - 与冻结基准不冲突：滚轮默认为 0，`compare.py --run` 仍逐字节一致（6,496 B）
- **`docs/save-snapshot.md`**：槽内「引擎状态快照」专文 —— 常量头 64 B、中段密度地标
  （`+0x19000` 共同分界 / 118,784 B 稀疏块）、常量尾 216×6B 表、17 字节记录流，
  以及「未解清单 + 继续研究的路线」。`docs/README.md` 索引已收录（27 份文档）
- **`tests/verify_save_preview.ps1`**：存档预览的端到端验收（新增，自动跑）
  - 真跑 `fvp.exe --run-seconds 6 --save-preview-every 2`，断言：退出码 0、
    ≥2 次写入、缩略图来自活画布、每个产出的槽都能被 `test_save` 读回（0 失败）、
    且**尾段全部保持模板的 598,950 字节**
  - 实测：4 个槽 `str 0/0/0 → 10/0/0 → 10/0/26 → 10/0/22`，45 项断言 0 失败
- **`--save-preview-every N`：运行中周期性写存档预览**（「按 F5 存档」的自动化版）
  - 实测 7 秒 / 每 2 秒：写出 4 个周期槽 + 1 个退出槽，文件名 `.1/.2/…`
  - **脚本进度会体现在内容里**：前两个 `str 0/0/0`，之后变成 `str 10/0/14` → `10/0/16` → `10/0/22`
    —— 这正是游戏自己的 `SaveCreate` 在喂数据，证明「存档数据从运行时到文件」这条链路是活的
  - 5 个槽全部被 `test_save` 读回：**52 项断言 0 失败**，每个槽的引擎状态尾段都是模板的 598,950 字节
- **118,784 B 稀疏块的实测结论（否掉了旧猜想）**：5 个槽里它几乎全零（31–112 个非零字节），
  只有「演出中途存的」s906 填了数据，而且是 **17 字节周期**的记录流
  （`f4 ff ff ff` = -12、`34 03 00 00` = 820、递增序号 6/7/8/9）
  → 它是「平时为空、演出时填」的表（像动画/事件队列），**不是 32 个文本窗口**
  - `docs/save-format.md` §2.4.2 新增，§2.4.1 里那条旧猜想同步改成「已否掉」
- **变长中段的密度结构（实测，6 个槽一致的地标）**：按 4 KiB 块 / 5% 非零密度分段后，
  6 个槽都出现同一串地标 —— `+0x19000`（102,400）是共同分界，之前是「前段 + 86,016 B 零区」，
  之后是随内容变化的 DENSE 区（229,376–331,776 B）；紧跟一个 **118,784 B 稀疏块**
  （与内容无关；118,784/32 = 3,712 B/窗口，与早先推定的 0xB1D=2,845 冲突，**待定论**），
  再之后是固定尾链 12,288 / 4,096 / 4,096–8,192 / 8,192 / 4,096 / 8,192 / 收尾 1.9–4.9 KB
  - 结论写进 `docs/save-format.md` §2.4.1
- **UTF-8 → CP932 / GBK 编码方向（`fvp_encode`）**：写存档需要（槽里的字符串是原编码）
  - 反查码表：单字节表 + 双字节行表；表里没有的码点写 `?`（对应 Python `encode(enc,'replace')`）
  - 验收：`tests/test_encode.c` **14 项** —— 含真实存档里出现过的字节序列往返
    （`83 76 83 8d 83 8d 81 5b 83 4f` = `プロローグ`、GBK `谁这样说道。`、全角空格 `A1A1`）
- **存档预览现在带脚本字符串**：`--save-preview` 会把 `SaveCreate` 准备好的字符串按脚本编码
  写进槽里。实测 6 秒运行写出的槽 `str 10/0/8`，其中
  `str0 = 83 76 83 8d 83 8d 81 5b 83 4f`（`プロローグ`）、`str2 = 81 40 81 63 81 63 81 42`（`　……。`）
  —— **与真实存档 s481.bin 的字节完全相同**；文件 689,381 B，`test_save` 读回 24 项断言全过
- **桌面外壳 `--save-template` / `--save-preview`：真实存档预览**（0.2 存档线第一次「写出一个能读的槽」）
  - 拿一个真实存档槽当模板，只替换**时间戳（本地时间）**与**缩略图（当前画面 box-filter 缩到 200×113）**，
    其余字节（含 500+ KB 引擎状态尾段）原样保留
  - 实测：`--save-template s485.bin --save-preview out/preview-s900.bin` →
    `689,363 bytes`（与模板同长）、`timestamp 2026-11-24 01:29`、缩略图是从活画布缩出来的
  - 再用 `test_save.exe --savebin ... out/preview-s900.bin` 读回：**24 项断言全过**
    （时间戳/缩略图/尾段 598,950 字节全部正确）
  - ⚠️ 仍未做：字符串段（存档里是 CP932/GBK，UTF-8 需转码）；槽内快照语义仍未解，所以没有 `SaveWrite`
- **保守写档路径的自动化验收**：`tests/test_save.c` 拿真实存档槽当模板，只改
  **已确证字段**（时间戳 / 章节名 / 缩略图），写盘后读回并断言
  **引擎状态尾段（500+ KB）逐字节不变**、新字段全部生效（真实样本 49 → **59 项**断言）
  - 这正是 `docs/save-format.md` §6「能安全做的」那条路径，现在有测试兜住
- **存档尾段常量区的内部结构（实测，`tools/tailstat.py` 的「常量区」小节）**
  - 1,438 B 常量尾 = **216 × 6B 定长表**（每条 `{u16 0, u16 100, u16 0}`，216 个 0x64 全部落在 +2）
    + 140 B 全零 + 末 2B `01 01`
  - 常量头 64 B = 24 个 0 + `02` + 23 个 0 + 之后每 4 字节一个非零（+48 / +52 / +56）
  - 结论写进 `docs/save-format.md` §2.4.1；写回时整段原样拷贝即可，不必解语义
- **桌面外壳 `--thumb-shot FILE`**：退出时把最后一帧按 200×113 缩成存档缩略图 BMP
  （`src/thumb.c` 的 box filter）。实测 `--run-seconds 4` 抓到的缩略图是**真实开场画面**
  （黑底 + 底部对话框），尺寸 200×113 / 90,400 字节
- **`tools/tailstat.py`：存档尾段统计/差分**（不猜字段语义，只量数据）
  - 6 个真实槽实测：尾段 502,020–598,984 B，非零只占 11–16%（其余零填充）；
    **前 64 字节完全相同**（24 个 0 后接 `02 00 00 00`）、**末 1,438–1,449 字节完全相同**
    （末两字节 `01 01`）
  - 由此确立：尾段 = `≥64B 常量头` + `变长中段` + `≥1438B 常量尾`；中段长度随内容变化 ~97 KB，
    所以写回时**必须整体保留原中段**，不能按固定偏移生成
  - 结论与复现方法写进 `docs/save-format.md` §2.4.0 / §5
- **存档缩略图缩放（`src/thumb.c` / `src/thumb.h`）**：把渲染层的 BGRA 画面 box-filter 缩到
  200×113（或任意尺寸），alpha 强制写成 0xFF —— 与引擎 `render_thumb()` 写进槽文件的格式一致
  - 面积平均而非双线性：1920×1080 → 200×113 这种大幅缩小时更接近原图整体亮度
  - 用 64 位整数算覆盖矩形，1920×1080 不会溢出
  - 验收：`tests/test_thumb.c` **14 项**（纯色保持、2×2 平均、真实 1920×1080→200×113 全像素检查、非法参数拒绝）
- **槽文件落盘（`fvp_save_slot_write_file`）**：序列化整块写文件（UTF-8 → 宽字符，失败退回 ANSI）；
  `test_save.c` 写盘回环 +6 项（共 56 项）
- **存档暂存区（`SaveCreate` 命令 93 / `SaveThumbSize` 命令 94）**：脚本要写的存档内容
  先记在 VM 里 —— id 0/1/2 = 章节名 / 未用 / 当前对话行，id 3 = 选槽号；
  缩略图尺寸按引擎范围钳制（20..200 × 15..150），初值 96×72
  - 真实脚本 900k 步实测：`save state slot=1 str=1/0/0 thumb=200x113`，
    **缩略图尺寸与真实存档样本的 200×113 完全一致**
  - `SaveWrite` 尚未实现（槽内引擎状态快照语义未解），所以只到「准备好数据」为止
  - `fvm` 汇总多一行 `save state ...`；`compare` 仍逐字节一致

- **文本窗口属性存储（`src/textattr.c` / `src/textattr.h`）**：`TextColor`（命令 112）、
  `TextSize`（126）、`TextSpeed`（129）不再落到兜底桩，而是按静态分析的范围钳制后存进
  VM 的 32 个窗口属性里（颜色 0..255、字号 12..64 × 8..32、速度 0..300000，窗口号 0..31）
  - 真实脚本 900k 步实测：`text attrs 4 window(s) configured`
  - 渲染层暂不消费这些值，所以**已验收的帧与逐字节文本基线都不变**（`compare` 仍 6,496 B 一致）
  - 验收：`tests/test_textattr.c` **29 项**（默认值、钳制两个方向、越界拒绝、configured 计数、reset）

### 变更

- `fvm` 汇总多一行 `text attrs N window(s) configured`；`make test_textattr`；
  `tests/run_all.py` 步数随新测试增加（0.3.0 收尾时 **18 步**）

### 验收（0.3.0 收尾实跑）

| 项 | 结果 |
|---|---|
| `tests/run_all.py` | **18/18 PASS / 57.0 s** |
| `compare.py --run` | **逐字节一致**（6,496 B / 195 行 / `7F9837FF…`） |
| `test_save` | 自检 **58 项** + 真实槽 **49 项**（含字节布局回归钉 `0x55bc2d006a29f2bf`） |
| `test_save_vm` | 合成 19 + 真实 13 |
| `test_flags` | 28 + 37 |
| `test_textattr` / `test_thumb` / `test_encode` / `test_wheel` | 29 / 14 / 14 / 8 |
| `verify_save_preview.ps1` | PASS（真跑外壳 → 4 个槽全部读回、尾段 598,950 B 不变） |
| `verify_save_encoding.ps1` | PASS（CP932 `83 76 …` / GBK `a5 d7 …` 与 Python codecs 一致） |
| `verify_render_vm.ps1` / `verify_audio.ps1` | PASS（端到端帧一致；BGM 出声、混音非静音） |
| 编译 | `make -B all lib test … win32` **0 警告 0 错误** |

## [0.2.0] — 已打 tag（v0.2.0 → `4c80b0e`）：音频接运行时 + 存档读写落地 + 全局 flag

0.2 的目标是「音频、存档可用」。当前状态：**音频已接运行时；存档到了「能读、能播种、能原样重写」
的程度；游戏内存档 UI 与槽内引擎状态语义仍未完成**。版本号直接从 0.1.1 跳到 0.2.0，
因为存档这条线是结构性新增，不是补丁。

### 新增

- **全局 flag 位图（`src/flags.c` / `src/flags.h`）**：2048 bit / 256 字节，索引 0..0x7FF
  - `FlagGet(idx)`（命令 18）返回该位；`FlagSet(idx, on)`（命令 19）arg2 为假则清位、否则置位
  - 越界（负数 / ≥2048）一律安全：读返回 0、写是 no-op；位序小端（idx 8 → 第 1 字节 bit0）
  - `save/save.bin` 里那 256 字节就是这张位图的原始拷贝，启动时直接播种（`fvp_save_flags_apply`）
  - 验收：`tests/test_flags.c` **28 项**合成用例 + 真实 `save.bin` **37 项**（含容量不足 / NULL 拒绝）
- **全局变量桥（`src/save_vm.c`）**：把 `save.bin` 里 `type==2` 的 145 条 int 全局变量灌进 VM 的
  `globals[]`；`type==1` 的 846 条语义未解，跳过并报数。处理了类型编号陷阱（存档 `2`=int，
  VM `FvpType` 里 int 是 `0`）与 `u32 → int32` 回绕
- **桌面外壳读档**：`fvp.exe` 在 `fvp_vm_start()` 之前查找 `<game>/save/save.bin`
  （退一步 `<game>/save.bin`），解析并播种全局变量 + flag 位图；实测日志
  `save: .../save/save.bin -> 145/1471 globals applied (145 int, 846 unknown type1, 480 unset), 0 flags bit(s) set, thumb 200x113, readmap 1048576 bytes`
- **存档文件层（`src/save.c`）**：槽文件（7 字节时间戳 / 三条长度前缀字符串 / 裸 BGRA 缩略图 /
  尾段原样透传）与 `save.bin`（从文件尾反推布局）的解析与重写；6 个真实存档槽**逐字节回环一致**
- **文件级便利函数**：`fvp_save_find_file()` / `fvp_save_bin_load_file()`；Windows 上路径按
  UTF-8 → 宽字符开文件，UTF-8 解码失败自动退回 ANSI 代码页（兼容 argv 来的路径）
- 新 CLI：`fvm --savebin FILE`、`fvm --dump-globals FILE`（退出码 5 = 读 savebin 失败）

### 变更

- **音频层接运行时**：`fvp_vm_attach_snd()` 让 `AudioPlay` / `AudioLoad` 等 14 条命令直接解码并混音；
  窗口内日志 `snd: attached to vm (6 audio archive(s) registered)`，`submitted` 递增、`underruns=0`
- 所有交付文档顶部统一加 `> 以下内容由 DeepSeek 生成,不一定代表作者个人意愿`，
  由 `scripts/check-docs.py` 强制检查（前 20 行内中/英任一写法，`ref/` 豁免），CI 已在跑
- `docs/win32-shell.md` §3 增加「启动时读取全局存档」小节；`docs/c-vm.md` 选项表补
  `--savebin` / `--dump-globals`；`docs/save-format.md` §7 补 C 实现、§7.1 补全局变量桥与 flag 播种
- `tests/run_all.py` 从 8 步扩到 **14 步**

### 已修复

- 存档解析里的两处边界误解：缩略图尺寸必须来自 `save.bin` 头或 alpha 连续长度推断；
  `save.bin` 的字体名长度是 `i16 含 NUL` 且**紧贴文件尾**，偏移按 `len - font_len - 3` 算
- **fuzz 的 E2E 阶段静默失效**：`syscall.c` 开始调用 `fvp_flag_*` 后，`harness_build.py` 的
  `CORE_SRC` 没有加 `flags.c`，导致 fuzz 测试二进制链接失败、`in-process / e2e` 两个阶段被跳过，
  而脚本仍打印 PASS（`mutant executions: 0`）。已补上源码并让 `verify_render_vm.ps1` /
  `verify_audio.ps1` 的编译行同步；现在恢复为 **27,000 次（含 fvm.exe 4,000）**

### 验收

| 项 | 结果 |
|---|---|
| `tests/run_all.py` | **14/14 PASS / 60.7 s**（新增 `test_flags` 自检 + 真实 `save.bin` 两步） |
| `test_save.c` | 自检 **50 项** + 真实样本 **49 项** |
| `test_save_vm.c` | 合成 **19 项** + 真实 **13 项** |
| `test_flags.c` | 合成 **28 项** + 真实 **37 项** |
| `compare.py --run` | **逐字节一致**（6,496 B / 195 行 / `7F9837FF…`） |
| `verify_render_vm.ps1` | PASS：端到端帧与参考一致；900k / 2M 步文本**逐字节一致**（`7F9837FF…` / `32D7D4E2…`） |
| `verify_audio.ps1` | PASS：BGM 出声、混音非静音、逐字节用例不变 |
| 编译 | `make -B all lib test test_save test_save_vm test_flags win32` **0 警告 0 错误** |

## [0.1.1] — 存档文件层 + 音频接入运行时

这一版没有新玩法，只有两件事：**音频真的接到虚拟机了**，以及**存档格式从「结构分析结论」变成「可执行代码」**。

### 新增

- **存档层（`src/save.c` / `src/save.h`）**：`save/sNNN.bin` 与 `save/save.bin` 的解析与重写
  - 槽文件：7 字节时间戳、三条 `u16` 长度前缀字符串、`w*h*4` 裸 BGRA 缩略图、尾段引擎状态
    **原样透传**（语义未解的部分不解释、不重排）
  - `save.bin`：**从文件尾反推布局**，不依赖计数器 → 1471 条全局变量、20 字节设置头、
    1 MB 已读位图、256 B Flag 位图、字体名 `メイリオ`
  - 字符串保留原始字节（CP932 / GBK 混合，不转码）
- **验收**：`tests/verify_save.ps1` + `tests/test_save.c`
  - 自检 **50 项**：空槽、超长字符串、截断 / 畸形输入拒绝、alpha 尺寸推断、`save.bin` 幂等回环
  - **6 个真实存档槽逐字节回环一致**（44 项断言），时间戳 / 章节名 / 对话行 / 缩略图尺寸全对得上
- **全局变量桥（`src/save_vm.c`）**：把 `save.bin` 里 `type==2` 的 145 条 int 全局变量
  灌进 VM 的 `globals[]`；`type==1` 的 846 条语义未解，跳过并报数
  - 新 CLI：`fvm --savebin FILE`（启动前播种全局变量）、`fvm --dump-globals FILE`（导出非零 int）
  - 验收：合成用例 **19 项**（含 `u32 → int32` 回绕、容量不足、只统计模式）、
    真实 `save.bin` **13 项**（1471 = 145 int + 846 unknown + 480 unset，值序列与文档 §3.3 逐个吻合）
  - `tests/verify_save_vm.ps1`；`run_all.py` 再加两步 → **12/12 PASS**
  - 命令行默认输出不变：`compare.py --run` 仍是 6,496 字节 / `7F9837FF…` 逐字节一致
- `make test_save` 目标；`src/save.o` 已并入 `bin/libfvp.a`
- `tests/run_all.py` 增加两步存档验收 → **10/10 PASS**

### 变更

- 音频层状态从「完成（未接运行时）」改为「已接运行时」：`fvp_vm_attach_snd()` 让
  `AudioPlay` / `AudioLoad` 等命令直接驱动解码与混音，窗口内日志出现
  `snd: attached to vm (6 audio archive(s) registered)`
- `docs/roadmap.md` 阶段 4 / 6 / 9 状态按实测更新；`docs/save-format.md` 增加 §7 实现章节

### 已修复

- 存档解析里"缩略图与尾段边界"的两种误解：尺寸必须来自 `save.bin` 头或 alpha 推断，
  否则会把尾段首字节吃进图里；`save.bin` 的字体名长度是 `i16 含 NUL` 且**紧贴文件尾**，
  因此偏移必须算作 `len - font_len - 3`

## [0.1.0] — 首个可用预览

第一次「能在桌面跑起来」的版本。核心目标不是功能齐全，而是**每一条结论都有可复现的验证**。

### 已实现

**资源层**
- BIN 包读取：12 个包、**21,725 条**索引/名字/偏移与独立实现逐条一致
- 自写 DEFLATE/zlib 解压（无 zlib 依赖）：**1,112/1,112** 个真实流、**2,539,861,820 字节**逐字节相同；12.5 万次随机输入 fuzz 无越界
- HZC1 图像解码（fmt 0–4，含多帧）：**3,654 帧 / 727,573,943 像素**与 Python 解码器 0 差异

**虚拟机**
- 40 个 opcode、协程简化版、全局/局部变量、temp 返回值寄存器
- `.hcb` 解析：`iroseka.hcb` **948,219 条指令**，198,337 处跳转目标 100% 落在指令边界
- 命令表：**148 条**（id/名字/argc 来自 `.hcb` 内嵌数据），本作实际用到 131 条
- 与 Python 参照实现**逐字节一致**：900k / 2M / 20M 步；C 比 Python 快 33–68 倍

**渲染层**
- 1920×1080 合成：图元表、group 树深度优先、缩放/旋转/alpha/裁切、文本排版（含 `[读音|正文]` 注音解析）
- 与 Python 原型比对：**位置误差 0 像素**、像素差异 **0.0141%**、PSNR **49.94 dB**、SSIM **0.99971**
- 热帧 **9.0–9.5 ms**

**音频层**
- Ogg Vorbis（`stb_vorbis`）+ RIFF/WAVE 解码，统一 48 kHz 立体声 PCM16
- 线性重采样、8 通道混音
- **已接到 syscall**：`AudioPlay` / `AudioLoad` / `AudioStop` / `AudioVol` / `AudioType` 等 14 条命令
  驱动解码与播放；900k 步实测触发 **4 次 BGM 播放**（`BGM2/002` 等），混音 peak **14004** / rms **2431.8**，解码失败 0
- 资源按前缀选包（`bgm/` `bgm2/` `voice/` `se/` …）——音频包之间有 67 个同名 basename，不能全局按名字查

**桌面运行器**
- Win32 窗口 + GDI 呈现 + 60 Hz 帧循环 + 鼠标/键盘 + waveOut
- 启动 **130–230 ms**、**60.00 fps**、上屏 avg 3.4–3.6 ms（慢帧 0）、音频 underrun 0

**跨作品**
- 第二部作品《星空のメモリア HD》同样跑通：**命令表 148 条逐条完全相同**、VM 零修改输出正确台词

**工具与文档**
- `tools/fvpprobe.py`：扫一个游戏目录，输出兼容性报告
- `tools/saveinfo.py`：存档槽解析（时间戳、章节名、对话行、缩略图）
- 20 份技术文档，`docs/README.md` 为索引
- 测试：`tests/run_all.py` 一键 **8/8 PASS / 78 s**，含 27,300 次 HCB 变异

### 已知限制

- 游戏内 UI 是占位（对话框为绘制圆角框，真实 `main_window` 贴图未接）
- 动画命令是空实现：立绘不会移动/淡入
- 协程与时间模型未进 C：约 50 句台词缺失，主循环会卡在等待时间的条件上
- 音频能播放，但没有音画同步、没有 BGM 淡入淡出、音量体系（主/分组/单源）还没接配置层；
  语音路径需要存档层提供 voice id，原型里为空所以尚未验证
- 仅验证两部作品；2004–2008 年的更早 FVP 版本未测试
- 4 个 opcode（`0x11` / `0x12` / `0x17` / `0x18`）语义未确认，按数组读写实现

### 已修复

- `hcb.c` 的操作数边界原先按文件长度检查，会读到 `script_len` 之后的数据；
  改为按 `script_len` 检查后，`truncated instruction` 诊断重新生效
- `snd.c` 的重采样未限制采样率比例，畸形 WAV 可触发 168 MB 单次分配（已加范围与帧数上限）

[0.1.1]: https://github.com/chuyingweilai1349816219-droid/openfvp/releases/tag/v0.1.1
[0.1.0]: https://github.com/chuyingweilai1349816219-droid/openfvp/releases/tag/v0.1.0