# FVP 渲染层模型（render.py）

> 以下内容由 DeepSeek 生成,不一定代表作者个人意愿

目标：把 `iroseka.hcb` 跑出来的状态画成 1920×1080 的 PNG，用来验证「脚本 VM → 图层 → 画面」这条链路。

本文只写**已经用出图验证过**的规则；猜测/近似全部集中在 §9「已知偏差」里，不混进主模型。

| 文件 | 作用 |
|---|---|
| `fvp/work/render.py` | 渲染层（继承 `fvm.VM`，**不改 fvm.py**）：prim / graph / parts / text 状态 + 合成 |
| `fvp/work/run_render.py` | 入口：跑 VM、按步数导出 PNG、写 manifest |
| `fvp/work/render_assets.py` | 6 个 `.bin` 图像包的名字索引 + 按需抓取/解码（只读游戏目录） |
| `fvp/work/render_probe.py` / `render_stackcheck.py` / `render_scriptdump.py` / `render_dis.py` / `render_pe.py` | 结构分析时用的探针（syscall 记录、栈平衡、HCB 静态分析、exe 按 VA 读字节） |
| `fvp/out/render/` | 导出的 PNG + `manifest.json` / `manifest_story.json` |

```bash
PY="C:\Users\Mechrevo\.dsh\dsh-runtimes\dsh-primary-runtime\dependencies\python\python.exe"

# 默认：跑 90 万步，均匀导出 12 帧到 fvp/out/render/
$PY fvp/work/run_render.py

# 指定帧数与步长（任务书要求的 CLI 形式）
$PY fvp/work/run_render.py --frames 10 --every 20000

# 指定检查点；--hold-text 让「已 TextClear 但还没打下一句」的帧复用上一句台词
$PY fvp/work/run_render.py --at 513600,528000,534000 --hold-text 30000 --prefix story

# 中文版（GBK）
$PY fvp/work/run_render.py --script zh --out fvp/out/render_zh

# 打印每帧的绘制清单 / handler 异常
$PY fvp/work/run_render.py --at 534000 --dump
$PY fvp/work/run_render.py --at 534000 --debug
```

---

## 1. 图层模型

### 1.1 prim（图元）

`engine + 0x61dd64 + id * 0x34`，id 合法范围 1..0xFFF（`PrimSetAlpha`/`PrimSetXY` 等 handler 里
都是 `cmp $0x1` / `cmp $0xfff` 的边界检查）。0x34 字节的字段偏移全部由 handler 静态分析读出：

| 偏移 | 类型 | 含义 | 写入命令（handler 证据） |
|---|---|---|---|
| +0x04 | u8 | type：0=none 1=group 2=tile 4=sprite 5=text | `sub_439bf0`（prim 初始化器，`mov %bl,0x4(%esi)`） |
| +0x05 | u8 | 是否绘制（1=绘制） | `PrimSetDraw` `mov %dl,0x61dd69` |
| +0x06 | u8 | 标志位：bit0=WH 已设、bit1=OP 已设、bit2=Z/RS 已设、bit6=0x40 dirty | `PrimSetWH/UV/OP/Z/RS` 里的 `orb` |
| +0x07 | u8 | alpha 0..255 | `PrimSetAlpha` `mov %dl,0x61dd6b` |
| +0x08 | u8 | blend 0/1 | `PrimSetBlend` `mov %dl,0x61dd6c` |
| +0x0a | u16 | 父 group id（0xFFFF=无） | `PrimGroupIn` `mov %bx,0xa(%eax)` |
| +0x0c | u16 | 第二个「父」链（渲染时用于把 x/y/alpha 上抛），初始化恒 0xFFFF | `sub_439bf0` `mov %ax,0xc(%esi)` |
| +0x0e/+0x10 | u16 | 同组前/后兄弟 | `PrimGroupIn` / `PrimGroupOut` |
| +0x12 | u16 | Z 值，钳制 100..10000 | `PrimSetZ` `mov %dx,0x12(%ecx)` |
| +0x14/+0x16 | u16 | X / Y（**偏移量**，见 §2） | `PrimSetXY` `mov %cx,0x14` / `0x16` |
| +0x18/+0x1a | u16 | W / H（0=用图自身尺寸） | `PrimSetWH` |
| +0x1c/+0x1e | u16 | U / V（贴图内偏移） | `PrimSetUV`（要求 `prim+4 == 4`） |
| +0x20/+0x22 | u16 | OP：**缩放/旋转轴心**（最终位置 = prim.xy + OP + R·S·(图头 xy − OP)，见 docs/opening.md §3） | `PrimSetOP` |
| +0x24 | u16 | 旋转角（**渲染 1 单位 = 0.05°**：`a/3600·π` rad）；`PrimSetRS/RS2` 里 `idiv $0xe10` 只是取模 | `PrimSetRS/RS2`（`idiv $0xe10`） |
| +0x26/+0x28 | u16 | 缩放 X/Y ×100（1000 = 1.0） | `PrimSetRS/RS2` |
| +0x2a/+0x2c | u16 | 首/末子节点 | `PrimGroupIn` |
| +0x2e | u16 | 图形槽 id（type==4） | `PrimSetSprt` `mov %dx,0x2e(%eax)` |
| +0x30 | u16 | tile 数 | `PrimSetTile` |
| +0x32 | u16 | 文本窗口号（type==5，0..31） | `PrimSetText` `mov %dx,0x32(%eax)` |

另有第二张同构小表 `engine + 0x61d064 + id*0x34`（64 项，id 0..0x3F），是 **parts（立绘部件）** 用的，
见 §3.3。

### 1.2 graph（图形槽）

`engine + 0x101064 + slot*0x51c`（slot 0..0xFFF）。已确认的字段：

| 偏移 | 含义 | 证据 |
|---|---|---|
| +0x508 | 已加载标志 | `0x428bab` `movb $0x1,0x508` |
| +0x509 | 类型（1 = 单帧 RGBA 上传路径） | `0x428e66` / `PartsSelect` 里的 `cmpb $0x1,0x509` |
| +0x510/+0x512 | W / H（贴图像素尺寸） | 绘制的「默认源矩形」分支 `0x42aef6`，`PartsSelect` 用它做边界 |
| +0x514/+0x516 | **图像头里的 x / y** | `0x428c75/0x428c80`（从 NVSG 头 +0xC/+0xE 拷贝） |
| +0x518/+0x51a | 绘制时叠加到位置的第二组偏移（实测恒 0） | `0x42b484` `movswl 0x51a(%ebx)` |

`GraphLoad` 的语义（handler `0x43a340`）：`(slot, name)`。
`name` 为字符串（type4/5）时 `0x428bd0` 解码并上传纹理；为 null（type0）时 `0x424150` 释放槽；
其它类型（如脚本里常见的 `GraphLoad(id, true)`）**handler 直接跳过**，是无副作用的空调用。

