# FVP (FAVORITE VIEW POINT SYSTEM) — VM 分发机制与命令语义

> 以下内容由 DeepSeek 生成,不一定代表作者个人意愿

目标：`iroseka_HD.exe`（PE32 i386，ImageBase 0x400000）+ `iroseka.hcb`（五彩斑斓的世界 HD / いろとりどりのセカイ HD Re:GENERATION）。
本文所有地址均为 VA，静态分析取自 `objdump -d` 输出 `fvp/work/iroseka_HD.asm`。

---

## 0. 结论速览

1. **`syscall` 操作数 = hcb 附加数据段命令表的条目下标（0..147），不是 exe 的注册下标。**
   引擎启动时扫描当前目录 `*.hcb`（`FindFirstFileA("*.hcb")` @0x438ef0），读取文件头：
   `u32 script_len` → 在 `script_len` 处读 `u32 main_start` → `u16 A, u16 B, u8 winmode, u8 pad, u8 title_len` → 标题 →
   `u16 n` → n 条 `{i8 argc, u8 len, name+NUL}`。
   对每条命令名，在“已注册命令表”（按名字排序的 vector，engine+0x6a66d8）里做二分查找（`lstrcmpA`），
   **校验 argc**，然后把该 24 字节 Entry* 依次 `push_back` 到 **engine+0x6a66e8** 这个 vector 里。
   → 因此分发数组的顺序 = hcb 表顺序 = 脚本里的编号；数组首指针存在 **engine+0x6a66f0**。
2. **syscall 分发（0x445940）**：
   `id = *(i16*)(code+pc); pc += 2;`
   `vec = engine->[0x6a66f0]; entry = vec[id];`
   `argv = &ctx->stack[ctx->sp - entry->argc];`
   `engine_arg = engine + entry->[0xc]; call entry->handler(argv)`（ecx = engine_arg，实际为 engine）。
3. **参数顺序**：VM 值栈是 8 字节 Variant 数组（`{i32 type, i32 value}`），**先压的在低地址**；
   `argv[0]` = 最先压入的参数 = 第 1 个参数；handler 里 `0x4(argv)`=arg1、`0xc`=arg2、`0x14`=arg3……
4. **返回值**：handler 往线程上下文的 `ctx+0x814(type)/+0x818(value)` 写返回值；脚本用 opcode `0x14 pushtemp` 把它压栈。
5. **`exe 注册下标 = hcb_id+1` 的规则只在部分区间成立**：exe 注册顺序是字母序，hcb 表是编译器声明序，
   从 id 24 起出现分叉；**必须按名字映射**。13 个 id 会被错配（见 §4），其中 Motion* 一组错得最严重。

---

## 1. VM 主体

### 1.1 线程上下文（thread context，0xb28 字节）

`engine + 0x68dfc0 + threadIndex * 0xb28`，最多 32 个（`0x440f3f` 处初始化 0x20 个）。
字段（由 opcode handler 反推）：

| 偏移 | 含义 |
|---|---|
| +0x000 | engine 指针（构造时写入，0x4454e6 `mov %eax,(%edx)`） |
| +0x004 | 脚本数据基址 = engine->[0x68dfac]（0x4454e8） |
| +0x008 | 值栈起点：Variant[?]，每项 8 字节 |
| +0x808 | 线程状态 flags（bit1=wait, bit2=sleep, bit4=dissolve wait…） |
| +0x80c | 等待/计时剩余（ThreadWait/ThreadSleep 写入） |
| +0x810 | 立即挂起标志（ThreadWait/ThreadSleep 置 1，VM 主循环检查） |
| +0x814 | **syscall 返回值**：type（0=null/false, 2=int, 5=字符串句柄…） |
| +0x818 | 返回值 value |
| +0x81c | **PC**（脚本内字节偏移） |
| +0x820 | **SP**（栈顶下标，单位为 Variant） |
| +0x824 | 帧基址（frame base，供 pushstack 等使用） |
| +0x828 | 字符串句柄引用计数表（0x100 × u16） |
| +0x928 | 另一类字符串句柄引用计数表（0x100 × u16） |

### 1.2 opcode 分发表

opcode 分发表在 `.data` 的 **0x4815e0**，40 项（0x00..0x27），由初始化函数 **0x4463e0** 在启动时填：

```
4463e0: movl $0x41a550,0x4815e0     ; 0x00 nop
4463ea: movl $0x445890,0x4815e4     ; 0x01 initstack
4463f4: movl $0x4458c0,0x4815e8     ; 0x02 call (局部函数调用，压返回地址)
4463fe: movl $0x445940,0x4815ec     ; 0x03 syscall  <-- 本任务的入口
...
446566: movl $0x4452e0,0x48167c     ; 0x27 ge
```

（注意：这是**运行期**填表，所以静态扫 PE 找不到 40 项代码指针数组。）

VM 主循环 **0x4454b0**：

```
4454b0: mov  0x81c(%esi),%eax        ; pc
4454b6: mov  0x4(%esi),%ecx          ; code base
4454b9: movzbl (%ecx,%eax,1),%ecx    ; opcode = code[pc]
4454bd: movzbl %cl,%edx
4454c0: inc   %eax
4454c1: mov  %eax,0x81c(%esi)        ; pc++
4454c7: mov  0x4815e0(,%edx,4),%eax  ; opcode_table[opcode]
4454ce: mov  %esi,%ecx               ; this = thread ctx
4454d0: call *%eax                   ; opcode_handler(ctx)
4454d2: cmpb $0x0,0x810(%esi)        ; 线程未挂起则继续
4454d9: je   0x4454b0
```

**`jmpcond`(0x07) 的判据是"类型字节"，不是数值**（handler **0x445a30**）：

```
445a30: decl 0x820(%ecx)      ; sp -= 1
445a36: mov  0x820(%ecx),%eax
445a3c: lea  0x8(%ecx,%eax,8),%edx   ; &stack[sp]  (值结构 8 字节，type 在 +0)
445a40: mov  0x81c(%ecx),%eax        ; pc
445a47: mov  0x4(%ecx),%esi          ; code base
445a4a: mov  (%eax,%esi,1),%esi      ; 目标地址 u32
445a56: cmpb $0x0,(%edx)             ; 值.type == 0 ?
445a59: jne  0x445a63
445a5b: mov  %esi,0x81c(%ecx)        ;   是 → 跳
...
445a63: push %edx; call 0x444600     ;   否 → 释放值后继续
```

也就是说这个 VM 用**值的 type 字段**表达真假：比较类 opcode（`eq`/`neq`/`gt`/`le`/`lt`/`ge`）
把结果 `0/1` 写进**类型字节**（`sete %cl; mov %cl,(%esi)`），`jmpcond` 只在 type==0 时跳。
实现里用"数值真值"表达等价行为即可（本站的 `src/vm.c` 就是这么做的），但**不要**把它
当成"数值 == 0 才跳"——异类型的值行为不一样。

**比较 opcode 用哪张表、`sete` 还是 `setne`（2026-11 复核）**：四个 handler 除了这两点
完全一样，表 A = `0x481450`、表 C = `0x481388`，两者的 int 比较器分别是
`0x4450a0: cmp; setg`（"a > b"）和 `0x445200: cmp; setl`（"a < b"）：

| opcode | handler | 表 | set 指令 |
| --- | --- | --- | --- |
| 0x24 | 0x445060 | A | `setne` |
| 0x25 | 0x445180 | C | `sete` |
| 0x26 | 0x4451c0 | C | `setne` |
| 0x27 | 0x4452e0 | A | `sete` |

