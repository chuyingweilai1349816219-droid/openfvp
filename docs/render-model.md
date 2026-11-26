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
| 未绑定的**任何**窗口（0..31） | **不画**（默认路径不再用版面表兜底） | 2026-11-26 用户要求 #3：文本一律按图元绝对坐标画；版面表只留给旧路径冻结基线 |
| 未绑定的 8（人名）/ 9（章节标题） | **不画** | 实机里这两个覆盖层只在脚本给它们建了文本图元时才出现；否则会凭空多出一个「标题」 |
| 对话框 / 人名板 | **引擎不画**：框和名牌是脚本自己 `GraphLoad` 的图元 | 旧路径的合成框是我们自己的近似 |

旧路径（外壳 `--text-fixed-layout`）：引擎画合成对话框 + 32 个窗口全部按
`gfx_layout_default(layer)` 画一遍。**冻结像素基线（`verify_gfx` 289 px）就是旧路径冻结的**，
`tests/test_gfx.c` 里显式调用 `gfx_set_text_fixed_layout(g, 1)` 钉住它，所以基线不变。
汇总行 `text prims N drawn, M bound (…; mode …)` 给出这一帧用图元画了几个窗口、绑定了几个。

**图元 alpha 已经乘进文字（2026-11-24），但是近似实现。** `PrimSetAlpha` 写 `prim+0x07`，
原版用它做淡入淡出。我们的实现把 alpha 折进颜色通道（`c * alpha / 255`）再画——
原因是本引擎的字形覆盖是"一次性写屏"：`ink_px()` 在覆盖率 `cov == 255` 时直接写 RGB、
不参与 DST 混合，所以真正的 src-over 混合需要改造 `ink_px`（连带会改变现有描边像素，
动 289 px 冻结基线）。因此：

- `alpha == 255`（默认路径与所有冻结基线）→ 一个像素都不变
- `alpha < 255` → 视觉上是淡出，但**不是**物理正确的 alpha 合成；文档不掩饰这一点
- 单元测试 `tests/test_textprims.c`（**40 项**）断言：未绑定窗口一律不画（无版面兜底）、无合成框、
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
| 15 | DissolveWait | 视实参而定 | 溶解是否结束：arg 为 null → 返回槽写 1；arg 非 null（true 等）→ **不写返回槽**、只置线程等待位（exe 0x437520，2026-11-26 按反汇编改正；旧的'一律返回 1'是近似） |
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
   `pushint16 252; syscall 44 (MotionAlphaTest); pushtemp; pushtrue; eq; jmpcond ...`
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

**验证**：`verify_gfx.ps1 -SkipDump` 仍是 **289 px / PSNR 49.95 dB / SSIM 0.99971 / 24/24 图元
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
- `verify_gfx` 仍是 **289 px / PSNR 49.95 / SSIM 0.99971**：528k 那一帧的 `tw9` 没有文字，
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

> **2026-11-27 补**：上面这句对 `d`（`tw+0x5AC`）不成立 —— 它**有**消费点，已实现，
> 见 §7.4.1。其余字段（`0x5A8/0x5AA/0x5AE/0x5B4/0x5B0`）至今没有读点或没有可靠语义。

#### 7.4.1 `TextFormat` 的 `d` 就是行首缩进（0..64）

| 事实 | 出处 |
|---|---|
| `d` 写 `tw+0x5AC`（0..64），并且把 `tw+0x5E8`（笔位）抬到 ≥d | `0x43CC9B..0x43CCAE` |
| 排版起点就是 `pen_x := tw+0x5AC` | `0x4337A9` 读 `0x5AC` → `0x4337BE` 写 `0x5E8` |
| 笔位逐字推进（` += 字宽`） | `0x4247C4`、`0x4247DA` |
| 换行判据：`pen_x + 字宽 > tw+0x5DC - 字距` 就换行 | `0x433011` |
| 换行后笔位恢复为行首保存值 —— 缩进对**每一行**都成立 | `0x4330A8` |
| 排版器给的字符 x 也会写进笔位 | `0x432B02` |

实现：`src/gfx.c` 在算出行的起点（版面 / `TextPos` / 图元原点）之后加
`tw->fmt[3]`，即"从原点再往里推"。单测 `tests/test_textprims.c` 断言
`arg4=40` 时首个墨迹列正好右移 40 px、清零后回到原位。
冻结基线：`ref/exe-text-layout.json` + 门禁 `tests/verify_exe_text_layout.py`（8 个站点 +
`0x5A8/0x5AA` 死存储 + `0x5AC` 两个读点）。

**冻结的像素基线不受影响**（289 px / PSNR 49.95 / SSIM 0.99971 均未变）—— 那一帧上
`fmt[3]` 是 0，缩进不参与。

#### 7.4.2 剩下 8 个字段是死存储（写了没人读）

同一轮把 `TextFormat`/`TextFunction` 其余字段在整个 exe 里扫了一遍（判据只看指令形状：
源操作数里的 `0xOFF(%e..)` 算读，`%reg,0xOFF(%e..)` 算写，`add/sub/and/or/xor/inc/dec`
带内存目标算读改写）：

| 字段 | 来源 | 写点 | 读点 | 读改写 |
|---|---|---|---|---|
| `tw+0x5A4` / `+0x5A5` / `+0x5A6` | `TextFunction(119)` 三个开关 | 各 2 | **0** | 0 |
| `tw+0x5A8` / `+0x5AA` | `TextFormat` arg1/arg2（`TextSpace(128)` 也写） | 各 3 | **0** | 0 |
| `tw+0x5AE` / `+0x5B0` / `+0x5B4` | `TextFormat` arg3/arg6/arg5 | 各 2 | **0** | 0 |

结论：`TextFormat`/`TextFunction`/`TextSpace` 里**只有 arg4（`0x5AC`）被消费**，
其余全部是死存储 —— 我们"只记录"是忠实的，不是偷懒。

> 注意 `ref/exe-text-fields.json` 里 `0x5AE` 的 `covering_reads` 曾列出
> `0x430E67`/`0x4337A9`：那是**宽度推断的伪命中**（这两条是 `mov 0x5ac(%esi),%ax`，
> 2 字节，只覆盖 `0x5AC..0x5AD`）。本节的判据不看宽度推断，只按指令形状，所以
> `0x5AE` 记为 0 读点。冻结在 `ref/exe-text-layout.json` 的 `dead_fields`。

#### 7.4.3 `TextSkip`(127) 相反：它有消费者

`TextSkip` 写 `tw+0x5D7`（0..3），低两位各选**一组输入掩码**：

| 位 | 掩码 | 命中后 |
|---|---|---|
| bit0（`0x43108C` `test $0x1`） | `0x90`（bit4+bit7） | 与 `engine+0x43C` 求交（`0x4310A4`）→ 命中就把 `tw+0x5D8` 置 1（`0x4310AC`） |
| bit1（`0x431096` `test $0x2`） | `0x60`（bit5+bit6） | 同上，`or $0x60` |

`tw+0x5D8` 随后**有读点**：`0x43110D`、`0x43111C` 的比较，以及排版循环里的
`0x4335BE cmpb $0x0,0x5d8`；另有三处清零（`0x431124`、`0x43239A`、`0x4337C5`）。
所以 TextSkip 不是死存储 —— 它决定"哪些输入会触发跳过"。

还有一处 `0x42DB78`（`mov 0x5d7(%edi),%al` → 写进流）是**存档序列化**，把 `0x5D7`
抄进状态块，不是行为读点。

**没做的**：命中之后排版循环具体怎么"跳"（立刻显示全文？跳到下一行？）还没跟完，
本轮只冻结站点与掩码，不实现渲染行为。

### 7.5 C 侧接线（`FvpTextAttr` / gfx 只存不画）

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
缺陷。这也是"接上 `PrimHit` 之后 `verify_gfx` 的 289 px / PSNR 49.95 / SSIM 0.99971
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

### 正文可以浮在 CG 上（2026-11-26 用户实机确认 + 结构证据）

用户在原版里截到一个画面：白底 CG 上只有一行正文「所以我想，如果……」，**没有对话框**。
这不是 bug，是原版的行为——**正文窗口的位置完全由图元决定**：

* 正文层（tw=0）的面板由 `TextBuff(0,1024,1024)` 建立（离屏 RGBA，见上）；
* 上屏只走 type-5 文本图元：五彩的正文图元是 `PrimSetText(551, tw=0, x=1, y=1)`
  （hcb 实测），之后由 `MotionMove`/`PrimSetXY` 把它挪到这一句要出现的位置；
* 对话框（`graph/main_window_mask` 那一类）是**另一组图元**，与文本窗口没有绑定关系，
  所以"有文字没框"和"有框没文字"在原版都属于正常状态。