### 1.3 text 窗口

`tw[i] = engine->[0x98 + i*4]`（i=0..31），`TextPrint(i, s)` 追加文本，`TextClear(i)` 清空。
实测层用途：**0 = 正文、8 = 人名、9 = 章节/地点标题**（见 §7 的导出帧）。
`TextColor(i, r, g, b)`、`TextSize(i, w, h)`、`TextSpeed` 等只影响排版/颜色，渲染层按层号用固定版式近似（§9）。

**文本窗口的屏幕位置来自"文本图元"，不是来自脚本（2026-11-24 确证）。**
`PrimSetText(86)`（handler **0x43BDC0**）把一个图元重建成 **type 5 = 文本图元**：

```
43bde9: mov %cx,0x32(%eax)   ; prim+0x32 = 文本窗口号（0..31）
43bded: mov %dx,0x14(%eax)   ; prim+0x14 = x（0，随后由 arg3 覆盖）
43bdf1: mov %cx,0x16(%eax)   ; prim+0x16 = y（0，随后由 arg4 覆盖）
43bdf5: movb $0xff,0x7(%eax) ; prim+0x07 = alpha 255
43bdf9: mov %cl,0x8(%eax)    ; prim+0x08 = blend 0
```

也就是说**文本窗口是"挂在图元上"画的**：位置由图元的 x/y（含父链累加）决定，alpha /
draw / 父组平移都跟着图元走。`TextPos`(122) 在两部目标作品里 **0 次调用**，这解释了
为什么"按层号固定版式"永远对不上层 6 这类窗口。

**2026-11-26 起默认改成「图元驱动 + 版面兜底」**（用户实机反馈驱动：原版正文
页面没有一个引擎自绘的「标题」，而且部分正文是浮在 CG 上方、不在对话框里）：

| 情形 | 默认（原版）行为 | 依据 |
|---|---|---|
| 窗口绑到可见 type 5 文本图元 | 画在**图元绝对坐标**（父链累加），alpha/draw 跟图元，且在图元树顺序里画 | `PrimSetText(86)` handler 0x43BDC0 写 `prim+0x32 = 窗口号`、`+0x14/+0x16 = x/y` |
| 未绑定的正文窗口（0..7、10..31） | 按窗口自己的版面画（目前仍是近似版式） | 实机里正文不需要脚本建文本图元也上屏 |
| 未绑定的 8（人名）/ 9（章节标题） | **不画** | 实机里这两个覆盖层只在脚本给它们建了文本图元时才出现；否则会凭空多出一个「标题」 |
| 对话框 / 人名板 | **引擎不画**：框和名牌是脚本自己 `GraphLoad` 的图元 | 旧路径的合成框是我们自己的近似 |

旧路径（外壳 `--text-fixed-layout`）：引擎画合成对话框 + 32 个窗口全部按
`gfx_layout_default(layer)` 画一遍。**冻结像素基线（`verify_gfx` 293 px）就是旧路径冻结的**，
`tests/test_gfx.c` 里显式调用 `gfx_set_text_fixed_layout(g, 1)` 钉住它，所以基线不变。
汇总行 `text prims N drawn, M bound (…; mode …)` 给出这一帧用图元画了几个窗口、绑定了几个。

**图元 alpha 已经乘进文字（2026-11-24），但是近似实现。** `PrimSetAlpha` 写 `prim+0x07`，
原版用它做淡入淡出。我们的实现把 alpha 折进颜色通道（`c * alpha / 255`）再画——
原因是本引擎的字形覆盖是"一次性写屏"：`ink_px()` 在覆盖率 `cov == 255` 时直接写 RGB、
不参与 DST 混合，所以真正的 src-over 混合需要改造 `ink_px`（连带会改变现有描边像素，
动 293 px 冻结基线）。因此：

- `alpha == 255`（默认路径与所有冻结基线）→ 一个像素都不变
- `alpha < 255` → 视觉上是淡出，但**不是**物理正确的 alpha 合成；文档不掩饰这一点
- 单元测试 `tests/test_textprims.c`（**29 项**）断言：正文按版面画、无合成框、
  未绑定的 8/9 不画、绑定后画在图元位置（含父链）、`alpha=128` 比 `alpha=255` 暗、
  `draw=0` 不画、`tw<0` 不算绑定、连渲两次结果相同、旧路径可随时切回

### 1.4 parts（立绘表情差分）

`PartsLoad(partsId 0..0x3F, name)`：载入一张 **fmt=2 的多帧图**（`0x4294a0` 处 `cmpw $0x2` 校验），
把 44 字节 hzc1 头的 `NVSG` 起 36 字节拷进 parts 结构。字段（相对 parts 结构）：
`+0x00` 目标图形槽、`+0x02` 当前帧号、`+0x14/+0x16` = W/H、`+0x18/+0x1a` = 图像头 x/y、
`+0x20` = 帧数、`+0x04` = 像素缓冲。

`PartsAssign(partsId, graphSlot)` 写 `+0x00`；`PartsSelect(partsId, frame)`（`0x429700`）把该帧
**按 parts 头 x/y 直接 blit 进目标 graph 的纹理**（不是独立图元）。

实例（真紅，`story_00528000`）：
`GraphLoad(1,'graph_bs/CHR_真紅_基_白衣制服')` → 1366×1905 的站立图，头 (884,709)；
`PartsLoad(1,'graph_bs/CHR_真紅_基_白衣制服_表情')` → 227×181×34 帧，头 (508,161)；
`PartsAssign(1,1)` + `PartsSelect(1,13)` → 表情第 13 帧贴到 body 图的 (508,161)，
正好落在这张站立图的脸部位置（已出图确认）。

---

## 2. 坐标与合成规则

### 2.1 位置 = 图头 + prim 偏移

命中测试函数 `0x43b5f0` 里算矩形时是：

```
x = graph[slot].x(0x514) + prim.x(0x14) + 额外的组偏移
y = graph[slot].y(0x516) + prim.y(0x16) + 额外的组偏移
```

即 **图头 x/y 是这张图的默认落点，prim.x/y 是脚本给的偏移**，两者相加。
`PrimSetXY` 的参数允许是 `true`（type=1）表示「保持原值」——脚本大量这样写，
所以背景/立绘的默认位置其实来自图头；`PrimSetXY(186, 1, 84)` 这种才是真的把 y 挪 84。

### 2.2 三类素材用三种锚点（由「图比画布大」的两种情况区分）

合成时按图的长宽比选锚点，这是出图试出来的、能自洽的唯一组合：