⚠️ **这里还差一步没能钉死**：表中"a/b"到底是"先压入的在前"还是"后压入的在前"，取决于
handler 把哪个指针作为第一个参数传给比较器。按 cdecl（最后压入的是第一个参数）读出来
是"先压入的在前"，据此 0x25 应为 `>=`、0x27 应为 `<=`；但**按这个把 0x25/0x27 对调之后，
iroseka 90 万步的文本输出直接变成 0 行**（这两条在 90 万步里被执行 88,325 次：0x27 58,880 +
0x25 29,445），而现在的实现跑出来是 195 行连贯台词。所以**现有实现的对应关系被实测反证为
可用，静态分析这边还缺一个证据**（可能是参数顺序或比较器内部取值顺序），在这点上本站
不改语义，只把矛盾如实记下来。

比较运算本身走 **7×7 类型对表** `0x481450`（`0x481388`/`0x481518` 是并排的另外两张同形状表）：
索引 = `a.type * 7 + b.type`（`445307: mov 0x481450(,%edi,4),%eax`），启动时由
`0x446580` 的循环把全部 49 项填成 `0x444f50`（返回 0），再把 16/17/23/24/32/33/39/40
这些类型对覆盖成专用比较函数（`0x4450a0`/`0x4450c0`/`0x4450f0`…）。所以
**异类型比较的默认结果是"相等"**（cmp 返回 0）——`ge` 因此在异类型上为真。

### 1.3 syscall 分发（0x445940）—— 核心证据

```
445940: push %ebx
445941: push %ebp
445942: push %esi
445943: push %edi
445944: mov  %ecx,%esi                ; esi = thread ctx
445946: lea  0x814(%esi),%edi         ; &ctx->retval
44594c: push %edi
44594d: call 0x444600                 ; 释放上一次返回值（若是字符串句柄）
445952: mov  0x81c(%esi),%eax         ; pc
445958: mov  0x4(%esi),%ecx           ; code base
44595b: movb $0x0,(%edi)              ; retval.type = 0 (false/null)
44595e: movswl (%ecx,%eax,1),%ecx     ; id = *(i16*)(code+pc)   <-- u16 操作数
445962: add  $0x2,%eax
445965: mov  %eax,0x81c(%esi)         ; pc += 2
44596b: mov  (%esi),%eax              ; eax = engine
44596d: mov  0x6a66f0(%eax),%edx      ; edx = dispatch_vector->buffer
445973: mov  (%edx,%ecx,4),%ebp       ; entry = dispatch[id]        <-- 24B Entry*
445976: mov  0x820(%esi),%ecx         ; sp
44597c: sub  0x4(%ebp),%ecx           ; sp -= entry->argc
44597f: lea  0x8(%esi,%ecx,8),%edx    ; argv = &ctx->stack[sp]      <-- 参数块首地址
445983: mov  0xc(%ebp),%ecx
445986: add  %eax,%ecx                ; ecx = engine + entry->[0xc] (注册时传 0 → engine)
445988: mov  0x8(%ebp),%eax           ; handler = entry->handler
44598b: push %edx                     ; 唯一的栈参数 = argv
44598c: call *%eax                    ; handler(argv)
...
4459a0: decl 0x820(%esi)              ; 逐项弹掉 argc 个参数
4459ac: mov  0x8(%esi,%edi,8),%al     ; v.type
4459b0: cmp  $0x5,%al
4459b2: jl   0x4459fd                ; type<5 无需释放
4459b8: ... 对 type==5/6 的字符串句柄做引用计数减一并释放
```

由此得到两条硬结论：

* **id 是 hcb 命令表的下标**：`dispatch[id]`，该数组由脚本加载器按 hcb 表顺序 push_back 生成，
  不是 exe 注册下标（注册表是按名字排序的另一个 vector，engine+0x6a66d8）。
* **参数顺序**：`argv = &stack[sp - argc]`，而 VM 压栈是 `stack[sp++] = v`（见 §2），
  所以 **`argv[0]` 就是最先压入的那个参数**，即脚本里第一个 push 的值对应命令的第 1 个参数。

### 1.4 Entry 结构与注册函数 0x4387d0

注册块（0x43f8d0 函数体内，共 149 段，形如）：

```
sub  $0x10,%esp
mov  %esp,%eax
mov  $0x439ce0,%ecx      ; handler
mov  %ecx,(%eax)
mov  %edx,0x4(%eax)      ; 0
mov  %edi,0x8(%eax)      ; 0
mov  %ebx,0xc(%eax)      ; 0
push $0x2                ; argc
push $0x45e3e4           ; "AudioPlay"
mov  %esi,%ecx           ; this = engine
call 0x4387d0
```

0x4387d0 分配 24 字节 Entry 并填充、插入名字表：

```
4387d2: push $0x18 ; call 0x447c05        ; operator new(0x18)
4387e8: mov %eax,(%esi)                   ; Entry+0x00 = name ptr
4387e5: mov %ecx,0x4(%esi)                ; Entry+0x04 = argc
4387ee: mov %edx,0x8(%esi)                ; Entry+0x08 = handler
4387f5: mov %eax,0xc(%esi)                ; Entry+0x0c = 0
4387fc / 438809: +0x10, +0x14 = 0
43880c: call 0x4346f0                     ; name→Entry 有序 vector 插入
```

### 1.5 脚本加载器（0x438eb0）与分发数组的构建

```
438eeb: lea  0x1c(%esp),%eax ; push %eax
438ef0: push $0x45dbb4                    ; "*.hcb"
438ef7: call *0x45d0bc                    ; FindFirstFileA
438f38: push %eax ; call *0x45d0b8        ; FindClose
438f3f: lea  0x48(%esp),%edx              ; cFileName
438f43: lea  0x68dfac(%edi),%esi          ; stream {buf,size,offset}
438f4c: call 0x4344c0                     ; CreateFileA/GetFileSize/ReadFile 整个文件
438f51..438f5f: u32 @offset → engine+0x68dfb4（同时就是 stream.offset = script_len）
438f65..438f73: u32 @script_len → engine+0x6a66d4   ; = main_start
438f79..438f88: u16 → engine+0x6a66c8     ; 本例 1549
438f8e..438f9d: u16 → engine+0x6a66cc     ; 本例 1471
（两者相加 ×8 字节 = 0x68dfbc 处的一张数组，与命令分发无关）
438ff3: movsbl ...  ; winmode 索引（查 0x45db50 的 16 项表）
43906f: incl 0x8(%esi)                    ; 跳过 1 字节
439072..4390a2: u8 title_len → 标题字符串 → engine+0x6a7e68
4390a7: u16 → n（本例 148）
4390ba 起每条: argc=movsbl, len=movzbl, name=base+off
4390e6: call 0x434320                     ; 名字二分查找（lstrcmpA）
4390f7: cmp %edx,0x4(%eax)                ; 校验 argc == Entry->argc
439107: call 0x434260                     ; push_back 到 engine+0x6a66e8
```

`0x434260` 是 vector::push_back（buffer=+8, size=+4），因此 **engine+0x6a66e8 = vector<Entry*>，
元素顺序 = hcb 命令表顺序**；`engine+0x6a66f0` = vector.buffer（push_back 增加时可能 realloc，
syscall 每次都重新读 buffer 指针，见 0x44596d）。