因此引擎**不能**：① 合成一个对话框；② 把正文强制摆进某个版面坐标。
我们的默认（图元驱动）就是这样；`--text-fixed-layout` 只是给冻结像素基线用的旧路径，
它画的合成框**不是原版行为**。

配套事实：`tests/verify_headless.py` 曾用"画面非黑"断言渲染层，而它依赖的正是那个
合成框。图元驱动成为默认后，无图片/字库宿主的 headless 外壳画不出任何东西，
本轮给外壳加了显式 `--text-fixed-layout` 开关，门禁改用它跑——断言测的仍是渲染层。

## 图元层级与绘制派发（A 类：`ref/exe-prim-hierarchy.json`）

正文文本"画在哪"这件事，之前只到"用图元的 x/y"这一层；本轮把引擎的**派发与父链**
读透并冻结成基线（`scripts/extract-exe-prim-hierarchy.py` 逐条校验 24 条关键指令，
`tests/verify_exe_prim_hierarchy.py` 门禁）：

| 事实 | 出处 |
|---|---|
| 图元基址 `engine+0x61DD64`，步长 **0x34**，共 **4096** 项（id 1..0xFFF） | `0x438AD1 imul $0x34`、`0x438AD4 lea 0x61dd64(...)` |
| `+0x04` type（1..7）、`+0x05` 内容标志、`+0x06` dirty/选项位、`+0x07` alpha、`+0x08` blend | `0x438AF5/0x438AFA/0x438AC2` 等 |
| `+0x0C` 父 id（0xFFFF = 无）、`+0x10` 下一个兄弟 id | `0x438AC2`、`0x438B44` |
| `+0x14/+0x16` x/y（u16，负数按补码存）、`+0x18/+0x1A` w/h、`+0x20/+0x22` OP、`+0x24` 角度（模 3600）、`+0x26/+0x28` 缩放 | `PrimSet*` handler 写点（见 `ref/exe-prim-hierarchy.json` 的 fields 表） |
| `+0x2A` 第一个子图元、`+0x2E` 图形槽、`+0x32` 文本层号 | 组迭代 / type-4 / type-5 分支 |
| **派发** `0x438AB0(this, x, y, prim)`：有父 → 把本图元的 x/y/alpha **抄进父图元**再上溯；无父 → `(type-1)` 查 `0x438C9C` 的 **7 项跳表** | `0x438AC2..0x438AF3`、`0x438AFA..0x438B08` |
| **组迭代**：子调用参数 `x = 组.x + 传入x`、`y = 组.y + 传入y`，沿 `+0x10` 遍历兄弟 | `0x438B22..0x438B48` |
| **type-5 文本**：`+0x32` 取层号 → `engine+0x98+层号*4` 取窗口 → `+4` 取内嵌 surface → 在**父链累积的 (x,y)** 上画 | `0x438C54..0x438C79` |

由此得到不变式：**图元屏幕坐标 = 父链上各级 x/y 之和**（叶子自身 x/y 由"抄进父图元"
那一步并入）。本引擎 `gfx.c` 的 `prim_abs_xy()` 正是这条，门禁里做了 C 侧交叉核对。

### 正文图元的位置追踪（本轮实测，未定论的部分如实记着）

`FVP_DEBUG_TEXT` 现在会打印文本图元的父链。把 60k..900k 步按 30k 采样跑一遍，
**tw=0 的文本图元始终只有 551 一个**，位置只在 `(1,1)` 与 `(1,27)` 之间变化，
父图元 767/850 都是 `type=1 xy=(0,0)` 的组：

```
text prim 551: tw=0 visible=1 lines=1 parent=767 xy=1,27 abs=1,27 wh=0,0 cell=64x32 panel=1024x1024
   chain 767: type=1 xy=0,0 parent=0 alpha=255 scale=1000,1000 angle=0
```

也就是说：**序章里脚本没有把正文图元挪到画面中部**。按上面的派发模型，正文面板
就应该画在 `(1,27)`。但用户实机截图里原版正文确实出现在画面中部偏左（白底 CG、无对话框）。

⇒ 说明还有一层没找到：要么那些场景里引擎**另外**建了一个绑到 tw=0 的图元
（我们的 VM 目前只看到 551/552/553/558 四个文本图元），要么面板内部的文字排版
带了一个屏内偏移（`+0x5AC/+0x5AE` 之外的字段还没映射）。这条记为**未定论**，
下一步用 `TextPrint` 调用点和文本窗口的 `+0x5xx` 未映射字段继续查，不猜。

### 序章正文：没有任何绑到 tw=0 的图元（2026-11-26，`--dump-text-prims` 实测）

用新加的外壳开关把每个文本图元按 500 ms 采样 22 s，序章里只有：

```
textprim id=552 tw=8 xy=375,852 alpha=0     (人名层)
textprim id=553 tw=9 xy=165,590 alpha=0     (章节标题层)
```

**没有 tw=0 的文本图元**。这推翻了我上一轮的一个隐含假设（"序章正文由 551 号图元画"）：
551 只在某些状态出现。结合 exe 侧的结论（文本只能经 type-5 图元上屏），剩下两种可能：

1. **对话文本图元由引擎自己创建**（不是脚本的 `PrimSetText(86)`；脚本那 13 个调用点
   只覆盖 8/9/10 这类覆盖层），位置来自引擎的消息窗口状态；
2. 引擎对**未绑定窗口**也有一条屏内绘制路径（那样 `gfx_layout_default()` 的版面表就得
   从 exe 里找出来，而不是拟合值）。

当前实现是"未绑定窗口按版面表兜底"，所以序章文字能显示，但位置是**近似值**。
下一步：在 exe 里找"引擎自建对话文本图元"的写点（候选：读 `engine+0x84/0x85`
文本开关附近的代码），或找未绑定窗口的绘制路径；两者任一成立就替换掉版面表兜底。

### 新证据 + 一个可检验的假设（2026-11-26，带 `--auto-click` 的 40 s 采样）

加上合成点击后，正文图元确实出现了：`551 tw=0 xy=(1,1) alpha=255`（可见），
另有 `558 tw=10 xy=(0,0) alpha=255`、`552 tw=8 alpha=0`、`553 tw=9 alpha=0`。
所以"序章没有正文图元"只是没推进到那一步；**正文确实由 551 在 (1,1) 上屏**。

**（这段里的 (1,1) 后来被推翻：它来自我们自己 VM 的一次运行，不是脚本数据。
详见下面"实测对照"与"脚本侧窗口几何"两节。）**

### 排版笔位与"居中"的说法（2026-11-26 修正：上一轮那条居中结论**撤回**）

上一段曾把 `0x433037..0x433048` 那条算术当成"每行水平居中"。本轮逐条指令读下来，
这个结论**不成立**，撤回：

| 事实 | 出处 |
|---|---|
| 排版笔位就是 `tw+0x5E8`（x）/ `tw+0x5EA`（y） | 字形落位 `0x424550` 一进来就读这两个字段（`0x42455C`、`0x42456F`） |
| 落位时按面板裁剪：`x+w > tw+0x5DC` 就截断，`y+h > tw+0x5DE` 就截断 | `0x4245DE..0x424604` |
| 落位顺带维护**本次排版的包围盒**：`+0x630`=minx、`+0x638`=maxx、`+0x634`=miny、`+0x63C`=maxy | `0x424616..0x42466B`（取 min/max 的四条 cmp/mov） |
| 每行/每段开始前先把包围盒初始化成"空"：`+0x630=面板宽`、`+0x638=0`、`+0x634=面板高`、`+0x63C=0` | `0x432C2D..0x432C55`、`0x432AB3..0x432ADD` |
| `0x433037` 那条算术的实际操作数：`pen_x += (edi - [esp+0x18]) / 2`，`edi` 与 `[esp+0x18]` **都是字形步进的和**（`0x432EF5..0x432F1D` 累加，`0x432F42` 再减掉首个字形的那一份），**不是面板左右边界** | `0x432F03..0x433048` |
| 该算术所在的分支：`pushint16 0x5D/0x7C/0x5B` 这类**控制码**（`0x432BF7 cmp $0x1,%bp`、`0x432E90 cmp $0x5B,%bp`）专用的排版通道；普通字符走 `0x4331BA` 之后的路径，不经过它 | `0x432BE8..0x432E94`、`0x4331BA` |
| 排版起点：`pen_x = tw+0x5AC`、`pen_y = 0` | `0x4337A9..0x4337BE`（`mov 0x5ac(%esi),%cx` → `mov %cx,0x5e8(%esi)`；`xor %edx,%edx` → `0x5EA`） |
| `TextFormat(118)` 写 `tw+0x5AC` 时**范围 0..64**，且是"只升不降"（`cmp %dx,0x5e8` 后 `jge` 才跳过） | `0x43CC9B..0x43CCAE` |