| 类 | 判据 | 落点 | 例子 |
|---|---|---|---|
| 背景/事件 CG | 横图且 `w>1920*1.15` 或 `h>1080*1.15` | **先居中**：`(1920-w)/2 + 图头x + prim.x`，`(1080-h)/2 + 图头y + prim.y`，1:1 画，超出画布裁掉 | `BG043_000` 2400×1350 头(0,0)，`prim.y=84` → 落点 (-240, -51)，满屏 |
| 立绘 | 竖图（`h > w`） | **底边中点锚**：`(图头x+prim.x - w*scale/2, 图头y+prim.y - h*scale)`，`scale = prim.scaleX/1000` | 真紅 1366×1905 头(884,709)，`prim.y=464`，`scale=0.6` → 落点 (474, 30)，头顶在上、脚出画 |
| UI / 表情补丁 | 其余 | 左上角直贴 `图头x+prim.x, 图头y+prim.y` | `main_window_mask` 1920×229 头(0,851) → 正好贴底；`fc_*` 功能图标都在右下角（已出图） |

`PrimSetWH/UV` 一旦被真正写过（标志 bit0=1），W/H 就是目标尺寸、U/V 是源图内偏移；
否则用整张图。

### 2.3 缩放、alpha、旋转

* 缩放：`prim.scaleX/Y / 1000`（`PrimSetRS(prim, angle, scale)` 同时写 X/Y）。
  立绘缩放围绕锚点做，尺寸先乘 scale 再落位。
* alpha：`prim.alpha/255` 乘到素材的 alpha 通道。
 * 旋转：`prim.angle` **渲染 1 单位 = 0.05°**（7200 单位 = 360°；exe 0x42B1E2 把
   单位角 `/3600·π` 交给 D3DXMatrixRotationZ，见 `ref/exe-rs-transform.json`）。
   `PrimSetRS2` 还能分别给 X/Y 缩放。渲染层目前只对 UI 类做旋转。
* blend：`prim.blend=1` 时按加色近似（`_paste_clipped(additive=True)`）。
* Z：`prim.z` 已经解析出来，但**引擎的绘制顺序不是按 Z 排序**，而是按 group 树深度优先
  （`0x438ab0` 的类型 1 分支：从 `prim+0x2a` 首子开始，沿 `prim+0x10` 兄弟链递归；
  画面上层 = 树里靠后的分支）。渲染层照这个顺序画，Z 只记录不排序。

### 2.4 绘制顺序

```
walk(prim 0):
    if draw == 0: return                       # 整棵子树隐藏
    if type == 1 (group): 按 children 顺序递归（= PrimGroupIn 的插入顺序）
    if type == 4 (sprite): 有图且 alpha>0 就画
    if type == 5 (text)  : 交给文字绘制
再补画所有「不在任何 group 里」的 sprite（按 id 升序）
```

group 树就是层次结构本身：`PrimGroupIn(child, parent)` 时 **parent 被初始化成 type=1**（`sub_439bf0(parent,1)`），
child 挂到 parent 的子链表尾；`PrimGroupOut(id)` 只摘掉 id 自己（`0x437290`）。

这条规则可以直接解释背景/立绘的前后关系：
`PrimGroupIn(8,770)`、`PrimGroupIn(7,770)` → 组 8（背景）先于组 7（立绘）绘制。

---

## 3. 命令 → 属性 对照表

id/argc 以 `fvp/work/cmd_table.json`（= hcb 自带命令表，也是 `vm_id_handler.json` 的同一份）为准。
下表只列渲染层真正处理的命令；其余命令落到 fvm 的默认实现或空实现。

### 3.1 图元属性

| id | 命令 | argc | 作出的修改 | handler |
|---:|---|---:|---|---|
| 77 | PrimSetAlpha | 2 | `(id, a)`：+0x07 = a（钳 0..255） | 0x437990 |
| 78 | PrimSetBlend | 2 | `(id, b)`：+0x08 = b（只接受 int 0/1） | 0x43ba20 |
| 79 | PrimSetDraw | 2 | `(id, d)`：+0x05 = d（0/1） | 0x4379d0 |
| 80 | PrimSetNull | 1 | 重置该 prim（type→0、draw→1、alpha→255、z→1000、scale→1000、graph→-1），**保留 group 关系** | 0x43ba60 → `sub_439bf0(id,0)` |
| 81 | PrimSetOP | 3 | `(id, a, b)`：+0x20/+0x22（仅 type==4 生效） | 0x43ba90 |
| 82 | PrimSetRS | 3 | `(id, angle, scale)`：+0x24 = angle%3600，+0x26=+0x28=scale | 0x43baf0 |
| 83 | PrimSetRS2 | 4 | `(id, angle, sx, sy)` | 0x43bb80 |
| 85 | PrimSetSprt | 4 | `(id, graphSlot, x?, y?)`：type=4，+0x2e=graphSlot，x/y/uv/wh/z 归零，alpha=255，scale=1000 | 0x43bd00 |
| 86 | PrimSetText | 4 | `(id, twIdx, x?, y?)`：type=5，+0x32=twIdx | 0x43bdc0 |
| 87 | PrimSetTile | 6 | `(id, ...)`：type=2，+0x30=tile | 0x43be40 |
| 88 | PrimSetUV | 3 | `(id, u, v)`：+0x1c/+0x1e（要求 type==4） | 0x43bef0 |
| 89 | PrimSetWH | 3 | `(id, w, h)`：+0x18/+0x1a | 0x43bf50 |
| 90 | PrimSetXY | 3 | `(id, x, y)`：+0x14/+0x16 | 0x43bfa0 |
| 91 | PrimSetZ | 2 | `(id, z)`：+0x12，钳 100..10000 | 0x43bff0 |

> `PrimSetXY/WH/UV/Z/RS` 的参数允许 bool（type=1）表示「不改这个分量」：
> 脚本里 `PrimSetXY(186, 1, 84)` 只改 y，`PrimSetXY(100, 1, 1)` 是纯 no-op。

### 3.2 组 / 生命周期

| id | 命令 | argc | 语义 | handler |
|---:|---|---:|---|---|
| 72 | PrimExitGroup | 1 | 设置「当前退出组根」`engine+0x651d64`（渲染入口会额外画这棵子树） | 0x43b960 |
| 73 | PrimGroupIn | 2 | `(child, parent)`：parent 变 type=1，child 挂到 parent 子链表尾 | 0x43d6d0 → 0x43d640 |
| 74 | PrimGroupMove | 2 | 组移动（本作用到 0 次，未实现） | 0x43b980 |
| 75 | PrimGroupOut | 1 | `(id)`：把自己从父组摘掉 | 0x437960 → 0x437290 |
| 76 | PrimHit | 2 | `(id, flag)` → bool：光标是否点在 prim 矩形内 | 0x43b9c0 |

### 3.3 图形 / 部件 / 文字 / 其它