> 顺带解释引擎启动顺序（0x440e60 函数）：初始化 1000 个线程槽 → 注册 149 条命令（0x43f8d0）
> → 清零 flag 区 → 清 engine+0x6a66c4=-1 → **调用 0x438eb0 加载 hcb** → 初始化 32 个线程上下文
> (0x68dfc0+i*0xb28) → `0x4453f0(main_thread, main_start)` 建主线程 → 0x439220 启动。

---

## 2. 值栈与参数顺序

opcode handler 直接给出 Variant 布局（0x0a pushint32 / 0x0b pushint16 / 0x0c pushint8 / 0x0d pushfloat / 0x0e pushstring）：

```
445a70 (pushint32):
445a70: mov  0x820(%ecx),%eax       ; sp
445a76: lea  0x1(%eax),%edx
445a79: mov  %edx,0x820(%ecx)       ; sp++
445a7f: movb $0x2,0x8(%ecx,%eax,8)  ; stack[sp].type = 2 (int)
445a84: mov  0x81c(%ecx),%edx       ; pc
445a8e: mov  (%edx,%esi,1),%esi     ; 立即数 (i32)
445a9a: mov  %esi,0xc(%ecx,%eax,8)  ; stack[sp].value
```

即 **栈项 = 8 字节 {i32 type @ +0, i32 value @ +4}，sp 递增方向为高地址**；
handler 里读 `0x4(argv)`、`0xc(argv)`、`0x14(argv)`… 分别对应 arg1、arg2、arg3。

**Variant type 取值**（由各 push opcode 与命令 handler 的类型校验汇总）：

| type | 含义 | 证据 |
|---|---|---|
| 0 | false / null（pushfalse；也用作“可选参数省略”） | 0x444940 `movb $0x0,0x8(...)` |
| 1 | true（pushtrue） | 0x444960 `movb $0x1,0x8(...)` |
| 2 | int | pushint8/16/32；大量 handler `cmpb $0x2` |
| 3 | float | 0x445b10 `movb $0x3,...` + flds/fstps |
| 4 | 字符串字面量：value = 相对脚本基址的偏移 | 0x445b50 写 type=4、value=pc（字符串内联在脚本里） |
| 5 | 字符串句柄：value = engine->[0x6a49ec + idx*4]（0x4445e0 分配） | TextPrint/GraphLoad `cmp $0x5` 分支 |
| 6 | 字符串变量/数组元素（带引用计数，另一张表 ctx+0x928） | 0x445cb0 / 0x445d30 |

**参数顺序实测**（脚本 → handler）：以 `TextColor(112)` 为例，脚本压 4 个值后 syscall，
handler 依次读 `0x4` / `0xc` / `0x14` / `0x1c`，分别 = 先压的第 1/2/3/4 个参数。
脚本侧 `syscall 109 (SysProjFolder)` 之前只压了 1 个 `pushstring ""`，handler 也取 `0x4`，吻合。
**结论：先压 = arg1，不需要反转。**（这也是脚本编译器把参数从左到右依次 push 的直接结果。）

---

## 3. 返回值（pushtemp）

命令 handler 通过写线程上下文的返回槽返回值，脚本用 opcode 0x14 取回：

```
0x4449f0 (opcode 0x14 pushtemp):
4449f0: mov  0x814(%ecx),%edx      ; retval.type
4449f6: mov  0x820(%ecx),%eax      ; sp
4449fc: mov  %edx,0x8(%ecx,%eax,8) ; push type
444a00: mov  0x818(%ecx),%edx
444a06: mov  %edx,0xc(%ecx,%eax,8) ; push value
444a0a: incl 0x820(%ecx)
444a10: movb $0x0,0x814(%ecx)      ; 清空返回槽
```

脚本里高频出现的 `syscall X; pushtemp; pushfalse; eq` 就是“取返回值并和 false 比较”，
用于 `...Test` 类命令（MotionMoveTest / PrimHit / InputXXX 等）。**实现解释器时：syscall 后
返回值不会自动入栈，必须实现 `pushtemp`（0x14）并在 handler 里写 ctx+0x814/0x818。**

---

## 4. 编号↔命令 映射的修正（重要）

`fvp/work/cmd_table.json`（hcb 附加数据）给出的 id→name/argc 是**权威**的。
但 **exe 注册下标不能用 `id + (id>=7 ? 1 : 0)` 得到**：exe 的 149 条注册是按 **名字字母序**
（AudioLoad…WindowMode，BREAKPOINT 插在 7），而 hcb 表是**编译器声明序**，二者在 id 24 起分叉：

| 区间 | exe 顺序与 hcb 顺序 |
|---|---|
| 0..23 | 一致 |
| 24..26 | hcb: IntToText, HistoryGet, HistorySet；exe 字母序: HistoryGet, HistorySet, …, IntToText |
| 27..48 | 重新对齐（区间内一致） |
| 49..56 | **严重错位**（见下） |
| 57..93 | 重新对齐 |
| 94..95 | hcb: SaveThumbSize, SaveData；exe: SaveData 在 SaveThumbSize 前 |
| 96..147 | 一致 |

错位的 13 个 id（shift 规则会指向错误 handler）：

| id | name | argc | 正确注册下标(按名字) | shift 规则会错指成 |
|---|---|---|---|---|
| 24 | IntToText | 2 | 27 | HistoryGet |
| 25 | HistoryGet | 2 | 25 | HistorySet |
| 26 | HistorySet | 2 | 26 | IntToText |
| 49 | MotionMoveStop | 1 | 56 | MotionMoveR (argc6) |
| 50 | MotionMoveTest | 1 | 57 | MotionMoveRStop |
| 51 | MotionMoveR | 6 | 50 | MotionMoveRTest (argc1) |
| 52 | MotionMoveRStop | 1 | 51 | MotionMoveS2 (argc8) |
| 53 | MotionMoveRTest | 1 | 52 | MotionMoveS2Stop |
| 54 | MotionMoveS2 | 8 | 53 | MotionMoveS2Test (argc1) |
| 55 | MotionMoveS2Stop | 1 | 54 | MotionMoveStop |
| 56 | MotionMoveS2Test | 1 | 55 | MotionMoveTest |
| 94 | SaveThumbSize | 2 | 96 | SaveData (argc3) |
| 95 | SaveData | 3 | 95 | SaveThumbSize (argc2) |

**验证（脚本“syscall 前连续 push 个数” = argc 的统计）**：131 个实际使用的 id 中，
110 个（84%）的主峰 push 数 == hcb argc；错位区间里 shift 规则会直接矛盾：

| id | hcb argc | 脚本 push 主峰 | 按名字映射 (正确) | shift 规则映射 |
|---|---|---|---|---|
| 49 MotionMoveStop | 1 | 1 ×71 | MotionMoveStop argc1 ✓ | MotionMoveR argc6 ✗ |
| 51 MotionMoveR | 6 | 6 ×18 | MotionMoveR argc6 ✓ | MotionMoveRTest argc1 ✗ |
| 54 MotionMoveS2 | 8 | 8 ×61 | MotionMoveS2 argc8 ✓ | MotionMoveS2Test argc1 ✗ |
| 95 SaveData | 3 | 3 ×41 | SaveData argc3 ✓ | SaveThumbSize argc2 ✗ |