所以现在能定下的只有：

1. 排版起点 = `0x5AC` 的缩进（0..64）**左对齐**往下排，`0x5EA` 每行加行高；
2. 垂直方向**没有**居中指令——"面板内垂直居中"这个旧说法**撤回**；
3. **"每行各自水平居中"同样撤回** —— 那条算术属于控制码分支，操作数是字形步进，
   不是面板边界。要证明面板内居中，得先找到普通字符路径上的居中写点（未找到）。

我们渲染器现在就是"从 `0x5AC` 缩进左对齐"，与这条修正后的结论一致，不再改。

至于"面板画在屏幕哪"，`0x433A91..0x433AFC` 是一段**反序列化**（从存档/状态流里
按顺序读 u32/u8/u16 填 `tw+0x5D0/0x5D5/0x5D6/0x5D7/0x5DA/0x5DC/0x5DE/0x5E0/0x5E4/
0x5E8/0x5EA`），调用点是 `0x43F38F`（存档读取路径），**不是** hcb 里的版式表。
所以"窗口版式表"这条假设也**撤回**：窗口的几何是脚本自己一条条设的，见下节。

### 实测对照：用户给的原版截图 vs 文档里的模型（2026-11-26）

用户贴了一张原版实机截图（窗口 2560×1600，上下黑边各 80 px，内容 2560×1440 =
1920×1080 ×1.3333）。逐像素量出来（`work/measure_orig_body4.py`）：

| 量 | 值（画布坐标） |
|---|---|
| 墨迹包围盒 | x **489.0 .. 585.8**（宽 96.8 ≈ 3 个 32 px 字格）、y **456.0 .. 485.2**（高 29.2） |
| 行数 | 只有一行，画面上没有别的墨迹 |

对照文档里那个模型（面板 1024×1024、prim (1,1)、每行水平居中 + 顶端对齐）：
预测左缘 `1 + (1024-96)/2 = 465`、上缘 `1`。

⇒ **水平差 24 px、垂直差 455 px。垂直这一项直接判模型不成立**；而且 3 个字格的
宽度（96.8）确实等于 `TextSize(0, 64, 32)` 的 32 px 步进，说明字体度量对得上，
错的是**窗口位置/面板**这一层。

### 脚本侧窗口几何（A 类：`ref/script-window-geometry.json`）

真正的窗口几何在 hcb 里，而且**坐标是设计空间 1024×640**，脚本里的 helper 先换算
再调引擎（`scripts/extract-script-window-geometry.py` + `tests/verify_script_window_geometry.py`）：

| 事实 | 出处 |
|---|---|
| x 乘 **1.875 = 1920/1024**、y 乘 **1.6875 = 1080/640**（`pushfloat 0x3FF00000 / 0x3FD80000`） | helper `0x35C10`（`0x35C24`、`0x35C3B`） |
| 窗口 helper 家族：`PrimSetText`/`PrimSetWH`/`PrimSetXY`/`syscall 94`/`TextBuff` 各一个，全部先换算 | `0x35C10`、`0x35CF2`、`0x35D14`、`0x35D56`、`0x35D76` |
| `TextBuff` helper 对换算后的宽高各自取**下限 1024**（≤1024 抬到 1024，>1024 原样） | `0x35DA0`（`pushint16 1024` + `le` + `jmpcond`）；实测正文层拿到 **1380×1024** |
| 名字窗口：prim **552** → layer **8**，设计 (200,505) → 画布 **(375,852)** | `0x364F1` |
| 正文窗口：prim **551** → layer **0**，设计坐标取局部变量默认 **(230,522)** → 画布 (431,881) | `0x39F82`；默认值 `0x39F33`/`0x39F41` |
| 覆盖窗：prim **558** → layer **10**，设计 (0,0) | `0x3A046` |
| 章节标题：prim **553** → layer **9**，设计 (88, 350) → 画布 **(165,590)** | `0x3AA7B` |

**引擎侧不要再缩一次**：换算发生在脚本里，引擎收到的已经是画布坐标（
`ref/script-window-geometry.json` 里写明）。我们实测我们的引擎对 552/553 拿到的
正是 (375,852)/(165,590)，与脚本一致；但 551（正文）拿到的是 (1,1) —— 说明脚本里
那段"取局部变量默认值 230/522"的流程在我们 VM 里没走对，`PrimSetText(86)` 的
x/y 就被 `true` 哨兵顶成了 1。

仍未定：用户那张截图里那行 3 个字在 (489,456)，**四个已知窗口都对不上**（正文窗口
若在 (431,881)、面板 1024×1024 且每行居中，应该出现在 (751,881) 附近）。所以要么
原版在那种"正文浮在 CG 上"的场景里把窗口**挪过位置**（脚本用运行时值 PrimSetXY/
Motion 改的），要么面板的落点不是 prim 的 x/y 而是创建时的 surface 矩形。
下一轮两件事：① 修 VM 里 551 那条默认值链路，让 --dump-text-prims 打印真实值；
② 在 exe 里把 type-5 绘制的落点彻底读清（`0x42AE40` 的 dest 用 prim x/y 还是
surface 矩形），再决定实现。

### 正文位置：把窗口设置函数的 6 个调用点全抄出来（2026-11-26 第 220 轮，A 类）

上面那个"仍未定"本轮有答案了，而且**不是靠截图拟合**：把脚本里窗口设置函数
`0x39F27` 的**全部调用点**连实参一起抄出来（`ref/script-window-calls.json`，
`scripts/extract-script-window-calls.py`，门禁 `tests/verify_script_window_calls.py`）。

函数本身（`initstack 5|0`，5 个参数占绝对槽 250..254，每个参数都是
`if (槽 == false) 槽 = 缺省值`）：

| 参数 | 槽 | 缺省值（脚本里的字面量） | 缺省值画布坐标 |
|---|---|---|---|
| x | 250 | `pushint16 230`（`0x39F33`） | 431.25 |
| y | 251 | `pushint16 522`（`0x39F41`） | 880.875 |
| w | 252 | 算式 `800-((26+2*30+2)*2)+26*2 = 736`（`0x39F4F..0x39F64`） | 1380 |
| h | 253 | `pushint8 120`（`0x39F6F`） | 202.5 → TextBuff 下限 **1024** |
| — | 254 | 无缺省（用途未定） | — |

调用点（全脚本只有 6 个）：

| 调用点 pc | 实参（设计坐标） | 画布落点 | 面板 |
|---|---|---|---|
| `0x0363CA` | 全 `pushfalse` | (431.25, 880.875) | 1380×1024 |
| `0x0365E7` | 4 个 `pushfalse` + `1` | (431.25, 880.875) | 1380×1024 |
| **`0x0367BD`** | **246, 258, 800, 320** | **(461.25, 435.375)** | **1500×1024** |
| `0x036933` | 全 `pushfalse` | (431.25, 880.875) | 1380×1024 |
| `0x03B1A0` | 全 `pushfalse` | (431.25, 880.875) | 1380×1024 |
| `0x052F06` | 全 `pushfalse` | (431.25, 880.875) | 1380×1024 |

⇒ **"正文浮在 CG 上"= 只有那一个调用点传了显式几何**（画布 y = 435.375），
它前后都是吃缺省的常规对话框 (431.25, 880.875)。这和用户 2026-11-26 的补充
「这些特殊位置的正文通常只持续一小段，然后就回归正常格式」**完全一致**：
脚本在一段剧情前后各调一次窗口设置函数，特殊位置只在中间那一段生效。

用户截图里的墨迹实测（`work/measure_orig_body4.py`，2560×1600 上下各 80 px 黑边，
换算到 1920×1080 画布）是 **x 489.8..585.0 / y 456.8..484.5**。面板落点
(461.25, 435.375) 与它相差 (+28.55, +21.4) —— 这是**面板内排版的前导量**
（`……` 三个点落在 32 px 字格的下半部分，横向还有字形左侧留白），量级合理；
而缺省窗口 (431.25, 880.875) 在垂直方向差 **424 px**，不可能是它。

**根因不在渲染侧，在我们 VM 的常量操作码**（2026-11-27 更正：这里的哨兵是
**op `0x08` = `pushfalse`**，handler `0x444940`，它整段只写 Variant 的
**类型字节 0x0**，也就是 `false/null` 这个独立类型）。各 `PrimSet*` handler 只对
Variant **type 2（int）** 才写字段，所以 `PrimSetText(551, 0, false, false)` /
`PrimSetXY(551, false, y)` 里的哨兵是"这一维不改"。我们以前把 `0x08` 压成 int 1，
于是这些哨兵真的把 x/y 写成了 **1** —— 这就是"正文跑到 (1,1)"的全部原因。
（旧注释把 `0x08` 叫 `pushtrue`，名字是错的：`pushtrue` 是 `0x09`/`0x444960`，
写类型字节 1。见 docs/vm-commands.md §0.1 与 `ref/exe-vm-ops.json`。）