| id | 命令 | argc | 语义 |
|---:|---|---:|---|
| 7 | ColorSet | 5 | `(idx, R, G, B, A)` → 调色板 `engine+0x6a44c0`（渲染层只记录，未用于取色） |
| 14 | Dissolve | 7 | 屏幕溶解（时间/遮罩/矩形），未实现 |
| 15 | DissolveWait | 1 | 溶解是否结束 → 返回 1（视为已结束，见 §9） |
| 20 | FloatToInt | 1 | float → int（脚本用它把 1.875×/1.6875× 的坐标换算成像素） |
| 22 | GraphLoad | 2 | `(slot, name|null)`，见 §1.2 |
| 23 | GraphRGB | 4 | 颜色调制，未实现 |
| 42 | MotionAlpha | 6 | `(id, from, to, ms, mode, flag)`：渲染层**直接取 to** |
| 45 | MotionAnim | 4 | `(id, animId, ms, loop)`：渲染层把 graph 的帧号设成 animId |
| 48 | MotionMove | 8 | `(id, x, y, z, ?, ms, mode, flag)`：直接写 prim 的 x/y/z |
| 49/51/54/57 | MotionMoveR / MoveS2 / MoveZ | 6/8/6 | 直接写位置/缩放/Z |
| 50/53/56/59/44/47 | *Test | 1 | 「动画是否还在跑」→ 一律返回 1（= 已结束，见 §9） |
| 64 | PartsAssign | 2 | `(partsId, graphSlot)` |
| 65 | PartsLoad | 2 | `(partsId, name|null)` |
| 66 | PartsMotion | 3 | 部件动画，未实现 |
| 71 | PartsSelect | 2 | `(partsId, frame)`：把该帧贴进 graph |
| 111 | TextClear | 1 | 清空窗口 |
| 112 | TextColor | 4 | `(tw, R, G, B)` |
| 118/119/120 | TextFormat / TextFunction / TextOutSize | 7/4/3 | 排版参数（渲染层只记 out_size）。**120 的语义已更正**：不是宽高，是 `tw+0x595`(0..12) / `tw+0x596`(0..8) 两个装饰分量，且两作实际传的 (64,32)/(64,1) 全部越界 → 空操作，见 `docs/vm-commands.md` |
| 121–131 | TextPause/TextPos/TextPrint/TextRepaint/TextShadowDist/TextSize/TextSkip/TextSpace/TextSpeed/TextSuspendChr | — | 见 §1.3 |
| 123 | TextPrint | 2 | `(tw, text)`：`[读音|正文]` 会先被解析掉只留正文 |
| 132/133/134/135/136/137 | Thread* | — | 见 §9；协程槽默认启用、对本作行为中立，见 [`docs/05-threads.md`](05-threads.md) §10 |

---

## 4. 图像资源：从命令到像素

1. 脚本里的资源名带分类前缀：`graph/…`、`graph_bg/…`、`graph_bs/…`、`graph_vis*/…`、`voice/…`、`bgm/…`。
2. **6 个图像包之间零重名**（2653 个名字，跨包重复 0）——所以 `render_assets.Archives` 建一张
   `名字 → (包, 偏移, 长度)` 全局表即可；查找时先按原样查，再按「去掉/替换前缀」回退
   （实测 `graph/BG063_000` 实际住在 **graph_bg.bin** 里，所以这个回退是必需的）。
3. 命中的条目按需从 `.bin` 里 `seek+read` 出单条 hzc1 字节流，落盘缓存到
   `fvp/out/render/assets/<包名>/<名字>`，再交给 **`hzc1.py` 原样解码**（它只接受路径）。
   首次运行会抓几十张图，之后复用缓存。游戏目录只读。
4. `Graph.image()` 返回「基准帧 + parts 补丁」的合成图；多帧图（fmt=2）按 `MotionAnim` 的
   `animId` 选帧。

---

## 5. 已验证的出图

`fvp/out/render/` 下两批（每批都有 manifest 记录 step / 绘制清单 / 文本）：

**A. 默认 12 帧（`manifest.json`，0–900k 步均匀采样）**

| 帧 | 步数 | 画面 |
|---|---:|---|
| `frame_00075000.png` | 75,000 | 开场：蓝天/风车背景 `BG063_000` + 特效 `EF_e08_B` + 标题层「空」 |
| `frame_00150000.png` | 150,000 | 同上（特效层变化） |
| `frame_00225000.png` | 225,000 | 标题/menu 流程，人名层「真紅／悠馬」 |
| `frame_00375000.png` | 375,000 | 主人公独白段，人名「悠馬」 |
| `frame_00525000.png` | 525,000 | **天台场景**：`BG043_000` + 章节标题「学校・屋上」+ 人名「真紅／悠馬」 |
| `frame_00675000.png` | 675,000 | 天台场景，人名「悠馬」 |
| `frame_00750000.png` | 750,000 | 天台场景，人名「澪」 |
| `frame_00900000.png` | 900,000 | 天台场景，人名「澪」 |

**B. 精选故事帧（`manifest_story.json`，`--hold-text 30000`）**

| 帧 | 步数 | 画面 |
|---|---:|---|
| `story_00513600.png` | 513,600 | 天台 + 章节标题 + 人名「悠馬」 |
| `story_00523500.png` | 523,500 | 天台 + 对话框 +「……おっと。人が来たみたいだな。私は消えてるよ」 |
| **`story_00528000.png`** | **528,000** | **天台 + 立绘（真紅）+ 对话框 + 人名「真紅／悠馬」** ← 任务书要的那张 |
| **`story_00534000.png`** | **534,000** | 同上（人名换成「悠馬」，立绘同框） |
| `story_00542500.png` | 542,500 | 天台 +「気持ちの問題なんだ、こういうのは」 |
| `story_00558000.png` | 558,000 | 天台 +「そう。気持ちの問題」 |
| `story_00566000.png` | 566,000 | 天台 +「誰かが近くにいるのに、その人に自分のことを全く見てもらえない」 |
| `story_00592000.png` | 592,000 | 「真紅は空の青に溶けて消える。」（立绘 alpha 已到 0，符合剧情） |

脚本位置对照：`iroseka.hcb` 里天台场景的建立点在 `0x049a20`（`GraphLoad(1,'graph_bs/CHR_真紅_基_白衣制服')`）
与 `0x0234d3`（`PrimSetNull`+`GraphLoad` 组合），背景是 `0x0234e0` 处的 `GraphLoad(186,'graph/BG…')`。

---

## 6. 与 fvm 的两个关键差异（否则会跑偏）

1. **argc 必须用 `cmd_table.json`，不能用 `cmd_argc.json`。**
   `cmd_argc.json` 有 6 条是「exe 注册下标」推导出来的错值：
   `49 MotionMoveStop(1 vs 6)`、`51 MotionMoveR(6 vs 1)`、`52 MotionMoveRStop(1 vs 8)`、
   `54 MotionMoveS2(8 vs 1)`、`94 SaveThumbSize(2 vs 3)`、`95 SaveData(3 vs 2)`。
   用错值 → syscall 多弹/少弹 5~7 个值，值栈立刻失去平衡（`render_stackcheck.py` 实测：
   用 `cmd_argc.json` 时 60 万步内出现 16 次下溢，用 `cmd_table.json` 时 **0 次**）。
   渲染层所有参数都因此可信。