（若引擎真按 shift 下标取 argc，脚本在 id 54 会少弹 7 个参数、栈立即崩塌；
因此引擎分发必然按 **hcb 表顺序**，与 §1.5 的加载器行为一致。）

完整的 id→name→argc→handler 对照已导出：**`fvp/work/vm_id_handler.json`**
（148 条；`vm_mapping.json` 另含 shift/name 两种映射的对照与用量）。

---

## 5. 高频命令语义

约定：`prim[id]` = `engine + 0x61dd64 + id*0x34`（id 有效范围 1..0xFFF；PrimSetAlpha 的越界检查
是 `1 <= id < 0x1000`）。`tw[i]` = `*(engine+0x98+i*4)`（文本窗口对象，i=0..31，取自全局
`g_481368`，实测 == engine）。偏移为 prim 结构内偏移。

| # | id | 命令 | argc | 参数（栈序） | 语义 | 关键证据 |
|---|---|---|---|---|---|---|
| 1 | 77 | PrimSetAlpha | 2 | (primId 1..0xFFF, alpha 0..255) | 设 primitive 透明度，写 `prim.alpha(+7)` | 0x437999-0x4379bd：`cmp $0x1000` / `cmpl $0xff` / `mov %dl,0x61dd6b(%eax,%ecx,1)`（=prim+7） |
| 2 | 48 | MotionMove | 8 | (primId, x, y, z, ?, timeMs 1..300000, mode 1..5, flag≠0) | 位置移动动画；未给的坐标沿用 prim 当前 X/Y(+0x14/+0x16)，提交给 motion 管理器 engine+0x65ba54 | 0x43abfe/0x43ac0a/0x43ac1f 校验 arg6=时间、arg7=模式；0x43ac3b 读 `0x61dd78/0x61dd7a`；0x43ac94 `lea 0x65ba54(%ebp)` + call 0x42d560 |
| 3 | 42 | MotionAlpha | 6 | (primId, fromAlpha?, toAlpha?, timeMs 1..300000, mode? 0..1, flag≠0) | 透明度补间；from/to 省略则取 prim 当前 alpha(+7) | 0x43a8d6 校验 arg4=time；0x43a919/0x43a93f 读 `0x61dd6b`；0x43a97a `add $0x651e04,%ecx` + call 0x42d460 |
| 4 | 50 | MotionMoveTest | 1 | (primId) | **返回 bool**：该 prim 的位置移动是否仍在进行 | 0x43ad13 call 0x4231a0(engine+0x65ba54,id) → `setne %al; mov %al,0x814(%esi)` |
| 5 | 22 | GraphLoad | 2 | (graphSlot 0..0xFFF, filename 或 null) | 把图片文件读入图形槽 `engine+0x101064+slot*0x51c`；arg2 为 null 时清空/释放槽 | 0x43a358 `cmpb $0x0,0x8` → 0x424150(slot)（无文件名）；否则 type4/5 → 0x428bd0(slot, name, 0)，再 0x437330(engine+0x45c, slot) |
| 6 | 76 | PrimHit | 2 | (primId 1..0xFFF, flag) | **返回 bool**：鼠标/光标是否命中该 primitive 的矩形。**已实现**：`gfx_prim_hit()`（`src/gfx.c`）+ `case 76`（`src/syscall.c`），三层算法（父链累加 / 类型分派矩形 / UV 检查）见 `docs/render-model.md` §8，单测 `tests/test_prim_hit.c` 47 项。⚠️ 矩形之后的逐像素掩码测试未实现（矩形内即算命中）；两部作品实测 0 命中，因为被测试的 11 个图元 `parent == -1`、graph 未加载 —— 原版同样返回 0 | 0x43b9f0 `cmp %dl,0x8` → flag；0x43b9fc call 0x43b8d0(id,flag)；0x43ba05 `movb $0x1,0x814`；父链累加在 0x43b8d0，矩形在 0x43b5f0 |
| 7 | 73 | PrimGroupIn | 2 | (groupId 1..0xFFF, groupId 1..0xFFF) | 把组嵌套/归入另一组（组播放入口） | 0x43d6fd push arg2, push arg1; call 0x43d640 |
| 8 | 79 | PrimSetDraw | 2 | (primId, 0/1) | 开关 primitive 是否绘制，写 `prim.draw(+5)` | 0x4379f4 `imul $0x34` + `mov %dl,0x61dd69(...)`（=prim+5） |
| 9 | 31 | InputGetDown | 0 | — | **返回 int**：当前按下的按键位掩码（engine->[0x43c]） | 0x437763 `movb $0x2,0x814`（ret type=int）；0x437770 `mov 0x43c(%ecx),%edx; mov %edx,0x818` |
| 10 | 45 | MotionAnim | 4 | (primId, animId 1..0xFFF, timeMs 1..300000, loop? 0/1) | 播放贴图/部件动画 | 0x43aa44-0x43aa6b 校验 arg2/arg3/arg4；0x43aa89 `add $0x65380c,%ecx` call 0x42d4e0 / 0x426ba0 |
| 11 | 7 | ColorSet | 5 | (colorIdx 0..255, R, G, B, A) 各 0..255 | 写调色板 `engine+0x6a44c0+idx*4`，字节序为内存 B,G,R,A（即 0xAARRGGBB） | 0x437402 `mov %bl,0x6a44c2`（R）→+2；0x437414 (+1 G)、0x437423 (+0 B)、0x437433 (+3 A)；0x43d520 处 memset 0x400 后 palette[0]=0xFF000000、[1]=0xFFFFFFFF |
| 12 | 90 | PrimSetXY | 3 | (primId, x, y) | 设屏幕坐标，写 u16 `prim+0x14/+0x16`，置 dirty/flags | 0x43bfb8 `lea 0x61dd64(%eax,%ecx,1)`；0x43bfcc `mov %cx,0x14(%eax)`、0x43bfda `0x16(%eax)`、0x43bfde `orb $0x40,0x6(%eax)` |
| 13 | 88 | PrimSetUV | 3 | (primId, u, v) | 设贴图 UV，写 `prim+0x1c/+0x1e`；要求 prim.type==4（贴图类） | 0x43bf0b `cmpb $0x4,0x61dd68`（=prim+4 type）；0x43bf2a `mov %cx,0x1c`；0x43bf3c `0x1e` |
| 14 | 34 | InputGetState | 0 | — | **返回 int**：输入状态位（engine->[0x444]） | 0x4378a3 type=2；0x4378b0 `mov 0x444(%ecx),%edx` → retval.value |
| 15 | 80 | PrimSetNull | 1 | (primId) | 重置/销毁 primitive（type→0 的初始化） | 0x43ba7b call 0x439bf0(id, 0)；0x439bf0 是按 type 初始化 prim 的公共函数 |
| 16 | 91 | PrimSetZ | 2 | (primId, z 100..10000) | 设 Z 值（u16 `prim+0x12`），钳制到 100..10000；arg2 为 bool 时仅重刷 flags；非 int 则清除 Z 标记 | 0x43c01c `cmp $0x64` / 0x43c035 `cmp $0x2710`；0x43c02e/0x43c042 `mov %dx,0x12(%ecx)`；0x43c051 起 bool/其他分支 |
| 17 | 54 | MotionMoveS2 | 8 | (primId, scaleX 100..10000, scaleY, ?, ?, timeMs 1..300000, mode 1..5, flag) | 缩放动画；省略时用 prim 当前缩放(+0x26/+0x28)，提交给 engine+0x67ce64 | 0x43af99 校验 arg6=time、arg7=mode；0x43afe9 读 `0x61dd8a/0x61dd8c`；0x43b07e `add $0x67ce64` + call 0x42d670 |
| 18 | 83 | PrimSetRS2 | 4 | (primId, angle, scaleX 100..10000, scaleY) | 设旋转角（`angle % 3600`，单位 0.1°）写 `prim+0x24`，X/Y 缩放分别写 `prim+0x26/+0x28` | 0x43bbba `mov $0xe10,%edi; idiv` 取余 → 0x43bbd0 `mov %dx,0x24`；0x43bbf9/0x43bc2d 写 0x26/0x28 |
| 19 | 49 | MotionMoveStop | 1 | (primId) | 停止位置移动动画 | 0x43accc `add $0x65ba54,%ecx; jmp 0x426ff0` |
| 20 | 82 | PrimSetRS | 3 | (primId, angle, scale 100..10000) | 旋转 + 统一缩放（X/Y 都写同一个值） | 0x43bb1e `idiv $0xe10` → +0x24；0x43bb6b、0x43bb4f 都写 0x26/0x28 |
| 21 | 112 | TextColor | 4 | (twIdx 0..31, R, G, B) 各 0..255 | 设文本窗口颜色：三个参数先经 `engine+0x6a491c` 的 dword 表转换，再分别存 `tw+0x598/0x59c/0x5a0`（表内容待运行时确认，见 §6-6） | 0x43c8e6 起分别 `cmpb $0x2`+`cmp $0xff` → 0x424800（R）/0x424830（G）/0x424890（B）；0x42480a `mov 0x6a491c(%edx,%eax,4),%eax`；0x424811 `mov %eax,0x598(%ecx)` |
| 22 | 9 | ControlPulse | 0 | — | 置 engine+0xfd8 = 1（控制脉冲/快进标志） | 0x437460 `movb $0x1,0xfd8(%ecx)` 后直接 ret |
| 23 | 20 | FloatToInt | 1 | (float) | **返回 int**：float 截断取整 | 0x4375b0 `cmpb $0x3,(%eax)`；0x4375bb flds 后 call 0x447c70；0x4375c3 `mov %eax,0x818`（type 已置 2） |
| 24 | 56 | MotionMoveS2Test | 1 | (primId) | **返回 bool**：缩放动画是否进行中 | 0x43b103 call 0x423bd0(engine+0x67ce64,id)；0x43b10c `movb $0x1,0x814` |
| 25 | 111 | TextClear | 1 | (twIdx 0..31) | 清空文本窗口 | 0x43c8af `mov 0x98(%ecx,%eax,4),%ecx`（tw）→ call 0x430d50(tw)（清空）；再 0x437370(engine, idx) |
| 26 | 44 | MotionAlphaTest | 1 | (primId) | **返回 bool**：透明度动画是否进行中 | 0x43a9ed `add $0x651e04,%ecx`; call 0x422990；`setne %al; mov %al,0x814` |
| 27 | 78 | PrimSetBlend | 2 | (primId, blend 0/1) | 设混合模式，写 `prim.blend(+8)`（arg2 必须 int 0/1） | 0x43ba4a `mov %dl,0x61dd6c(%eax,%ecx,1)`（=prim+8） |
| 28 | 123 | TextPrint | 2 | (twIdx 0..31, text) | 向文本窗口追加字符串。text 可为 type4（脚本内联字面量）或 type5（字符串句柄）；追加前用 lstrlenA 检查 <0x200 | 0x43cad4 `mov 0x98(%edx,%eax,4),%ebp`；0x43cadf type==4 → `engine->[0x68dfac]+value`；type==5 → `engine->[0x6a49ec+value*4]`；0x43cb01 lstrlenA；0x43cb51 call 0x433610(tw, str) |
| 29 | 24 | IntToText | 2 | (value, opt 1..10) | **返回字符串句柄**：把整数格式化为十进制文本（wsprintfA），分配字符串句柄并压返回值（type5）。**已实现**：`case 24`（`src/syscall.c`）用 arena 造同内容的 `FvpStr`；`opt` 没有证据说明影响格式，先忽略 | 0x437646 `push engine+0x481374; call wsprintfA`（格式串 0x472c4c="%d"）；0x437669 call 0x4445e0 分配句柄；0x43767a `movb $0x5,0x814` |
| 30 | 139 | TimerSet | 2 | (timerId 0..15, timeMs 1..100000) | 启动 16 个定时器之一：`engine+0x1a997f+id*12` 处 {active=1, elapsed=0, duration=ms} | 0x43cfbc `lea 0x1a997f(%eax,%eax,2),%eax; lea (%ecx,%eax,4)` → `movb $0x1,(%eax); movl $0,0x4; mov %edx,0x8`；**已实现**：`src/timer.c`，ms 钳到 1..100000 |

