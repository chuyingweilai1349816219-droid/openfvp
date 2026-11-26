# FVP 引擎侦查报告（阶段 0）

> 以下内容由 DeepSeek 生成,不一定代表作者个人意愿

目标游戏：`いろとりどりのセカイ HD Re:GENERATION` / 《五彩斑斓的世界 HD》
样本：`C:\Users\Mechrevo\Downloads\五彩斑斓的世界HD`（9.36 GB，73 个文件）

本文只记录**已经验证过**的结论，推测部分单独标注。

---

## 1. 引擎与文件构成

| 项 | 结论 | 依据 |
|---|---|---|
| 引擎 | FVP（FAVORITE VIEW POINT SYSTEM），闭源 | 主程序版本信息、社区资料 |
| 主程序 | `iroseka_HD.exe` 1.0.0.95，PE **i386** | PE 头 `machine=0x14c` |
| 渲染 | Direct3D 9（`d3d9.dll` / `d3dx9_31.dll`） | 导入表、字符串 `D3DXCreateTexture` |
| 音频 | DirectSound 8 + Ogg Vorbis | `DSOUND.dll`、`OggS`、`oggsound::SoundBuffer` |
| 视频 | WMV（`movie\01.wmv`，234 MB），`Movie`/`IAMovieSetup` | 字符串与目录 |
| 中文化 | `FVPLoaderGui.exe` + `FVPKernel.dll` 钩 GDI 字体（`CreateFontIndirectW`/`NtGdiHfontCreate`） | 导入表、X'moe 版本信息 |

资源包（7 个）：

```
graph.bin      516 MB  ← 事件 CG / 立绘 / 背景，HZC1 多帧格式
graph_bg.bin   786 MB
graph_bs.bin   675 MB
graph_vis.bin  1.62 GB
graph_vis1.bin 1.99 GB
graph_vis2.bin 1.83 GB
bgm.bin        448 MB  / bgm2.bin 634 MB / voice.bin 1.04 GB / se*.bin 175 MB
```

脚本：`iroseka.hcb`（4,275,282 B，**日文原版，Shift-JIS**）与 `.iroseka.hcb`（4,369,357 B，**简体中文，GBK**）。
两者结构完全一致，中文版由 loader 替换注入；中文文本是 GBK 双字节，**不是 UTF-16**。

---

## 2. BIN 包格式（格式已完全确认并实测）

```
+0x00  u32  文件数
+0x04  u32  名字表总长度
+0x08  N × { u32 name_off, u32 data_off, u32 size }     // 12 字节/条
       u32 × N 之后紧跟名字表
名字表  SJIS 字符串，NUL 分隔，偏移相对名字表起点
数据区  data_off 为文件内绝对偏移
```

第 0 条数据的典型头：`68 7a 63 31` = `hzc1`。

---

## 3. HCB 脚本格式（格式已完全确认，198,337 处跳转 100% 落地）

```
+0x00            u32  脚本长度 script_len
+0x04            ...  脚本体（指令流），到 script_len 结束
+script_len      u32  主脚本起点（main_start）
+script_len+4    ...  附加数据（命令表 + 结尾 2 字节 00 00）
```

### 3.1 指令流

栈式虚拟机，40 个 opcode：

| opcode | 名字 | 操作数 |
|---|---|---|
| 0x00 | nop | – |
| 0x01 | initstack | i8 局部数, i8 ? |
| 0x02 | call | u32 绝对地址 |
| 0x03 | **syscall** | **u16 命令号** |
| 0x04/0x05 | ret / ret2 | – |
| 0x06 | jmp | u32 |
| 0x07 | jmpcond | u32 |
| 0x08/0x09 | pushfalse / pushtrue | – |
| 0x0a/0x0b/0x0c | pushint32 / 16 / 8 | u32/u16/u8 |
| 0x0d | pushfloat | u32 |
| 0x0e | **pushstring** | u8 长度（含 NUL）+ 字节 + NUL |
| 0x0f | pushglobal | u16 全局变量号 |
| 0x10 | pushstack | u8 局部变量号 |
| 0x11/0x12/0x17/0x18 | 未确认 | u16/u8/u16/u8 |
| 0x13/0x14 | pushtop / pushtemp | – |
| 0x15 | popglobal | u16 |
| 0x16 | copystack | u8 |
| 0x19–0x27 | neg add sub mul div mod test logand logor eq neq gt le lt ge | – |

验证：`call` 184,798 处、`jmp` 5,398 处、`jmpcond` 8,141 处，**目标全部落在指令边界上，无一处错位**。

### 3.2 命令表（关键发现）

`.hcb` 的附加数据段自带命令表，**它就是 syscall 操作数的编号来源**：

```
+0x00  7 字节头: 0d 06 bf 05 0e 00 26
+0x07  游戏标题（SJIS，NUL 结尾）: "いろとりどりのセカイ HD Re:GENERATION"
       u16 条目数（本例 148）
       条目 × N: { u8 argc, u8 名长（含 NUL）, 名字字节..., 00 }
末尾   00 00
```

- 已解析：148 条，id 0..147，导出在 `fvp/work/cmd_table.json`
- 交叉验证（用「syscall 前压栈个数」比对 argc）：131 个实际用到的 id 中 **103 个吻合（79%）**
- 强证据：id 7 `ColorSet`(argc=5) 有 115/117 处正好压 5 个；id 112 `TextColor`(argc=4) **66/66 全部压 4 个**；id 147 `WindowMode`(argc=1)

注意：**不要用 exe 的注册顺序去推 hcb 命令号**。exe 里那张注册表是按**字母序**建立的名字→handler 查找表，而 `.hcb` 里的命令表是**编译器声明序**，两者从 id 24 起就分叉了（有 13 个 id 会错位）。