2. **`*Test` 类命令必须返回 1。** 脚本的等待循环是
   `pushint16 252; syscall 44 (MotionAlphaTest); pushtemp; pushfalse; eq; jmpcond ...`
   —— 返回 1 时跳出等待、返回 0 时死循环（实测返回 0 会一直卡在 `0x5843b` 的 poll 循环，
   90 万步都进不了正片）。`ThreadExit` 也不能无条件结束主线程：C 版里主线程不在 32 个协程槽里，
   `ThreadExit` 只收槽里的子线程（[`docs/05-threads.md`](05-threads.md) §10）。

---

## 7. 已知偏差 / 未实现的语义

1. **Motion* 立即生效**：动画不做补间，直接取终点值（alpha 取 to、坐标直接写）。
   因此淡入淡出过程看不到，某些"人物正在淡出"的帧会直接显示成 alpha 0。
2. **协程默认开启，但对本作行为中立**（`--no-threads` 可关）。2026-11-24 实测：900k / 2M 两档
   开关两种模式下文本**逐字节相同**，因为这份脚本的每个协程入口都是
   `initstack; pushint8 0; jmpcond <ret>`（条件是编译期常量 0），协程建起来就退出。
   所以「协程里拼字符串导致约 50 句台词缺失」这个旧结论**不成立**，已按实测改正；
   证据与 A/B 表见 [`docs/05-threads.md`](05-threads.md) §10。
   `--hold-text` 只是快照技巧：某层为空时重画它上一句真实文本，不代表引擎行为。
3. **对话框是画出来的**：引擎的文本框/窗口贴图没有结构分析（text window 对象 `tw` 只确认了坐标/字号/颜色字段），
   渲染层用「浅色圆角框 + 人名小框 + 字号 34」近似；层 9 居中、层 0 左对齐首行缩进、
   折行按文本框宽度自己算（脚本不给断点）。注音 `[a|b]` 只保留正文 b，**不画上方小字**。
4. **背景用「居中 + 图头/prim 偏移」1:1 贴**：这是让画面满屏且与 `prim.y=84` 自洽的最简规则，
   但没有从渲染端静态分析确认（引擎里有相机/视差机制，`Dissolve`/`MotionMove` 会改）。
5. **立绘用「底边中点」锚点**：同上，是出图调出来的规则；不是从 `0x42ae40` 里读出的唯一解
   （那段代码在做纹理 UV 与缩放矩阵，锚点枚举没完全逆完）。
6. **Z 不参与排序**（按 group 树序）；`PrimSetOP` 写进去的 +0x20/+0x22 是**缩放/旋转轴心**（2026-11-25 解出：exe 0x42AF08 的 OP 分支取局部坐标 (图头 − OP)，0x42B41F..0x42B477 的平移是 prim + OP；本站实现在 `gfx_draw_image_op_xform()`，见 docs/opening.md §3），`PrimSetBlend=1` 用加色近似。
7. 未实现：`Dissolve`(14)、`GraphRGB`(23)、`PartsMotion`(66)、`V3D*`、`Movie`、`Snow*`、
   `TextFont/TextFormat` 的真实排版、`SaveThumbSize` 缩略图。
8. `graph-not-found`：`taiken_start`、`title_trial`、部分 `SHINKU_e01*`、`graph/BG062_500(-b)`
   确实不在 6 个图像包里（多半在 `graph_vis*` 之外的包或运行时合成），属资源缺失而非解析错误。

### 7.1 `TextShadowDist`(125) / `TextSpace`(128)：只记录，不画（2026-11-24 两次修正）

**现象**：外壳实跑时，对话框里的文字看起来有「残影」——每个字右边都拖着一份深色重影
（用户实测报告）。抓帧放大 4 倍后确认是**一份向右偏移的实心深色文字副本**。

**第一层根因（已修）**：把 `TextShadowDist` 当成"两个分量的偏移"直接拿去渲染了
（`draw_text_window()` 里在 `(x+dx, y+dy)` 先用 `(16,20,30)` alpha=255 画一遍再画正文）。
撤掉绘制后用户确认「现在没有了」。

**第二层根因（本轮修的）**：连语义都读错了。静态分析逐条核对后：

| id | 命令 | handler | 语义 |
|---:|---|---|---|
| 125 | `TextShadowDist` | **0x43CBB0** | `(twIdx, dist)`：dist **0..12**（<0 当 0、>12 当 12），转 `0x4248B0` → 只写 `tw+0x597` 一字节 |
| 128 | `TextSpace` | **0x43CE10** | `(twIdx, x, y)`：两个分量各 **-32..32**，分别写 `tw+0x5A8` / `tw+0x5AA`（i16），超范围的分量单独跳过 |

以前把 **0x43CE10 当成了 TextShadowDist**，于是：
- 125 被实现成"两个 -32..32 的分量"（那其实是 128 的语义）；
- 128 一直没有 case，一直被算作"未实现"；
- 而脚本对 125 传的是 **`dist=12`**（阴影距离拉满），被我们当成 `dx=12, dy=0` 画成了向右 12 px 的重影。

**实测（900k 步，`fvm --dump-textattr`）**：

```
tw  0: mask=0x1f color=10,51,100 size=64x32 speed=-1 ms shadow=12 space=0,0  skip=3
tw  8: mask=0x0f color=10,51,100 size=64x8  speed=1  ms shadow=12 space=0,0  skip=0
tw  9: mask=0x2f color=17,18,100 size=64x8  speed=1  ms shadow=12 space=32,1 skip=0
tw 10: mask=0x07 color=10,1,1    size=64x8  speed=0  ms shadow=0  space=0,0  skip=0
```

- 三条命令都被脚本用到：**阴影距离 12**、**tw9（章节标题）字间距 32 / 行间距 1**、skip 3
- `tw9` 的 `space=32,1` 正是"章节标题字拉得很开"的来源 —— 说明这套字段解释是对的

**为什么两条都只记录、不渲染**：
- 阴影：只知道**距离**（0..12），**形状/颜色/透明度/合成方式**都没有静态分析证据
  （`0x4248B0` 只把值存进 `tw+0x597`，读取端还没逆出来）；
- 间距：`0x43CE10` 只写两个 i16，排版端怎么用（加在字宽上？行高上？缩放前还是后？）也没有证据；
- 我们自己的 Python 参考实现 `work/render.py` 里，`TextWindow.shadow` 与 `space`
  都是**只存不用**（`@h('TextShadowDist')` 只做 `self.tw(a[0]).shadow = _int(a[1])`，
  绘制代码完全没引用它们）；
- 所以"画出来"只会让帧偏离参考实现、也与实测观感不符。**只记录数值**是唯一同时满足
  「忠实记录命令语义」和「不伪造像素」的做法。