### 补充（同批分析，非前 30 命中）

| id | 命令 | argc | 语义 / 证据 |
|---|---|---|---|
| 0 | AudioLoad | 2 | 音频资源加载：engine+0x6a7e34 为音频子系统；arg1=0..3 通道。0x439c80 起 |
| 1 | AudioPlay | 2 | arg1 = 通道 0..3；从 `engine->[0x6a7e34]->[0x94+ch*4]` 取通道对象；arg2 为 null(type0) → 0x4410b0(chan,0,0)，否则 (chan,1,0)（播放/停止） |
| 18 | FlagGet | 1 | (flagIdx 0..0x7FF)：**返回 bool**，读全局 flag 位图 `engine+0x6a48c0` 的 bit；`setne → 0x814`。**已实现**：`src/flags.c`，越界返回 0 |
| 19 | FlagSet | 2 | (flagIdx 0..0x7FF, bool)：置/清 `engine+0x6a48c0` 的 bit（arg2 type==0 → 清位，否则置位）。**已实现**：arg2 为 int 0 即清位，越界是 no-op |
| 110 | TextBuff | 3 | (twIdx, buffNo?, buffSize?)：`0x430ea0(tw, arg2(默认8), arg3(默认8))`，文本缓冲配置 |
| 121 | TextPause | 2 | (twIdx, bool)：bool→`tw+0x5da`；arg2 为 null → 返回 int = `tw+0x5da` 并置线程文本状态 2 |
| 122 | TextPos | 3 | (twIdx, x, y)：u16 写 `tw+0x5e8`(x)/`tw+0x5ea`(y) |
| 124 | TextRepaint | 0 | 强制重绘文本：`0x441790(engine)` |
| 126 | TextSize | 3 | (twIdx, w 12..64, h 8..32)：字节写 `tw+0x592`(w)/`tw+0x594`(h)。**已实现**：存进 `FvpTextAttr`，按范围钳制 |
| 127 | TextSkip | 2 | (twIdx 0..31, mode 0..3)：字节写 `tw+0x5d7`。**已实现**：`fvp_textattr_set_skip()` 存进 `FvpTextAttr.skip_mode`（超范围整条命令忽略）。静态分析出处 **0x43CDD0**：`cmp $2,(%eax)` 校验 arg0 是 int、`test %ecx; jl` / `cmp $0x1f; jg` 校验 twIdx、`test %edx; jl` / `cmp $3,%edx; jg` 校验 mode、`mov %al,0x5d7(%ecx)` 落盘 |
| 113 | TextFont | 3 | (twIdx, fontId, fontId2)：**字体选择**。handler **0x43DA70**：arg0 必须是 int 0..0x1F；arg1 是 int 且 **-5 <= v <= (引擎字体数-1)** → 0x4247F0 写 `tw+0x590`（u16）；arg2 同一区间 → 0x424860 写 `tw+0x593`（u8）；两个分量各自独立判定，超范围的跳过。**已实现：只记录不换字模** —— 引擎字体号 -1/-2 对应哪套字模没有证据（我们只有 4 个图集），渲染仍按字号挑字体。实测 900k 步：`(0,-1,1)` 正文、`(8,-1,1)` 人名、**`(9,-2,1)` 章节标题**、`(10,-1,1)`；两部作品同形 |
| 118 | TextFormat | 7 | (twIdx, a, b, c, d, e, f)：**排版参数**。handler **0x43CC10**，六个字段各自独立判定（逐字段 jl/jg，超范围的字段单独跳过）：a/b → `tw+0x5A8`/`tw+0x5AA`（**-32..32，与 TextSpace 同一对字段**）；c → `tw+0x5AE`（0..64）；d → `tw+0x5AC`（0..64，且会把 `tw+0x5E8` 抬到不小于 d）；e → `tw+0x5B4`（-16..16）；f → `tw+0x5B0`（-16..16）。**已实现：只记录不参与排版**。实测 tw0 = (32,32,64,64,16,16)、tw8 = (32,1,1,1,1,1) |
| 119 | TextFunction | 4 | (twIdx, a, b, c)：**三个开关**。handler **0x43CCF0**：a → `tw+0x5A5`（**0..1**）、b → `tw+0x5A4`（**0..2**）、c → `tw+0x5A6`（**0..2**），各自独立判定（注意写盘顺序与字段号不一致）。**已实现：只记录**（三个开关的语义未知）。实测 tw0 = (1,2,2)，其它窗口都是 (0,0,0) |
| 125 | TextShadowDist | 2 | (twIdx, dist 0..12)：**阴影距离**。handler **0x43CBB0**：arg0 必须是 int 0..0x1F；arg1 不是 int 时按 0 处理，是 int 时 **<0 当 0、>12 当 12**（钳制），然后转 0x4248B0(tw, dist)，那里就是 `mov %al,0x597(%ecx)` —— **只存一字节**。**已实现**：`fvp_textattr_set_shadow()` + `gfx_text_set_shadow()`，**只记录不渲染**（原版阴影的形状/颜色/透明度都还没有证据）。实测脚本给的是 **dist=12**（tw 0 / 8 / 9 都设了）。⚠️ **2026-11-24 修正**：以前把 0x43CE10 当成这条命令，其实 0x43CE10 是 TextSpace(128)，见下一行 |
| 128 | TextSpace | 3 | (twIdx, x, y)：**字间距 / 行间距**。handler **0x43CE10**：arg1 / arg2 各自独立判定，落在 **-32..32** 才写 `tw+0x5A8`（i16）/ `tw+0x5AA`（i16），超范围的那一个分量跳过、另一个仍然生效。**已实现**：`fvp_textattr_set_space()` + `gfx_text_set_space()`，只记录不渲染。实测脚本给的是 **tw9 = (32, 1)**（章节标题：字间距拉满 32、行间距 1），其它窗口 (0,0) |
| 129 | TextSpeed | 2 | (twIdx, speed 0..300000)：dword 写 `tw+0x5e0`（每字毫秒） |
| 135 | ThreadSleep | 1 | (ms)：线程 flags |=4，`ctx+0x80c = ms`，`ctx+0x810 = 1`（挂起） |
| 136 | ThreadStart | 2 | (threadNo 0..31, entryOffset)：threadNo==0 先重置全部 32 个线程(0x444860)，再 0x4453f0(ctx, offset) 以 offset 为 PC 建线程 |
| 137 | ThreadWait | 1 | (ms)：flags |=2，ctx+0x80c=ms，ctx+0x810=1（等待） |
| 138 | TimerGet | 2 | (timerId 0..15, opt 1..10000)：**返回 int** 定时器已过时间；给了第 2 参数则返回 elapsed*arg2/duration（百分比）。**已实现**：`src/timer.c`，elapsed 由 `vm.virtual_ms` 推导（`--timeline` 关闭时恒 0 → 返回 0，与旧行为一致）；opt 换算用 64 位算再夹到 INT32_MAX |
| 14 | Dissolve | 7 | (timeMs 1..300000, mask 1..255 或遮罩文件名, l,t,r,b)：屏幕溶解/淡入淡出；`engine+0x101018`=时间、`+0x101010`=状态(3/4/5/6)、`+0x100ff0..ff6`=矩形 |
| 17 | ExitMode | 1 | (mode 0..4)：退出/返回相关引擎标志 engine->[0x81]/[0x82]/[0x6a7e72] |
| 75 | PrimGroupOut | 1 | (groupId 1..0xFFF)：组解散/移出 → 0x437290 |
| 15 | DissolveWait | 1 | 若溶解进行中（engine+0x101010≠0）：arg 为 null → 返回 false；否则置线程 flags|=0x10 并挂起（等待溶解结束） |
| 26 | HistorySet | 2 | (kind, value)：**台词历史（回想）写入**。引擎对象 `engine+0x6a49c0` = {count, pos, base}，记录 12 字节 = {人物名, 台词, 行 id}。handler **0x43A5E0**：arg0 为 null → count=min(count+1,100)、pos=(pos+1)%100、该槽清成 {NULL,NULL,-1}；arg0=0/1 → **仅当 arg1 是字符串**时写名字/台词（0x438DF0 取串，0x437E30 / 0x437E80 整串替换）；arg0=2 → **仅当 arg1 是整型且 ≥0** 时写行 id；其余整条忽略。**已实现**：`src/history.c` + `fvm --dump-history` |
| 25 | HistoryGet | 2 | (kind, idx)：**读历史**。arg0 为 null → 返回 count；arg0=0/1 → 返回该条名字/台词的字符串（字段为空时返回**空串**，0x43A518–0x43A540 先建空句柄再按需追加）；arg0=2 → 返回行 id，为 -1 时返回 null。idx 必须 `0 <= idx < count`，否则返回 null；槽位 **slot = pos − idx**（负数 +100 回绕），即 idx 0 是最近一条。证据 **0x43A490** + 0x438090/0x4380C0/0x4380F0。**已实现**：`src/history.c` |
| 41 / 108 / 141 | MenuMessSkip / SysAtSkipName / TitleMenu | 1 / 2 / 1 | **纯空操作**。三者注册的 handler 都是 **0x437950**，而 0x437950 整个函数体只有一条 `ret $0x4`（不读参数、不写状态、不设返回值）。所以这里"空桩"是与原版一致，不是缺口 —— 注意这三个 id 只是"被脚本调用时什么也不会发生"，不代表原版没有菜单（菜单在 exe 原生代码里，不经命令表） |