本轮修法：`fvp.h` 加 `FVP_BOOL`（对应原版 Variant type 1）、`fvp_make_bool()`
→ `fvp_value_bool()`；`fvp.c` 的 `fvp_as_number()` 把 BOOL 当 1（比较/算术一字不变），
`fvp_value_truth()` 把 BOOL 当真；`vm.c` 的 op `0x08` 改压 `FVP_NULL`（`--faithful-ops`
下；默认路径为保冻结基线仍压 BOOL，见 `vm.h` 的开关说明）。
实测效果（同一份 `--dump-text-prims`）：

```
修复前： textprim id=552 tw=8 xy=1,852      ← x 被 true 哨兵写成 1
修复后： textprim id=552 tw=8 xy=375,852    ← 与脚本 (200,505)×(1.875,1.6875) 一致
```

冻结文本基线（`ref/ref-900k.txt`）**逐字节不变**（6496 B / 195 行 / sha256
`7f9837ff…`），像素基线也不变（289 px / 0.0139% / PSNR 49.95 / SSIM 0.99971）——
因为 BOOL 只在"handler 的类型检查"这一层可区分，数值语义与 int 1 完全等价。

**仍未解决的部分（诚实记录）**：我们的 30 s 采样窗口里 `PrimSetText(551, …)`
**一次都没被调用**（只有 552/553），所以正文图元目前还没吃到显式几何
(461.25, 435.375)。也就是说"正文位置"的**链路已经修对**（哨兵语义 + 落点规则），
但**这一段剧情在我们 VM 里还没跑到**（脚本 551 的绑定在 `0x39F82`，它被
`pushglobal 249 == true` 的分支保护，见 `0x39F78..0x39F7D`；global 249 是什么、
何时置真，下一步查）。

### 帧模型：`initstack A|B` 与绝对槽号（2026-11-26 本轮）

顺着"正文窗口为什么拿到 (1,1)"往下查，问题不在 exe 也不在脚本，而在**我们自己的
VM 帧模型**。脚本侧的证据（全部在 `ref/script-window-geometry.json` 的
`frame_model` 段，门禁逐条复核）：

| 函数 | `initstack` | 用到的绝对槽 | 说明 |
|---|---|---|---|
| `0x35C10` PrimSetText helper | `4|2` | 251..254 | 4 个实参 + 2 个局部变量（0/1） |
| `0x35D14` PrimSetXY helper | `3|2` | 252..254 | 3 个实参 + 2 个局部变量 |
| `0x35D76` TextBuff helper | `3|2` | 252..254 | 3 个实参 + 2 个局部变量 |
| `0x39F27` 窗口设置 | `5|0` | 250..253 | 5 个"带默认值的实参"，无额外局部变量 |

⇒ **`initstack A|B`：A = 参数槽个数，B = 额外局部变量个数；参数占绝对槽
`255-A..254`（固定以槽 254 收尾），额外局部变量占它们下面紧挨的 B 个槽。**

另外 `false` 是**调用方显式压入的哨兵**：窗口设置函数的调用点 `0x363C5..0x363C9`
连压 5 个 `pushfalse`，函数体再 `if (槽 == false) 槽 = 默认值` 吃默认值
（`0x39F33` → 230、`0x39F41` → 522、`0x39F4F` → 736、`0x39F6F` → 120）。所以
"没传的实参"在脚本语义里是 **0**，不是 `true`。

#### 两个独立样本复核（2026-11-26，第 179 轮）

为了确认"参数槽 = `255-A..254`"不是只看一个函数得出的，又扫了两个高频函数，它们都用
同一个 `if (槽 == false) 槽 = 默认值` 惯用法：

| 函数 | `initstack` | 用到的绝对槽 | 结论 |
|---|---|---|---|
| `0x23804` | `2|0` | 253、254（`copystack 254` 写参数 2） | A=2 的参数窗口正好是 253..254 ✓ |
| `0x3F389` | `8|0` | 250..254（`copystack 251` 写参数 1） | A=8 的参数窗口是 247..254，251 在窗口内 ✓ |

⇒ 模型成立：**`initstack A|B` A = 参数槽个数（绝对槽 `255-A..254`）、B = 额外局部变量个数**。

#### 实现：`FVP_FRAME_SLOTS` 开关（默认关，保住冻结基线）

`src/vm.c` 里两条路并存：

| 模式 | 行为 | 文本基线 |
|---|---|---|
| 默认（环境变量没设） | 旧模型：`initstack` 第一个字节当局部变量个数，参数按"离 255 多远"映射 | `ref/ref-900k.txt` **逐字节不变**（sha256 `7f9837ff…`） |
| `FVP_FRAME_SLOTS=1` | 修正模型：参数槽 `255-A..254`、局部变量块在参数块下面、`copystack` 可写参数槽 | 输出变成 **6760 B / sha256 `fb5b53fb614f07f1…`** |

修正模型下实测：`PrimSetText prim=551 tw=0 **x=431 y=880**`（= 设计 (230,522) ×
(1.875, 1.6875)，截断），名字/章节/覆盖窗仍是 (375,852)/(165,590)/(0,0)。

**为什么默认不打开（2026-11-26 第 181 轮有了判据，结论：不能重新冻结）**

用 HCB 的字符串数据当判据（`tests/verify_text_from_hcb.py`，`run_all` 6.49649）：

| 检查 | 结果 |
|---|---|
| HCB 里有 `……。`（cp932 `81 63 81 63 81 42`） | 1345 次 |
| HCB 里有 `……。」`（追加 `81 76`） | **0 次** |
| 冻结基线 `ref/ref-900k.txt` 的 195 行（去掉 `[层] ` 前缀与行首全角缩进）能否在 HCB 找到 | **195/195 全找到，0 编造** |
| `FVP_FRAME_SLOTS=1` 的 195 行里有几行在 HCB 找不到 | **70 行**（每行多一个「」） |

⇒ **冻结基线是对的，修正帧模型的文本通道会"多读一个字符"**。所以：
1. 不能把 `FVP_FRAME_SLOTS` 切默认、也不能重新冻结基线；
2. 修正模型虽然把窗口位置修对了（(431,880)），但它在别处弄坏了文本长度/终止符，
   得先找出"多读一位"的根因（下一步：对照 `TextClear`/`TextPrint` 读取的槽位）。

门禁 `tests/verify_frame_slots.py`（`run_all` 6.49647）把两种模式都钉住：默认模式
文本 sha256 必须等于冻结基线、`551 == (1,1)`；`FVP_FRAME_SLOTS=1` 时必须
`551 == (431,880)` 且文本 sha256 与冻结基线不同。

#### 已知差异（默认模式下仍在）

我们的 C VM 现在把 `copystack N` 限制在 `N < initstack 第一个字节`，于是脚本对槽
250..254 的默认值写入**全部被丢掉**，实测
`PrimSetText prim=551 tw=0 x=1 y=1`，而脚本要的是设计 (230,522) → 画布
**(431, 880)**（230×1.875 = 431.25、522×1.6875 = 880.875，脚本的转换是截断）。

本轮试过按上面的模型改 VM（槽寻址 255-A..254 + `copystack` 可写参数槽），
**`tests/compare.py` 的冻结文本基线立刻变了**：`ref/ref-900k.txt` 是 6496 B，
新输出 6760 B，第 3 行多出一个「」。这说明书**那份基线本身是用旧帧模型生成的**——
要么新输出更接近原版（那就要拿原版文本重新验证后**有意**重新冻结），要么新模型
仍在某个地方搞错了窗口（比如把别的函数的局部变量当成了参数槽，扫出来
`0x2381C`（`initstack 2|0`）、`0x3F396`（`8|0`）这类高频写入点就是嫌疑）。

所以本轮**回滚**了 VM 改动（工作树干净、compare.py 仍 PASS），把证据和差异冻进
基线，把修复留到下一轮：先用原版可执行文件的解释器（或原版文本）验证"槽窗口
到底以哪个字节为准"，再一次性改 VM + `ref/fvm_ref.py` + 重新冻结文本基线。

## 文本面板画在哪、多大（A 类：`ref/exe-text-draw.json`）

"面板是不是画在 prim 的 x/y 上"这个问题一直悬着。本轮把 `0x42AE40`（type-4/5 共用
的贴图绘制）的目标矩形算法读通并冻结（`scripts/extract-exe-text-draw.py` 逐条校验
24 条指令，`tests/verify_exe_text_draw.py` 门禁）：