**改动**：
- `FvpTextAttr` 拆成 `shadow_dist`(0..12) / `space_x` / `space_y`(-32..32)，掩码新增
  `FVP_TEXTATTR_SPACE`；
- `syscall.c` 的 `case 125` 改成单参数钳制，新增 `case 128`（逐分量判定）；
- `gfx_text_set_shadow(g, layer, dist)` / 新增 `gfx_text_set_space(g, layer, x, y)`，
  两者都只存不画；`draw_text_window()` 里不再有任何阴影层；
- `test_textattr` 扩到 **56 项**（含 125 的钳制语义、128 的逐分量跳过）。

**验证**：`verify_gfx.ps1 -SkipDump` 仍是 **293 px / PSNR 49.94 dB / SSIM 0.99971 / 24/24 图元
0 px**（528k 帧上 `shadow=12` 但 `space=0,0`、且两者都不参与绘制，所以像素不变）；
外壳 45 s 的 `unimpl cmds` 从 20 种降到 **18 种**（128 消失）。

> 这个 bug 静态门禁抓不到：它只在 `TextShadowDist` 被调用**并且**我们去画它时才会显形，
> 528k 帧的像素对比看不到"多画了一层"。是用户实跑抓到的 —— 记在这里当经验。

### 7.2 `TextFont`(113)：只记录，不换字模

`TextFont(twIdx, fontId, fontId2)` 是脚本指定字体的入口，handler **0x43DA70**：

- arg0 必须是 int 0..0x1F（twIdx）
- arg1（int，**-5 <= v <= 引擎字体列表长度-1**）→ `0x4247F0` 写 `tw+0x590`（**u16**）
- arg2（同一区间）→ `0x424860` 写 `tw+0x593`（**u8**）
- 两个分量各自独立判定，超范围的那一个跳过（两个 if 互不影响）

**实测（900k 步，两部作品）**：形态固定为

| 窗口 | 值 | 用途（按窗口号推） |
|---|---|---|
| 0 | `(-1, 1)` | 正文（124 次/900k） |
| 8 | `(-1, 1)` | 人名 |
| 9 | **`(-2, 1)`** | 章节标题 |
| 10 | `(-1, 1)` | 其它 |

也就是**字体号是负数**（-1 / -2）—— 原版用它区分"内置字模风格"，而 `-5` 那个下界正是
给这类特殊值留的。第二字段恒为 1。

**为什么只记录不换字模**：负数字体号对应哪套字模没有任何证据（我们只有 4 个从游戏包
里解出来的字模图集，且图集与 `-1/-2` 的映射无从对应）；`tw+0x5A8` 那类"间距"字段同理。
`work/render.py` 也只存不画。所以渲染层仍按**字号**挑字集 —— 这也是像素基线能保持
不变的原因。

**改动**：`FvpTextAttr.font_id` / `font_id2`（掩码 `FVP_TEXTATTR_FONT`）、
`fvp_textattr_set_font()`（`-5..FVP_TEXT_FONT_MAX`，逐分量）、`gfx_text_set_font()`
（记录到文本窗口）、`fvm --dump-textattr` 打印 `font=id,id2`。

**验证**：`test_textattr` **65 项**；外壳 45 s 的 `unimpl cmds` 从 18 种降到 **17 种**
（113 消失）；`verify_gfx` 像素不变（不参与绘制）。

### 7.3 字形兜底：图集缺字不再画豆腐块（2026-11-24）

**现象**：窗口正中会出现一个"白边黑方块"（用户问"中间那个方块是什么"）。那是
`draw_tofu()` 画的缺字占位符。

**根因**：我们的位图字集是**按实际用到的字抠出来的**，覆盖面很窄：

```
gfx-font-32.bin  size=32  228 glyph
gfx-font-34.bin  size=34  228 glyph   （＝序章 195 句台词用到的字）
gfx-font-56.bin  size=56    5 glyph   （・上学屋校）
```