#### 台词历史（`HistorySet` / `HistoryGet`）实测

`HistorySet` 在两部作品里都只在"打印一句台词"时被调 4 次，形态固定：

| 形态 | iroseka（90 万步） | 星空のメモリア（90 万步） | 作用 |
|---|---|---|---|
| `(0, 字符串)` | 67 | 52 | 写人物名（实测 `"　　 悠馬　　 "` / `"　女の子　"`） |
| `(1, 字符串)` | 70 | 54 | 写台词正文 |
| `(2, 整数)` | 120 | 106 | 写行 id（实测 `2000130` / `600010`，序列 10/20/30/40…） |
| `(0\|1, 整数)` | 53 / 170 | 54 / 159 | **原版忽略**（类型不符），脚本发的是 `pushtrue` 的 1 |
| `HistorySet(null)` | **0** | **0** | 从不调用 |
| `HistoryGet` 动态调用 | **0** | **0** | 从不调用 |

所以 `count` 恒 0、`pos` 恒 0，历史里永远只有槽 0 那一条"当前台词"。这不是移植
缺陷 —— 原版程序里能让 `count` 增长的唯一路径就是 null 调用（另一个写 count 的
地方是存档反序列化 0x43F264）。实测 `fvm --dump-history` 的槽 0：
`value=2000130 text1="　指をさす。" text0="　　 澪　　 "`。
另外，槽尾快照里 0x43F264 处的 `count/pos/base` 就是这三样东西 —— 对以后解
存档快照有用。