正确的做法是：**命令号以 `.hcb` 内嵌的命令表为准**（它同时给出 argc 和名字），handler 地址去 `fvp/work/vm_id_handler.json` 按 id 查，那张表是语义组用「hcb 表顺序 → 名字 → 注册表二分查找」重新对齐过的。

分发机制（已从静态分析确认）：

```
id    = *(i16*)(code + pc);  pc += 2
entry = vec[id]                       // 加载 hcb 时按命令表顺序 push_back 出来的数组
argv  = &stack[sp - entry->argc]      // 指向最早压入的那个参数
call entry->handler(argv)
```

### 3.3 脚本规模

```
指令           948,219 条
字符串          55,461 个（平均 38.3 字节，>=20 字节的 34,850 个）
syscall          5,064 处 / 131 种命令
call            184,798 处 / 2,227 个不同目标
initstack        3,435 处  ← 函数（或线程体）起点
主脚本起点       0x22f48
```

### 3.4 文本显示惯用法（已确认）

```
0x072fda  pushint8 1                  ; 循环条件
0x072fdc  jmpcond 0x7303c
0x072fe1  pushglobal 1429             ; 线程 id
0x072fe4  pushstring "それは例えば、…"
0x07302b  syscall 123                 ; TextPrint  ← 只消费字符串
0x07302e  syscall 133                 ; ThreadNext ← 消费线程 id，让出执行权
0x073031  pushglobal 1429
0x073034  syscall 111                 ; TextClear
0x073037  jmp 0x72fda
```

结论：**FVP 是协程/线程驱动的**，文本逐段输出后 `ThreadNext` 让出，由主循环调度。
解释器必须实现线程栈，不能只做单线程解释。

文本层实测（跑通后从输出里看出来的）：

| 层号 | 用途 |
|---:|---|
| 0 | 正文对话框 |
| 8 | 人名 |
| 9 | 章节标题 / 地点名 |

### 3.5 VM 执行语义（原型验证通过）

以下几条是写解释器必须知道的，已用 Python 原型跑到 90 万步验证：

| 机制 | 语义 |
|---|---|
| `jmpcond`(0x07) | 弹出条件，**为假时跳转**（等价于 `if (!cond) goto`） |
| 参数传递 | 调用时参数落在被调帧的槽位 246–254；`pushstack i` 越界时按 `idx = i - (255 - 参数个数)` 取参数窗口 |
| 返回值 | 命令返回值**不进栈**，写进 `temp` 寄存器；`pushtemp`(0x14) 把它压栈 |
| 协程 | `ThreadNext`(133) 让出；`ThreadStart/ThreadWait/ThreadExit` 管理线程 |
| `initstack a b` | 为当前帧分配 `a` 个局部槽位 |
| 测试类命令 | `*Test` 命名（`MotionAlphaTest`、`PartsMotionTest`…）返回 1 = 动画结束，0 = 继续等待；脚本用「Test + ThreadNext」做等待循环 |

原型里一条重要经验：**未实现的 `*Test` 命令必须返回 1**，否则脚本会卡在等待循环里空转（曾导致 24 万次无效调用）。

---

## 4. 常用命令（按调用点数量）

| id | 命令 | argc | 调用点 |
|---:|---|---:|---:|
| 77 | PrimSetAlpha | 2 | 487 |
| 48 | MotionMove | 8 | 473 |
| 42 | MotionAlpha | 6 | 270 |
| 50 | MotionMoveTest | 1 | 229 |
| 22 | GraphLoad | 2 | 219 |
| 76 | PrimHit | 2 | 211 |
| 73 | PrimGroupIn | 2 | 174 |
| 79 | PrimSetDraw | 2 | 174 |
| 31 | InputGetDown | 0 | 147 |
| 45 | MotionAnim | 4 | 120 |
| 7 | ColorSet | 5 | 117 |
| 90 | PrimSetXY | 3 | 105 |
| 88 | PrimSetUV | 6 | 100 |
| 34 | InputGetState | 0 | 91 |
| 91 | PrimSetZ | 2 | 80 |

148 条里 **17 条从未被本作使用**：`ExitDialog, FlagGet, FlagSet, InputGetEvent, InputSetClick,
LipAnim, LipSync, MenuMessSkip, PartsMotionPause, PartsMotionStop, PrimGroupMove,
SoundSilentOn, SysAtSkipName, TextPos, ThreadRaise, ThreadSleep, TimerSuspend`。
即实际需要实现的是 **131 条**。

---

## 5. 待解决

1. `syscall` 的参数在 VM 栈上的顺序（先压的是第 1 个还是最后一个）
2. 已知 argc 的命令里，少数几条实测压栈数与 argc 不符（`MotionMove` argc=8 但 474 处压 10 个），需确认是否表里的 argc 有特殊含义
3. `0x11/0x12/0x17/0x18` 四条未确认指令的语义
4. HZC1 图像格式细节（见 `hzc1-format.md`）
5. 每条命令的画面语义（见 `vm-commands.md`）

---

## 6. 工具

| 文件 | 用途 |
|---|---|
| `work/hcbstat.py` | 脚本统计（opcode/syscall/字符串直方图） |
| `work/dis.py` / `dis2.py` | 静态分析（dis2 会解析 syscall 命令名） |
| `work/findsc.py` | 按命令号找调用点 |
| `work/findstr.py` | 按字符串长度找上下文 |
| `work/cmdtable.py` | 解析 hcb 内的命令表 |
| `work/argcheck2.py` | 用压栈数交叉验证命令号映射 |
| `work/stats.py` | 整体结构统计 |
| `tools-src/vn-tools-x/.../bin_archiver` | BIN 包解包/打包 |