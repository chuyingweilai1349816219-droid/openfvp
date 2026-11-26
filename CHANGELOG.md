# 变更记录

> 以下内容由 DeepSeek 生成,不一定代表作者个人意愿

格式参考 [Keep a Changelog](https://keepachangelog.com/zh-CN/1.1.0/)，版本号语义化。

## [未发布] — 2026-11-26（十五续）：查清"存档字段在哪应用"——不是 0x4344C0，是 0x43EE90 的流式反序列化

上一轮留给下一轮的问题是"`0x4344C0` 应用存档时到底写哪些字段"。本轮读清了两件事：

- **`0x4344C0(engine, path)` 只负责读文件**：`CreateFileW` → 释放旧缓冲 →
  `GetFileSize` → 重设缓冲（`engine+0x0/+0x4`）→ `ReadFile` → `CloseHandle`；
  失败时用 `0x45D968` 的 `'%s'のオープンに失敗しました` 报错并 throw（code 0x80）。
- **字段应用在 `0x43EE90` 复位之后的流式反序列化里**：每段是"2 字节长度前缀 +
  数据"，读长度（`0x43EFD5`/`0x43F028`）→ `0x4470EB` 分配 → `0x447800` 拷贝 →
  `0x433C60` 压进 `engine+0xFB4`、`engine+0xFB8`、…… 容器；流游标是 `engine+0x8`。

基线 `ref/exe-load-flow.json` 新增 `load_apply` 段（文件读取调用链、两段容器、
五条主步骤、游标字段），`load_impl` 的措辞从"0x4344C0 应用存档"改正为"读文件"，
`unknown` 改成"流式反序列化每段对应哪些状态"。门禁新增 5c 节（12 条断言）。

## [未发布] — 2026-11-26（十四续）：审计收尾到 10 个 handler；TextFormat/TextFunction 也修掉"非整数当 0"

`ref/exe-text-handlers.json` 扩到 **10 个 handler / 56 条关键指令**，补上最后两个：

| 命令 | handler | 规则 |
|---|---|---|
| `TextFormat(118)` | `0x43CC10` | a1/a2 -0x20..0x20 → `0x5A8/0x5AA`；a3/a4 0..0x40 → `0x5AE/0x5AC`（a4 还会把笔位 `0x5E8` 抬到不小于它）；a5/a6 -0x10..0x10 → `0x5B4/0x5B0` |
| `TextFunction(119)` | `0x43CCF0` | a 0..1 → `0x5A5`；b 0..2 → `0x5A4`；c 0..2 → `0x5A6` |

**第三处同类偏差（已修）**：这两个 handler 原来把**非 int 的分量当 0** 传下去，而 0 在
它们每个范围内（space/pad/off/func 都是 0 起）——等于用"省略"覆盖了原值。改为带
"分量是否提供"掩码的 `_ex` 入口（`fvp_textattr_set_format_ex` /
`fvp_textattr_set_function_ex`），`syscall.c` 按 `args[i].type == FVP_INT` 逐位填掩码。

## [未发布] — 2026-11-26（十三续）：文本 handler 审计扩到 8 个；修掉 TextFont 的"非整数被当 0"偏差

`ref/exe-text-handlers.json` 从 4 个 handler 扩到 **8 个**（44 条关键指令逐条校验）：
新增 `TextFont(113)`、`TextShadowDist(125)`、`TextSkip(127)`、`TextSpace(128)`。

新读出的规则：
- `TextFont(113)`：两个字体 id 各自 int 且 **-5..(字体表项数-1)**——上界是**动态**的
  （`0x43DAA3 sar $2` + `0x43DAA6 dec` = `(end-begin)/4-1`）；
- `TextShadowDist(125)`：距离**钳位**到 0..0xC（负数→0、>0xC→0xC），非 int→0 ——
  这是这一族里唯一钳位的；
- `TextSkip(127)`：mode int 0..3 → `tw+0x5D7`；
- `TextSpace(128)`：x/y 各自 int -0x20..0x20 → `tw+0x5A8/0x5AA`
  （与 `TextFormat(118)` 前两个分量同字段）。

**修掉的第二处偏差**：`TextFont` 原来把非 int 分量当 **0** 传下去（0 是合法字体号，
等于覆盖了原值），上界也用静态 31。现在改成 `fvp_textattr_set_font_ex()`
（has 标志 + 动态上界 = `fvp_vm_font_count(vm)-1`）。

## [未发布] — 2026-11-26（十二续）：文本类 handler 实参规则成串冻结；修掉 TextColor 的钳位偏差

四个文本 handler（`TextBuff 110` / `TextColor 112` / `TextOutSize 120` / `TextSize 126`）
是同一族写法：层号 arg0 必须是 int 且在 0..0x1F，否则整条忽略；其余分量**逐分量**
判 int + 范围，不满足**保持原值**（不是钳位）。冻结成 `ref/exe-text-handlers.json`
（21 条指令、4 个 handler），新增 `scripts/extract-exe-text-handlers.py` +
`tests/verify_exe_text_handlers.py`（`run_all` 6.496496）。

**修掉一处真实偏差**：`TextColor` 原来无条件调 `gfx_text_set_color()`（内部 `& 255`），
`TextColor(0,300,0,0)` 会把 R 写成 44；原版是保持原值。改为逐分量入口
`gfx_text_set_color_ex()`（新增 `gfx_text_get_color()` 给测试用），
`tests/test_textprims.c` 37 → **40** 条检查。

## [未发布] — 2026-11-26（十一续）：TextBuff(110) 的实参规则按 handler 对齐（w/h 缺省 8）

读 handler `0x43C830` 并补进 `ref/exe-textbuff.json`（20 条指令逐条校验）：
`arg0` 层号必须是 int 且在 **0..0x1F**；`w`/`h` **各自缺省 8**，只有"是 int 且 >= 0"
才覆盖；随后才交给 `0x430EA0`（越界抛异常）。

我们的 `case 110` 原来在"`arg1` 不是 int"时**整条跳过**，所以 `TextBuff(layer)`
这种只给层号的调用不会建出 8×8 缺省面板。本轮按 handler 改成：
先判层号范围，w/h 缺省 8、只按 int 且 >=0 覆盖。门禁里加了 4 条 C 侧断言
（用 arg0 当层号、范围检查、缺省 8、覆盖条件）。

顺带确认：默认帧模型下层 0 的 `TextBuff` 拿到的是 1024×1024（合法、面板会建），
而修正帧模型下拿到 1380×1024（越界）——两个模型的实参换算差异又多了一条证据。

## [未发布] — 2026-11-26（十续）：TextBuff 越界是**抛异常**（code 0x80），不是我上一轮猜的"被我们丢掉"

上一轮的怀疑（"我们的引擎把 1380 宽的面板丢了、原版不会"）**不成立**。读透 `0x430EA0`：

- 上限检查：`0x430EA8 cmpl $0x400,w`、`0x430EBA cmpl $0x400,h`，任一越界 → `0x430FF8`；
- `0x430FF8` 不是返回，而是构造异常并 **throw**（`push $0x80` → `0x447C05` →
  格式串 `0x45D8C4` + `0x4258F0` → `0x44837C`）；
- 未越界才写 `tw+0x5DC/0x5DE` 面板尺寸、建 surface、分配 `w*h*4` 到 `tw+0x520`。

⇒ 原版对 `TextBuff(0,1380,1024)`（脚本正文层）**也会中断**。我们引擎同一条件返回 0
（VM 无异常语义，最近似行为），并新增 `FVP_DEBUG_TEXT` 提示一行。

新增 `scripts/extract-exe-textbuff.py` + `ref/exe-textbuff.json` +
`tests/verify_exe_textbuff.py`（`run_all` 6.496495，15 条指令逐条校验）。

## [未发布] — 2026-11-26（九续）：用 HCB 字符串判定"冻结基线对、修正帧模型错"（不能重新冻结）

上一轮留了个悬而未决的问题：要不要把 `FVP_FRAME_SLOTS=1` 切默认并重新冻结
`ref/ref-900k.txt`。本轮用**游戏文件里的字符串数据**给出判据（新增
`tests/verify_text_from_hcb.py`，`run_all` 6.49649）：

| 检查 | 结果 |
|---|---|
| HCB 里 `……。`（`81 63 81 63 81 42`） | 1345 次 |
| HCB 里 `……。」`（追加 `81 76`） | **0 次** |
| 冻结基线 195 行能否在 HCB 里逐行找到 | **195/195，0 编造** |
| `FVP_FRAME_SLOTS=1` 输出里找不到的行 | **70 行**（每行多一个「」） |

⇒ **冻结基线是对的；修正帧模型的文本通道多读一个字符**。所以：
- **不切默认、不重新冻结**（撤销上一轮"候选新基线"的提议）；
- 修正模型只解决了一半问题（窗口位置对了、文本长度错了），下一步要找"多读一位"的根因。

顺带把带点击的 headless 运行跑通了：`--click 200000` + `--steps 20000000` 在 0.6 s 内
跑完序章（比 GUI 快两个数量级），后续抓"窗口位置时间线"用这条路。

## [未发布] — 2026-11-26（八续）：文本窗口位置是运行期可变的（MotionMove 实证）

给 `MotionMove(48)` / `MotionMoveR(51)` 加了 `FVP_DEBUG_TEXT` 日志（只加日志，不改行为），
带点击的 60 s 运行抓到：

```
MotionMove prim=551 x=1 y=880 z=1 ms=1 mode=3 (instant)
MotionMove prim=552 x=1 y=852 z=1 ms=1 mode=3 (instant)
```

即正文窗口先由 `PrimSetText(551,0,230,522)`→画布 (431,880) 建好，随后被 `MotionMove`
挪到 **(1,880)**（名字窗口同理挪到 (1,852)）。所以"原版正文为什么在某个位置"必须按
当时的 MotionMove 参数解释，只看 `PrimSetText` 初值会错。

## [未发布] — 2026-11-26（七续）：文本面板落点/尺寸冻结成 A 类基线（`ref/exe-text-draw.json`）

`0x42AE40`（type-4/5 共用的贴图绘制）的目标矩形算法读通：
- 尺寸：WH 写过 → `prim+0x18/+0x1a`；否则 → **surface `0x510/0x512`（面板尺寸）**；
  `PrimSetText(86)` 在 `0x43BDE1` 清掉 WH 位，所以**文本永远用面板尺寸**；
- 落点：OP 写过 → `prim + OP + 父链累积`；否则 → `prim + surface(0x518/0x51a) + 累积`；
  `surface+0x514/+0x516` 是源裁剪原点（不是落点）。
⇒ **文本面板 1:1 画在 prim 的 (x,y) 上，尺寸 = 面板尺寸**，不缩放、无版式偏移。
这与 `gfx.c` 的 `prim_abs_xy()` → `draw_text_window()` 一致（门禁交叉核对）。

新增 `scripts/extract-exe-text-draw.py` + `ref/exe-text-draw.json` +
`tests/verify_exe_text_draw.py`（`run_all` 6.49648，24 条指令逐条校验）。

## [未发布] — 2026-11-26（六续）：TextBuff 面板尺寸是**下限 1024**，不是"钳到 1024"

`FVP_FRAME_SLOTS=1` 下 script 真的给正文层建了面板：**`TextBuff layer=0 w=1380 h=1024`**
（设计 736×120 → 1380×202，宽原样 / 高抬到 1024）。回看 helper `0x35D95..0x35DA3`：
`pushstack w; pushint16 1024; le; jmpcond` —— w ≤ 1024 时把 w 设成 1024、w > 1024 时
原样传下去，是**下限**不是上限。基线里的 `textbuff_clamp` 因此改名
`textbuff_min_size` 并写清语义；门禁同步。

## [未发布] — 2026-11-26（五续）：帧槽修正模型落地成 `FVP_FRAME_SLOTS` 开关（默认关）

### 证据（A 类，进 `ref/script-window-geometry.json` 的 `frame_model`）
新增两个独立复核样本，都用 `if (槽 == true) 槽 = 默认值` 惯用法：
`0x23804`（`initstack 2|0`，参数槽 253/254）与 `0x3F389`（`8|0`，用到槽 250..254）。
⇒ `initstack A|B`：**A = 参数槽个数**（绝对槽 255-A..254，固定以 254 收尾）、
**B = 额外局部变量个数**（占参数块下面紧挨的 B 个槽，也可用 0..B-1 寻址）。

### 实现
`src/vm.c` 增加 `FVP_FRAME_SLOTS` 开关：
- **默认关**：旧模型，`ref/ref-900k.txt` 逐字节不变（sha256 `7f9837ff…`）；
- `FVP_FRAME_SLOTS=1`：修正模型，实测 `PrimSetText(551, 0)` = **(431, 880)**
  （脚本设计 (230,522) × (1.875,1.6875)），名字/章节/覆盖窗仍正确；
  文本输出变成 6760 B / sha256 `fb5b53fb614f07f1…`（**候选**新基线）。

### 为什么默认不打开
修正模型会改掉冻结文本基线（6496 → 6760 B）。重新冻结属于"已验证行为"的决定，
需要明确后再切默认；本轮只把开关、证据、候选基线准备好。

### 门禁
新增 `tests/verify_frame_slots.py`（`run_all` 6.49647）：默认模式文本 sha256 ==
冻结基线且 `551 == (1,1)`；`FVP_FRAME_SLOTS=1` 时 `551 == (431,880)` 且文本与基线不同。
`verify_script_window_geometry.py` 的帧模型段扩到 6 个函数（含两个复核样本）。

## [未发布] — 2026-11-26（四续）：帧模型证据冻结；正文窗口 (1,1) 的根因定位到我们自己的 VM

### 定位
"正文窗口为什么拿到 (1,1)"的根因**不在 exe、不在脚本，在我们自己的 VM 帧模型**：
脚本对槽 250..254 的默认值写入被丢掉，`PrimSetText(551, 0, …)` 于是拿到 (1,1)，
而脚本要的是设计 (230,522) → 画布 (431,880)。
证据（A 类，新增到 `ref/script-window-geometry.json` 的 `frame_model` 段）：
`initstack A|B` = A 个参数槽（绝对槽 255-A..254）+ B 个额外局部变量；
`0x35C10`(4|2)→251..254、`0x35D14`(3|2)→252..254、`0x35D76`(3|2)→252..254、
`0x39F27`(5|0)→250..253；`true` 是调用方显式压的哨兵（`0x363C5..0x363C9` 连压 5 个
`pushtrue`），没传的实参是 0。

### 为什么本轮没修（重要）
按该模型改 VM 后，`tests/compare.py` 的冻结文本基线立刻变了（6496 → 6760 B，
第 3 行多一个「」）——说明 `ref/ref-900k.txt` 本身是用旧帧模型生成的。修它必须先
用原版证据确认"槽窗口以哪个字节为准"，再有意重新冻结基线；**不能顺手改冻结基线**。
本轮已把 VM 改动全部回滚（工作树干净、compare.py PASS），只留证据与差异记录。

### 门禁
`verify_script_window_geometry.py` 新增第 7 节（4 个函数的 initstack/槽窗口、
5 个哨兵、known_gap 的期望画布），`run_all` 步骤 6.49646 仍 PASS；
make check / verify_gfx / compare / check-docs 全部 PASS。

## [未发布] — 2026-11-26（三续）：撤回"正文在 (1,1) + 面板垂直居中"；脚本侧窗口几何冻结

### 撤回（重要）
上一轮文档里写的"正文窗口在 prim (1,1)、面板 1024×1024、面板内水平+垂直居中"
**是错的两个来源混在一起**：(1,1) 来自我们自己 VM 的运行值，不是脚本数据；垂直居中
是从用户截图目测反推的。用户本轮再给了一张原版截图，逐像素量出来：那行 3 个字格的
墨迹在画布 **x 489.0..585.8、y 456.0..485.2**，而旧模型预测 (465, 1) ——
**垂直差 455 px，模型不成立**。
另外 `0x433A91..0x433AFC` 那段不是"版式表加载"，是**存档反序列化**（调用点
`0x43F38F`），"窗口版式表"假设一并撤回。

### A 类基线（新增 `ref/script-window-geometry.json`）
- 脚本坐标是**设计空间 1024×640**：窗口 helper 先把 x 乘 **1.875 = 1920/1024**、
  y 乘 **1.6875 = 1080/640**（`pushfloat 0x3FF00000 / 0x3FD80000`，helper `0x35C10`）
  再调引擎；**引擎侧不能再缩一次**。
- `TextBuff` helper 把换算后的宽高各自**钳到 1024**（`0x35DA0`），所以"面板
  1024×1024"是全屏设计尺寸被钳出来的结果，不是版式常量。
- 四个文本窗口的绑定（脚本字面量）：名字 `552/8` 设计 (200,505)→画布 (375,852)；
  正文 `551/0` 设计 (230,522)→(431,881)（坐标来自局部变量默认值）；
  覆盖窗 `558/10` (0,0)；章节标题 `553/9` 设计 (88,350)→(165,590)。
- `tests/verify_script_window_geometry.py`（`run_all` 步骤 6.49646）门禁：重新提取
  逐字节一致 + 换算常量（bit 级）+ helper 家族 + 四个绑定 + 钳位 + C 侧 case 86 写 X/Y。

### 已知差异（下一轮修）
我们引擎对 552/553 拿到的画布坐标与脚本一致，但 **551（正文）拿到 (1,1)**：脚本里
"取局部变量默认 230/522"那段流程在我们 VM 里没走对（`PrimSetText(86)` 的 x/y 被
`true` 哨兵顶成 1）。用户截图那行 3 个字在 (489,456)，四个已知窗口都对不上，所以
还有"运行时挪窗口"或"面板落点用 surface 矩形"两种可能，下一轮按这两条查。

## [未发布] — 2026-11-26（再续）：引擎对象表基线；正文"居中"找到指令出处

### A 类基线（新增）
- `scripts/extract-exe-engine-objects.py` → `ref/exe-engine-objects.json`：
  `0x448833` 是 `Construct(base, elemsize, count, ctor)`（`0x448843..0x448861`），
  引擎初始化里 14 张对象表的**基址/元素大小/项数/构造函数**全部从调用点实参抄出。
  最大一张是 sprite 表 `engine+0x101064`（`0x51C`×4096 = 5.11 MiB）。
- `tests/verify_exe_engine_objects.py`（`run_all` 步骤 6.49645）：重新提取必须与
  基线逐字节一致；三条互证——① `sprite`→`prim_alt`→`prim` 三表**首尾相接**
  （排除把 elemsize/count 看反）② `prim` 的 (0x61DD64, 0x34, 4096) 与
  `ref/exe-prim-hierarchy.json` 完全一致 ③ 7 个容器构造函数的
  (size,count,元素构造函数) 与注册表 7/7 对上。

### 正文"面板内居中"不再是假设
在 `0x432B60` 里定位到居中的那条算术：`pen_x += (右边界 − 左边界)/2`
（`0x433037..0x433048`，`cltd; sub; sar $1; add`）；笔位是 `tw+0x5E8/+0x5EA`，
字形落位 `0x424550` 读笔位、按 `tw+0x5DC/0x5DE` 裁剪并维护包围盒
`+0x630/+0x638/+0x634/+0x63C`；排版起点 `0x4337A9` 是 `pen_x = tw+0x5AC`、
`pen_y = 0`。`TextFormat(118)` 只允许把 `0x5AC` 写进 **0..64** 且只升不降
（`0x43CC9B..0x43CCAE`）。
两条结论：**每行水平居中**、**垂直顶端对齐**（垂直没有对应居中指令）——旧文档里
"面板内垂直居中"的假设据此**撤回**。差异不再靠截图推，实现前先去 `0x433A91`
那个"窗口版式记录加载点"把数据格式查清（下一轮）。

## [未发布] — 2026-11-26（续）：外壳 `--auto-click`；正文图元 551 确实在 (1,1) 上屏

### 工具
- `bin/fvp.exe` 新增 `--auto-click MS`：每 MS 毫秒注入合成点击边沿，走真实点击同一条
  路径（click_edges → click_armed → InputGetDown 轮询），可与 `--no-input` 同用，
  让"推进对话"可确定性复现。

### 证据（40 s，900 ms 点击，`--dump-text-prims`）
- `551 tw=0 xy=(1,1) alpha=255`（正文，可见）、`558 tw=10 xy=(0,0) alpha=255`、
  `552 tw=8 alpha=0`、`553 tw=9 alpha=0`。
  ⇒ 上一轮"序章没有正文图元"只是没推进到那一步；正文确实由 551 在 (1,1) 上屏，
  与"文本只经 type-5 图元 + 图元坐标 = 父链和"一致。
- 新**假设（待确认）**：面板内部排版居中，不是从左上角排。依据是
  `TextBuff(0,1024,1024)` 与用户截图里那行居中文字的像素换算
  （竖直 1024/2≈512 vs 截图≈520；水平 (1024-500)/2≈262 vs 截图≈266）。
  下一轮在 `0x432b60` / `0x424550` 里找每行起始 x 的计算点，**确认后才实现**。

## [未发布] — 2026-11-26（再再补）：外壳 `--dump-text-prims`；序章正文没有绑定图元

### 工具（诊断正文位置）
- `bin/fvp.exe` 新增 `--dump-text-prims`：每 500 ms 把每个 type-5 文本图元的
  `id / tw / xy / abs（父链累加）/ parent / alpha` 写进日志，渲染行为不变。

### 发现（序章 22 s 实测）
- 序章里**只有** `552(tw=8)` / `553(tw=9)` 两个文本图元，且 alpha=0；
  **没有绑定 tw=0 的图元**。所以画面上看到的叙述文字在当前实现里来自
  "未绑定窗口按版面表兜底"——位置是近似值，不是原版事实。
- 结合 exe 侧结论（文本只能经 type-5 图元上屏），下一轮的两条候选：
  ① 对话文本图元由**引擎自己**创建（脚本那 13 个 PrimSetText 只覆盖 8/9/10 覆盖层）；
  ② 引擎对未绑定窗口另有屏内绘制路径（则版面表要从 exe 找，不能用拟合值）。
  如实写进 `docs/render-model.md`，不用截图拟合去补。
- 上一轮"序章只有 551 号图元"的表述已作废（那是另一段状态的采样）。

## [未发布] — 2026-11-26（再补）：图元层级/绘制派发冻结成 A 类基线

### 基线（正文文本"画在哪"有出处了）
- 新增 `scripts/extract-exe-prim-hierarchy.py` + `ref/exe-prim-hierarchy.json` +
  `tests/verify_exe_prim_hierarchy.py`（已登记进 `tests/run_all.py`）。逐条校验
  **24 条关键指令**，冻结：
  * 图元基址 `engine+0x61DD64`、步长 **0x34**、4096 项、id 1..0xFFF；
  * 字段表（type/内容标志/dirty 位/alpha/blend/父 id/兄弟 id/x/y/w/h/OP/角度/缩放/
    首子/图形槽/文本层号）；
  * 派发 `0x438AB0`：有父 → 把本图元 x/y/alpha 抄进父图元再上溯；无父 → `(type-1)`
    查 `0x438C9C` 的 7 项跳表；
  * 组迭代 `0x438B22..0x438B48`：子调用参数 = 组.x + 传入x（**父链坐标相加**）；
  * type-5 文本：`+0x32` 层号 → `engine+0x98` 窗口表 → 窗口 `+4` 内嵌 surface，
    在父链累积坐标上画。
- 不变式"屏幕坐标 = 父链上 x/y 之和"在门禁里与 C 侧 `prim_abs_xy()` 交叉核对。

### 记录（正文图元位置追踪：60k..900k 步）
- `FVP_DEBUG_TEXT` 增加文本图元父链转储；实测序章里 tw=0 只有图元 551，位置只在
  `(1,1)`/`(1,27)` 之间变化，父图元是 `type=1 xy=(0,0)` 的组。
- 因此"原版正文出现在画面中部"的那一层机制**尚未找到**（候选：那些场景里另有绑到
  tw=0 的图元，或文本窗口内还有未映射的屏内偏移字段）。如实写进
  `docs/render-model.md`，不猜、不用截图拟合补。

## [未发布] — 2026-11-26（补）：headless 外壳的旧版面开关 + 正文浮在 CG 上的行为记录

### 修正（run_all 里 verify_headless 的黑帧）
- 根因是**测试过期**，不是渲染坏了：第 168 轮把默认文本模型换成图元驱动（不再合成
  对话框）后，没有任何图片/字库宿主的 headless 外壳就不可能有墨迹；而
  `verify_headless.py` 的"画面非黑"断言其实一直在测旧版面的合成框。
- 给 `bin/fvp-headless.exe` 加显式 `--text-fixed-layout`（旧版面），门禁改用它跑：
  `screenshot is not black in legacy-layout mode (mean 34.8)`。断言没有被删除。
- 完整 `run_all.py` 复跑：**74 passed / 1 failed（此条）/ 1 skipped**；修后单跑 PASS。

### 记录（正文可以浮在 CG 上：原版行为）
- 用户实机截图确认：原版存在"只有一行正文、没有对话框"的画面。结构证据：正文窗口的
  上屏只经过 type-5 文本图元（五彩 `PrimSetText(551, tw=0, x=1, y=1)`，之后由
  `MotionMove`/`PrimSetXY` 挪位置），对话框是**另一组图元**，两者没有绑定。
- 写进 `docs/render-model.md`：引擎不得合成对话框、不得把正文强制摆进版面坐标；
  `--text-fixed-layout` 的合成框明确标注为"仅冻结像素基线用、不是原版行为"。
- `docs/cross-platform.md` 记录 headless 的文本模式开关。

## [未发布] — 2026-11-26：标题背景动画修两处显示错误 + 全分辨率；文本窗口几何来自 exe

### 修正（风车剪影入场位置太靠中：仿射锚点少了背景裁剪原点）
- path B 的锚点 = OP（=1200,675），但 OP 是**设计画布**（2400×1350）坐标；
  屏幕看到的是裁掉 (240,135) 之后的 1920×1080。少减裁剪原点 ⇒ 整层恒定平移
  (+240,+135)。修成 `锚点 = OP − (bg_src_x, bg_src_y)`。
- 自检不变式（新增 `tests/test_gfx_xform.c` §5，12 项）：缩放 1000‰、角度 0 时
  仿射路径必须与静态直贴**逐字节相同**；锚点不减裁剪原点时整层正好平移
  (crop_x, crop_y)（这就是回归的样子）。
- 顺带修了两个采样约定（同一函数）：逆映射要减 0.5（像素中心），末行/末列
  基址退到 `cw-2/ch-2`；否则恒等映射落在半像素上（发糊 + 少 1 px）、
  90° 旋转的包围盒少一整行。

### 修正（五彩斑斓标题背景：多出一道黑色柱状剪影）
- 根因：`title_draw_overlay()` 先画**静态**叠加层、再调背景动画；而动画路径自己也会
  把三个叠加层按仿射变换画一遍 ⇒ 风车（B 层）被画两遍，静态那份不跟动画走。
- `title_draw_bg()` 改为返回 int（1 = 背景+叠加层已整组画完），调用方据此跳过静态
  叠加层；动画关闭时仍走原路径，静态标题帧与修前**逐字节相同**（0 差异）。

### 修正（图层拿错图：静态按显示顺序装槽、动画按 slot_tag 取图）
- 新增 `fvp_titlebg_slot_tag_for_name()`（`src/titlebg.c/.h`）：叠加层槽号统一按脚本
  GraphLoad（258=A / 259=B / 260=C / 261=D）装载与绘制；此前动画里"风车"取到的是
  C 的图、真风车被当 D 层按 4000‰/62.5° 画。

### 修正（背景动画发糊：1/8 分辨率合成 → 全分辨率）
- `gfx_draw_affine_layers()` 重写：16.16 定点增量映射、8×8 覆盖掩码剔除稀疏层、
  不透明层快路径；新增 `slot_compo_opaque()` / `slot_compo_mask()`（按 compo_version
  缓存，`slot_drop_compo()` 一并释放）。
- 默认 `div=1`（全分辨率，直接写画布）：实测 **36-41 fps**、清晰度（Laplacian 比）
  **0.78**（旧 1/8 分辨率约 0.2）；新增 `--title-bg-div N`（1..8）手动覆盖。
- 门禁 `tests/verify_title_bg_anim.py` 新增**清晰度断言**（动画帧 ≥ 静态帧 45%），
  iroseka fps 阈值 35→25（全分辨率基线）、hoshimemo 45→38（环境抖动留余量）；
  五彩 mean_abs 28.04 / fps 41 / sharpness 0.78。

### 实现（文本窗口几何：面板/字模格/光标/上屏都来自 exe）
- `TextBuff(110)` 接进 gfx：`gfx_text_set_buff()/get_buff()`（缺省 8、上限 0x400、
  越界整条放弃）；图元驱动模式下面板宽 = 换行宽度、面板高/行高 = 行数上限。
- `TextSize(126)` 逐分量语义：`gfx_text_set_size_ex()`（先要 int 再各自 12..64 / 8..32，
  不满足的那个保持原值）；新增 `size_h_set`，避免把构造函数默认 32 当成"脚本指定"。
- `TextPos(122)` 逐轴语义：`gfx_text_set_pos_ex()`（两款作品 0 次调用）。
- `TextOutSize(120)` 在 syscall 层改成逐分量传哨兵，非 int 不再写成 1。
- `tests/test_textprims.c` 29 → **37** 项（面板 set/get/越界/缺省/NULL + 面板文本仍上屏）。
- 文档：`docs/title-menu.md` §25 重写（两处错误的原因、优化表、实测表、门禁数字）、
  `docs/c-gfx.md`（仿射合成 + 文本几何接口）、`docs/render-model.md`（文本几何四层）、
  `docs/win32-shell.md`（`--title-bg-div`）。

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

### 修正（默认文本渲染改成图元驱动：正文可浮于 CG，不再有凭空出现的“标题”）

- **用户实机反馈**：正文页面出现原版没有的「标题」；原版部分正文是**浮在 CG 上方**、
  不在对话框里。
- **根因**：默认渲染是「引擎合成对话框 + 32 个窗口按 `gfx_layout_default(层号)` 固定
  版式画」。那张版式表来自我们自己的 Python 参考渲染器（`gfx.c` 注释就写着
  `render.py TEXT_LAYOUT`），不是原版；它把 9 号窗口（章节标题）在 y=380 用 56 号字
  居中画出来。原版里 8（人名）/9（章节标题）是**覆盖层窗口**，只在脚本给它们建了
  `PrimSetText(86)` 文本图元时才上屏。
- **新默认（`src/gfx.c`）**：
  * 绑到可见 type 5 图元的窗口 → 在**图元树顺序**里按图元绝对坐标（含父链累加）画，
    alpha / draw 跟图元（原版 `0x43BDC0` 写 `prim+0x32 = 窗口号`、`+0x14/+0x16 = x/y`）；
  * 未绑定的正文窗口（0..7、10..31）→ 按窗口自己的版面画；
  * 未绑定的 8 / 9 → **不画**（这就是“凭空标题”的来源）；
  * 引擎**不再画合成对话框/名牌**：框和名牌是脚本自己 `GraphLoad` 的图元。
- **旧路径完整保留**为外壳 `--text-fixed-layout`（合成框 + 版面表）；
  `tests/test_gfx.c` 显式调用 `gfx_set_text_fixed_layout(g, 1)` 钉住它，
  所以 `verify_gfx` 的冻结像素基线 **293 px / 0.0141% / PSNR 49.94 / SSIM 0.99971
  一个数都没变**。
- **门禁**：`test_textprims` 20 → **29 项**（正文按版面画、无合成框、未绑定的 8/9 不画、
  绑定后按图元位置/父链/alpha/draw 画、旧路径可切回、兼容旧 API 名）；
  `test_glyph_fallback` 给层 8 补一个文本图元后照旧通过；
  `verify_multigame` 的文本图元断言改成「日志是 `mode prim-driven` 且 `bound ≥ 1`」
  （alpha=0 的绑定图元合法地不画，旧断言“drawn>0”会误报）。
- **文档**：`render-model.md` §1.3（模式表重写）、`c-gfx.md`、`testing.md`、
  `win32-shell.md`、`roadmap.md`、`status.md` 同步。
- **仍是近似，不掩饰**：未绑定正文窗口的版面仍是 `gfx_layout_default` 的近似
  （下一步从 exe 构造函数/脚本的窗口矩形逆真值）；图元 alpha 仍是“折进颜色”的近似，
  不是物理正确的 src-over 合成。
- 实例：14 s 开局截图里正文「……お願い」直接浮在天空 CG 上，既没有合成对话框，
  也没有那个多余的标题。

### 修正/新增（台词历史环固定写 100 条；外字表格式逆清；槽尾走到 4096 项表首项）

- **台词历史环固定写 100 条记录，与 count 无关**：写档 `0x438190` 的循环
  `edi` 从 0 到 `0x4B0` 步进 `0xC`（100 次），每条走记录写手 `0x437FA0`
  （`u32 len0 + text0 + u32 len1 + text1 + i32 value`，长度为 0 不写字节）；
  空槽 = `{0,0,-1}`（12 字节）。所以环段长度 = `8 + Σ(100)`，样本实测
  0x4B8（没写入任何文本）/ 0x4C0（1 条）/ 0x566（s905 的 11 条）。
  上一轮只用 `count` 条算环长，导致环之后的偏移整体偏小。
- **外字表（第 21 段）格式逆清并验证**：`u32 组数`；每组
  `u32 字数 + 字数×u16`（子写手 `0x43DC30`），再 `u32 记录数 +
  记录数×(u32 码 + u16 长度含 NUL + 字节)`（串走 `0x434410`）。
  6 份真实存档**逐份常量**：4 组 / 4 字 / 12 条 / **0x15A 字节**。
- **槽尾继续往后走通到 4096 项表首项**：外字表之后的 0x24 字节为零，
  紧接着是 4096 项表（`0x4292B0`）的第一个非空项
  `01 01 17 00 "graph/main_window_mask\0"`（6/6 一致）。该表的完整字段规则
  （`u8 flag` 之后的 `u8 flag2` 及其后字段）只逆出一部分，所以段总长仍未知。
- 基线 `ref/exe-save-serialize.json`：新增 3 个子读/写器
  （`history_ring` / `gaiji_table` / `array4096`）与 1 条不变量
  `post_ring_gaiji`；`tests/verify_exe_save_serialize.py` 增加数据侧校验
  （环长按冻结值 `0x4B8/0x4C0/0x566`、外字表常量、4096 项表首项），
  sub_readers 集合断言从 3 条扩到 6 条。
- 文档：`save-format.md` §2.4.5（段表 20/21/22 行、走查链、未解清单）、
  `save-snapshot.md` §4.3、`status.md`、`roadmap.md`、`run_all.py` 说明同步。
- **仍未知**：4096 项表的完整字段规则；64 项表（`0x429570`）/ 32 槽
  （`0x42D880`）/ 窗口（`0x4221E0`）/ 脚本对象（`0x441320`）/ 9 个补间管理器
  的字节数；`0x6A49E8 / 0x6A4DEC / 0x6A65FC+0xC0 / 0x651D64` 的语义。

### 修正（0x600 的"缺口"其实是 256 项表只走了 128 项；槽尾到历史环已无缺口）

- 上一轮写的"调色板与历史环之间还有一段 0x600 未解块"是**我自己走查漏了项**：
  `0x6A4DF0` 那张表是 **256 项**、不是 128 项。写档循环 `0x43E32B` 的计数器
  `ebx` 是上一段 `mov ebx,0x100`（`0x43E26C`）留下的 `0x100`，读档侧
  `0x43F1D0` 也是 `mov ebp,0x100`；内存里 `0x6A61F0 - 0x6A4DF0 = 0x1400`
  正好 = 256 × 0x14，两边互证。上一轮只走了 128 项 → 少 128 个空项 × 12 B =
  **0x600**，于是凭空多出一个"未解块"。
- 修正后 6/6 存档走查**终点就是台词历史环起点**（s481 `0x2061a`、s485 `0x328c6`、
  s800 `0x328bd`、s901/s906 `0x1aea5`、s905 `0x2062b`），环 count/pos 与记录区
  起点仍与独立扫描逐份一致（`0x20622 / 0x328ce / 0x328c5 / 0x1aead / 0x20633 /
  0x1aead`）。**槽尾从全局变量到台词历史环已经全部对上**。
- `ref/exe-save-serialize.json`：第 14 段 `count: 0x80 → 0x100`、读档第 12 步
  改"256 项表"、`samples` 去掉 `walk_gap_before_history_ring`、换成
  `items_table_count` / `items_table_memory_bytes`，不变量 `walk_to_ring` 改成
  "无缺口"，`unknown` 删掉 0x600 那条。
- 门禁：`verify_exe_save_serialize.py` 增"第 14 段 256 项 × 0x14 = 0x1400 =
  `0x6A61F0-0x6A4DF0`"两条静态断言，走查改为 256 项且要求落点 == 环起点；
  文档断言从"写了 0x600"改成"写了 256 项"。
- 文档：`save-format.md` §2.4.5（段表 14 行、走查链、未解清单）、
  `save-snapshot.md` §4.3、`status.md`、`roadmap.md`、`run_all.py` 说明同步；
  "0x600 未解块"结论**作废**。
- **仍未知**：历史环之后各子序列化器（外字表 / 4096 项 / 64 项 / 32 槽 / 窗口 /
  脚本对象）的字节数；`0x6A49E8 / 0x6A4DEC / 0x6A65FC+0xC0 / 0x651D64` 的语义。

### 修正（槽尾全局变量条数 1471 → 1549；"字符串池"其实是 128 槽动态字符串表）

- **两处旧结论作废**：① 槽尾开头不是 1471 条，而是 **G1 = 1549 条 × 8 字节
  = 0x3068**；② 32 个协程上下文写盘每项是 **0xB1D**（读档器 `0x445540` 逐段
  推进游标：0x800 + 2×u32 + u32 + u32 + u8 + 3×u32 + 0x100 + 0x200 = 2845），
  不是内存步长 0xB28，所以段总长 = **0x163A0**（不是 0x16500）。
- **独立证据链（不是拟合）**：全局变量数组共 `G1+G2` 条；`save.bin` 的写档
  （`0x439743`）源指针 = 数组 + `G1×8`、条数取 `engine+0x6A66CC`，
  即 `save.bin` 存的是**后半段 G2 = 1471 条**（由文件长度反推，`fvm --savebin`
  自己也打印 1471 条）；槽尾写**前 G1 条**。脚本里最大全局下标 = 3019
  （扫 HCB 的 `pushglobal/popglobal` 实参）⇒ `G1+G2 = 3020` ⇒ **G1 = 1549**，
  与 `0x3068 + 0x163A0 = 0x19408` 完全吻合。
- **`0x19408` 处的 `0x20` 不是"池标签"，而是线程下标**：读档侧先读 u32 →
  `engine+0x6A66D0`（线程下标；`0x20` = 存档时没有活跃脚本线程），再读 u32 →
  `engine+0x6A49E8`（非空字符串条数），随后才是 **128 槽动态字符串表**
  （`engine+0x6A49EC`，u16 长度含 NUL + 字节，空槽 2 字节 0）。表里的字符串
  就是当时的脚本全局字符串（图名 / 当前对话行）；`type 5` 的全局变量条数与
  表里非空槽数**逐份相等**（1,1,0,0,2,0）。
- **整条走查链打通**：从 tail 开头逐段走（全局 → 32 上下文 → 线程/条数 →
  128 槽表 → 0x100×2 → u32 → 128 项表 → 0x200×2 → 1 B + 16×9 B → 0x400
  调色板）之后，只剩一段 **0x600** 字节未解，紧接着就是台词历史环；
  6/6 存档的环起点与**独立结构扫描**出来的记录起点完全一致
  （`0x20622 / 0x328ce / 0x328c5 / 0x1aead / 0x20633 / 0x1aead`），
  环 count/pos 也合法（s905 = 11/10）。
- **代码**：`src/savetail.c/.h` 基址从 `0x16610` 改成 **`0x163A0`**
  （"线程下标 + 条数 + 字符串"的字节布局不变），新增"其余空槽必须全零"校验
  （排除全零区里的假候选），并允许**空表**（条数 0；3/6 存档此前被当成解析
  失败）；`FvpSaveTailInfo` 增 `thread_index` 字段。
- **门禁**：`test_savetail` 29 → **49 项**、`test_savelist` 84 → **85 项**；
  `verify_exe_save_serialize.py` 新增子读/写器判据（`0x445540` = 0xB1D 的
  7 条累加指令、`0x446210` = `12 + 12×count`、`0x439743` = 数组后半段）
  与 6/6 走查断言；`ref/exe-save-serialize.json` 新增 `sub_readers` / `samples`
  两块，第 6/7/10/14 段的说明改正。
- **文档**：`save-format.md` §2.4.5（第三版重写）、§2.5.1、§3.3；
  `save-snapshot.md` §1、§1.4、§4.1.3、§4.3、§4.6；`status.md`、`roadmap.md`、
  `vm-commands.md`、`03-architecture.md`、`title-menu.md` 与 `tools/*.py`、
  `scripts/*.py` 注释同步；旧的
  `0x2DF8 / 1471 / 0x16610 / 0x16500 / 0x19408 = 0x16610 + 1471×8` 一套
  明确标注为**作废**。
- **仍未知**：调色板与历史环之间那段 `0x600` 字节的归属；历史环之后各子序列化器
  的字节数；`0x6A49E8 / 0x6A4DEC / 0x6A65FC+0xC0 / 0x651D64` 的语义。

### 修正（读档侧顺序冻结；"tail 开头 = 协程快照"作废，其实是全局变量表）

- **读档侧顺序**已冻结：实现在 `0x43EE90` 里内联，`0x43EFCC` 起把游标置 **7**
  （跳过 7 字节时间戳），然后按与写档完全相同的顺序读回，共 **20 步**
  （字符串 → 缩略图 → 每槽全局变量 → 32 协程快照 → 线程下标 → u32 →
  128 槽字符串表 → 0x100 块×2 → u32 → 128 项表 → 0x200 块×2 → 1 字节+16 项 →
  调色板 → 历史环 count/pos + 100 条）。写 31 段 / 读 20 步都进了
  `ref/exe-save-serialize.json`，门禁逐条校验判据字节。
- **作废一条旧结论**：上一轮写的"32 个协程快照紧贴 `tail+0`"是把槽尾开头的
  **1471 条 × 8 字节每槽全局变量**误读成了快照字段。实测（6/6）：
  `tail[0:0x2DF8]` 的 1471 条记录 type ∈ {0,1,2,4,5,6}、`tail+0x18 = 02` 是
  第 4 条的 type；128 槽字符串表在 `0x19300..0x19400`；字符串池标签
  `0x19408 = 0x16610 + 1471×8`。门禁与 `save-format.md` §2.4.5、
  `save-snapshot.md` §1.1/§4.1.3、`status.md` 已同步改写。
- 读档侧 20 步里确认了两个"变长"子读取器（`0x445540` 协程快照、`0x446210`
  128 项表），这解释了为什么"按 0xB28 当定长"去算绝对偏移会对不上；
  各子序列化器的字节数仍是未解项。

### 修正（槽尾第 6 段是 1471 条全局变量，不是 0；字符串池位置随之平移）

- 上一轮"E 段结束于 `tail+0x16608`"的结论只对了一半：6 份真实存档的槽尾**开头**
  是 **1471 条 × 8 字节**的 Variant 形状记录（`{u8 type, 3B pad, u32 value}`，
  type 只出现 0/1/2/4/5/6），即 0x2DF8 字节；把这段算进去之后，**字符串池标签
  正好落在 `0x16610 + 0x2DF8 = 0x19408`，6 份全中**（可证伪预测，已进门禁）。
- 因此 `+0x19408` **不是固定偏移**：`src/savetail.c` 改成**扫描定位**
  （`[0x16610, 0x16610+0x4000)` 按 8 字节步进，找唯一的"标签 0x20 + 条数 ≤ 8 +
  字符串合法"候选，多候选放弃），新增 `fvp_savetail_find_pool()`。
- 新增单测 `tests/test_savetail.c`（**29 项**：全局变量 0 / 1471 / 100 条三种
  合成槽尾、标签缺失 / 条数 > 8 / 字符串无 NUL / 两个候选 四类畸形、
  s905 / s481 / s800 三份真实槽），`make check` 33→**34 组**、run_all 75→**76 步**。
- `verify_exe_save_serialize.py` 的数据侧新增 6/6 断言：标签偏移 =
  `0x16610 + 1471×8`；`ref/exe-save-serialize.json` 第 6 段 note 同步改正。
- 文档修正：`save-format.md` §2.4.5 / §2.5.1、`save-snapshot.md` §1.4 / §4.1.3
  都改成"位置随条数平移"；§4.1.3 里"槽尾没有 1471 条全局表"的旧结论明确标注为
  **已被实测推翻**。**仍不确定**：这张表与 `save.bin` 的全局变量表是否同源
  （值不同、`save.bin` 侧布局尚未按同格式对上），先记为"Variant 形状表"，不冒充已解。

### 实现（读档 step-2 第一段：槽尾「台词历史环」回灌 + `fvm --save-history`）

- 上一轮把槽尾第 20 段认成台词历史环后，这一轮**把它真的读回来**：
  新增 `src/savehist.c/.h`（`fvp_savehist_locate` / `fvp_savehist_apply`），
  对应原版读档路径 `0x43F25A`（先读 count/pos 写回 `engine+0x6A49C0/C4`，
  再 100 次调 `0x437EE0` 逐条读回）。`--load-restart` 现在在场景复位后、
  `fvp_vm_start` 前回灌，日志 `history: restored count=11 pos=10 filled=11
  scan=tail+0x20633 newest="　谁这样说道。"`。
- 无窗口可复现：`fvm --script <hcb> --save-history <sNNN.bin> --dump-history
  --steps 0` 直接打印回灌后的 11 条台词。
- **定位靠结构判据、不靠固定偏移**（第 11 段起子序列化器长度还没逆出来）：
  前面 8 字节像 `(count,pos)`、100 条能解析完、未写槽必须是 `{0,0,-1}`、
  文本能按 GBK/CP932 之一解成"像人话"的串、父串非空时最新一条 `text1` 必须
  等于槽头第 3 条字符串；**候选不唯一时整段放弃**。
  `count == 100`（环写满）的候选**一律拒收**：pos 任意、100 条都有历史值，
  判据区分度不够（实测 s485 数据里有 3 个 count=100 的假候选）。
- **编码要自动判定**：汉化版脚本是 CP932、槽里的台词却是 GBK，所以不能沿用
  `--encoding`；两种编码都解一遍，按"汉字/假名/全角标点加分、半角片假名与
  U+FFFD 扣分"选分数高的。
- 验收：`tests/test_savehist.c`（53 项：合成环 + 9 种畸形输入 + s905/s481 真实槽）、
  `verify_load_restart.py` 新增 A/C 段（485 → count=1/pos=0；905 → count=11/pos=10）、
  run_all 74→**75 步**、`make check` 32→**33 组**。
- 顺带把 `save-snapshot.md` §4.6 那句已经过时的"外壳做不到从存档继续执行脚本"
  改成当前真实边界（step-1/step-2 各做到什么、精确续玩还缺什么）。

### 结构分析/修正（台词历史：count 初值 = 1、begin 判据是 Variant type 0、槽尾第 20 段就是历史环）

- `HistorySet(26)` 0x43A5E0 / `HistoryGet(25)` 0x43A490 的**每一条判据**（地址 +
  指令字节）、容器构造、记录环的读写顺序、读档路径的回填，全部冻结进
  `ref/exe-history.json`（提取器 `scripts/extract-exe-history.py`，门禁
  `tests/verify_exe_history.py`，run_all 73→**74 步**）。
- **修正两条**（旧注释写错了）：
  1. `count` 初值是 **1** 不是 0 —— 构造函数 `0x442C4A movl $0x1,(%esi)`，
     "当前这一行"天生算第 1 条；`pos` 初值 0，`begin` 先 `pos++` 再写
     （`0x43A5FC`），所以回绕前恒有 `pos == count-1`；
  2. `begin` 的判据是 Variant **type 0（null）**，**int 0 不算** —— 旧代码用
     `argc < 1` 当 null 分支（永远不成立），现已改成
     `fvp_value_is_null(&args[0])`，并抽成 `fvp_vm_history_set/get` 由
     `tests/test_history.c` 的 VM 级用例钉住（172→**202 项**）。
- **槽尾第 20 段是台词历史环**（不是上一版写的"全局变量 + flag 位图"）：
  `u32 count + u32 pos + 100 × {u32 strlen(text0), text0?, u32 strlen(text1),
  text1?, i32 value}`。6 份真实存档全部解析成功：`s481` 环在 `tail+0x20622`
  （count=1/pos=0）、`s905` 在 `tail+0x20633`（count=11/pos=10，11 条真实台词，
  最新一条 == 槽头当前台词）；`s485` 这类"槽头台词为空"的档里历史仍留着上一句。
- **仍未解**：静态扫描两作各只有 7 个 `HistorySet` 调用点（arg0 全是 int/bool），
  看不到 null，但存档里 count 能到 11 ⇒ `begin` 的真实触发源（引擎内部换行？
  读档链？）还没找到，列进基线的 `unknown`。
- 顺带：`save-format.md` §2.4.5 第 20 段、`vm-commands.md` 历史小节已按上面改。

### 结构分析（存档槽写档顺序：31 段逐指令冻结，读档续玩 step-2 的地基）

- 把写档序列化器 `0x43DE90..0x43E67B`（**2,028 字节 / 31 段**）按指令流读完，
  不再靠"调用序列人工读"：每段都带**引擎来源偏移 + 长度来源 + 判据字节**，冻结成
  `ref/exe-save-serialize.json`（提取器 `scripts/extract-exe-save-serialize.py`，
  门禁 `tests/verify_exe_save_serialize.py`，run_all 72→**73 步**）。
- 段序：时间戳 7 B → 三条字符串（`engine+0xFB4/0xFB8/0xFBC`）→ 缩略图 →
  **每槽全局变量**（`count=0`）→ **32 个协程快照**（`0x68DFC0`，`32×0xB28=0x16500`）
  → 线程下标 u32 → u32 → **128 槽动态字符串表**（`0x6A49EC`，`0x434410`）→
  两个 0x100 块 → u32 → 128×20 B 表 → 两个 0x200 块 → 1 B → 16×12 B →
  **调色板 0x400** → 全局变量+flag（`0x438120`）→ 外字表（`0x43DCE0`）→
  4096×0x51C 对象数组 → 64×0x34 → **prim 数组 0x34000** → 9 个补间管理器 →
  u32 → 32 项 → 窗口 `+0x2D4` → 脚本引擎 256 子对象 → `0x458` → `(0x85!=0)`。
- 新查到两种**长度语义不同**的字符串写法：槽头三条字符串的 u16 长度**不含** NUL
  （`len=strlen`），而 `0x434410` 的动态串长度**含** NUL（`len=strlen+1`，空串只写
  u16 0）。旧文档把两者混为一谈，已改。
- 数据侧用 6 份真实存档验证 4 条**可证伪**推论，全部成立：32 个协程快照紧贴
  `tail+0`（活跃恒为 0..7 号，另有个别 16/17/19/20）、128 槽字符串表正好在
  `tail+0x16608` 结束（全空）、`tail+0x18` 恒为 `02`（旧文档那个"64 B 常量头"
  其实是 0 号协程快照的开头）、末两字节恒为 `01 01`。
- 仍未解（写进基线）：各**子序列化器**的字节数 ⇒ `0x16608` 之后的绝对偏移还算不
  出来；`0x6A49E8 / 0x6A4DEC / 0x6A65FC+0xC0 / 0x651D64 / 0x458 / 0x85` 的语义。

### 实现（读档续玩 step-1：`--load-restart`）

- 按上一轮冻结的读档复位序列落地第一版（默认关，`--load-restart` 打开）：
  1. 读槽文件（`<game_dir>/save/sNNN.bin`，解析章节/对话行长度、缩略图尺寸、
     尾段长度）；
  2. 场景复位：`gfx_prim_reset`(4096) + `gfx_motion_clear` + `gfx_text_clear`(32)
     + 清画布（对应 exe `0x438D20`）；
  3. 重建 VM：`fvp_vm_free → fvp_vm_init → 回填 globals/globals_set/flags →
     attach gfx/snd → fvp_vm_start`（对应 exe `0x441350` + 线程 `+0xFD9` 重跑）。
- 门禁 `tests/verify_load_restart.py`（run_all 71→**72 步**）：槽 485 → thumb
  200×113 / tail 584.9 KB、场景复位、`restarted at entry pc=0x22f48`、重启后脚本
  继续跑（总 40952 步、有文本行）、结束帧非黑（mean 38.4）；另断言不开开关时仍是
  旧行为（日志 `not implemented` / `pass --load-restart to restart`）。
- **还没做**：槽尾"引擎快照"的逐段应用（文本窗口/图元/动画），等字段语义逆清。
- 文档：`docs/win32-shell.md`、`docs/save-format.md`（step-1 落地说明）、status。

### 结构分析（读档链路补完：0x4344C0 = 整文件读取助手，复位序列）

- 上一轮冻结了 `Load(40)/SaveWrite(96)` 的请求/消费点，这一轮把"读到之后做什么"
  补上（`ref/exe-load-flow.json` 扩展，门禁 `verify_exe_load_flow.py` 5b 段）：
  - `0x4344C0` = **通用整文件读取助手**：CreateFileW(`0x45D070`) →
    GetFileSize(`0x45D090`) → 缓冲写 `engine+0x0/+0x4` → ReadFile(`0x45D08C`) →
    CloseHandle(`0x45D058`)；失败串 `0x45D968` = CP932
    `'%s'のオープンに失敗しました`；
  - 读档复位序列：`0x441680`（文本/渲染复位）→ `0x42A6C0`（gfx）→
    **`0x438D20`（场景/图元树复位）** → `0x42C4B0`（gfx）→
    **`0x441350`（脚本引擎复位，配合线程 `+0xFD9` 从入口重跑）**；
  - 厘清：**槽文件装引擎快照，`save.bin` 装全局变量/flag**，`Load(slot)` 主要
    作用在快照上，脚本靠启动时灌入的全局变量选场景。
- 文档 `docs/save-format.md` 补"槽文件读取与复位序列"。

### 修正（截图门禁不再依赖宿主鼠标位置：--title-no-hover）

- `verify_opening_render.py` 偶发失败（hoshimemo 结束帧 vs 静态帧 mean_abs 0.42 /
  1.0% 像素）：查到"静态"那次运行时鼠标恰好停在菜单上（日志
  `title: hover -> 2 (mouse 1033,738)`），静态参考帧其实是 hover 变体。
- 新增 `--title-no-hover`（完全不按鼠标位置算 hover），三个渲染门禁
  （opening / title anim / bg anim）都带上它；复跑两作都回到**逐字节 0 差**。
- 文档 `docs/win32-shell.md`、status/CHANGELOG。

### 结构分析（存档/读档引擎侧流程：Load(40)/SaveWrite(96)）

### 结构分析（存档/读档的引擎侧流程：Load(40)/SaveWrite(96)）

- 新增 `scripts/extract-exe-load-flow.py` + 基线 `ref/exe-load-flow.json` +
  门禁 `tests/verify_exe_load_flow.py`（run_all 70→**71 步**）：
  - 存档路径格式 `0x45DA64 = "%s/save/s%03d.bin"`（root=engine+0x6A7E64、
    slot=engine+0xFCC），与外壳现在 `<game_dir>\save\sNNN.bin` 一致；
  - `SaveWrite(96)` handler `0x43C1E0`：int 0..999 → `engine+0xFCA=1`、
    `0xFCC=slot`；`engine+0xFC9 != 0` 时立即调 `0x438540`（缩略图/索引缓存）；
  - `Load(40)` handler `0x43A870`：int 0..999 → `engine+0xFCB=1`、`0xFCC=slot`、
    线程对象 `+0x814=1`；
  - 每帧消费：存档 `0x43F7E6`（sprintf → `0x433FB0` → **`0x438300` 写文件**）；
    读档 `0x43F876` → **`0x43EE90`**（sprintf → **`0x4344C0` 应用存档** → 重置
    256 个线程对象 → 场景/计时复位）→ `engine+0x6A7E6C=0`、线程 `+0xFD9=1`；
    重启：`0x43F614/0x43F641/0x43F68D` 看到 `+0xFD9` 就**本帧跳过线程执行**
    并把线程下标 `engine+0x6A66D0` 归零 → 下一帧线程从入口重跑；
  - 脚本侧两作都是 Load 9 / SaveWrite 3 / HistorySet 7 个调用点（槽号 800/900/
    901/902/906 等）。
  - **还没逆清**（写进基线 `unknown`）：`0x4344C0` 的字段映射、线程重跑后脚本靠
    哪个全局量选场景。所以本站仍只做"部分恢复视图"，真正读档续玩等这两条。
- 文档 `docs/save-format.md` 新增"引擎侧 Load(40)/SaveWrite(96) 的流程"一节。

### 实现（五彩斑斓四层标题背景动画：path A/B + 半分辨率合成，~52 fps）

- 把 §22 冻结的参数 + §RS 段的两条变换路径接到画面：
  - A(258) 是唯一设过 `PrimSetRS` 的层 ⇒ **path A**（锚点 = 画面中心）：1300→900、
    150→0（7.5°→0）；
  - C/D/B(260/261/259) ⇒ **path B**（锚点 = OP=(1200,675)）：2500→800/550→0、
    4000→800/1250→0、1600→800/250→0；全部 25000 ms、mode 3。
- 新增 `fvp_titlebg_layer_count/def/state()`（`src/titlebg.c/.h`，逐层时间轴数学）；
  `tests/test_titlebg.c` 503 → **2167 项**（层定义、t=0/12500/25000 的精确值、
  单调/有界、越界/NULL）。
- 新增 `gfx_draw_affine_layers()`（`src/gfx.c`）：按
  `屏幕 = 锚点 + S·R·(图头+源像素−OP)` 逆映射重采样 + src-over；内部按 **1/4
  分辨率合成**再双线性放大（全分辨率四层要 ~55 ms/帧=18 fps，半分辨率后
  **45–52 fps**）。`--title-bg-anim` 打开时生效；**静态标题仍 1:1 直贴、像素基线不变**。
- 门禁 `tests/verify_title_bg_anim.py` 扩到两作：五彩 动画中 vs 静态 mean_abs
  **20.96**（47.5% 像素）、fps **52**、4 s 序列相邻帧差 min 0.178 / median 0.386 /
  max 1.13（无平台/跳变）；星空原有的"40 s 后 == 静态帧逐字节 0 差"保留。
- 文档 `docs/title-menu.md` §25（层表、实现、折中）、`docs/win32-shell.md`、
  status/CHANGELOG。

### 结构分析（RS 标志决定两条缩放/旋转路径；星空 = 左上锚被静态分析验证）

- `prim+6 & 0x4`（RS 标志，由 PrimSetRS/RS2 设置）把绘制分成两条：
  - **path A**（0x42B21E 起，有 RS）：`final = 画面中心 + S·(R·(图头−OP)+prim+accum)`；
  - **path B**（0x42B3A3 起，只被 MotionMoveS2 改 scale）：`final = prim + 锚点 +
    S·R·(图头 − 锚点)`，锚点 = OP（未设 OP 时以图头为原点）。
- 脚本事实：五彩斑斓**只有 258 调过 PrimSetRS** ⇒ 258 走 path A（绕画面中心），
  259/260/261 走 path B（绕 OP=(1200,675)）；**星空无 OP/RS ⇒ path B，图头 (0,0)
  ⇒ 左上锚缩放**——这正好验证了我们已实现的 `gfx_draw_image_zoomed` 语义。
- 诚实记录一次 B 类像素实验：按"动画终态"（A=0.9、B/C/D=0.8）摆 4 层与原版帧比，
  总差 21.35，比现在静态 13.11 更差 ⇒ 那张原版帧大概率不是动画稳态（或来自窗口
  缩放截图），**不能当摆放判据**；摆放以脚本+静态分析为准。
- 更新 `ref/exe-rs-transform.json`（path A/B 的指令字节与公式）、
  `tests/verify_exe_rs_transform.py`（新增 3b/3c 两组，交叉核对
  `ref/title-bg-motion.json` 的 RS 调用点）、docs/render-model.md、
  animation-baseline §7、title-menu §22。

### 结构分析（global3 / global236 / global232：把"语义未定"定清）

- 新增 `scripts/extract-script-globals.py` + 基线 `ref/script-globals.json` +
  门禁 `tests/verify_script_globals.py`（run_all 69→**70 步**）：
  - **global3 = 体验版布局开关**：==1 时显示 `title_trial`(277)、把 1521/1522、
    1528/1529 用 `PrimSetXY(prim, true, -48)` 挪到屏幕外**隐藏**（`-48` 是 y 的
    绝对值，不是"上移 48"）、1522/1529 的 MotionMove 走 −81 px 分支。
    **两作脚本里都只有一个写点、写 0**（五彩 0x22F7D、星空 0x36AD4）⇒
    这条体验版分支在零售脚本里**不可达**；本站"不隐藏、不上移、不显示 trial 徽标"
    是正确的零售行为。
  - **global236（五彩）/ global232（星空）= 跳过动画开关**：写点 0/1 都有，
    标题进场/开场读点 25 / 24 处；本站默认 0（正常播放），入口暂未实现。
    另：星空的 global236 有 4 个写点但**没人读**，是死变量。
- 文档 `docs/title-menu.md` §24（含"为什么零售版看不到 48 px 隐藏"），§22 的
  "global3 语义未定"改成指向 §24。

### 修正/实现（标题背景缩放"卡顿"：改成逐帧分数映射双线性，10 → 52-59 fps）

- 用户两次反馈"缩放很卡"。查明两个原因并都修掉：
  1. 1920×1080 三角形滤波一次 ~80 ms（10 fps）；上一版用 16 px 源矩形量化 + 缓存
     换到 48 fps，但每 ~0.6 s 跳 16 源像素，**台阶可见**；
  2. 现在**不量化**：`gfx_draw_image_zoomed` 按
     `源坐标 = (屏幕像素+0.5)/缩放 − 0.5` 做分数映射双线性（x/y 表预计算、
     内层纯整数），每帧重算 ~10 ms ⇒ **52-59 fps**，相邻帧差
     min 0.386 / median 0.917 / max 1.587（max/median 1.73，无 0 平台/无跳变）。
- 新增调试选项 `--shot-every MS --shot-div N --shot-prefix P`：一次运行里周期性抓
  降采样帧，用来拼平滑 demo（本轮产出 `title-bgzoom-smooth.mp4` / `.gif`，out/ 不入库）。
- 门禁 `verify_title_bg_anim.py` 增加平滑度段落（4 s 序列、30+ 帧、min>0.02、
  max<3·median+1）并把 fps 断言提到 ≥45；`docs/title-menu.md` §23 记录两次踩坑与
  最终方案；`docs/win32-shell.md` 记录新选项。

### 修正（旋转角单位是 0.05°，不是 0.1°；RS 分支变换冻结）

- 逐指令对完 exe：`PrimSetRS`(82) 的 `idiv 3600` 只是**取模**；渲染 0x42B1E2
  `fild 角度 → fdiv 3600.0 → fmul π → D3DXMatrixRotationZ` ⇒
  **渲染角 = 单位 × π / 3600 rad = 0.05°/单位，7200 单位 = 360°**。
  之前 render-model / title-menu / vm-commands / 代码（`/10.0`）都按 0.1° 算，是错的。
- 影响：开场 p1..p3 的 `MotionMoveR 3600→0` 渲染是 **180°→0**（不是 360°→0）；
  标题背景 150/250/550/1250 单位 = **7.5/12.5/27.5/62.5°**。
- 源码：`src/gfx.c` 四处角度换算 `/10.0 → /20.0`；`angle_x10` 统一改名 `angle_units`
  （gfx/openseq/titlebg/外壳/测试）；`tests/test_gfx_xform.c` 的 90°/180° 断言改用
  1800/3600 单位。
- 新增 `scripts/extract-exe-rs-transform.py` + 基线 `ref/exe-rs-transform.json`：
  - 角度单位（含指令字节：`dc3578d74500` / `dc0d202e4700` / `bf100e0000`）；
  - RS 分支（`test byte [prim+6],4`）矩阵顺序 R → T1 → S → T2、
    `T2 = (engine+0x60/2, engine+0x64/2)` 画面中心 ⇒
    `final = T2 + S·(R·(图头−OP) + prim + accum)`；
    自洽特例：背景 s=1 → (−240,−135) 居中裁剪、s=0.8 → (0,0) 整屏；
    并**明确记录** "prim.xy≠0 的图元会多出 (画面中心−OP) 偏移，尚未接进渲染器"。
- 门禁 `tests/verify_exe_rs_transform.py`（重提取逐字节 + 源码 /20.0 + 文档无旧说法）；
  `run_all` 68→**69 步**；文档 render-model/title-menu/vm-commands/opening/
  animation-baseline §6 全部改到新单位。

### 实现（星空标题背景缩放：2000→1000 per-mille / 40000 ms，带缩放缓存）

- 把 §上一轮冻结的背景参数里**规则最干净**的一条接到画面：星空单层 1920×1080、
  无 PrimSetOP（左上锚）、不旋转，脚本只有 `MotionMoveS2(258,2000,1000,2000,1000,
  40000,3)`（hcb 0x6FD73..0x6FD87）。
- 新增 `src/titlebg.c/.h`（纯时间轴：`2000-1000·ease3(t/40000)`；五彩斑斓
  `animated=0` 明确未实现）+ `tests/test_titlebg.c`（503 项）。
- `src/gfx.c` 新增 `gfx_draw_image_zoomed()`：只处理屏幕可见的源区域、源矩形量化
  到 16 px 并缓存缩放结果。**性能从 10 fps 提到 ~48 fps**（无缓存时一次 1920×1080
  三角形滤波 ~80 ms）。
- `main_win32.c` 新增 `--title-bg-anim`（默认关；`--title-anim` 不自动打开），
  `title_draw_bg()` 走缩放路径；五彩斑斓会打印 `iroseka: not implemented, static`。
- 门禁 `tests/verify_title_bg_anim.py`：中途 vs 静态 mean_abs 48.09 / 74.6% 像素差；
  **40 s 后 == 静态帧逐字节 0 差**；fps ≥25；日志断言。`run_all` 66→**68 步**
  （+test_titlebg +bg-anim 门禁）。`tests/test_gfx_xform.c` 加第 9 组
  （cropped_scaled 的 1:1/缩放/越界/边界/确定性，53→64 项）。
- 文档：`docs/title-menu.md` §23（实现表、缓存原因、门禁数字、未做项）；
  `src/titleanim.h` 注释区分"星空已实现 / 五彩未实现"；status/CHANGELOG。

### 结构分析（标题背景的缩放/旋转：旋转在 HCB 脚本里，图像包里没有）

- 回答"背景旋转能不能靠解包解出"：**图像包（HZC1）头只有 w/h/x/y/fmt，
  不含旋转**；旋转/缩放是 HCB 的 `MotionMoveR`(51)/`MotionMoveS2`(54) 调用。
- 新增 `scripts/extract-title-bg-motion.py` + 基线 `ref/title-bg-motion.json` +
  门禁 `tests/verify_title_bg_motion.py`（run_all 65 → 66 步），冻结：
  - 五彩斑斓 4 层 258..261：缩放 1300/1600/2500/4000→900/800/800/800、
    旋转 150/250/550/1250→0（=15/25/55/125°→0°），都 25000 ms / mode 3；
    OP=(1200,675)；Z=1000/900/850/800；`PrimSetRS(258,keep,1000)` 原文写 4 次。
  - 星空：背景只有槽 258（依次装 title_bg3→2→1），缩放 2000→1000、
    **40000 ms**，**没有 MotionMoveR（不旋转）**，也没有 PrimSetOP。
  - 时长来自栈局部 7 的（五彩）把"正常 25000 / 跳过 1"两个分支候选都记进基线，
    不藏歧义。
- `src/titleanim.h` 背景常量补上旋转/OP 与星空缩放（尚未接到画面）。
- 文档：`docs/title-menu.md` §22（三层结论 + 冻结表 + 未实现项）。

### 验证（第二部作品的开场也端到端过门禁）

- `tests/verify_opening_render.py` 从"只跑五彩斑斓"扩成**两个作品**（各自的开场 +
  标题进场时长不同，done 时刻不同；`--opening` 门禁现在两作都覆盖）。
- 实测（连续两次重跑一致）：五彩 早期近白 98.7%、早期 vs 淡出 mean_abs 47.97、
  淡出中 B−R=59.7；星空 97.5% / 32.34 / B−R=15.3（星空标题背景是亮海景，蓝色差小）；
  **两作结束帧都与静态标题帧逐字节 0 差**（开场六层 + 标题进场都归位）。
- 顺手把 done 的抓图余量加大到 ~1.5 s：首帧要解码六张素材，开机慢时
  "进场还差几十毫秒"会导致 2% 像素的非逐字节差（实测遇到一次），现在稳定 0 差。

### 实现/结构分析（标题进场扩到第二部作品：星空的进入时刻比五彩早 1000 ms）

- **`scripts/extract-title-enter.py` 泛化成两作品**，`ref/title-enter.json` 现在同时有
  `iroseka` 与 `hoshimemo`（进场块 0x6FEDE..0x701E2）。两作**结构相同、参数不同**：
  - 五彩斑斓：只等两次 1000 ms → logo 2000 ms 起；菜单 2400 起（900/820/步进 40）、
    纵向 +23 px→0；版权行 2560 起 820 ms、横向 +30 px→0；总计 **4000 ms**
  - 星空：只等一次 1000 ms（装饰层 259/260 与 logo 265+276 同时）→ logo 1000 ms 起；
    菜单 1400 起（同时长），纵向 **+50 px**→0；版权行 1560 起、横向 **+76 px**→0；
    总计 **3000 ms**
  - 位移来自各自的 float32 因子：五彩 1.875/1.6875（14→23、16→30），星空 2.4/1.8（28→50、32→76）
- `src/titleenter.c/.h` 改成**每作品表**（`FvpTitleEnterGame`，`fvp_titleenter_game_from_name`），
  `main_win32.c` 按标题 profile 的 game 取时间轴（和 `--opening` 同一套识别）。
- 测试/门禁：`tests/test_titleenter.c` 3496→**3513 项**（两作）；`verify_title_enter.py`
  改成两作；`verify_title_anim_render.py` 扩展成两作端到端（五彩 mean_abs 5.02 / 早期 6.42，
  星空 3.31 / 4.90，**两作进场结束后都与静态帧逐字节 0 差**）。
- 文档：`docs/title-menu.md` §21 补星空表；`src/titleanim.h` 每作品宏。

### 实现/结构分析（标题进场不是统一淡入：逐元素错开，500 ms 那个值是错的）

- **上一轮冻结的 `FVP_TITLEANIM_FADE_MS = 500` 作废。** 那个 500 来自 hcb 0x59120
  的 `local5`，但同一函数的标题进场在此之前已被 **0x592C7** 重设为
  `local5 = 2000 / local7 = 3000`；菜单项在 0x593B7/0x593D9 分支是 `900 / 820`。
- **进场块 0x5933E..0x59650 整段解出**（新增 `scripts/extract-title-enter.py` +
  基线 `ref/title-enter.json`）：
  - logo(265)：**2000 ms 起、2000 ms** 淡入（0→255）
  - 菜单 1501：2400 ms 起、**900 ms**，纵向 **+23 px→0**；
    1508/1515/1522/1529：2440/2480/2520/2560 起（+40 ms 步进）、各 **820 ms**，同样 +23→0
  - 版权行(278)：2560 ms 起、**820 ms**，横向 **+30 px→0**
  - 装饰层 264（1000/2000）、263（2000/2000 + 纵向 −47）、262（2000/**3000**）：
    本站尚未绘制（`title_frame`/`title_frame2` 在全脚本里找不到 `GraphLoad` 调用点），
    只冻结参数
  - 总时长 **4000 ms**（logo 最后结束）；逻辑像素按 float32 因子 1.875/1.6875 换成 HD
- **实现**：新增 `src/titleenter.c/.h`（纯时间轴数学，3496 项单测）；`main_win32.c`
  把统一的 `title_fade` 换成逐元素 alpha/偏移；默认全 255 / 零偏移 ⇒ 静态画面
  逐字节不变；`--title-anim` / `--opening` 才打开。
- **不实现但记录**：1522/1529 的 `global3` 分支第二条 `MotionMove`（终点 −81 px）
  记在基线的 `move_alternatives`，不静默丢掉。
- **门禁**：新增 `tests/test_titleenter.c`（3496 项）、`tests/verify_title_enter.py`
  （重新提取 == 基线 + C 常量 + 外壳用法）；重写 `tests/verify_title_anim_render.py`
  （进场中 mean_abs 5.05、早期只有背景、4000 ms 后与静态帧**逐字节 0 差**）；
  `verify_title_anim.py` 去掉 500 ms 断言、保留场景事实与背景常量。
- **验收**：`make check` PASS（31 组）；`run_all.py` 65 步（新增 2 步）；
  `compare.py --run` 仍 6496 B / `7F9837FF…`；`verify_gfx` 仍 293 px / PSNR 49.94；`check-docs` PASS（33 篇）。

### 实现/结构分析（开场 FAVORITE logo：从抓帧拟合换成脚本时间轴，六层全画）

- **`--opening` 的时间轴不再是"近似值"，全部从 hcb 开场函数读出来**
  - 开场函数 `0x58096..0x584CB`（星空同构 `0x6EFF1..0x6F415`）整段解出：
    六层 `GraphLoad`（250 `logo_bg` / 252 `logo_favo` / 254 `logo_favo_view` /
    255..257 `logo_favo_view_p1..p3`）、逐层 `PrimSetAlpha/Z/OP`、
    淡入 1200/1400 ms、logo 淡出 2500 ms、白底淡出 3500 ms、100 ms 收尾
  - **总时长 3800 ms**（旧拟合值 5400 ms 作废）；`0x5834A` 的
    `MotionAlphaTest(252)` 决定淡出从 1200 ms 开始
  - 新增 `scripts/extract-opening-scene.py` + 基线 `ref/opening-scene.json`
    （两个作品：装载/实参/动画/时间轴/素材头，只含参数）+ 门禁
    `tests/verify_opening_anim.py`（重新提取逐字节比对 + C 常量逐项断言）
  - 新增 `src/openinganim.h`（脚本常量）+ 重写 `src/openseq.h/.c`
    （层表 + 每作品表；淡入线性、缩放/旋转/位移 mode 3；`gfx` 无关，可单测）；
    `src/titleanim.h` 里的开场常量删除，避免两处数字打架
  - **三层 p1..p3 现在真的画出来了**：46×36 三角碎片，`10×→1×` +
    `360°→0°` + 从 (±200,±200) / (0,-200) 收拢到「V」字（mode 3 缓出）
  - **两份作品的差异也从文件里读出来**：逻辑画布 1024×640 / 800×600
    （helper 里的 float32 因子 1.875/1.6875 与 2.4/1.8，按 **float32** 算才对得上
    原版：`300×1.8f` 在 float32 下正好是 540.0）、p1..p3 的 OP y = 628 / 610

- **顺带从 exe 解出 `PrimSetOP` 的语义（缩放/旋转轴心）**
  - `0x42AF08` 的 OP 分支取局部坐标 `(图头 xy − OP)`，`0x42B41F..0x42B477` 的平移是
    `prim.xy + OP + 父链累加`；矩阵函数 `0x446744/0x44674A/0x446732/0x446750` =
    `D3DXMatrixScaling/Translation/Multiply/RotationZ`
  - 即 **最终位置 = prim.xy + OP + R(angle)·S(scale)·(图头 xy − OP)**（identity 时
    OP 抵消）。这解释了脚本为什么先 `PrimSetOP(960,540/628)` 再排缩放/旋转
  - 新增 `gfx_draw_image_op_xform()`（`src/gfx.c/.h`）；`tests/test_gfx_xform.c`
    加第 8 组：identity 抵消、OP=左上角与 xform 逐字节相同、0.5× 绕中心 20×15、
    90° 宽高互换、180° 位置尺寸不变、alpha=0/确定性
  - `docs/render-model.md`：`+0x20/+0x22` 从"语义未明"改成"缩放/旋转轴心"，
    并删掉"OP 未用于渲染"的旧结论；新增 `docs/opening.md` 专文

- **发现脚本顺序：标题场景先建、开场后跑（渲染上最重要的一条）**
  - `0x590CC call 0x584D8` 先建标题场景（载入 258..261、排 25 s 的
    `MotionMoveS2 1300→900` 与 `MotionMoveR 150/250/550/1250→0`），
    `0x591EC call 0x58096` 才跑开场。**所以白底淡出露出来的是标题背景**，不是黑屏
  - `main_win32.c`：开场期间先画一次 `title_draw_overlay()` 并把 `title_fade`
    临时压成 0（标题 logo/菜单的淡入是开场返回后才排的），再画六层
  - 端到端门禁 `tests/verify_opening_render.py`：早期帧近白 98.6%、
    淡出帧 B−R = 59.7（标题背景）、vs 早期 mean_abs 47.98、
    **结束帧与静态标题帧逐字节 0 差**、标题淡入日志出现在开场结束之后
  - `main_win32.c` 的 `--shot-at` 日志在开场期间附带 `opening t=… ms`，
    门禁可以按开场自己的时间轴核对（墙钟里含窗口创建/解码素材开销）

- **标题进场的 500 ms 被标记为"待复核"（诚实记录，本轮不改）**
  - `local5 = 500` 那一处（0x59120）属于背景动画分支；标题对象第一次进场的时长在
    `0x592C7` 被重设为 `local5=2000 / local7=3000`，菜单项在 `0x593B7` 是
    `local5=800` ⇒ 标题进场应是"逐元素时长 + 1000 ms 间隔 + 菜单 800 ms"。
    已写进 `src/titleanim.h` 文件头、`tests/verify_title_anim.py` 断言说明和
    `docs/opening.md` §5.1，下一轮重做

- **验收**：`make check` PASS（30 组；`test_openseq` 从 20 项扩到 **2620 项**）；
  `tests/compare.py --run` 仍 6,496 B / `7F9837FF…`；`verify_gfx` 仍 293 px /
  PSNR 49.94；`run_all.py` 45 → **62 步**（新增两个开场门禁）全 PASS；
  `check-docs.py` PASS（33 篇）

### 修正（标题进场动画挂错了图元：标题是 500 ms 淡入，不是 10 倍缩放）

- **用户实机查看后指出「原版标题是淡入淡出」——按 hcb 加载顺序复核，确认之前挂错了图元**
  - 槽位归属（从 GraphLoad 调用点逐条抄）：**250/252/254/255/256/257 = 开场 FAVORITE
    logo**（`logo_bg` / `logo_favo` / `logo_favo_view` / `logo_favo_view_p1..p3`）；
    **265 = `title_logo1`（标题 logo，0x58B72）**；258..261 标题背景；278 版权行
  - 真正的动画：开场 255/256/257 = `MotionAlpha(0,255,1400)` +
    `MotionMoveS2(10000,1000,…,1400,3)`（**这不是标题**）；标题 262..265 与菜单项 =
    `MotionAlpha(→255, <local5=500>)` = **500 ms 淡入**；背景 258..261 =
    `MotionMoveS2(1300/1600/2500/4000 → 900/800/800/800, <local7=25000>)` = 25 s 极慢缩小
  - `src/titleanim.h` 拆成三组常量（标题淡入 / 背景慢缩放 / 开场 logo），每个带 hcb pc
  - `main_win32.c`：标题绘制改为 **alpha 淡入**（logo / 版权行 / 菜单贴图 / 菜单文字），
    默认 `title_fade=255` → 冻结基线不变；`--title-anim`/`--opening` 才打开；
    计时改用**墙钟时间**（首帧加载贴图可能几百毫秒，累加 frame_dt 会把淡入一帧跳完）
  - **修了一个真 bug**：`src/gfx.c` 的 `blit_sprite()` 一直忽略 `p->alpha`，只用了源像素
    自身 alpha ⇒ `gfx_draw_image_alpha()` 画的图根本不会变淡（淡入"没效果"的根因）。
    修法是新增 `blit_sprite_alpha()` 给 `gfx_draw_image_alpha()`/`gfx_draw_image_xform()`
    专用，**不动** `blit_sprite()` —— 后者是场景渲染（528000 步画面）的基线行为，
    直接改会让 verify_gfx 的 PSNR 从 49.94 掉到 35 左右（实测踩过并回退）
  - 测试/门禁更新：`tests/test_gfx_xform.c`（37 项，含 alpha=128 亮度≈一半的回归）；
    `tests/verify_title_anim.py` 重写（槽位映射 + 500/25000/1200/1400/10000→1000 常量
    + 标题不再用 xform）；`tests/verify_title_anim_render.py` 用 `--shot-at 300`
    抓淡入中途（mean_abs 2.59、9.2% 像素不同），跑完与静态帧逐字节相同
  - 文档：`docs/title-menu.md` §19 整节重写（槽位表、动画表、实现、验证、未做项）

### 结构分析（文本窗口默认值：字号 16x12、字体号 -2/-2，来自 exe 构造函数）

- **文本窗口的默认值从 exe 构造函数 0x430EA0 抄出来，并改正我们实现的偏差**
  - 新增 `scripts/extract-exe-text-defaults.py` + 基线 `ref/exe-text-defaults.json` +
    门禁 `tests/verify_exe_text_defaults.py`（`run_all` 60 → 61 步）
  - 抄出来的默认值：字号 **+0x592=16 / +0x594=12**、字体号 **+0x590=0xFFFE /
    +0x593=0xFE（word/byte 都是 -2）**、间距/排版/开关/阴影/速度全 0、
    `+0x5D4=1`、`+0x5D9=1`；尺寸上限 **0x400=1024**；
    清空例程里 **+0x5E8 := +0x5AC**（行宽累计从初始宽度/下限起算）
  - **改正偏差**：`src/textattr.c` 原来写 12×12、字体号 0（照文档抄的），
    与 exe 不一致 → 改成 **16×12、-2/-2**；`src/textattr.h` 注释同步
  - `tests/test_textattr.c` 增加默认值断言（默认 16×12、-2/-2、间距/阴影/skip 全 0），
    单测 97 项全过
  - 门禁第 5 节把 `src/textattr.c` 里 `fvp_textattr_reset()` 的每个默认值与
    exe 常量逐项对比 —— 以后谁改默认值都会被抓住
  - 文档：`docs/02-text-format.md` 新增"文本窗口的默认值"一节

### 结构分析（文本样式字段的读点地图 + 行宽下限语义）

- **扫出文本样式字段的全部读/写/读改写点，回答"渲染侧怎么用"**
  - 新增 `scripts/extract-exe-text-fields.py` + 基线 `ref/exe-text-fields.json` +
    门禁 `tests/verify_exe_text_fields.py`（`run_all` 59 → 61 步）：按指令文本分类
    写/读/读改写，并按读宽度（1/2/4 字节）记录"被更宽读覆盖"的情况
  - 结论 1：**九个样式字段的写点只来自样式命令 handler + 一处结构拷贝例程**
    （0x430E00..0x431100）
  - 结论 2：**+0x5A8/+0x5AA/+0x5B0/+0x5B4 只写不读**（连覆盖读都没有）——它们被拷贝
    例程搬进排版对象，消费点在那边；文档写清楚，避免误判"没用"
  - 结论 3：**+0x597（TextShadowDist 写的字节）写两次、读零次** ⇒ 我们"只记录、
    不画阴影层"有结构分析依据，不是偷懒
  - 结论 4：**+0x5E8 是累计行宽**（0x4247C4/0x4247DA 用 `add` 累加，0x433006 读出来
    与 +0x5DC 比做换行）；TextFormat arg4 只把它**抬高**（下限语义）
  - 实现：`src/textattr.c` 新增 `fmt_width_floor`（TextFormat arg4 只升不降），
    单测 `tests/test_textattr.c` §8 锁住"只升不降 / 越界两字段都不变 / TextSpace 不碰它"
  - 文档：`docs/02-text-format.md` 新增"渲染侧怎么用这些字段：读写点地图"一节

### 结构分析（文本样式命令的字段与范围：从 exe handler 抄出来）

- **把 `TextFormat`(118) / `TextFunction`(119) / `TextShadowDist`(125) /
  `TextSkip`(127) / `TextSpace`(128) / `TextFont`(113) 的字段语义从 handler 抄出来**
  - 新增 `scripts/extract-exe-text-params.py` + 基线 `ref/exe-text-params.json` +
    门禁 `tests/verify_exe_text_params.py`（`run_all` 58 → 59 步）：用一个小数据流
    （类型检查 / 实参寄存器 / 字段写 / 立即数钳位 / 寄存器比较）把 handler 抄成结构化事实
  - 共同形态：**文本层号 0..31**，样式对象 = `settings(0x481368) + 0x98 + layer*4`
  - 抄出来的字段：`TextFormat` arg1→**+0x5A8**、arg2→**+0x5AA**、arg3→**+0x5AE**、
    arg4→**+0x5AC**（并把 +0x5E8 抬到见过的最大值）、arg5→**+0x5B4**、
    arg6→**+0x5B0**，范围分别是 -32..32 / 0..64 / -16..16；
    `TextFunction` arg1→**+0x5A5**(0..1)、arg2→**+0x5A4**(0..2)、arg3→**+0x5A6**(0..2)；
    `TextSkip` arg1→**+0x5D7**(0..3)；`TextSpace` arg1/arg2→**+0x5A8/+0x5AA**
    （与 TextFormat 的 arg1/arg2 **是同一对字段**）
  - 语义：**越界 = 保持原值**（逐字段跳过 `mov`），只有 `TextShadowDist` 显式钳到
    **0..12** 再尾调方法 `0x4248B0`；`TextFont` 下限 **-5**、上限 = 字体表长度-1
    （`cmp %esi,%eax`），分别调 `0x4247F0`/`0x424860`；`TextSize` 12..64/8..32；
    `TextOutSize` 0..12/0..8；`TextSpeed` -1..300000
  - 文档：`docs/02-text-format.md` 新增"文本样式命令的真实字段与范围"一节
    （含 asm 形态、字段表、三条直接可用的语义）

### 新增（标题进场动画：参数来自脚本，时间轴复用 motion）

- **把标题进场动画实现出来：logo alpha 0→255 + 缩放 10.0→1.0，1400 ms，mode 3**
  - 参数全部来自脚本常量（`ref/title-scene.json` 的 `layer_motion`：图元
    255/256/257 的 `MotionAlpha(0,255,1400)` + `MotionMoveS2(10000,1000,10000,1000,1400,3)`），
    冻结进新头文件 `src/titleanim.h`（每个宏的注释里带 hcb 的 pc）
  - 新增 `gfx_draw_image_xform()`（缩放 + alpha 直贴，左上锚；scale=1000 时内部
    转调 `gfx_draw_image_alpha`，**字节等价**）；时间轴复用 `src/motion.c`
    （mode 1..5 缓动曲线有静态分析依据）
  - win32 外壳：`--title-anim` 打开、`--opening` 时自动打开；帧循环里推进补间，
    logo 用 xform 画，补间结束后回到静态绘制。**默认关闭**（冻结基线不变）
  - 新单测 `tests/test_gfx_xform.c`（32 项，进 `make check` 与 `run_all`）：
    identity xform 与静态绘制逐字节相同、50%/200% 缩放的包围盒、
    alpha 钳位、t=0/700/1400 的补间取值（mode 3 缓出 → 191.25 / 3250）、
    **终端画布与静态绘制逐字节相同**
  - 新门禁 `tests/verify_title_anim.py`（进 `run_all`，55 → 57 步）：
    从 `ref/title-scene.json` 读脚本参数，逐项对比 `src/titleanim.h` 的宏；
    断言 `main_win32.c` 用了这些宏且默认关闭；断言背景四层 258..261 的
    `MotionMoveS2` 是 7 ms（"离开标题"的缩小，不是进场）
  - **一处更正**：冻结基线里 255/256/257 只有 `MotionMoveRStop`、没有
    `MotionMoveR` 起点 ⇒ 进场动画**不含旋转**，`docs/title-menu.md` §16 那一行作废
  - 文档：`docs/title-menu.md` 新增 §19（参数表、实现位置、验证、明确的未做项）

### 结构分析（D3D9 设备创建参数：后台缓冲格式/交换方式/行为标志）

- **把 `CreateDevice` 的 `D3DPRESENT_PARAMETERS` 从 exe 里抄出来，回答"后台缓冲有没有 alpha"**
  - 新增 `scripts/extract-exe-d3d-present.py` + 基线 `ref/exe-d3d-present.json` +
    门禁 `tests/verify_exe_d3d_present.py`（`run_all` 54 → 55 步）：从
    `call 0x44672c`（`Direct3DCreate9`）定位初始化函数，用 `memset(.., 0x38)` 确认
    **56 字节 = sizeof(D3DPRESENT_PARAMETERS)**，再靠极简寄存器跟踪把每个字段的写入还原
  - 抄出来的硬事实：**窗口模式** `BackBufferFormat = D3DFMT_UNKNOWN`、`Windowed=TRUE`、
    `SwapEffect=COPY(3)`、`BackBufferCount=1`、多重采样关、`PresentationInterval=DEFAULT`；
    **全屏模式** `BackBufferFormat = D3DFMT_A8R8G8B8(21)`、`Windowed=FALSE`、宽高来自
    设置对象 `+0x60/+0x64`；`CreateDevice`：`Adapter=0`、`DeviceType=HAL(1)`、
    **`BehaviorFlags=0x24 = MULTITHREADED | SOFTWARE_VERTEXPROCESSING`**、
    设备指针写回 `this+0x8`
  - 交叉验证：同一个 `IDirect3D9*` 上 0x18/0x1c/0x20 三个 vtable 调用的实参形态分别
    对上 `GetAdapterModeCount`/`EnumAdapterModes`/`GetAdapterDisplayMode`，
    0x40 = 第 16 号 = `CreateDevice` —— 这组自洽性证明 vtable 偏移没读错
  - **结论**：窗口模式跟随桌面格式（无 alpha 语义），全屏虽 A8R8G8B8 但 `Present` 只取
    RGB ⇒ **混合公式里目标 alpha 不影响可见结果**，我们加色路径把 `d[3]` 写成 255 无害；
    另外 `SwapEffect=COPY` + `BackBufferCount=1` 是最朴素双缓冲，`PresentationInterval=DEFAULT`
    说明**没有显式开关垂直同步**（抓帧相位差异的一个来源）
  - 文档：`docs/c-gfx.md` 新增"D3D9 设备创建参数"一节（含方法与参数表）

### 结构分析（D3D9 `SetRenderState` 常量：混合模型从"拟合"升级为"抄出来的"）

- **从 exe 的 `SetRenderState` 调用点抄出 D3D9 渲染状态常量，冻结成基线并加门禁**
  - 新增 `scripts/extract-exe-d3d-states.py` + 基线 `ref/exe-d3d-states.json` +
    门禁 `tests/verify_exe_d3d_states.py`（`run_all` 53 → 54 步）：按 COM vtable 调用
    形态（`mov (%eax),%ecx; mov 0xe4(%ecx),%edx; push value; push state; push this;
    call *%edx`）还原 (state, value)；值来自分支的调用点收多个候选值并标 `conditional`
  - 覆盖率是硬的：asm 里 `mov 0xe4(...)` 共 **36 处**，解析出 **31 处**，
    另 **5 处**实参来自寄存器/更远分支 → 记进 `unresolved_sites`，不做推测
  - 抄出来的硬事实：`ALPHABLENDENABLE` 只设为 **TRUE**；`SRCBLEND` 只出现
    `SRCALPHA`(5) / `ONE`(2)；`DESTBLEND` 只出现 `INVSRCALPHA`(6) / `ONE`(2) /
    `ZERO`(1)；**`BLENDOP` 从未设置（D3D9 默认 ADD）**；`ZENABLE=FALSE`、
    `CULLMODE=NONE`、`LIGHTING=FALSE`、`COLORVERTEX=TRUE`、
    `DIFFUSEMATERIALSOURCE=COLOR1`、`ALPHAREF=255`、`ALPHAFUNC=GREATEREQUAL`
  - **三种混合配置**：straight-alpha over（`SRCALPHA`/`INVSRCALPHA`）、
    additive（`SRCALPHA`/`ONE`）、overwrite（`ONE`/`ZERO`，只用于 `Clear` 之前的覆盖写）
  - **交叉验证**：4 个调用点的 `DESTBLEND` 由同一模式字节在 `{INVSRCALPHA, ONE}`
    之间选，而 `PrimSetBlend`(78) 的 handler 只接受 **0..1** 并写 **prim+8** ——
    两边同域一一对应：`prim+8=0` → over、`1` → 加色。所以加色是原版**真实使用**的，
    我们的 `GFX_F_BLEND` 语义正确（注释同步改正为"over / additive"）
  - 意义：第 15 轮"54 候选拟合出直通 alpha over"的结论现在有结构分析依据，抓帧降级为验证；
    同时确认**不需要实现** `BLENDOP` 的 SUBTRACT/MIN/MAX（exe 从不设置）
  - 文档：`docs/c-gfx.md` 新增"D3D9 混合状态"一节；`docs/title-menu.md` §15 加交叉引用

### 结构分析（PrimSet* 的字段偏移、脏标记与"非 int = 保持原值"缺省规则）

- **把 `PrimSetXY/WH/OP/Blend/Alpha` 的实参语义从 handler 里抄出来，并改正上一轮一处误读**
  - `scripts/extract-exe-motion-params.py` 扩到 5 条 `PrimSet*`（77/78/81/89/90，加原有
    42/54/55/82/91 共 10 条），新增三类提取：绝对寻址字段写
    （`mov %dl,0x61dd6b(%eax,%ecx,1)`，基准 `0x61dd64`）、prim+6 的脏标记位、
    以及"逐实参 int 检查 ⇒ 只写那一个字段"
  - `ref/exe-motion-params.json` 新增 `abs_field_writes` / `dirty_flag_bits`；门禁
    `tests/verify_exe_motion_params.py` 加 §6/§7/§8，并把 `MotionAlpha` 的旧断言
    （"arg2 钳到 255"）按 asm 改正为 **arg1/arg2 都是 0..255 的 alpha 值**、
    arg3 时长、arg4 0..1、arg5 布尔，调用 `0x0042d460` 注册补间
  - 抄出来的硬事实：`PrimSetAlpha`(77) 直接写 **prim+7**、`PrimSetBlend`(78) 写
    **prim+8**、`PrimSetWH`(89) 写 +0x18/+0x1a 且置脏位 **0x1|0x40**、
    `PrimSetXY`(90) 写 +0x14/+0x16 只置 **0x40**、`PrimSetZ`(91) 把 <100 的 z
    钳到 100、`PrimSetOP`(81) 只对 **type==4** 的图元生效
  - **关键规则**：缺省语义是**逐字段**的——某个实参不是 int 就跳过那一条 `mov`，
    其余字段照写。所以 `PrimSetXY(prim, true, -48)` 是"x 不动、**y := -48**（绝对值）"，
    不是"y 减 48"
  - 由此改正 `docs/title-menu.md` §14 的误读：脚本里
    `PrimSetXY(1521/1522, true, -48)`（`iroseka.hcb` `0x58d97`/`0x58da1`，同一条件分支）
    是把高亮图元**挪出屏幕隐藏**，不是 hover 上移；脚本对高亮变体是
    `PrimSetDraw(prim,1)` + `PrimSetAlpha(prim,0)` 常驻 + alpha 切换，
    hover 的实现路线据此改写（`docs/title-menu.md` 新增 §18）
  - 这一项把"演出参数怎么生效"又坐实一格：字段偏移、脏位、缺省语义**只存在于 exe
    handler 里**，抓帧拿不到

### 新增

- **与原版实拍帧的回归门禁（把"和原版差多少"变成可测量的数字）**
  - 新增 `tests/verify_vs_original.py` + `tests/original-baseline.json`：
    直接用 `out/` 里的**原版截图**做基准（客户区偏移/重现参数/冻结阈值都写在
    JSON 里），`mean_abs <= baseline + tol` 才算通过；缺语料时打印 SKIP 返回 0
  - 实测：iroseka 标题 **13.79**（基线 13.93 + 1.5，通过）；星空标题 36.09
    （帧归属是推断的，只记录不断言）
  - 接入 run_all（43 → 44 步）；docs/original-verification.md 新增 §7 说明
    语料位置、JSON 字段含义、刷新基线的方法
  - 这一项把"离原版一模一样多远"从感觉变成了可回归的数字

### 新增（原版绘制协议静态提取）

- **⑥「原版 UI 一模一样」的一半缺口不用再靠实拍：从 exe 静态提取 GDI 绘制协议**
  - 新增 `scripts/extract-exe-gdi.py`：解析 PE 节表建立 VA→文件偏移映射，解析
    `work/iroseka_HD.asm`，按 `work/vm_imports.json` 的 IAT 把 `call *0x45dXXX`
    解析成导入名，再从调用点往前收 `push` 还原实参；立即数静态数据（`MAT2`）直接
    从 exe 读
  - 产物 `ref/exe-gdi-params.json`（只含参数，不含游戏数据）：`CreateFontA` 全 exe
    **仅 1 个调用点**（`0x0042d24d`），`fdwCharSet = 0x80`（`SHIFTJIS_CHARSET`）、
    宽度/倾斜/字重/质量/间距全 0、族名来自寄存器（**数据驱动，不写死在 exe 里**）；
    `GetGlyphOutlineA` 3 个调用点只用 `GGO_METRICS(0)` 与 **`GGO_BITMAP(1)`**；
    3 次共用 `MAT2 @ 0x0045D8F8` = 单位矩阵
  - 关键结论：**原版文字是 1bpp 位图、不抗锯齿**；我们回退路径是
    `TextOutW + 32bpp DIB + ANTIALIASED_QUALITY` —— 这是"像素级一致"剩余差异的
    协议级原因（不是参数没调好）
  - 新门禁 `tests/verify_exe_gdi_params.py`：`--check` 逐字节比对 + 关键不变量断言
    + 打印"原版 vs 我们"对照表；接入 `run_all`（44 → 45 步），缺 exe 时 SKIP
  - **顺带解掉一个老悬案**：字体注册表一直在 exe 的 `.data` 里——2 份相同的 4 项
    指针表（`0x0047F07C` / `0x0047F0D0`）→ ShiftJIS 族名 `ＭＳ ゴシック` /
    `ＭＳ 明朝` / `ＭＳ Ｐゴシック` / `ＭＳ Ｐ明朝`，与 `TextFontCount` 量级对得上；
    门禁断言这 4 个名字与顺序
  - 由此推出：`CreateFontA` 只有 1 个调用点、能建的只有这 4 个日文非衬线族名，
    所以标题菜单的**衬线拉丁字不是走 GDI 现场画的**，更可能是预渲染贴图
    （`docs/title-menu.md` §3 与 §4.11 看似矛盾，现在有了解释）；下一步应找贴图的
    graph id，而不是继续拿字体去凑相关度
  - `docs/original-verification.md` 新增 §8：两类"原版事实"的区分、提取方法、
    提取结果表、与实现的差距表、以及"⑥ 的结论修正"；
    `docs/vm-commands.md` 的"我们没有那份表"同步更正

### 结构分析（Motion*/PrimSet* 实参约束）

- **用 exe handler 裁决"缩放到底 1.0 还是 0.9"——1.0 对，帧扫描不再是依据**
  - 新增 `scripts/extract-exe-motion-params.py` + 基线 `ref/exe-motion-params.json`
    + 门禁 `tests/verify_exe_motion_params.py`（`run_all` 52 → 53 步）：
    从 handler 原版程序里抄出实参类型/范围检查、单位常量、写入的 prim 字段偏移、
    调用的子例程
  - 抄出来的硬事实：
    · `MotionMoveS2`(54) handler `0x0043af70`：8 个实参 —— 图元 1..0xFFF、
      **缩放 4 个 100..10000（per-mille）**、**时长 1..300000 ms**、
      **缓动 mode 1..5**、布尔标志；最后调 `0x0042d670` 注册补间
    · `MotionMoveS2Stop`(55)：只有图元号，跳 `0x00427460` —— 该例程在 0x200 条
      补间表里清 active 标志，**不改回数值**（只取消，不回滚）
    · `MotionAlpha`(42)：目标值**钳到 255**、时长 1..300000
    · `PrimSetRS`(82)：角度 `idiv 3600`（0.1°）写 `prim+0x24`、缩放 ≥100 写
      `prim+0x26/+0x28`；`PrimSetZ`(91)：z ≥100 写 `prim+0x12`
  - 结论：`MotionMoveS2(258,1300,900,…,7,3)` 的**时长 7 ms**、且属于"离开标题"
    的缩小 —— **标题稳态缩放就是 1.0**，与我们的静态绘制一致；第 130 轮用原版帧
    扫描得出的结论对，但方法不再作为依据（只做验证）
  - 顺带坐实 ④「缓动 mode 1..5」：范围 **1..5** 是 exe 的范围检查，不是猜的
  - `docs/title-menu.md` §17：handler 表 + 矛盾如何解开

### 分析（标题动画参数 + 排除法）

- **标题的缩放/淡入动画参数从脚本读出来；并证明原版帧处在动画终点（缩放 1.0）**
  - `scripts/extract-title-scene.py` 新增 `layer_motion`（标题图层/logo 图元的
    `Motion*` 调用，250..261 才收），冻结进 `ref/title-scene.json`；
    门禁 `verify_title_scene.py` §5 断言：
    背景层 `MotionMoveS2` = A 1300→900 / B 1600→800 / C 2500→800 / D 4000→800；
    logo 图元 255/256/257 首次 `MotionAlpha` = 0→255 / 1400 ms
  - `scripts/analyze-title-bg-blend.py` 候选从 18 扩到 **54**（新增缩放维度：
    all-1.0 / 动画终点 900·800 / 动画起点 1300·1600·2500·4000）
  - 实测（原版帧纯背景区）：**all-1.0 = 11.562 最优**；动画起点 32.006；
    动画终点 35.976；Z 升序/降序 13.632/12.257；加色/预乘 17.816/35.233
  - 结论：原版帧是"动画结束后"的稳态（缩放 1.0），我们的静态绘制是对的；
    顺序/混合/底色/缩放四维全部排除，剩余 11.56 属渲染器级差异（D3D 采样/rounding/DPI），
    不再往下抠
  - `docs/title-menu.md` §16：动画参数表 + 54 候选结果 + 下一轮（把动画实现出来）

### 分析（标题背景合成模型）

- **用原版帧验证 18 个背景合成候选：我们的做法（A→C→D→B + 直通 alpha over）就是最优**
  - 新增 `scripts/analyze-title-bg-blend.py` + 基线 `ref/title-bg-blend.json`
    + 门禁 `tests/verify_title_bg_blend.py`（`run_all` 51 → 52 步）
  - 做法：把背景四层素材解成 BGRA，在 numpy 里按 3 种顺序 × 3 种混合 × 2 种底色
    共 18 个候选各合成一遍，**只比原版帧的纯背景区域**（扣掉 logo/菜单/版权行，
    1,559,720 px）
  - 结果：measured/over **11.562** 最优 ／ z-descending/over 12.257 ／
    z-ascending/over 13.632 ／ add 17.816 ／ premul 507.207（素材非预乘）；
    底色黑/白相同（第一层 A 是不透明 BGR24）
  - 结论：合成模型不再是"试出来的"，有原版帧作证；而背景区仍有 11.56 残差
    ⇒ 差异来自**动态量**——脚本标题初始化里确有 MotionAlpha(42)/MotionMoveR(51)/
    MotionMoveS2(54)/MotionMoveZ(57) 一大串动画命令，原版这些层/logo 是动的
  - `docs/title-menu.md` 新增 §15（18 候选表 + 三条结论 + 下一轮计划）

### 修复（标题版权行）+ 分析（误差分布）

- **版权行素材认出来了，而且发现它一直被一条过早的 `return` 跳过**
  - 资源包里 `title_copyright` = **590×37 @ (1189,1043)**（素材头原值），填进
    iroseka profile；但填完数字不动 —— 真因是 `title_draw_overlay()` 的贴图模式
    分支画完菜单就 `return`，版权行绘制在其后，**贴图模式下永远画不到**。
    把版权行挪到 logo 之后（贴图分支之前）后生效
  - 实测：iroseka 13.13 → **13.11**；星空 36.09 → **36.01**（基线同步刷新）
  - 新增 `scripts/analyze-title-diff.py` + 基线 `ref/title-diff-regions.json`
    + 门禁 `tests/verify_title_diff.py`（`run_all` 50 → 51 步）：按**素材头矩形**把
    总差拆成 logo / menu / copyright / rest 四块，采样口径与 `verify_vs_original`
    一致（隔 2 像素），并冻结"copyright 是误差最大区域"的排名
  - 实测分布：**copyright 56.86**（最差，素材对不齐）／menu 19.09（原版帧含 hover 态）／
    logo 15.67／rest 占差异像素的 88%（背景四层合成仍是总量大头）
  - 顺带发现：脚本对菜单图元有 `PrimSetXY(prim, true, -48)` —— **hover 会整行上移
    48 px**，本轮只记录未实现（下一轮）
  - `docs/title-menu.md` 新增 §14（版权行修复 + 误差分布表 + 三条结论）

### 分析（标题背景四层与 Z 顺序）

- **背景四层的 Z 值从脚本读出来了；但它不是剩余像素差的主因（如实记录）**
  - `scripts/extract-title-scene.py` 新增 `layer_z`：抓 `PrimSetZ` 的两个字面量实参；
    基线 `ref/title-scene.json` 冻结五彩斑斓背景层
    **A(258)=1000 / B(259)=900 / C(260)=850 / D(261)=800**，门禁
    `tests/verify_title_scene.py` §4 断言
  - 顺带补上裁剪量的来源：脚本对每层设 `PrimSetOP(prim, 2400/2, 1350/2)`（设计画布
    2400×1350 居中到 1920×1080 → 裁 240/135）与 `PrimSetRS(..., 1000)`（缩放 1.0），
    与我们 `bg_src_x/y=(240,135)` 完全一致
  - **三种顺序实测**（原版标题帧 mean abs diff）：A→C→D→B **13.13**（保留）／
    A→B→C→D 13.65 ／ D→C→B→A 14.38 ⇒ Z 顺序只影响 0.5–1.3，且按 Z 的两种自然
    排法都更差。没有凭 Z 改默认顺序（不猜），而是冻结 Z 值 + 留出 profile 字段
    `bg_on_top`（当前两作均为 0），并把这个结论写进文档
  - 渲染代码顺手拆成 `title_draw_bg()` / `title_draw_overlays()`，叠放顺序由
    `bg_on_top` 决定（为下一轮验证留口）
  - `docs/title-menu.md` 新增 §13（Z 表 + 裁剪来源 + 三种顺序实测 + 下一轮方向）

### 实现（标题 hover 态）

- **五彩斑斓的 hover 态实现：换画高亮贴图 `title_<item>1`（映射由静态像素比对定出）**
  - 判据（`ref/title-variants.json`，门禁 `tests/verify_title_variants.py` §2.5）：
    变体 1 比常态**高 11 px**、**B−R 从 36 → 121**（明显偏蓝）、alpha 不下降
    ⇒ 高亮/选中态；变体 2..5 的 alpha **93→81→66→52→35** 严格递减
    ⇒ 淡出序列（不可用态过渡）。5 个菜单项全部一致
  - `src/titlemenu.h`：`FvpTitleItemDef` 新增 `asset_hover`
  - `src/titlemenu.c`：五彩斑斓 5 项填 `"title_<item>1"`；星空留空（走 `_ray` 发光层）
  - `src/platform/win32/main_win32.c`：hover 贴图装进槽 `0xF10+i`；被 hover 的行
    换画高亮图（坐标取它自己素材头，与常态同左上角）
  - `tests/verify_multigame.py`：**删掉**"五彩斑斓 hover 暂不改变画面"的已知缺口
    断言，恢复严格断言（差异 >200 px 且落在被 hover 的行）；band 下端按高亮图实际
    高度放宽 12 px。实测 hover 触发 **18,982 px**，其中 **98.3%** 在该行 band 内
  - `tests/test_titlemenu`：243 → **263 项**（hover 贴图名 = `<asset>1`、星空无 hover 图）
  - `scripts/analyze-title-variants.py`：新增每张图的均值 BGRA，供门禁判定"高亮/淡出"
  - `docs/title-menu.md` 新增 §12（映射表 + 实现 + 门禁变更 + 诚实说明：
    尚无原版 hover 实拍帧可做像素对照）

### 新增（标题状态变体分析）

- **状态变体是"整幅不同的帧"，不是"轻微高亮"——hover 不能用换一张图实现**
  - 新增 `scripts/analyze-title-variants.py` + 基线 `ref/title-variants.json`
    + 门禁 `tests/verify_title_variants.py`（`run_all` 49 → 50 步，约 0.9 s）
  - 做法：读 `graph.bin` 索引 + HZC1（44 字节头 + zlib，BGRA），以基准态
    `title_<item>` 为参照，逐像素统计每个变体的差异像素数/占比/包围盒/最大通道差/
    变亮变暗像素数（不靠截图猜）
  - 实测（五彩斑斓）：每项 **5 个变体**，与基准 **69.7%–81.0%** 的像素不同、
    最大通道差 **255**，每个变体都是"变暗远多于变亮"；变体 1 比基准**高 11–12 px**
    （基准 80/80/80/90/80 → 变体 1 为 91/91/91/92/92）
  - 实测（星空）：每项只有 **1 个变体**（`title_<item>2`），同样记录
  - 结论：变体是脚本状态机选的**素材池**——6 个名字先读进局部变量（`local 4..9`、
    `10..15`…），`GraphLoad` 用变量而不是字符串常量装进图元 `1501..1506`、
    `1508..1513`…，再由脚本 `PrimSetDraw` 决定画哪张。所以 hover 的正确实现是
    **解开这段状态机**，不是换一张高亮图
  - `docs/title-menu.md` 新增 §11（数据表 + 推理 + 对 §10.1 的修正），
    并保留 `verify_multigame` 的"hover 暂不改变画面"已知缺口断言（等状态机解开再改）

### 实现（标题菜单改用贴图）

- **五彩斑斓的标题菜单改成贴图模式：与原版标题帧的平均差 13.79 → 13.13**
  - 更正一个旧判断：§3 说"iroseka 的 `title_start*` 是 New Game 的状态变体、
    其它项由引擎用字体画"——**不对**。素材头清单里 `title_continue` /
    `title_album` / `title_option` / `title_end` 都存在且尺寸不同
    （290×80 / 225×80 / **225×90** / 140×80），右边缘统一 1785
  - `src/titlemenu.h`：`FvpTitleItemDef` 新增 `tex_x/tex_y`（0 = 未提供时按墨迹反推）
  - `src/titlemenu.c`：iroseka 5 项填上素材名 + 素材头尺寸/位置；
    hoshimemo 的 5 项也改成素材头原值（820,580 / 845,670 / 880,760 / 875,850 / 905,940）
  - `tests/test_titlemenu` 从 227 项扩到 **243 项**（每项贴图尺寸/位置/素材名）
  - `tests/verify_title_rects.py` 新增第 4 段：直接检查 `src/titlemenu.c` 的常数
    是否等于 `ref/title-rects.json` 的素材头原值（代码漂移立刻红）
  - `tests/original-baseline.json` 的 iroseka 冻结值刷新为 **13.13**（±1.5）
  - `tests/verify_multigame.py`：五彩斑斓的期望改成贴图模式（`textures=5`、
    右边缘 1785）；新增 `hover_glow=False` 分支，把"五彩斑斓 hover 暂不改变画面"
    **冻结成已知缺口**（变体映射未定，按方法论应静态比对变体像素后再实现）
  - `docs/title-menu.md` 新增 §10（这次更正与实测数字）与 §10.1（hover 已知缺口）

### 新增（标题按钮矩形 = 素材头原值）

- **标题菜单的按钮矩形从 `graph.bin` 的素材头读出来了（不再靠截图反推）**
  - 新增 `scripts/extract-title-rects.py`：直接读 BIN 索引（格式见
    `docs/c-assets.md` §3）+ 每条 44 字节 HZC1 头，取 `x/y/w/h`；产物
    `ref/title-rects.json`（不含像素数据）；门禁 `tests/verify_title_rects.py`
    （接入 `run_all` 48 → 49 步，约 0.3 s）
  - **五彩斑斓**（右对齐）：`title_start` (1445,495,340,80)、`title_continue`
    (1495,595,290,80)、`title_album` (1560,695,225,80)、`title_option`
    (1560,795,**225,90**)、`title_end` (1645,895,140,80) → **右边缘全部 1785**、
    行距**恰好 100**、行 y=495/595/695/795/895
  - **星空的记忆**（居中）：(820,580,275,40)、(845,670,230,40)、(880,760,160,40)、
    (875,850,170,40)、(905,940,110,40) → 中心 x=**960**（GAME START 因素材宽 275
    为奇数差 2.5 px）、行距**恰好 90**
  - 与 `docs/title-menu.md` §1「截图上量的墨迹」逐项自洽（墨迹偏移稳定 15–20 px）
    → 以前的测量是对的，但现在有了来源
  - 门禁还冻结了"每项 5 / 2 个状态变体"（选中态是换图）
  - 顺带发现两处该改的实现细节：`src/titlemenu.c` 的 `img_y0 = 492` 与素材头的
    **495** 不符；`title_option` 素材高 **90** 而非 80 —— 下一轮按素材头改正并量像素差

### 新增（标题画面脚本侧事实）

- **标题画面是脚本画的，菜单项就是图元（不是引擎 UI、不需要抓帧）**
  - 新增 `scripts/extract-title-scene.py` + 基线 `ref/title-scene.json` + 门禁
    `tests/verify_title_scene.py`（接入 `run_all` 47 → 48 步）
  - 静态提取到的证据（两部作品）：`title_*` 素材字符串 39 / 27 条；
    `title_*` 的 `GraphLoad` 5 / 8 处（槽位 258–261 背景、277 体验版按钮）；
    菜单项素材 `title_start/continue/album/option/end` 各带状态变体
    （五彩斑斓每项 6 个变体、星空每项 2 个 → 选中/悬停是**换图**不是改色）；
    标题区交互命令 `InputGetCursX/Y`、`InputGetDown`、`InputGetState`、`PrimHit`
  - 关键结论：命中由脚本对**图元**调 `PrimHit`，引擎只判"鼠标在不在图元矩形里" →
    **按钮位置分布 = 图元矩形**，全在脚本里；菜单项图元号：五彩斑斓
    **1501/1508/1515/1522/1529**（步长 7）、星空 **266/268/270/272/274**（步长 2）
  - `docs/title-menu.md` 新增 §7（argc 实测对账）与 §8（标题画面归属 + 图元号表），
    `docs/status.md` 的下一步改为"解出这些图元的 XY/WH/UV 得到按钮矩形"

### 新增（脚本侧实参审计）

- **命令表 argc 用真实脚本对账；顺带否掉"TitleMenu 传布局参数"的猜测**
  - 新增 `scripts/audit-script-argc.py`：不用跑原版、不用截图，直接看脚本在每个
    `syscall` 之前实际压了几个值（基本块回溯；遇到 call / 表达式 / 上一条 syscall
    的返回值记为"不可判定"），与 `ref/cmd_table.json` 的 argc 对账
  - 产物 `ref/script-argc.json`：五彩斑斓 **948,219 条指令 / 825 个可判定调用点 /
    82 条命令的 argc 被证实**；星空 **1,135,159 / 865 / 83**；异常各 3 条且
    **两作完全相同**（IntToText 24 / TextFontCount 114 / TextFontName 116，
    成因是表达式临时值留在栈上，不是表错）
  - 新门禁 `tests/verify_script_argc.py`（接入 `run_all` 46 → 47 步，约 2 s）：
    `--check` 逐字节 + 异常清单只允许这 3 条 + **TitleMenu(141) 的 argc=1 且每个
    调用点紧邻前一条都是 `pushtrue`**（两作 6 / 7 处）
  - 结论修正：`TitleMenu` 只收 1 个布尔（"进/不进标题菜单"），handler 与
    `MenuMessSkip`/`SysAtSkipName` 同为纯空操作 `0x437950` → **标题按钮位置不在
    脚本参数里**；HCB 命令名表里名字后随的那个字节也不是 argc（`TitleMenu` 后面是
    `0x06` 却只传 1 个实参，整表按该布局解析只能对上 135 条且与调用点冲突）
  - `docs/title-menu.md` 新增 §6/§7 记录这条被证否的线索与对账方法；
    `docs/status.md` §6 的下一步同步改向（查"标题画面由谁画"的两条候选路径）

### 新增（原版 GDI 协议实现）

- **⑥ 的第二块：缺字回退可按原版同协议栅格化（`--glyph-original`）**
  - `src/platform/win32/win32_gdi_text.c` 新增 `win32_gdi_text_mask_original()` /
    `win32_gdi_text_advance_original()`：`CreateFontA` 用 `SHIFTJIS_CHARSET`、
    字重/质量/间距全 0；`GetGlyphOutlineA` 用 `GGO_METRICS` 取进位
    （`gmCellIncX`）、`GGO_BITMAP` 取 **1bpp 位图**；`MAT2` 用单位矩阵
    —— 全部参数来自 `ref/exe-gdi-params.json`（从 exe 静态提取）
  - UTF-8 → cp932（`WC_NO_BEST_FIT_CHARS`，禁止猜替代字）→ 按 DBCS 拆 `uChar`；
    `GGO_BITMAP` 行按 4 字节对齐；成品掩码**只有 0/255**
  - cp932 表示不了的字符返回 0：原版是 ANSI 程序，**它自己也画不出来** ——
    汉化文本里 cp932 之外的简化字在原版里本来就不可能正常显示
  - 外壳：`--glyph-original` 切换协议、`--glyph-face` 换族名（默认 exe 注册表第 0 项
    `ＭＳ ゴシック`）；日志多一行 `gfx: glyph fallback = original GDI protocol (...)`
  - 单测 `tests/test_gdi_ggo.c`（**28 项**，`make test_gdi_ggo`，接入 `run_all`
    45 → 46 步）：二值掩码、`gmCellIncX` 的 N 倍进位、cp932 之外的字符返回 0、
    默认族名可改可复位、NULL/越界安全
  - 实测协议差：抗锯齿路径「日」@32 有 **54/1224 ≈ 4.4%** 的像素是中间值，
    这些像素原版协议**根本不会产生**
  - 默认仍是抗锯齿路径 ⇒ 冻结基线不动（图集路径不变）
  - 诚实记录：真机 smoke（`--menu-at` / `--load-slot 905`）本次没触发 GDI 回退
    （`0 tofu, 0 from fallback atlas`），所以"协议差在实拍帧上是多少"还没量化

### 方法论（写进项目目标）

- **先结构分析原版文件，不用抓帧/视觉反推**（用户 2026-11-25 追加，已写入 goal 目标）
  - 判据：能靠结构分析 exe（静态分析/IAT/常量/RTTI/结构体布局）、HCB（内嵌命令表、调用点
    实参）、存档、资源包拿到的事实，一律从文件里读出来并冻结成基线；
    抓帧只用于最终像素验证（B 类）与影片解码后的画面，且只用来**验证**结构分析结果
  - 落到文档：`docs/status.md` §6 按此重排了下一步清单；`docs/original-verification.md`
    §8.1 补了方法论小节
  - 首个按此方法执行的任务：`TitleMenu` 是脚本命令（cmd 141，exe 的 61 个 RTTI 类里
    没有 TitleMenu 类，脚本里 6 处 `03 8D 00` 调用）→ 按钮命中矩形从调用点实参提取，
    并查清 `ref/cmd_table.json` 的 argc=1 与 HCB 名字表字节 0x06 的矛盾

### 文档（收尾对账）

- **新增 `docs/status.md`：项目现状与差距总表（第 120 轮收尾）**
  - 一段话说清现状 + 三个维度的估计（引擎 85–90% / 内容可玩 55–65% / 逐像素"部分"）
  - 七项目标逐条对账（状态、证据门禁、还差什么）
  - 六个缺口的最新状态：⑥ 从"必须实拍逼近"降级为"A 类协议已静态提取 + B 类最终像素
    仍需实拍"，并记录"标题衬线字更可能是预渲染贴图"的新线索
  - 可复核数字表（逐字节 sha256 / 293 px / mean_abs 13.79 / 协议差 4.4% /
    run_all 45·0·1 / make check 30 组 / check-docs 31 篇）
  - 一键复核命令、等原版日文版就绪后的 5 步清单、已知限制（不粉饰）
  - `docs/README.md` 索引补上这一页
- README 的两行陈旧数字更正：`run_all` 从 "37 PASS / 38 步" 改为
  **45 PASS / 0 FAIL / 1 SKIP（46 步）**；"原版真值对照（进行中）"改为
  "（部分完成）"并写清绘制协议已静态提取、`--glyph-original` 已实现

### 新增（设置）

- **② OPTION 面板可调主音量（可写设置的第一块）**
  - 新增 `fvp_snd_bridge_set_master()` / `fvp_snd_bridge_master()`：夹紧 0..100、
    立即把新增益推到正在播放的通道；`syscall.c` 里与命令 101 共用同一实现
  - 外壳：OPTION 面板打开时 ↑/↓ 每次 ±10%，面板实时读回显示（实测 100→90→80→70）
  - `--menu-adjust N` 连调 N 步（截图/门禁钩子）
  - 单测 `test_snd_gain` 扩到 51 项：桥默认 100、set 50/150/-5 的夹紧、读回、
    NULL 安全、attach/detach
  - 门禁 `verify_menu_actions` 增加断言：OPTION 三连调后日志出现 90%/70%

### 新增（菜单动作）

- **② 游戏内菜单的三个动作变成真动作（SAVE/LOAD/OPTION）**
  - `SAVE`：模板槽 + 当前画面写一份真实存档，只写 `--save-preview` 指定的路径，
    不碰游戏 `save/`（实测写出 616,572 B、时间戳 2026-11-25）
  - `LOAD`：先扫描 `save/sNNN.bin` 再打开列表（实测 6 槽与磁盘一致），真读档仍是
    部分恢复（§4.6）
  - `OPTION`：只读设置面板（主音量/文本速度/跳过模式当前值），不做可写设置
  - 新增 `--menu-pick N` 自动激活第 N 项（截图/门禁用）
  - 新门禁 `tests/verify_menu_actions.py`（run_all 42 → 43 步）：SAVE 文件大小/年份、
    LOAD 槽数与磁盘一致、OPTION 面板墨迹 7979 采样
  - 修两个门禁的 BMP 行序错误：我们的写出器是自下而上，读取时必须按 `h-1-y` 翻转
    （`verify_gamemenu` 之前靠"菜单在画面中部"碰巧通过）

### 新增（音频）

- **⑤ 三级音量链抽成纯函数 + 40 项单测**
  - 新增 `float fvp_snd_gain_calc(slot_vol, master_vol, type_vol, silent)`：
    有效增益 = 槽/100 × 主/100 × 类型/100，夹到 [0,1]，silent 恒 0
  - `syscall.c` 的 `fvp_snd_gain()` 改为"取三层当前值 → 调纯函数"，逻辑不再藏在
    静态函数里；三层音量改动后仍会 `fvp_snd_refresh_gains()` 推到正在播放的通道
  - 新增 `tests/test_snd_gain.c` **40 项**：默认 1.0、单层减半、三层相乘（0.125/0.1）、
    任一层 0 → 0、静音优先、越界夹紧、两层单调性扫描、分档语义；已加入 make check
    （单测数 28 → 29）
  - docs/04-audio.md 新增"三级音量"一节，待办里的音量体系一项标为完成

### 新增（UI）

- **② 游戏内菜单（引擎侧 UI）：`src/gamemenu.c` + 50 项单测 + 门禁**
  - 原版没有画菜单的脚本命令（只有 MenuMessSkip / TitleMenu），游戏内菜单由 exe 自己画，
    所以这里与标题菜单一样归外壳实现
  - 纯逻辑 `src/gamemenu.{h,c}`：布局/命中/键盘导航（跳过不可用项、循环）/激活/记录
  - `tests/test_gamemenu.c` **50 项**：坐标边界、跳过不可用、循环、打开时 hover、
    关闭后不响应、NULL 与上限安全；已加入 make check（单测数 27 → 28）
  - 外壳：游戏中 ESC 开关菜单，↑/↓ 或鼠标选行，Enter/左键确认；画法=压暗背景+居中
    5 项+hover 高亮，复用标题字库；TITLE 回标题、QUIT 退出，SAVE/LOAD/OPTION 明确记 stub
  - `--menu-at N` 截图/门禁钩子；门禁 `tests/verify_gamemenu.py`（run_all 41 → 42 步）：
    日志时间基准 407 ms vs 请求 400 ms、截图 1920×1080×32、菜单区墨迹 446 采样
  - 行为变化：ESC 不再直接退出窗口（capture_nav 常开），已在 docs/win32-shell.md 写明

### 新增（跨平台）

- **无窗口（headless）外壳 `make headless`：核心 + 渲染层不依赖 Win32**
  - 新增 `src/platform/portable/main_headless.c`：只用 `platform.h` 的四个系统能力，
    加载 hcb → VM → 挂 `src/gfx.c` 离屏渲染 → 跑 N 步 → 写 24 位 BMP + 文本行
  - Makefile 新增 `headless` 目标（`bin/fvp-headless.exe`），并纳入 `make clean`
  - 新增门禁 `tests/verify_headless.py`（`run_all` 第 6.6 步，40 → 41 步）：
    构建成功、步数与 `fvm` 一致（200000 vs 200000）、截图 1920×1080 且非全黑
    （平均亮度 34.8）、文本回调 32 行
  - 意义：把 ⑦ 从口号变成可验证事实——无窗口系统也能跑核心 + 渲染层
  - docs/cross-platform.md 新增第 4 节说明用法与门禁数字

### 新增（OS 缝）

- **⑦ 跨平台：抽出最小 OS 缝 + 便携实现 + 16 项单测 + 文档**
  - 新增 `src/platform/platform.h`：四个系统能力的契约（单调时钟秒/毫秒、
    写一行 UTF-8 日志、读整个文件）
  - 新增 `src/platform/portable/platform_portable.c`：**只用 C 标准库**的参考实现
  - 新增 `tests/test_platform.c`（16 项）：时钟单调、日志空指针安全、文件读入的
    长度/内容、缺失文件与三个 NULL 参数的错误路径；已加入 `make check`
    （单测数 26 → 27）
  - 新增 `docs/cross-platform.md`：最小缝契约、**还没抽出的部分**（窗口/渲染/
    音频/GDI 字体回退/日志落盘）、一个平台外壳的最小构成、现状与下一步；
    含必须的 DeepSeek 声明
  - 顺手修掉 Makefile 里一个被合并的规则（test_snd_fade/test_platform 目标混在
    一起导致 "overriding recipe" 警告）

### 新增（音频）

- **BGM 交叉淡入淡出接线（两条 BGM 通道轮换）**
  - 通道布局：`SND_CH_BGM=0`、`SND_CH_BGM_ALT=1`（淡入伙伴）、`SND_CH_VOICE=2`、
    `SND_CH_SE=3`（SE 轮转 3..7，比原来少一条）
  - 播放桥里 BGM 改成两条通道轮换：当前通道在响时新曲装到另一条，然后
    `snd_crossfade(旧, 新, 600 ms)`；统计记 `bgm_crossfades`，日志打印
    `snd: bgm crossfade A -> B (600 ms)`；可用 `fvp_vm_snd_set_bgm_xfade()` 调整
  - `snd_crossfade` 在 syscall.c 里声明为弱符号并做 NULL 保护（fvm/部分单测只
    链接 syscall.o 不链接 snd.c，否则会 undefined reference）
  - 单测新增交叉淡变用例（`test_snd_fade` 24 项）：过渡中两条通道同时有声、
    结束后旧通道停止、新通道满音量
  - 如实记录：iroseka 前 25 秒没有 BGM 切换，所以真实运行里还没观察到该日志行——
    需要一个"场景切换"区间才能验证

### 新增（音频基础件）

- **音频通道淡入淡出基础件（`snd_fade`）+ 17 项单测**
  - `src/snd.c`：通道新增 `gain`（当前生效增益，混音按帧推进）与 `vol`（目标音量）；
    `snd_fade(ch, target, ms)` 线性过渡，`target == 0` 淡出完成后通道自动停止；
    淡出中途可反向淡入（不停止）；`ms == 0` 立即生效
  - 新增 `snd_channel_gain()` 与 `snd_play_pcm()`（直通 PCM，测试/流式用）
  - 新增 `tests/test_snd_fade.c`（17 项）：8 kHz 常数 PCM 验证淡出逐块下降、
    >500 ms 后静音且自动停止；淡入逐块上升并收敛；`ms=0` 立即生效；中途反向不停止
  - Makefile 新增 `test_snd_fade` 目标并加入 `make check`（单测数 25 → 26）
  - 混音公式不变（`pcm * gain` + 饱和裁剪）→ 冻结基线不受影响
  - 文档：`docs/04-audio.md` 新增一节，并明确"真正的交叉淡入需要两条通道，接线是下一步"

### 新增（研究）

- **`Load(40)` 边界收尾：可恢复 / 必须重放 的对照表 + 工具**
  - 新增 `scripts/save-recoverable.py`：逐槽打印今天能恢复的字段（时间戳/章节名/台词/图名/缩略图/槽尾诊断），并列出必须重放才能得到的东西
  - 关键细节：**字符串池比 str0/str2 更可靠**——实测 s485 的 str2 是空串，池里仍有台词
  - 明确写死能力边界：Continue 能"列存档 + 缩略图 + 还原那帧画面与那句台词"，
    但**不能从存档继续执行脚本**（没有 pc/栈、没有全局变量）；日志与界面都写 PARTIAL
  - docs/save-snapshot.md 新增 §4.6（含两张表），roadmap 的 Load 缺口改成指向 §4.6

- **让 FLAKY 可诊断：第一次失败的完整输出落盘 + 摘出失败行**
  - 背景：`run_all.py` 只保留重试成功的输出，FLAKY 只剩一句"failed once"，
    这几轮反复出现却定位不到是哪条断言
  - 现在首次失败的原样输出写到 `out/fuzz/run_all-<step>-attempt1.txt`，并把
    `FAIL ...` / `RESULT: FAIL` 行摘进 note：
    `FLAKY: failed once, passed on retry. first attempt failed at: FAIL ... ;
    first-attempt log: ...`
  - 新增 `save_failed_attempt()` / `failed_lines()`，并做了直接调用测试
    （写文件、内容正确、失败行抽取都对）
  - docs/testing.md 记录用法；下次 FLAKY 出现即可定位到具体断言与数值
### 新增（研究）

- **差异分类工具 + 更正 §4.5 的结论 + 记录"前进一行"实验的失败过程**
  - `scripts/diff-save-body.py --classify` 把差异分成 periodic（等距记录数组）/
    small-counter（小幅跳动的计数器）/ other（结构性候选），不做自动判定
  - 实测：s483 vs s484（同台词）periodic 37 / small 13 / other 5（共 6 字节）；
    s481 vs s905（不同台词）other 里含 GBK 文本字节
  - **更正**：上一版写"55 段全部 ±1/±2/±4"不准确（有几段幅度到 +130）；
    正确描述是"集中在动画/计时器记录里"，判据应是"偏移是否落在等距链上"
  - 记录失败过程：iroseka_HD.exe 能在本机启动但几秒内自退；D3D9 全屏窗口
    PrintWindow 抓帧失败、CopyFromScreen 只在窗口存活期间有效 → "前进一行"的
    UI 自动化需要真实桌面会话，暂未取得样本
  - docs/save-snapshot.md §4.5 更正并新增 §4.5.1

- **找到天然"受控对照"两份存档：同台词存档只差 55 段计时器噪声**
  - 原版引擎自己写的 `s483.bin` / `s484.bin`（同尺寸、同台词、相隔 3 分钟）
  - 32 个协程上下文**完全一致**；正文区只 55 段差异，全部是 ±1/±2/±4 的动画/计时器
    计数，并呈现 17/18/22 字节周期的记录（与早先发现的"动画/事件队列"一致）
  - 结论一：上下文区装的是**脚本状态**（同台词不动），不是画面/动画状态
  - 结论二：找行号必须先过滤 delta≤4、长度≤2 的计时器噪声 → 给
    `scripts/diff-save-body.py` 加 `--ignore-small`
  - 结论三：现有 6 个槽两两差异 ≥9.5 万字节，没有"前进一行"的样本；下一份受控
    实验必须由原版引擎产出（UI 自动化）或等官方存档
  - docs/save-snapshot.md 新增 §4.5（含差异特征与工具用法）

- **正文区朴素差分：不足以定位行号（负结果，附正确方法与工具）**
  - 新增 `scripts/diff-save-body.py`：先跳过变长字符串池，再逐字节比较正文区，
    按差异游程长度排序（短游程优先，孤立标量字段才会凸显）
  - 实测 s481 vs s905：差异游程 **32,397 段**，从第一个字节就分叉 —— 两个序章
    存档的演出状态不同（逐字显示中 vs 已停住），朴素差分被非标量差异淹没
  - 结论：定位"行号/场景 id"必须用**受控存档对**（同一存档读档 → 只前进一行 →
    另存一槽，再差分），这才是读档路线的决定性实验
  - 先把工具与负结果写进 docs/save-snapshot.md §4.4，避免重复试错

- **用地标把槽尾分段画出来（`scripts/save-tail-map.py`）**
  - 只用四个已被独立证实的地标（32×0xB1D 上下文 / 字符串池 0x19408 /
    末尾 1,438 B 常量块 / 其余为正文区），不猜任何字段
  - 实测 6 个槽：`0x163A0..0x19408` **全零**（§2.4.4 的 5–7 段本作是空的），
    所以"固定 91,040 B 之后直接是字符串池"，分段顺序确定
  - 收尾常量块 6 槽完全一致（sha256 前缀 7bab52ab6ea15d15、末 2 字节 0101）
  - 字符串池条数会变（0/1/2）：s905 有图名 `BG063_000b`、s481 只有台词、其余为空
    → 它是"动态字符串表"，不是固定字段（解释了早先个别槽解析不一致）
  - 下一步明确：正文区（池后 400–500 KB）才是随进度变化的内容，两份"同场景不同行"
    的存档在这里差分即可圈出"行号/场景 id"
  - docs/save-snapshot.md 新增 §4.3（含 6 槽实测表与工具表）

### 修复

- **hover 门禁的偶发失败：改成"同步骤内自采基线"**
  - 根因：`verify_multigame` 的 hover 断言拿**前面阶段**拍的背景帧当对照，
    高负载下两次截图可能落在同一状态 → 出现过两次 `hover state changes pixels (0)`
    的假失败；直接复跑同一对命令都是 2973 px 差异，无法复现
  - 修法：hover 步骤先在同一轮里拍"无 hover 基线帧"（同参数、带 `--no-input`），
    再拍 hover 帧对比；基线文件缺失时才退回旧路径，不会崩
  - 结果：iroseka 差异 2973 px、hoshimemo 33343 px；两次 `run_all` 均无 FLAKY
  - docs/testing.md 记录根因与修法

### 新增

- **缺字回退改成逐码点（混排字距连续），并有单测**
  - 之前是"整串一次栅格化"，混排（图集字 + 回退字）时字距对不齐；现在
    `draw_text_run()` 只把缺字的**单个码点**交给平台回退，推进宽度取
    `GfxGlyphFallback.advance_of()`（GDI GetTextExtentPoint32W），可选项
  - 新增 `win32_gdi_text_advance()`；外壳把 mask_of + advance_of 一起装上
  - 单测：`tests/test_glyph_fallback.c` 用**假的平台回退**（4×4 掩码 + 固定
    推进 12）验证"缺字不再算 tofu、glyph_fb_runs 计 1、卸载后恢复豆腐块"，
    不依赖 GDI，18 项断言全过
  - 实测：部分恢复视图里回退触发 48 次、`谁这样说道。` 正常、字距比之前均匀
  - 已知限制（已写进 docs/c-gfx.md）：全角空格走回退时首字与空格的字距仍略有
    偏差；彻底一致需要把回退字形并入图集并复用图集推进表

- **缺字回退：图集里没有的码点（汉化文本的汉字）用 GDI 现场栅格化**
  - `src/gfx.h` 新增 `GfxGlyphFallback` 钩子（平台层只交 8 位覆盖率掩码，
    配色/混合留在 gfx.c，像素格式不外泄）；`gfx.c` 在 `draw_line_font()` 里
    检测"整串是否存在图集缺字"，存在才走回退
  - 新增 `src/platform/win32/win32_gdi_text.c`：CreateFontW(-px, "Microsoft YaHei")
    → 32bpp DIB → TextOutW → 取覆盖率；外壳启动时安装，退出前日志报次数
  - 实测：存档 905 的部分恢复视图里 `　谁这样说道。` 正常显示，回退触发 13 次
    （此前是豆腐块/白条）
  - **基线安全**：冻结基线文本无缺字 → 回退不触发；verify_gfx 仍 293 px
    （0.0141%）、PSNR 49.94、SSIM 0.99971，compare.py 逐字节一致
  - 门禁新增：部分恢复视图的日志里必须出现 `glyph fallback used`
  - 已知限制：整串一次栅格化，字距按 GDI，不并入图集推进表；混排时字距可能
    略有差异（够用，非最终形态）

- **`verify_multigame` FLAKY 复核：连跑 3 次全绿，未复现**
  - `run_all` 曾把该步标成 "failed once, passed on retry"；随后单独连跑 3 次
    （`--steps 300000`，每次约 80 s）全部 0 FAIL
  - 判断为高负载下的时序抖动（截图/窗口初始化按固定秒数取帧），不是断言错误；
    已把判断依据与复现方法写进 `docs/testing.md`

- **存档列表 → "部分恢复"视图（Load 的诚实中间态）**
  - 列表里按 Enter/点行：整屏铺开该槽记录的背景图 + 画出台词 + 左上角常驻
    `[PARTIAL RESTORE] visual + text only`；日志写 `load: partial restore slot=N`
  - 明确不是读档：不恢复全局变量/文本窗口/脚本位置（存档里本来没有 pc，见 §4.2）；
    `--load-slot N` 可直接进入该视图（截图/门禁用），ESC 退回列表
  - **顺带修掉一个真实 bug**：同一存档里章节名 CP932、汉化台词 GBK，之前按单一编码
    解码会把台词解成半角片假名乱码；现新增"看着不对就换编码"启发式（半角片假名/
    替换字符占比 > 25% 时重试另一种），并有单测（85 项，含 GBK 行解码成 CJK 的断言）
  - 门禁：部分恢复视图必须记录 partial 日志且整屏平均亮度 > 80

- **"32 个连续上下文"用密度证据确认，撤回上一轮的"矛盾"结论**
  - 新增 `scripts/save-context-map.py`：按 0xB1D 切出 32 个上下文，打印每个的
    非零字节数与 7 个 u32 字段值
  - 记录大小用**指令计数**复核：`add 0x800 + add 0x8 + 5×add %ebx(4) + inc(1) +
    add 0x100 + add 0x200 = 2,845 = 0xB1D`，与字段清单一致
  - 密度：6 个槽里**只有 ctx 0–7 有数据（9–695 B），ctx 8–31 几乎全零**，
    上下文区总和仅 1,068–2,193 B / 91,040 B → 正是"32 个协程上下文、大部分空闲"的样子
  - 上一轮说的"周期 8 字节重复表不像上下文"是**误读**：那是记录内 `+0x81D` 的
    0x100 字节块（32×8B 小表）。结论修正，文档同步
  - 仍存疑：部分字段值呈"字节平移"（如 `0x0A000000`），记录**内部**字段偏移还差
    最后一次核对（可能是 u16 对或跨字段的 4 字节写入）

- **上下文记录长度由调用循环定死（0xB1D × 32），但数据里出现"槽尾开头不是连续上下文"的矛盾**
  - 静态分析 `0x43DE90` 里的循环：`lea ctx[0]` → 32 次 `call 0x445640` → 每次 `add 0xb28`，
    读回侧 `0x43F110` 是同样 32 次循环 → **流里每个上下文 0xB1D、32 个连续 = 91,040 字节，
    无额外头**；0xB28 只是内存结构大小。上一轮"按 0xB28 更规整"的怀疑被否掉
  - 但实测矛盾：按 0xB1D 切，第 1 个上下文 `+0x800` 起不是文本缓冲，而是**周期 8 字节的
    重复表**（s481 `.. 0a` 反复、s905 `00 04 00 14` 反复）→ 槽尾开头**不像** 32 个连续
    上下文记录；可能 (a) 写入顺序与文件顺序不一致，或 (b) 缩略图尺寸不是 200×113 导致
  - 缩略图尺寸已用 alpha 通道验证**确为 200×113**（320×180 / 256×144 / 240×135 都不成立）→ 排除"tail 起点偏移"，更可能是**槽内各段实际顺序与写入顺序不一致**
  - 下一步：用已知地标（字符串池 `+0x19408`）反推正确起点/顺序，再重切切片
  - docs/save-snapshot.md §4.2 把"步长已定"与"排布矛盾"分开写清楚

- **复核上下文序列化的字段来源，并记下"文件排布不是简单连续 0xB1D"的现象**
  - 写侧 `0x445640` 的完整写出量重算：`0x800 + 8 + 4 + 4 + 1 + 12 + 0x100 + 0x200
    = 2,845 = 0xB1D`；两个字节块的内存来源其实是 **`ctx+0x828` / `ctx+0x928`**
    （流内偏移 `+0x81D` / `+0x91D`），内存结构 0xB28，多出的 11 字节不序列化
  - 新增 `scripts/diff-save-contexts.py`：对两份存档按上下文逐字段差分
  - 现象：按 0xB1D 切第 1 个之后的上下文会出现"错位感"，按 0xB28 切反而更规整
    → **文件里的上下文排布可能不是简单连续的 0xB1D 记录**（待专门核对）
  - 但两种切法下都没有 pc 字段，"读档 = 场景重放"的结论不受影响
  - docs/save-snapshot.md §4.2 补上这段复核

- **静态分析确认：序列化的协程上下文里没有 pc / 栈 → 读档只能是"场景重放"**
  - 静态分析 `0x445640`（写上下文）/ `0x445540`（读回）：一个上下文的序列化布局固定
    0xB1D = 2,845 B = 文本缓冲 0x800 + 7 个 u32（ctx+0x808/80C/814/818/81C/820/824）
    + u8(ctx+0x810) + 0x100 + 0x200 两个字节块，与"每窗口 0xB1D"的旧推定量级一致
  - **清单里没有 pc、sp、调用栈或帧** → 存档不是执行状态快照，"从断点继续"不可能；
    这也解释了前两轮为什么找不到 pc / pc 数组（它本来就不存在）
  - 结论：`Load(40)` 的正确目标是"**场景/行号重放**"（恢复状态 → 从场景入口重跑
    → 快进到存档那一行）；候选行号位置：上下文里那 7 个 u32、动态字符串表、
    SaveData 已读标记、全局变量里的场景计数器
  - 下一步具体化：用"同场景不同行"的两份存档做逐字段差分，圈出随进度变化的字段
  - docs/save-snapshot.md 新增 §4.2，roadmap 缺口 #1 表述改准确

- **在 32×0xB28 协程上下文块里找 pc：负结果（过滤器太弱）**
  - 新增 `scripts/find-save-contexts2.py`：按 §2.4.4 的布局，在槽尾开头
    32 个协程上下文（每块 0xB28）里找"合法指令地址"的 u32
  - 结果：命中 156–246 个（s485 达 444），绝大多数是 `0x000102`/`0x010000`
    这类**小整数恰好也是早期指令地址**的假阳性；跨槽候选只有零星几处，
    无法确认 pc 字段
  - 结论：靠值域过滤认 pc 行不通，必须按结构体字段偏移读——需要静态分析
    `0x445540`（读上下文）与 `0x43F0C7`（槽反序列化），这正是下一步
  - 同时确认：槽尾前 91,648 B 的稀疏性（只有活动协程非零）与"32 个协程上下文"
    布局一致 → 该布局表可信

- **实测："只跳 pc"不能恢复读档——还需要保存的调用栈**
  - `fvm` 新增研究钩子 `--pc N`，`src/vm.c` 新增 `fvp_vm_start_at()`
    （从指定指令地址起跑，不经过 main_start）
  - 实测 iroseka：对照组 `main_start=0x22F48` 跑满 20,000 步；候选
    `0x05AB0E` 只跑 85 步就结束（ret 弹空栈）；`0x03AB18` 跑 2,147 步；
    候选 + `--savebin save.bin`（先灌全局变量）仍是 85 步
  - 结论：候选地址落在函数体内，恢复它需要保存下来的**调用栈/帧**；
    结合上一轮"槽尾没有每线程 4 字节 pc 数组"的结论，context 的存储方式
    要么指针化、要么字段更宽——这决定了真读档的可行路线
  - 文档写进 `docs/save-snapshot.md` §4.1.4 与 `docs/c-vm.md`

- **为"真读档"做定向搜索：脚本位置有候选、pc 数组不存在、全局变量表不在槽里**
  - 新增三个只读工具：`scripts/find-save-pc.py`（搜输入轮询指令地址）、
    `find-save-contexts.py`（搜连续合法指令地址游程）、
    `find-save-variants.py`（按 `{u32 value, u32 type}` 扫 Variant 表）
  - 结果一：6 个槽在同一偏移 **`tail+0x3BB1`** 都含 `0x05AB0E`——iroseka 里
    合法指令地址（`syscall 29`），附近是成对的 `{value,type}` 记录（Variant 数组
    的样子）→ **存档脚本位置候选**（尚不能证明是主线程 pc）
  - 结果二：槽尾**没有** 4 字节步长的 pc 数组（≥8 条的游程都是小整数误判）
  - 结果三：槽尾**没有** save.bin 那张 1471 条全局变量表；最像 Variant 表的只是
    **63~65 条 `{int,float}` 成对记录**（@+0x2904，type 2:32 / 6:32），11,245 条的
    最长游程其实是 86 KB 零区 → 与"本作 countA 疑似 0"的旧推定一致
  - 文档写进 `docs/save-snapshot.md` §4.1（含工具表与限定表述）

### 文档

- **roadmap 刷新到当前状态（2026-11-25）**：新增"现状快照"一节（规模/六道门禁的当前数字/已落地的引擎侧 UI/按优先级排列的已知缺口），并把阶段 7/9/10/13/14 的状态从旧结论改成实测结论（Win32 运行器=基本完成、存档=文件层完成但 Load 未做、健壮性=已完成、动画=部分完成、游戏内 UI=部分完成）

### 新增

- **New Game 的白场过渡（原版行为：淡到白 → 白场峰值启动脚本 → 淡出）**
  - 选中 New Game 后白场 0→255（默认 400 ms）、**峰值处**才 `fvp_vm_start()`、
    再 255→0 露出脚本首帧；`--title-fade N` 可改时长（0 = 直接进）
  - 新增 `--pick N` 截图/门禁钩子：把第 N 项当成已被选中
  - 实测（iroseka）：450 ms 帧均值 223（接近全白）、1600 ms 帧均值 0.4（脚本画面），
    日志 `start transition -> vm started at 400 ms` / `done (800 ms)`
  - 门禁：断言白场峰值亮度 > 200 且脚本确实在过渡中启动、之后画面回到脚本

- **推进 `tw+0x5E8` 的语义分析（TextFormat d 的去向）**
  - 审计工具新增 `--context N`：把每个读点前后各 N 行静态分析打出来，便于定位语义
  - 四个读点的上下文显示：`0x5E8` 与 `0x5EA` 成对按有符号 16 位读入绘图路径；
    与"本次字宽"相加后与 `0x5DC - 窗口字段` 比较、超过就调换行函数；
    还有一处按字宽累加并写回 —— 结论：**`0x5E8` 是文本行的行内偏移（排版游标）**，
    `TextFormat d`（0..64）= 初始行内缩进，`0x5DC` ≈ 窗口宽度上限
  - 另发现一处 `0x5E8` 被写进带容量/回绕的 40 字节记录缓冲区 → 它属于**会被
    序列化的状态**，对 ② 真读档是线索
  - 实现待定：把 d 当缩进会改变文本排版、动到冻结基线（参考帧 + compare 195 行），
    按约束不悄悄改基线，先记录为"需批准 + 一起重生成参考帧"的项

- **静态分析审计：③ 里的 TextSpace / TextShadowDist 在原版里也只写不读**
  - 新增 `scripts/audit-text-fields.py`：解析原版程序里对文本窗口字段的每次访问，
    按 AT&T 语法区分读/写
  - 结果：`tw+0x5A8/0x5AA`（TextSpace x/y、TextFormat a/b）、`0x5AE`、`0x5B0`、
    `0x5B4`、`0x596`、`0x597`（TextShadowDist）**读点都是 0**；只有
    **TextFormat 的 d（`0x5AC`）被读两处**，且两处都是"复制进 `tw+0x5E8`"，
    而 `0x5E8` 有 8 个读点、10 个写点（含 `add` 累加）
  - 结论：本站对这些命令"只记录不渲染"**与原版一致**（忠实而非偷懒）；③ 真正
    该做的是解 `tw+0x5E8` 的语义，而不是给 space/shadow 加渲染
  - 文档写进 `docs/02-text-format.md`（含完整表格与可信度说明）

- **标题菜单接上专用字库（Palatino Bold 61）：相关度 0.27 → 0.73**
  - `tests/gfx_dump.py` 的 TTF 选择新增 `FVP_FONT_TTF` 覆盖（默认不变，
    冻结基线不受影响），用 `make-font-atlas.py --size 61` 生成专用图集
  - `gfx` 新增 `gfx_draw_text_line_font()` / `gfx_text_width_font()`：
    指定字体绘制；外壳新增 `--title-font <图集>`，标题菜单文字用它画
  - 验收（与原版 "New Game" 墨迹框做归一化相关）：内建位图字体 **0.266**
    （墨迹 219×43）→ Palatino Bold 61 图集 **0.726**（墨迹 311×44，原版 309×42）
  - `scripts/identify-title-font.py` 新增 `--check-frame`：对我们自己的渲染帧
    跑同一套相关比对，作为字体还原的验收指标
  - 图集是构建产物（`bin/` 已 ignore），文档给出复现命令与系统字体依赖
  - 剩余差距：我们仍带 1 px 描边（原版是柔和阴影，参数未测）+ 抗锯齿阈值

- **量出了标题菜单的字体：Palatino Linotype Bold ≈61 px（两条独立证据）**
  - 新增 `scripts/identify-title-font.py`：把 11 个本机候选字体渲染成 "New Game"，
    质心对齐 + ±8 px 平移搜索后与原版标题帧的墨迹框做归一化相关
  - 结果：**Palatino Linotype Bold 61 相关 0.745**（次优 Palatino 63 = 0.700、
    Georgia Bold 58 = 0.580），且**渲染墨迹宽 309 px 与原版完全一致**
    （高 44 vs 42，差 2 px 是抗锯齿阈值）
  - 结论写进 `docs/title-menu.md` §4.11，并明确标注"最接近"而非"确定"；
    下一步用该字库生成图集、给标题菜单文本一条"指定字体"通路，再用同一指标验收

- **存档预览：选中槽显示"当时那张背景图 + 那句台词"（明确不是读档）**
  - 槽尾字符串池里记的图名（如 `BG063_000b`）装进预览槽、等比缩到 640×360
    画在右下角，底部写出该槽的台词；`--preview-slot N` 可预选槽
  - 新增 `gfx_draw_image_scaled()`；这是**只读预览**，不恢复任何游戏状态，
    `Load(40)` 依旧标注未实现
  - 门禁：选中记有图名的槽时必须出现 `preview graph ... loaded` 且预览区域
    有非黑像素（实测 3600 个采样点亮）
  - 负结果：照 save.bin 的 1471×8 记录格式在槽尾搜全局变量表，只匹配到全零区
    → **槽尾全局变量段不是 save.bin 那种格式**，已记录
- **根因修复：门禁偶发失败（真实鼠标/键盘事件干扰）→ 新增 `--no-input`**
  - 之前 `verify_multigame` 偶发"整帧黑/比对照不上"，根因是跑测时外壳仍接收
    真实光标与点击事件（可能触发菜单项）；现在所有门禁跑法都带 `--no-input`，
    截图只由程序状态决定；连跑两次全绿

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

## [0.3.0] — 已打 tag（v0.3.0 → 4d5a324）：存档写入闭环（模板路径）+ 结构分析文档

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

## [0.2.0] — 已打 tag（v0.2.0 → `981318d`）：音频接运行时 + 存档读写落地 + 全局 flag

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