### GaijiLoad(21) / V3DMotion(142)（**已实现：只记录参数**）

| id | 命令 | argc | 语义 / 证据 |
| ---: | --- | ---: | --- |
| 21 | GaijiLoad | 3 | `(char, size, image)`。handler **0x43f4f0**：第 1/3 个参数必须是字符串（原版 type 4/5），第 2 个必须是 int 且在 **12..0x40**；然后 `call 0x43e9c0(engine+0x6a49cc, str1, size, str2)`。实测参数形态：`size=64, "ハ", "graph/gaiji_heart68"` → **这是"外字"：把某个字符替换成 graph 包里的一张图** |
| 142 | V3DMotion | 6 | `(x?, y?, z?, timeMs, mode, flag?)`。handler **0x43cfe0**：arg4 必须 int 在 **1..0x493e0**（和 MotionMove 同一区间）、arg5 必须 int 在 **1..5**、arg6 当 bool，arg1..arg3 缺省取 `engine+0x6a65f0/f4/f8`。本作没有 3D 场景 |

两者本站都只记录（`gaiji_calls/size/name1/name2`、`v3d_calls/time_ms/mode`），
外壳与 `fvm` 汇总各打印一行。`GaijiLoad` 的渲染侧（字符 → 贴图）还没做，
是文本呈现里最后一块，见 `docs/roadmap.md` 的"下一步"。

### WindowMode(147) / ExitMode(17)（**已实现：只记录模式值**）

| id | 命令 | argc | 语义 / 证据 |
| ---: | --- | ---: | --- |
| 147 | WindowMode | 1 | handler **0x43d0d0**：参数必须 int 且 **-1..6**；模式 0/1 把窗口状态置 0/1（`engine->[0x8c]->[0x1fc]`）并把这个值作为返回值，模式 2 返回当前值（且当前值 ==2 时返回 **-1**），-1 走"切回"分支（还读 `[0x201]`，没读完） |
| 17 | ExitMode | 1 | handler **0x43a1a0**：参数必须 int 且 **0..4**；0 分支清 `engine+0x82` 并置当前线程 ctx 的 `+0x68e7d4 = 1`（"请求退出"一类），1..4 有各自分支 |

两个命令真正改的都是**窗口/线程层**字段，本站没有对应结构，所以只把最后一次的模式值
记进 `FvpVM`（`window_mode_last / window_state / exit_mode_last` + 调用计数），
外壳与 `fvm` 汇总打印 `window mode=.. state=.. ; exit mode=..`。

实测（iroseka 90 万步）：`WindowMode(0)` 1 次、`ExitMode(2)` 1 次。

### TextBuff(110)（**已实现：只记录尺寸**）

handler **0x43c830**（argc 3）：

```
43c834  cmpb $0x2,(%eax)        ; twIdx 必须是 int
43c840  cmp  $0x1f,%ecx         ; 且 0..31，否则整条返回
43c845  cmpb $0x2,0x8(%eax)     ; 第 2 个参数是 int?
43c864  test %edx,%edx          ;   且 >= 0 才采用
43c85d  mov  $0x8,%esi/%edi     ; 否则默认 8（第 3 个参数同理）
43c87b  call 0x430ea0(tw, a, b)
```

setter **0x430ea0**：`a`/`b` 任一 **> 0x400** 就整条放弃；否则写 u16
`tw+0x5dc = a`、`tw+0x5de = b`，并释放旧缓冲（`tw+0x520`）后重建。

所以这是"文本窗口的缓冲尺寸"（两个 0..0x400 的数，缺省 8/8），而不是排版参数。
本站只把两个数记进 `FvpVM.text_buff[32]`（我们的文本是按行存的动态串，没有固定缓冲
这一层），外壳/`fvm` 汇总打印 `text buff N call(s)`。

### GraphRGB(23) / SysProjFolder(109)（**已实现：只记录状态**）

| id | 命令 | argc | 语义 / 证据 |
| ---: | --- | ---: | --- |
| 23 | GraphRGB | 4 | `(graphId 0..0xFFF, r, g, b)`。handler **0x43a3d0**：槽号必须 int 且在 0..0xFFF；三个分量**各自**"是 int 且在 0..200"才采用，否则取默认 **100**（= 不调制）；然后 `call 0x428910(engine+0x101064+id*0x51c, r,g,b)` 并 tail-call `0x437330(engine+0x45c, id)` 通知刷新（和 GraphLoad 同一个"槽变了"回调） |
| 109 | SysProjFolder | 1 | `(folder)`。handler **0x43c7f0**：只接受 type 4/5（字符串）参数，其余直接返回；取到字符串后 tail-call `0x433c60(engine+0x6a7e78, str)` |

本站两个都**只记录**：`FvpVM` 里有 `graph_rgb[4096][3] + graph_rgb_set[]`（每个槽的
三通道）与 `proj_folder`（arena 里的字符串拷贝），外壳汇总打印
`graph rgb N call(s), folder=...`。不消费的理由：渲染端没有"100 是不是 1.0 倍、
200 是过曝还是叠色"的证据；工程目录字符串具体影响哪条资源路径也没解出。

实测（iroseka 45 s）：`GraphRGB` 调 4 次、`SysProjFolder` 调 1 次（参数是空串）。

### Control\* / Cursor\*（8 / 9 / 10 / 11，**已实现：只记录状态**）

| id | 命令 | argc | 语义 / 证据 |
| ---: | --- | ---: | --- |
| 8 | ControlMask | 1 | `engine+0x458 = (参数是 null)`。handler **0x437440**：`cmpb $0x0,(%eax)`（只看**类型字节**）+ `sete %cl` + `mov %cl,0x458(%edx)`，参数字面值不参与 |
| 9 | ControlPulse | 0 | `engine+0xfd8 = 1`。handler **0x437460**：`movb $0x1,0xfd8(%ecx)`；消费方在帧更新 0x43f6df 一带 |
| 10 | CursorChange | 1 | 参数必须是 int 且 0..3，否则整条命令无效；合法时 tail-call **0x441810** → `engine+0x88 = id`，并按 `engine+0x85/0x84` 换 Win32 光标（`*0x45d2c4` / `*0x45d250`） |
| 11 | CursorMove | 3 | `(x, y, force)`：x/y 必须是 int；鼠标设备可用时直接做，不可用时要求第三参数的**类型字节非 0**；满足则调 **0x4417c0(engine, x, y)** |