> **2026-11-24 更新（覆盖面已修）**：现在用 `scripts/make-font-atlas.py` 生成
> 「两作脚本里所有 `pushstring` 字符串的并集」图集，并补齐渲染端真正会请求的
> **四档字号 30 / 32 / 34 / 56**（`gfx_layout_default()` 的默认层字号是 30，
> 之前漏了这一档 → 该层整层退回内置 8×16 字体 → 该层日文全是豆腐块）。实测：
>
> | 作品 | 改前 | 改后 |
> |---|---:|---:|
> | 五彩斑斓的世界 HD | 1 tofu | **0 tofu** |
> | 星空的记忆 HD | 19 tofu | **0 tofu** |
>
> 图集每个 2764 glyph、约 12–34 MB，放在 `bin\`（gitignore，不进仓库）；
> 生成命令见 `docs/win32-shell.md` §5 第 2 条。跨图集兜底（下面 1..3 条）仍然保留，
> 只是正常情况下不再触发。

而章节标题层（`tw9`）的排版字号是 **56** → 只能选那张 5 字图集，标题正文「空」不在里面
→ 豆腐块。原版靠 `TextFont(9, -2, 1)` 选字体，而 `-2` 对应哪套字模没有证据（§7.2），
所以我们不能"猜字体"，只能把**取字**做得更稳。

**修法（跨图集兜底）**：
1. 目标图集有 → 正常画（不变）
2. 目标图集没有 → 在其它已注册图集里找同一个码点（优先**字号最接近**的），
   把那个字形按 `目标字号 / 源字号` **最近邻缩放**画上去；推进量同比例缩放，
   所以排版/居中仍然对（`font_fallback()` / `draw_glyph_scaled()` / `char_advance()`）
3. 所有图集都没有 → 才画豆腐块，并记数

**新增诊断计数**（`GfxStats`）：`fallback_glyphs`（本次 render 走了兜底的字形数）、
`tofu_glyphs`（豆腐块数），每次 `gfx_render()` 归零。外壳汇总里打印：

```
glyphs         0 tofu, 1 from fallback atlas
```

**验证**：
- 实测 8 s 抓帧：章节标题「空」从 34px 图集缩放画出来（`out/title-zoom.png`），
  外壳汇总 `0 tofu, 1 from fallback atlas`
- 单测 `tests/test_glyph_fallback.c` **16 项**：造两个临时 GFXF 图集（34px 只有 'B'、
  16px 只有 'A'），验证「命中不兜底 / 缺字走兜底且不画豆腐块 / 都缺才豆腐块 /
  计数每次 render 归零」
- `verify_gfx` 仍是 **293 px / PSNR 49.94 / SSIM 0.99971**：528k 那一帧的 `tw9` 没有文字，
  所以像素不变（这个 bug 同样只有跑到标题出现时才显形）

### 7.4 `TextFormat`(118) / `TextFunction`(119)：只记录

两条命令都是"把一串小整数写进文本窗口的固定偏移"，handler 逐字段判范围：

| 命令 | handler | 参数 → 字段（范围） |
|---|---|---|
| `TextFormat`(118) | **0x43CC10** | a→`tw+0x5A8`(-32..32)、b→`tw+0x5AA`(-32..32)、c→`tw+0x5AE`(0..64)、d→`tw+0x5AC`(0..64，且把 `tw+0x5E8` 抬到 ≥d)、e→`tw+0x5B4`(-16..16)、f→`tw+0x5B0`(-16..16) |
| `TextFunction`(119) | **0x43CCF0** | a→`tw+0x5A5`(0..1)、b→`tw+0x5A4`(0..2)、c→`tw+0x5A6`(0..2) |

**实测（900k 步，`fvm --dump-textattr`）**：

```
tw  0: ... space=32,32 ... fmt=64,64,16,16,32,32 func=1,2,2
tw  8: ... space=32,1  ... fmt=1,1,1,1,32,1     func=0,0,0
tw  9: ... space=32,1  ... fmt=0,0,0,0,32,1     func=0,0,0
tw 10: ... space=0,0   ... fmt=0,0,0,0,0,0      func=0,0,0
```

**一个有用的交叉验证**：`TextFormat` 的 a/b 两个参数写的正是 `TextSpace` 的两个字段
（`tw+0x5A8` / `tw+0x5AA`）。tw8 的 `space=32,1` 既可能来自 `TextFormat(8,32,1,...)`
也可能来自 `TextSpace`，而 tw9 只收到过 `TextSpace(9,32,1)`（所以 `fmt` 全 0 但
`space=32,1`）—— 与实际 dump 完全吻合，说明这两条命令的字段对应没读错。

**为什么只记录不实现**：这九个值（排版边距/间距/开关）在原版排版端怎么消费没有证据；
`work/render.py` 里这些字段也只有声明、没有参与绘制。渲染层的排版仍是 §1.3 那套近似。

**改动**：`FvpTextAttr` 新增 `fmt_pad_a/fmt_pad_b/fmt_off_a/fmt_off_b` 与
`func_a/func_b/func_c`（掩码 `FVP_TEXTATTR_FORMAT` / `FVP_TEXTATTR_FUNCTION`），
`fvp_textattr_set_format()` 六字段逐字段判范围、`fvp_textattr_set_function()` 三开关；
gfx 侧 `gfx_text_set_format()` / `gfx_text_set_function()` 只存不画。
`test_textattr` **65 → 82 项**。

---

## 8. 命中测试（`PrimHit` 76）：已落地（矩形层）

`PrimHit(primId 1..0xFFF, flag)` 是脚本判断"鼠标点没点在这个图元上"的唯一入口
（90 s / 480,942 步外壳实测调用 **110 次**；528k 步 render_vm 实测 **132 次**）。
静态分析已经把算法完整解出来了，**2026-11-24 已按下面三层落地**：实现在
`src/gfx.c` 的 `gfx_prim_hit()`，入口在 `src/syscall.c` 的 `case 76`，
单测 `tests/test_prim_hit.c`（47 项）。

**第一层 `0x43B9C0`（handler）**
- `arg0` 必须是整型、值 **1..0xFFF**（注意 0 非法）；`arg1` 只用来算一个布尔
  `flag = (arg1.type != 0)`，然后 `0x43B8D0(primId, flag)`
- 命中则把返回值类型置 1（bool）—— 值本身由下层写

**第二层 `0x43B8D0`（沿父链累加偏移）**
1. `p = prim[primId].parent`（`prim+0x0A`）；若 `p == 0xFFFF` → **直接返回 0**
   （没被 `PrimGroupIn` 放进任何组的图元不可命中）
2. 若 `flag == 0` 且 `prim[primId].draw(+0x05) == 0` → 返回 0（隐藏的图元不可命中）
3. 沿 `parent` 往上走，每层累加 `x/y`（`+0x14` / `+0x16`）；**任何祖先的 `draw == 0`
   → 返回 0**（父组隐藏则整棵子树不可命中）
4. 链必须终止在 **prim 0（根）**；若先走到 `0xFFFF` 而没经过根 → 返回 0
5. 把 `(primId, 累加 x, 累加 y)` 交给 `0x43B5F0`

**第三层 `0x43B5F0`（矩形判定）**
- 按 `prim+0x04` 的类型分派：**1（组）/ 2（tile）/ 4（sprite）** 才可能命中，
  其它类型（含 5 = 文本）直接返回 0
- sprite 分支：`x0 = graph[slot].hdr_x(+0x514) + prim.x + 累加x`，
  `y0 = graph[slot].hdr_y(+0x516) + prim.y + 累加y`
  - `prim+0x06` 的 **bit0（WH 已被 `PrimSetWH` 写过）** 为 1 时
    `w = min(prim.w, graph.w)`、`h = min(prim.h, graph.h)`，否则用 `graph` 的整幅尺寸
- tile 分支（`0x43B842`）：`prim+0x07`（alpha）为 0 直接返回 0；否则矩形 =
  `(prim.x + 累加x, prim.y + 累加y, prim.w, prim.h)`
- 组分支（`0x43B891`）：从 `prim+0x2A`（首子）沿 `prim+0x10`（兄弟）遍历，递归调用
  本函数，任一子命中即命中。**注意它不额外加组自己的 x/y** —— 这是原版行为，
  已在单测里固定住
- 命中条件 `x0 <= cx < x0+w` 且 `y0 <= cy < y0+h`（**左闭右开**，`cx/cy` = 全局光标）
- sprite 命中后还要过 UV 检查：`u = prim.u + (cx - x0)`、`v = prim.v + (cy - y0)`，
  必须仍在图内（`0x43B720`）

**唯一没落地的部分**
- 矩形+UV 之后的按行像素循环（`0x43B750` 与 `0x4259B0`）看上去是**逐像素掩码/透明
  测试**（只点不透明像素才算中）。那一段没解完，所以现在**矩形内即算命中** ——
  这是 `gfx_prim_hit()` 唯一的近似，`gfx.h` 的注释里也写着。要落地需要先解
  `graph+0x488` / `graph+0x4C8` 那两个数组的语义。

**实测：这两部作品里 `PrimHit` 一直是 0，而且原版也应该是 0**

`--dump-prims` 把每个图元的字段 dump 出来后一目了然。两部作品都把命中测试
**只**用在 11 个固定 id 上（`52, 55, 58, … 82`，步长 3），而这些图元在本作前
200 万步里始终是：

```
52 4 1 -1 1 1 0 0 52 0      # type=4 draw=1 parent=-1 x=1 y=1 w=0 h=0 graph=52
```

即 **`parent == -1`（从没被 `PrimGroupIn` 放进组）、`graph == 自己的 id`（该 graph
槽从没 `GraphLoad` 过）、w/h 都是 0**。按第一层的规则 `parent == 0xFFFF → 返回 0`，
所以 528k 步 132 次调用、2M 步 407 次调用**全部 0 命中** —— 与原版一致，不是移植
缺陷。这也是"接上 `PrimHit` 之后 `verify_gfx` 的 293 px / PSNR 49.94 / SSIM 0.99971
一分不变"的原因：画面路径根本没被这条命令影响。

（这 11 个图元应该是脚本给"选择支/菜单"预留的槽位，本作跑到 2M 步还没走到需要
显示它们的分支。）

**落地时踩到的坑**
- 命中用的原点是 **图头 `+0x514/+0x516` + prim 偏移**，不是 §2.2 那三种锚点算出来的
  绘制落点。对 UI 图元两者一致；对**立绘**（底边中点锚 + 缩放）两者不同 —— 也就是说
  原版对站立的立绘做命中测试时用的是"未缩放的左上角矩形"。照抄即可，别"修正"成绘制矩形。
- `wh_set` 得单独记：引擎是 `prim+0x06` 的 bit0，我们原来只在 `flags` 里没写过，
  所以 `GfxPrim` 新增了一个 `wh_set` 字段，由 `GFX_F_W` / `GFX_F_H` 置位。
- 环状父子关系（`A→B` 且 `B→A`）原版会无限循环，这里加了 4096 次上限，返回 0。

---

## 9. 调试工具速查

```bash
# 每个 syscall 的实参（含返回值、栈深度、线程数）
$PY fvp/work/render_probe.py --steps 900000 --click 4000 --out fvp/work/render_calls_900k.txt