| 事实 | 出处 |
|---|---|
| 调用约定 `0x42AE40(this, argx, argy, prim, surf)`；type-5 在 `0x438C54` 把**父链累积**的坐标透传 | `0x438C54..0x438C79` |
| 尺寸：prim 的 WH 写过（`prim+0x06` bit0）→ `prim+0x18/+0x1a`；否则 → **surface 的 `0x510/0x512`**（面板尺寸） | `0x42AEC8`、`0x42AEF6/0x42AEFD` |
| 落点：OP 写过（bit1）→ `prim(x,y) + OP(0x20/0x22) + 累积`；否则 → `prim(x,y) + surface(0x518/0x51a) + 累积` | `0x42B41F..0x42B445`、`0x42B484..0x42B4A6` |
| `surface+0x514/+0x516` 是**源裁剪原点**（缩放分支里被减掉），不是落点 | `0x42AECE..0x42AEE6`、`0x42AF10` |
| `PrimSetText(86)` **清掉 WH 位**（`andb $0xfe,0x6(%eax)`） | `0x43BDE1` |

⇒ **结论：文本面板 1:1 画在 prim 的 (x,y) 上，尺寸就是 TextBuff 建出来的面板尺寸；
不缩放、没有版式偏移。** 本引擎 `gfx.c` 的 type-5 路径（`prim_abs_xy()` →
`draw_text_window()`）实现的正是这条。

于是落点规则闭环：`PrimSetText(551, 0)` 在修正帧模型下建在 **(431,880)**、
面板 **1380×1024**（TextBuff 的尺寸下限 1024），面板就画在 prim 的 (x,y) 处。

**但 x/y 是运行期可变的**：带点击的 60 s 实测（`FVP_DEBUG_TEXT=1` 抓 MotionMove）里，
脚本随后就把正文/名字窗口挪走了：

```
MotionMove prim=551 x=1 y=880 z=1 ms=1 mode=3 (instant)
MotionMove prim=552 x=1 y=852 z=1 ms=1 mode=3 (instant)
```

也就是说这些窗口的位置**不是一次设定就不动的**：`MotionMove`(48) 会改 `prim+0x14/+0x16`
（我们的实现：`--motion` 关时直接写值、开时插补间）。所以：
1. "原版正文为什么在某处"要按**那个场景当时的 MotionMove 参数**去解释，不能只看
   `PrimSetText` 的初值；
2. 抓窗口位置必须抓 MotionMove（本轮已给 48/51 加了 `FVP_DEBUG_TEXT` 日志）。

### 文本类 handler 的实参规则（A 类：`ref/exe-text-handlers.json`）

四个文本 handler 是同一族写法：**层号 `arg0` 必须是 int 且在 `0..0x1F`**（否则整条
命令忽略），其余分量**逐分量**先判 `type==2`（int）再过范围检查，不满足就**跳过那一个
分量、保持原值**——不是钳位、也不是取低位。

| 命令 | handler | 分量规则 |
|---|---|---|
| `TextBuff(110)` | `0x43C830` | w/h 缺省 8，int 且 ≥0 才覆盖；越界交给 `0x430EA0` 抛异常 |
| `TextColor(112)` | `0x43C8D0` | R/G/B 各自 int 且 **0..0xFF** |
| `TextFont(113)` | `0x43DA70` | 两个字体 id 各自 int 且 **-5..(字体表项数-1)**，上界是**动态**的 |
| `TextOutSize(120)` | `0x43C960` | arg2 **0..0xC**、arg3 **0..0x8** |
| `TextShadowDist(125)` | `0x43CBB0` | 距离 **钳位** 到 0..0xC（负数→0、>0xC→0xC），非 int→0 |
| `TextSize(126)` | `0x43CD60` | w **0xC..0x40**、h **0x8..0x20** |
| `TextSkip(127)` | `0x43CDD0` | mode int **0..3** → `tw+0x5D7` |
| `TextSpace(128)` | `0x43CE10` | x/y 各自 int **-0x20..0x20** → `tw+0x5A8/0x5AA`（与 TextFormat 的前两个分量同字段） |
| `TextFormat(118)` | `0x43CC10` | a1/a2 -0x20..0x20 → `0x5A8/0x5AA`；a3/a4 0..0x40 → `0x5AE/0x5AC`（a4 还会把 pen `0x5E8` 抬到不小于它）；a5/a6 -0x10..0x10 → `0x5B4/0x5B0` |
| `TextFunction(119)` | `0x43CCF0` | a 0..1 → `0x5A5`；b 0..2 → `0x5A4`；c 0..2 → `0x5A6` |

注意 `TextShadowDist` 是这一族里**唯一钳位**的（其余都是"越界跳过、保持原值"）。

本轮修的第二处偏差：`TextFont` 原来把非 int 的分量当 **0** 传下去（0 是合法字体号，
于是覆盖了原值），而且上界用静态 31。原版是"非 int 跳过 + 上界 = 字体表项数-1"。
现在改成 `fvp_textattr_set_font_ex()`（带 has 标志 + 动态上界），`syscall.c` 的
case 113 用 `fvp_vm_font_count(vm)-1` 当上界。

第三处同类偏差：`TextFormat(118)`/`TextFunction(119)` 同样把**非 int 的分量当 0** 传下去，
而 0 在它们的每个范围里（space 0、pad 0、off 0、func 0）——等于用"省略"覆盖了原值。
现在两者都改成带"该分量是否提供"掩码的 `_ex` 入口（`fvp_textattr_set_format_ex` /
`fvp_textattr_set_function_ex`），`syscall.c` 里按 `args[i].type == FVP_INT` 逐位填掩码。
顺带确认 `TextFormat` 的 a4 还有个副作用：**新值比笔位 `0x5E8` 大时把笔位抬上去**
（`0x43CC9E` 的 `cmp %dx,0x5e8` + `jge`），我们的 `fmt_width_floor` 实现的正是这条。

我们这轮修掉的一处真实偏差：`TextColor` 原来无条件调 `gfx_text_set_color()`（内部
`& 255`），于是 `TextColor(0, 300, 0, 0)` 会把 R 写成 44；原版是**保持原值**。现在改成
逐分量入口 `gfx_text_set_color_ex()`（新增 `gfx_text_get_color()` 给单元测试），
`tests/test_textprims.c` 从 37 条检查加到 **40 条**。

### TextBuff 的尺寸上限是"越界即抛异常"（A 类：`ref/exe-textbuff.json`）

上一轮我怀疑"我们的引擎把 1380 宽的面板丢掉了、原版不会"。本轮把 `0x430EA0` 读透，
结论是**我们没错，而且原版更狠**：

| 事实 | 出处 |
|---|---|
| `w > 0x400` 或 `h > 0x400` → 跳到 `0x430FF8` | `0x430EA8/0x430EB0`、`0x430EBA/0x430EC0` |
| `0x430FF8` 是**抛异常**：`push $0x80` → `0x447C05` 构造 → 压 `0x400,0x400,0x45D8C4`（格式串）→ `0x4258F0` 格式化 → `0x44837C` throw | `0x430FF8..0x431035` |
| 没越界：面板尺寸写 `tw+0x5DC/0x5DE`、`0x4289F0` 建/改 surface、`w*h*4` 挂 `tw+0x520` | `0x430ECD/0x430ED4/0x430EDB/0x430EFB..0x430F22` |

handler `0x43C830` 的**实参规则**（同一份基线里，20 条指令逐条校验）：

| 实参 | 规则 | 出处 |
|---|---|---|
| `arg0` 层号 | 必须是 int 且在 **0..0x1F**，否则整条命令忽略 | `0x43C834`、`0x43C840` |
| `arg1` w | **缺省 8**；只有"是 int 且 >= 0"才覆盖 | `0x43C858`、`0x43C861` |
| `arg2` h | **缺省 8**；只有"是 int 且 >= 0"才覆盖 | `0x43C85D`、`0x43C870` |

我们原来的 `case 110` 与这条不一致：`arg1` **不是 int 时整条命令被跳过**，于是
`TextBuff(layer)` 这种"只给层号"的调用**不会**建出 8×8 的缺省面板。本轮按 handler
改成：先判层号 0..0x1F，w/h 各自缺省 8、只在 int 且 >=0 时覆盖。

⇒ 规则是 **`w > 1024 || h > 1024` 抛异常（code 0x80）**，不是"悄悄放弃"。
脚本里正文层的 `TextBuff(0, 1380, 1024)`（设计 736×202 换算后）**正好越界**，所以在
原版里那段窗口设置会中断（除非脚本接住异常）——这也解释了为什么正文层的面板在原版里
也不该存在。我们引擎在同一条件下返回 0、并在 `FVP_DEBUG_TEXT` 下打印一行提示。

## 引擎自建对象表（A 类：`ref/exe-engine-objects.json`）