**本站的实现边界（照实写，不猜）**：这四个字段在 `FvpVM` 里都有对应状态
（`control_mask` / `pulse` / `cursor_shape` / `cursor_x, y`）+ 调用计数，外壳汇总
打印 `control mask=.. pulse=.. cursor=.. pos=..`。但：

* 原版用 **type==0 表示 null**，本站的值模型没有 null（`FVP_INT == 0`，未赋值的
  global 读出来是 int 0），所以 ControlMask 的"1 分支"**当前不可达**，CursorMove
  的第三参数判定也退化成"只记录类型"（`control_mask_type` / `cursor_force_type`）。
  这是**已知不可达**，不是猜出来的语义。
* 光标/屏蔽位的下游消费（换系统光标、影响动画时间轴、参与输入屏蔽）没有实现：
  没有证据说明"3 张光标位图对应什么形状"，也没有证据说明 0x458 具体屏蔽什么。

实测调用量（iroseka 45 s 外壳）：ControlMask ×38、ControlPulse ×35、
CursorChange ×1、CursorMove ×1；`unimpl cmds` 从 11 种降到 **7 种**。

### Motion\*Stop（43 / 46 / 55 / 58，**已实现**）

| id | 命令 | argc | 语义 / 证据 |
| ---: | --- | ---: | --- |
| 43 | MotionAlphaStop | 1 | (primId) 撤该图元的 alpha 补间。handler **0x43a990**；同组：`MotionAlpha`(42)、`MotionAlphaTest`(44, 0x43a9c0) |
| 46 | MotionAnimStop | 1 | (primId) 撤帧动画。handler **0x43aae0** |
| 55 | MotionMoveS2Stop | 1 | (primId) 撤缩放补间。handler **0x43b0a0** |
| 58 | MotionMoveZStop | 1 | (primId) 撤 Z 补间。handler **0x43add0** |

四条都是"动画被半路打断"时才调用：45 s 外壳实测 43×4、46×25、55×7、58×6。

本引擎的接法：补间表只有三类（`MOVE_XY` / `ALPHA` / `SCALE`，`src/motion.c`），
`fvp_motion_cancel(m, prim_id, kind)` 按"图元 + 种类"精确撤销（`kind < 0` = 全部），
`gfx_motion_cancel()` 只是薄封装，`src/syscall.c` 里 43 → ALPHA、55 → SCALE，
**46 / 58 没有对应种类 → 空操作**（原版同样是在空表上撤）。没有 gfx 上下文时
（`fvm.exe`）仍报未实现。单测 `tests/test_motion.c` 16 项（共 47）。

---

## 6. 尚未完全确证 / 下一步

1. **MotionMove 的 arg4/arg5 语义未定**：8 个参数中 (id, x, y, ?, ?, time, mode, flag) 已确定 6 个，
   arg4/arg5 的取值范围无法从 handler 判断（handler 只做类型检查后原样转发给 0x42d560/0x42d670）。
   验证方法：静态分析 motion 管理器 0x42d560（位置）与 0x42d670（缩放）的参数使用，或对脚本里
   arg4/arg5 的取值分布（vm_stats.py 输出）建模。
2. **PrimSetOP(81) 的 arg2/arg3 含义**（写 prim+0x20/+0x22，仅 type==4 生效）未定，疑为
   “不透明度/优先级”一类，需要看渲染端怎么读 prim+0x20。
3. 少数命令（Parts*/V3D*/Movie*/Snow*/LipSync*、TextFont*/TextFormat/TextFunction/TextOutSize/
   TextSuspendChr、SaveCreate/SaveWrite/Load/TitleMenu/MenuMessSkip、Debmess/BREAKPOINT）本次未展开；
   `fvp/work/vm_cmd_facts.json` 里已有它们的 handler 范围、argv 槽位读写、调用目标、字符串/全局引用，
   可直接接着做。
4. `PrimSetText(86)` 的 arg2 写入 prim+0x32（疑似“文本窗口编号”），arg3/arg4 写 X/Y；
   与 TextPrint 的 twIdx 关系还需用脚本样例确认（该命令只用 13 次）。
5. 字符串句柄 type 5/6 的两张表（ctx+0x828 / ctx+0x928）具体分工未明；解释器实现时按
   “syscall 后必须递减引用计数”处理即可与原引擎对齐。
6. **一个未解的数据布局异常**：引擎对象布局中 `engine+0x6a44c0..0x6a48c0` = ColorSet 的 256×4
   调色板（0x43d520 处 `memset(…,0,0x400)`，palette[0]=0xFF000000、palette[1]=0xFFFFFFFF），
   `engine+0x6a48c0..0x6a49c0` = flag 位图（0x440f03 `memset(…,0,0x100)`、FlagGet/Set 按 bit 访问）。
   但 TextColor 的转换表基址 `engine+0x6a491c` 落在 flag 位图内（0x6a48c0+0x5c），若按 256 项×4
   读取会越过 flag 区并覆盖 `engine+0x6a49c0`（字符串表基址）等字段。两种可能：
   (a) 该项实际上是恰好位于该区间的另一张短表（但参数上限 0xff 说明应有 256 项）；
   (b) 该表在运行期另行初始化/映射。**建议用调试器在 0x42480a 下断，dump
   `[g_481368+0x6a491c .. +0x400]` 确认内容与写入者**；解释器实现可先按“256 项 dword LUT”处理，
   或在未确认前直接把 R/G/B 当 0..255 颜色值做近似。

---

## 7. 复现脚本

| 文件 | 作用 |
|---|---|
| `fvp/work/vm_re.py` | PE 解析（段/VA↔文件偏移/字符串）、asm 静态分析索引、跳转表扫描、基础子命令 |
| `fvp/work/vm_lib.py` | 注册块解析（149 条 name/argc/handler）、hcb 表读取、注释工具 |
| `fvp/work/vm_map.py` | 验证 `id→handler` 的两种假设（shift vs name），输出 `vm_mapping.json` |
| `fvp/work/vm_stats.py` | 脚本侧逐命令参数统计（每个槽的取值直方图 + 样例点） |
| `fvp/work/vm_cmd.py` | 按 id 输出 handler 注释静态分析 + 自动提取 argv 槽位/调用/字符串/全局 |
| `fvp/work/vm_scan.py` | 静态分析查询小工具（show/near/xref/refs/callers） |
| `fvp/work/vm_imports.json` | IAT(0x45d000+) → dll!函数 对照（用于注释间接 call） |
| `fvp/work/vm_id_handler.json` | **最终 id→{name,argc,handler,use,reg_index,push_hist} 全表** |
| `fvp/work/vm_mapping.json` | shift/name 两套映射对照（含差异与用量） |
| `fvp/work/vm_reg_handlers.json` | 从静态分析重新抽取的 149 条注册记录 |
| `fvp/work/vm_cmd_facts.json` | 148 个 handler 的自动事实（槽位/调用/字符串） |
| `fvp/work/vm_handlers.txt` | 148 个 handler 的完整注释静态分析（441KB） |

运行示例（Python 用 `C:\Users\Mechrevo\.dsh\dsh-runtimes\dsh-primary-runtime\dependencies\python\python.exe`）：

```
python fvp/work/vm_map.py            # 重新生成映射并打印差异
python fvp/work/vm_stats.py 48 54 95 # 指定 id 的脚本参数统计
python fvp/work/vm_cmd.py 77 48 42   # 指定 id 的 handler 静态分析
python fvp/work/vm_scan.py show 0x445940 120   # VM syscall 分发
```