# 值栈平衡检查（argc 是否正确）
$PY fvp/work/render_stackcheck.py --steps 600000

# HCB 静态分析（按字节偏移）
$PY fvp/work/render_scriptdump.py 0x58430 0x58480

# 命中测试审计：把每个非空图元的字段 + 光标处是否命中都 dump 出来
render_vm story.hcb --steps 528000 --no-png --cursor 960,540 --dump-prims out/c/vm-prims.txt

# exe 静态分析（按 VA）/ 按 VA 读 .rdata（vtable、RTTI）
$PY fvp/work/render_dis.py 0x43bfa0 0x43c060
$PY fvp/work/render_pe.py dwords 0x45d98c 8

# 图像包索引 / 单条查询
$PY fvp/work/render_assets.py "graph/BG063_000" "graph_bs/CHR_真紅_基_白衣制服"
```

`render.py` 里 `RenderVM.stats` 是「命令名 → 调用次数」，`graph_log`/`text_log` 记录每次
`GraphLoad`/`TextPrint`/`TextClear` 的步数与参数，排查"哪一帧该有什么"很方便。

## RS 分支（缩放/旋转）的变换 —— 结构分析自 exe（`ref/exe-rs-transform.json`）

`test byte [prim+0x6], 4`（RS/缩放脏位）成立时，exe 走 0x42B21E 起的另一条矩阵合成
（和"只有 OP"的分支 0x42AF08 不是同一条）：

* 四边形局部坐标 = **(图头 xy) − OP**（OP 在 `prim+0x20/+0x22`）；
* `T1 = (prim+0x14, prim+0x16) + 累加平移`；
* 矩阵顺序（D3D 行向量，M 从 I 起依次右乘）：`R`(0x446750) → `T1`(0x44674A) →
  `S`(0x446744) → `T2`(0x44674A)，每步之间 `Multiply`(0x446732)；
* **`T2 = (engine+0x60 / 2, engine+0x64 / 2)` = 画面中心**（0x42B343..0x42B37D：
  读 `[engine+0x64]`、`[engine+0x60]` 再算术右移 1 位）。

⇒ **`最终 = T2 + S·(R·(图头 − OP) + prim + accum)`**（**path A**，要求
`prim+6 & 0x4` 置位，即脚本对这条图元调过 `PrimSetRS/RS2`）。

只被 `MotionMoveS2` 改过 scale、没设 RS 标志的图元走 **path B**（`0x42B3A3`）：

`最终 = prim + 锚点 + S·R·(图头 − 锚点)`，锚点 = OP（没设 OP 时以图头为原点）。
五彩斑斓的 258 有 RS（path A，绕画面中心），259/260/261 没有（path B，绕
OP=(1200,675)）；星空没有 OP/RS ⇒ path B，图头 (0,0) ⇒ 左上锚缩放。

自洽特例：背景层（prim=0、R=I、OP=图心）在 s=1 时 `最终 = 画面中心 − OP =
(1920−2400)/2, (1080−1350)/2 = (−240,−135)`，正是本工程一直用的"超尺寸图居中裁剪
240/135"；s=0.8 → (0,0) 整屏铺满；s=0.9 → (−120,−67.5)。

**还没用上的一条**：`prim.xy ≠ 0` 的图元（例如开场 p1..p3）在这个分支里会多出
"画面中心 − OP" 的偏移。渲染器目前仍用 `prim + OP + R·S·(图头−OP)`，接这部分时
要单独验证，不能默认两者等价。


## 文本窗口的真实几何（2026-11-26，A 类：exe handler + 脚本实参）

以前 `gfx_layout_default()` 的 x/y/字号是**从参考实现的截图拟合表**来的。把 exe 的
文本系统读透之后，几何其实是四层：

1. **离屏面板**：`TextBuff(110)` → `0x430EA0` 给该层分配 `w*h*4` 的 RGBA 缓冲
   （tw+0x520）、把 `w/h` 写进 tw+0x5DC/+0x5DE，并建一个 surface（tw+4）。
   上限 0x400、缺省 8。**两款作品的正文层都是 `TextBuff(0,1024,1024)`**。
2. **字模格**：`TextSize(126)` → tw+0x592/+0x594（12..64 / 8..32）。
   正文层：`TextSize(0,64,true)` 设宽、`TextSize(0,64,32)` 设高 ⇒ 32 px 字模格。
3. **面板内光标**：`TextPos(122)` → tw+0x5E8/+0x5EA。**两款作品 0 次调用** ⇒ 恒 (0,0)。
4. **上屏**：只有 type-5 文本图元（`PrimSetText(86)` 的 prim，+0x32 存层号）会把面板
   贴到**图元的 x/y**（`0x438C54` 取面板 → `0x42AE40` 画）。正文图元实测
   `PrimSetText(551, tw=0, x=1, y=1)`，屏幕位置来自父链累加。

所以引擎**没有**"对话框版式表"这种东西：位置在图元上，尺寸在 TextBuff/TextSize 上。
我们现在的实现：图元驱动模式下用面板宽当换行宽度、面板高/行高当行数上限、
`TextSize` 的高当字号；未绑定图元的 0..7/10..31 仍按版面表兜底（原版未绑定就
不画，但那会让没跑完脚本的窗口空着，这里保留兜底并在 `docs/status.md` 记着）。
