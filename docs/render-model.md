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
| +0x20/+0x22 | u16 | OP 参数（语义未明） | `PrimSetOP` |
| +0x24 | u16 | 旋转角，单位 0.1°（%3600） | `PrimSetRS/RS2`（`idiv $0xe10`） |
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
* 旋转：`prim.angle` 单位 0.1°，`PrimSetRS2` 还能分别给 X/Y 缩放。渲染层目前只对 UI 类做旋转。
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
| 118/119/120 | TextFormat / TextFunction / TextOutSize | 7/4/3 | 排版参数（渲染层只记 out_size） |
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
6. **Z 不参与排序**（按 group 树序），`PrimSetOP` 写进去的 +0x20/+0x22 未用于渲染，`PrimSetBlend=1` 用加色近似。
7. 未实现：`Dissolve`(14)、`GraphRGB`(23)、`PartsMotion`(66)、`V3D*`、`Movie`、`Snow*`、
   `TextFont/TextFormat` 的真实排版、`SaveThumbSize` 缩略图。
8. `graph-not-found`：`taiken_start`、`title_trial`、部分 `SHINKU_e01*`、`graph/BG062_500(-b)`
   确实不在 6 个图像包里（多半在 `graph_vis*` 之外的包或运行时合成），属资源缺失而非解析错误。

---

## 8. 调试工具速查

```bash
# 每个 syscall 的实参（含返回值、栈深度、线程数）
$PY fvp/work/render_probe.py --steps 900000 --click 4000 --out fvp/work/render_calls_900k.txt

# 值栈平衡检查（argc 是否正确）
$PY fvp/work/render_stackcheck.py --steps 600000

# HCB 静态分析（按字节偏移）
$PY fvp/work/render_scriptdump.py 0x58430 0x58480

# exe 静态分析（按 VA）/ 按 VA 读 .rdata（vtable、RTTI）
$PY fvp/work/render_dis.py 0x43bfa0 0x43c060
$PY fvp/work/render_pe.py dwords 0x45d98c 8

# 图像包索引 / 单条查询
$PY fvp/work/render_assets.py "graph/BG063_000" "graph_bs/CHR_真紅_基_白衣制服"
```

`render.py` 里 `RenderVM.stats` 是「命令名 → 调用次数」，`graph_log`/`text_log` 记录每次
`GraphLoad`/`TextPrint`/`TextClear` 的步数与参数，排查"哪一帧该有什么"很方便。