`0x448833` 是引擎的 `Construct(base, elemsize, count, ctor)`：先
`base += elemsize*count`，再从表尾倒序 `base -= elemsize; call *ctor`，共 count 次
（`0x448843..0x448861`）。引擎初始化里那一串调用点的**实参**就是每张对象表的
基址/元素大小/项数/构造函数。本轮把它们逐条抄成基线（`scripts/extract-exe-engine-objects.py`，
`tests/verify_exe_engine_objects.py` 门禁）：

| 表 | 基址 | 元素 | 项数 | 占用 |
|---|---|---|---|---|
| sprite | `engine+0x101064` | `0x51C` | 4096 | 5,357,568 B |
| prim_alt | `engine+0x61D064` | `0x34` | 64 | 3,328 B |
| **prim** | `engine+0x61DD64` | `0x34` | **4096** | 212,992 B |
| （6 张 0x18/0x1C/0x20/0xB28 的结构表，名字未定） | `0x651E08`、`0x653810`、`0x65BA58`、`0x679A60`、`0x67CE68`、`0x680A70`、`0x683E78`、`0x68DFC0` | — | — | — |
| 两段内联数组 | `0x6A49EC`（0x4×0x80）、`0x6A4DF0`（0x14×0x100）、`0x6A7E7C`（0x4×0x2） | — | — | — |

三条**互证**（门禁里逐条断言）：

* **首尾相接**：`sprite` 结束于 `engine+0x61D064`（= `prim_alt` 基址），`prim_alt`
  结束于 `engine+0x61DD64`（= `prim` 基址）——说明解出的 (elemsize, count) 不是
  把两个实参看反了；
* **两份基线一致**：`prim` 的 (base, 0x34, 4096) 与 `ref/exe-prim-hierarchy.json`
  里冻结的 stride/count 完全一致（两条独立抄写路径）；
* **容器构造函数 7/7 对上**：每张带 vtable 的表都有一个容器构造函数
  （`0x4347B6` 家族）：先在 `(%ecx)` 写 vtable，再 `add $0x4,%ecx` 把 `this+4`
  当元素数组首址传给 `Construct`——它解出的 (elemsize, count, 元素构造函数)
  必须能在初始化注册表里找到同一组三元组。7 处全部对上。

合计 5.59 MiB，和 `engine` 的 bss 规模对得上（最大一张 5.11 MiB 的 sprite 表，
正好铺满 `0x101064..0x61D064`）。

### 命令 id → handler（A 类：`ref/exe-cmd-dispatch.json`）

原版在 `0x43E000..0x440E00` 的构造函数里**逐条注册命令**，每条长这样：

```
mov    $0x43XXXX,%ecx      ; handler 地址
mov    %ecx,(%eax)         ; 填进注册记录
push   $argc
push   $0x45Exxx           ; 命令名（ASCII 串）
call   0x4387d0            ; register(entry, name)
```

名字在注册顺序里是**字典序**，所以命令 id 要从 HCB 侧的 `ref/cmd_table.json`
（id → name）反查——三方连起来就是 **id / 名字 / argc / handler**。

第一版按"`mov $handler,%ecx` 紧跟 `mov %ecx,(%eax)`"的模式扫，只连上 133/148；
后来改成**名字驱动**（找每个 `push $命令名串`，往回扫最近的 `mov $handler,%ecx`），
补到 **147 / 148**（只剩 `AudioLoad` 的注册形态不同）。

**prim 命令族全表**（全部来自这张表，不再是推断）：

| id | 命令 | handler | 写什么 |
|---|---|---|---|
| 78 | `PrimSetBlend` | `0x43BA20` | `prim+0x08`（0..1） |
| 80 | `PrimSetNull` | `0x43BA60` | `0x439BF0(id, 0)` 清成 type 0 |
| 81 | `PrimSetOP` | `0x43BA90` | `+0x20/+0x22` |
| 82 | `PrimSetRS` | `0x43BAF0` | `+0x24`（angle）/`+0x26,+0x28`（scale） |
| 84 | `PrimSetSnow` | `0x43BC40` | 造 **type-7（snow）** 图元 + x/y |
| 85 | `PrimSetSprt` | `0x43BD00` | 造 **type-4（sprite）** 图元，arg1(-2..0xFFF)→`+0x2E` |
| 86 | `PrimSetText` | `0x43BDC0` | 造 **type-5（text）** 图元，层号 0..0x1F→`+0x32` |
| 87 | `PrimSetTile` | `0x43BE40` | 造 **type-2（tile）** 图元，arg1(-1..0xFF)→`+0x30` |
| 88 | `PrimSetUV` | `0x43BEF0` | `+0x1C/+0x1E`（u/v） |
| 89 | `PrimSetWH` | `0x43BF50` | `+0x18/+0x1A` |
| 90 | `PrimSetXY` | `0x43BFA0` | `+0x14/+0x16` |

⇒ **图元类型码有了名字**：2 = tile、4 = sprite、5 = text、7 = snow（我们原来的注释只有
1/2/4/5，现已补 7）。

据此把我们的 5 个实现对齐了一遍（第 204 轮）：

| 命令 | 我们原来 | 改成 |
|---|---|---|
| `PrimSetSprt(85)` | graphSlot 不做类型/范围检查 | 只接受 int 且 **-2..0xFFF**（越界保持 0） |
| `PrimSetText(86)` | 直接把 arg1 当 TW | 只接受 int 且 **0..0x1F**（越界保持 0） |
| `PrimSetTile(87)` | 只写 tile，**丢掉 x/y/w/h** | tile 限 **-1..0xFF**，并补上 arg2..arg5 → x/y/w/h |
| `PrimSetUV(88)` | 不截断 | u/v 按 **int16** 截断 |
| `PrimSetSnow(84)` | 只记一个布尔，不造图元 | **造 type-7 图元**（复位）+ 开关 0..1 + x/y（渲染端仍不画 snow，与原版一致） |

关键条目（其它）：

| id | 命令 | handler |
|---|---|---|
| 78 | `PrimSetBlend` | `0x43BA20` |
| 81 | `PrimSetOP` | `0x43BA90` |
| 89 | `PrimSetWH` | `0x43BF50` |
| 90 | `PrimSetXY` | `0x43BFA0` |
| 110 | `TextBuff` | `0x43C830` |
| 141 | `TitleMenu` | `0x437950` |

门禁 `tests/verify_exe_cmd_dispatch.py` 除了可复现性，还与**文本 handler 基线**
（110/112/120/126）和**图元 setter 基线**（81/89/90）互证——两条独立线都对上，
说明这张表没抄错。

### 图元 setter handler 的实参规则（A 类：`ref/exe-prim-setters-args.json`）

上一轮锁定 `0x43BACA..0x43BFDA` 之后，本轮把里面的四个 handler 读完了：

| handler | 写入 | 规则 |
|---|---|---|
| `0x43BA90` | `prim+0x20` / `+0x22`（op1/op2） | id 1..0xFFF 且 `prim+4` 必须是 4；arg1/arg2 各自 int → u16，逐分量 `flags \|= 0x02`；末尾无条件 `flags \|= 0x40` |
| `0x43BAF0` | `prim+0x24`（angle）/ `+0x26,+0x28`（scale） | **angle = v % 3600**（负数归一化）；**scale 钳位 100..10000**（per-mille）；每个已写分量 `flags \|= 0x40` |
| `0x43BC40` | `prim+0x2E`、x/y | `0x439BF0(id, 7)` 造 **type-7** 图元 → 字段全复位（`+0x12=1000`、scale=1000、alpha=0xFF）→ arg1（0..1）写 `+0x2E` → arg2/arg3 写 x/y |
| `0x43BD00` | 同上 | `0x439BF0(id, 4)` 造 **type-4** 图元；arg1 范围 -2..0xFFF |

**因此修掉一处偏差**：我们的 `case 82`（`PrimSetRS`）原来把 angle 原样存入、scale 也不钳位。
现在按原版做 `angle % 3600`（负数归一）与 `scale` 钳位 100..10000。

第 199 轮又把剩下的五个读完（共 **9 个** handler）：

| handler | 写入 | 规则 |
|---|---|---|
| `0x43BF50` | `prim+0x18/+0x1A`（w/h） | id 1..0xFFF（无类型检查）；arg1/arg2 各自 int → u16，逐分量 `flags \|= 0x01`；末尾 `flags \|= 0x40` |
| `0x43BFA0` | `prim+0x14/+0x16`（x/y） | id 1..0xFFF；arg1/arg2 各自 int → u16（**不置任何"已写"位**）；末尾 `flags \|= 0x40` |
| `0x43BEF0` | `prim+0x1C/+0x1E`（u/v 对） | 要求 `prim+4` 是 4；逐分量 u16 + `flags \|= 0x01`；末尾 `flags \|= 0x40` |
| `0x43BDC0` | `prim+0x32`（**文本层号**）+ x/y | `0x439BF0(id, 5)` 造 **type-5（文本）** 图元，清 `flags` 的 bit0；文本层号范围 **0..0x1F**（与 `TextBuff` 的层号一致） |
| `0x43BE40` | `prim+0x30` + x/y/w/h | `0x439BF0(id, 2)` 造 **type-2** 图元；arg1 范围 **-1..0xFF** → `+0x30`；arg2..arg5 → x/y/w/h |
| `0x43BA20`（**cmd 78 `PrimSetBlend`**） | `prim+0x08`（blend） | id 允许 **0..0xFFF**（这一条允许 0）；arg1 只接受 **0..1**（0 = over / 1 = additive），越界保持原值；不置任何 flags 位 |
| `0x437990`（**cmd 77 `PrimSetAlpha`**） | `prim+0x07`（alpha） | id **1..0xFFF**；arg1 int 且 **0..0xFF**（`cmpl $0xff; ja` 无符号比较），越界保持原值；不置 flags。**全片最高频命令**（两部作品合计 1052 次） |
| `0x43A9C0`（**cmd 44 `MotionAlphaTest`**） | 线程 `+0x814` | id 1..0xFFF；查 `engine+0x651E04`（与 `MotionAlpha(42)` 同一容器）。**已接到** `FVP_TWEEN_ALPHA` |
| `0x43B0D0`（**cmd 56 `MotionMoveS2Test`**） | 线程 `+0x814` | id 1..0xFFF；查 `engine+0x67CE64`（与 `MotionMoveS2(54)` 同族）。**已接到** `FVP_TWEEN_SCALE` |
| `0x43D6D0`（**cmd 73 `PrimGroupIn`**） | 图元树父子关系（`0x43D640(parent, child)`） | child int **1..0xFFF**、parent int **0..0xFFF**（**允许 0**），两个都合法才挂接；不置 flags。两部作品合计 349 次 |
| `0x43ACE0`（**cmd 50 `MotionMoveTest`**） | 线程 `+0x814`（返回值） | id 1..0xFFF；`0x4231A0(engine+0x65BA54, id)` 查该图元动作是否还在跑，**无条件**写返回值：1 = 还在跑。两部作品合计 447 次 |
| `0x43A8B0`（**cmd 42 `MotionAlpha`**） | 尾调 `0x42D460(engine+0x651E04, …)` | id 1..0xFFF；**from/to 各自 int 0..0xFF，缺/越界回退成当前 `prim+0x07`**；**ms 必须 int 1..300000，否则整条忽略**；mode 0..1（缺省 0）；flag = `type != 0`。两部作品合计 553 次 |
| `0x43B9C0`（**cmd 76 `PrimHit`**） | 线程 `+0x814`（返回值） | id 1..0xFFF；arg1 用 `type != 0` 当布尔；`0x43B8D0(id, flag)` 命中才把返回值置 1（未命中不写）。389 次（标题菜单命中就靠它） |
| `0x4379D0`（**cmd 79 `PrimSetDraw`**） | `prim+0x05`（可见性） | id **0..0xFFF**（允许 0）；arg1 **只接受 0 或 1**，其它保持原值。绘制分派第一步就是 `cmpb $0x0,0x05(%prim)`，为 0 直接返回（`0x438AB5`） |

**关键一致性事实**：这些字段全部用 `mov %xx,off`（**u16 写入**）、渲染时按 int16 读回，
所以 x/y/w/h/op/u/v 实际是 **int16**。我们原来存 32 位原值——本轮已改成 int16 截断
（`FVP_S16`），超出范围时才与原版不同。

读侧证据单独冻结在 `ref/exe-prim-fields-width.json`（门禁
`tests/verify_exe_prim_fields_width.py`）：每个字段都有一条 `movswl`（有符号 16 位扩展）
的读取点，落在绘制/分派区（`0x424xxx~0x42Bxxx`）：

| 字段 | 偏移 | 读取点 |
|---|---|---|
| x / y | `0x14` / `0x16` | `0x42450E` / `0x424518` |
| w / h | `0x18` / `0x1A` | `0x42541C` / `0x424665` |
| u / v | `0x1C` / `0x1E` | `0x42AED5` / `0x42AEE2` |
| op1 / op2 | `0x20` / `0x22` | `0x42450A` / `0x42AF2A` |
| angle / scale | `0x24` / `0x26` | `0x424596` / `0x42B25A` |

⇒ 范围 **-32768..32767**，写侧（u16）+ 读侧（movswl）双边证据齐全。

仍未知（`unknown` 里写着）：这些 handler 与命令 id 78/81/89/90 的对应还是**推断**、
`prim+0x06` 的 `0x40` 位在渲染端怎么用、`0x43BE40` 里 `+0x30`（-1..0xFF）的语义。

### 图元类型分派与 snow（A 类：`ref/exe-prim-type-dispatch.json`）

绘制前的类型分派在 `0x438AFA`：`type = (int8)prim+0x04`、`index = type-1`，
越界（`>6`）直接返回，否则跳到 `0x438C9C` 的**7 项跳表**：

| type | 名字 | 落点 | 说明 |
|---|---|---|---|
| 1 | group | `0x438B0F` | 递归子图元 |
| 2 | tile | `0x438BE1` | |
| 3 | （空） | `0x438C94` | 直接返回 |
| 4 | sprite | `0x438B54` | |
| 5 | text | `0x438C54` → `0x42AE40` | 文字面板绘制（与文字基线一致） |
| 6 | （空） | `0x438C94` | 直接返回 |
| 7 | **snow** | `0x438C83` → `0x42AA10` | 雪 |

**snow 的结构**（`0x42AA10`）：
- `prim+0x2E`（有符号 u16）= **雪系统下标**（不是布尔开关！）；
- 记录在 `engine+0x684378 + idx*0x5048`（每条 `0x5048` 字节）；
- 启用标志在 `engine+0x689378 + idx*0x5048`，为 0 就完全不画；
- 这个容器正是读档恢复顺序里 `0x43F3D4` 恢复的 `0x431E70(engine+0x684378)`。

也就是说：`PrimSetSnow(id, idx, x, y)` 里那个 0..1 的参数是**选 0 号或 1 号雪系统**。

### 雪系统记录的字段访问图（A 类：`ref/exe-snow-system.json`）

把 `0x42AA10` 里所有 `0x5xxx(%edi)` 访问全量抄出来（10 个偏移 / 10 次访问）：

| 偏移 | 读写 | 类型 | 说明（有证据的才写） |
|---|---|---|---|
| `+0x500C` | read | u32 | **精灵下标**（`imul $0x51c` 后在 `0x1014C0` 的 sprite 表里取条目） |
| `+0x5010` | read | u32 | 粒子条数（`dec %eax` 当上界） |
| `+0x5014` | read | u32 | 另一维计数（同样 `dec` 后当上界） |
| `+0x501C/0x5020/0x5024` | read | int→float | 三个 `fildl` 参数，进浮点运算 |
| `+0x5028` | read | u32 | 整型参数 |
| `+0x503C/0x5040/0x5044` | read | u32 | 三个整型参数，两两做差（`0x5044-0x503C`、`0x5040-0x5044`） |

设备方法调用（vtable 偏移）：`0xE4`、`0x104`、`0x10C`、`0x164`，共 10 处。
粒子位置/速度/寿命的**公式**与各 render state 常量仍是未知（写在基线 `unknown` 里）。

### 图元字段的写入点全量扫描（A 类：`ref/exe-prim-setters.json`）

为了把 `PrimSetXY(90)/PrimSetWH(89)/PrimSetOP(81)/PrimSetBlend(78)` 的实参语义从
"我们的实现"提升到"原版的实现"，先在 exe 里做了一次**全量扫描**：在
`0x43A000..0x43C900` 里找出**所有**往 0x34 步长图元结构体写字段的指令。

结果很干净：**57 条，全部集中在 `0x43BACA..0x43BFDA` 这一段**（跨度 < 0x600 字节）：
`x/y/w/h`（`0x14/0x16/0x18/0x1A`）、`op1/op2`（`0x20/0x22`）、`0x1C/0x1E`、
`angle`（`0x24`）、`scale_x/scale_y`（`0x26/0x28`）。也就是说那几个 setter handler
就挤在这一小段里。

**仍然未知**（写在基线的 `unknown` 里）：命令 id（78/81/89/90）与具体 handler
的对应关系——那需要先定位命令分派表，本轮没钉死，所以不给猜测。


## 两部作品实际用到的命令（`ref/command-usage.json`）

把两部作品的脚本全量扫一遍（`ref/fvm_ref.py` 解码），统计 `syscall` 立即数：

| 作品 | 指令数 | syscall 总数 | 用到的命令 id 数 | 引擎未实现 |
|---|---|---|---|---|
| 五彩斑斓的世界 HD | 948,219 | 5,064 | **131** | **0** |
| 星空的记忆 HD | 1,135,159 | 4,983 | **131** | **0** |

最后一条缺口是 `PartsMotionTest(69)`（`0x43B4B0`：槽号 **1..0x3F**，把"该槽部件动作
是否还在跑"写进当前线程 `+0x814`）。我们的引擎不跑部件动画，所以该查询恒返回 0——
与我们的模型一致，也不会让脚本卡在等动画的循环里。

门禁 `tests/verify_command_usage.py`（`run_all` 6.49655）钉住：两部作品都是 131 个
id、`missing_in_engine` 为空、除 `AudioLoad(0)` 外每个 id 都能在分派表里找到 handler。


### ColorSet(7) = 256 项 BGRA 调色板（A 类：`ref/exe-colorset.json`）

handler `0x4373E0`（两部作品合计 244 次）：

| arg | 角色 | 落点 |
|---|---|---|
| 0 | index int **0..0xFF** | 调色板项 |
| 1 | R | `engine+0x6A44C0 + idx*4` 的 **+2** |
| 2 | G | **+1** |
| 3 | B | **+0** |
| 4 | A | **+3** |

每个分量各自判 `type==2`，非 int 的保持原值；分量没有范围检查（截断成字节）。

所以 `engine+0x6A44C0` 就是 **256 项 BGRA 调色板**：
- 写点 = `ColorSet`；读点 = 图元绘制里 type-2（tile）路径的
  `mov 0x6a44c0(%edi,%eax,4),%edi`（索引色查表）；
- 与读档恢复顺序里那条 `memcpy 0x400 → engine+0x6A44C0` 是同一块缓冲
  （**本轮纠正**：它原来被标成"图形槽 → 图元 id 表"，实际是调色板）。

我们原来 `case 7` 是**空操作**——本轮实现了 `fvp_vm_color_set()`（写 `vm->palette[256][4]`，
BGRA，逐分量 has 标志）。渲染端暂时还不采样它（tile 索引色查表是后续一轮）。

### GraphLoad(22) 的实参规则（A 类：`ref/exe-graph-load.json`）

handler `0x43A340`（两部作品合计 454 次）：

| arg | 角色 | 规则 |
|---|---|---|
| 0 | slot | int **0..0xFFF**（允许 0；sprite 表就是 4096 项） |
| 1 | name | 按 Variant 类型分派：**type 0（null/false）→ 释放该槽**（`0x424150`）；type 4（数据段串偏移）→ `*(engine+0x68DFAC)+payload`；type 5（动态串表槽号）→ `*(engine+0x6A49EC+idx*4)`；其它忽略 |

解出非 NULL 串 → `0x428BD0(sprite_entry, str, 0)` 加载，随后 `0x437330(engine+0x45C, id)`。

**我们原来的缺口**：只在 arg1 是字符串时加载，**type 0 的释放没实现**——脚本用
`GraphLoad(slot, null)` 收回槽时，我们会保留旧图（画面残留）。本轮已补上。

顺带印证：type 5 的"动态字符串表"就是 `engine+0x6A49EC` 那张 0x80 项 × 4 的表
（与读档恢复顺序第 3 步同一个容器）。

### MotionMove(48) 的实参布局（A 类：`ref/exe-motion-params.json` 的 `arg_layout`）

`MotionMove` 是两部作品合计 **480 次**的命令，argc **8**。handler `0x43ABD0`：

| arg | 角色 | 规则 |
|---|---|---|
| 0 | id | int **1..0xFFF** |
| 1 / 2 | **from_x / from_y** | int；缺/非 int → 当前 `prim+0x14/0x16` |
| 3 / 4 | **to_x / to_y** | int；缺/非 int → 当前 x/y |
| 5 | ms | int **1..300000**（必须） |
| 6 | mode | int **1..5**（必须） |
| 7 | flag | 任意类型，`type != 0` → 1 |

判据（真实调用点 `pc=0x5B304`）：压栈序列 `[405, true, true, 65016, true, 1, 1, true]`
——from 两个都传 `true`（= 用当前位置），**to_x = 65016**（int16 即 -392），ms=1、mode=1。
也就是"把 405 号图元横向挪到 -392"。

我们原来把 arg1/arg2 当**目标**，这条调用会变成"什么都不动"——本轮按 (from,from,to,to)
顺序修正，并补上 ms/mode 的范围校验。

### 七条 `*Test` 命令的容器表（A 类：`ref/exe-motion-tests.json`）

| 命令 | handler | 容器 | 我们的接线 |
|---|---|---|---|
| `MotionAlphaTest(44)` | `0x43A9C0` | `engine+0x651E04` | `FVP_TWEEN_ALPHA` ✅ |
| `MotionAnimTest(47)` | `0x43AB10` | `engine+0x65380C` | — （帧动画未建模，返回 1） |
| `MotionMoveTest(50)` | `0x43ACE0` | `engine+0x65BA54` | `FVP_TWEEN_MOVE_XY` ✅ |
| `MotionMoveRTest(53)` | `0x43AF20` | `engine+0x679A5C` | — （旋转未建模） |
| `MotionMoveS2Test(56)` | `0x43B0D0` | `engine+0x67CE64` | `FVP_TWEEN_SCALE` ✅ |
| `MotionMoveZTest(59)` | `0x43AE00` | `engine+0x680A6C` | — （Z 未建模） |
| `V3DMotionTest(145)` | `0x437CE0` | `engine+0x683F04` | — （3D 未建模；**无 id 参数**） |

共同形状：`id 1..0xFFF` → 查询对应容器 → `setne` → **无条件**写线程 `+0x814`
（1 = 还在跑）。

**交叉印证**：这张表里的七个容器（`0x651E04/0x65380C/0x65BA54/0x679A5C/0x67CE64/
0x680A6C/0x683F04`）全部出现在读档恢复顺序的 `restore_steps` 里——两套独立抄出的
数据对得上。

---

## 附：浮游旁白的面板几何链条（2026-11-26）

用户要求（正文渲染要求 #2）：浮游旁白没有可见对话框，位置取自脚本 `PrimSetText`
的显式几何——面板原点画布 (461.25, 435.375)、面板 1500×540。来源链条（HCB 静态）：

```
0x367B0  pushint16 246 / 258 / 800 / 320 ; pushfalse
0x367BD  call 0x39F27            ; 几何 helper（缺省 230/522/736/120）
0x39F82  pushint16 551 ; pushint8 0 ; x ; y ; call 0x35C10   ; PrimSetText(551,0,x,y)
0x39F90  pushint8 0 ; w ; h ; call 0x35D76                   ; TextBuff(0,w,h)
0x39FA1  syscall 113                                         ; TextFont
0x35C10  ×1.875（0x3FF00000 = 1920/1024）
0x35D76  ×1.875 与 ×1.6875（0x3FD80000 = 1080/640）
```

⇒ (246×1.875, 258×1.6875) = **(461.25, 435.375)**；(800×1.875, 320×1.6875) = **(1500, 540)**。

门禁 `tests/verify_text_geometry.py` 冻结上面每一条（helper 结构、四个立即数、
两个 float 常量、画布几何）。

**未完成**：Python 参考模型在含「……羽根だ。」的那一步（step≈47000）里 `tw0` 有台词，
但没有任何 type-5 图元绑到 `tw0`，`prim 551` 那一刻是 `type=0`（被 `PrimSetNull`
清过）。也就是说旁白最后走的是"未绑定窗口的版面表兜底"，并没有画到 (461.25, 435.375)。
下一步要查：脚本为什么在旁白前把这个图元清成 null，或未绑定窗口是否应沿用上一次
`PrimSetText` 的坐标。

### 补充：旁白绑定块执行 0 次（2026-11-26）

`prim 551` 的定位/绑定在 HCB `0x36782..0x367C2`：先把窗口 4/3/2/1/551 挂进组
（`call 0x23804`），再 `call 0x39F27` 用 (246,258,800,320) 建文本图元 + 面板 +
字体，最后 `MotionMoveTest(244)` 等位移动画。

实测（`--steps 900000 --pctrace`）：`0x36782 / 0x367A0 / 0x367BD / 0x367C2` **全部 0 次**；
46 个 `pushint16 551` 站点里只有 3 个执行过（`0x23748` PrimGroupIn ×8、
`0x3A860` PrimSetAlpha(551,255) ×1、`0x3A8C9` PrimSetAlpha(551,0) ×1）。

旁白那一帧（step≈47000）`prim 551` 只有 `PrimSetAlpha`×2 与 `PrimSetDraw`×1 碰过，
没有 `PrimSetText`，所以 `type=0 / tw=-1` —— 没有绑定是必然的，不是渲染层丢的。
