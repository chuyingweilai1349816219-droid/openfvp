# FVP 虚拟机 C 重写（引擎内核）

> 以下内容由 DeepSeek 生成,不一定代表作者个人意愿

状态：**完成**。C 版对冻结 Python 基准 **逐字节一致**，900k / 2M 两个验收用例全部通过，
并额外在 123457 / 777777 / 1500000 / 20,000,000 步上做了同样的比对。

---

## 1. 验收结果

| 用例 | C 输出 | 参照 | 字节/行数 | SHA256 | 结果 |
|---|---|---|---:|---|---|
| 主验收 900k | `fvp/out/c/c-text.txt` | `fvp/ref/ref-900k.txt` | 6496 / 195 | `7F9837FF…C5D3` | **PASS 逐字节一致** |
| 长跑 2M | `fvp/out/c/c-text-2000k.txt` | `fvp/ref/ref-2m.txt` | 15378 / 450 | `32D7D4E2…856E` | **PASS 逐字节一致** |
| 额外 20M | `fvp/out/c/c-text-20m.txt` | Python 20M 输出 | 40168 / 1170 | `B482F032…64A7` | **一致**（Python 实跑对照） |
| 额外步数 | — | Python 同参数 | 19 / 163 / 343 行 | — | 123457、777777、1500000 全部一致 |
| stdout（非 quiet） | — | Python 同参数 | 4972 字节 | — | **逐字节一致**（含 `[debmess]` 行与 summary） |

C 输出与基准的换行符也一致：**CRLF**（Python 在 Windows 上以文本模式写文件，`\n` → `\r\n`）。

冻结基准（`fvp/ref/`）：

| 文件 | SHA256 |
|---|---|
| `fvm_ref.py` | `1446D9D7DDAE548A68C27354E697C81568B656DB221EB517499ADDA0DF35FD00` |
| `cmd_table.json` | `88F5E2E4EBB09C914A8002540E53664112BF5670CC0E7F49665359E4742D0A36` |
| `cmd_argc.json`（修订后） | `0ECAD110CBF5360E10514C30440C90F666B2690D349302A778EF87AB7489D1CD` |
| `ref-900k.txt` | `7F9837FFAF3B7B5759581C625698C9554E83F2CDBDEADF635DCA6FC9C01B3EA2` |
| `ref-2m.txt` | `32D7D4E2DCE441738A79828D57EC6A7522CB639204141B5571BB42BF9BB3856E` |

C 侧内嵌的 argc / 名字 / returns 表由 `fvp/tests/gen_tables.py` 从 `fvp/ref/*.json`
生成（生成头文件里记录了这两个 JSON 的 SHA256），因此和冻结基准同源。

---

## 2. 构建

一条命令：

```powershell
powershell -ExecutionPolicy Bypass -File fvp\build.ps1
```

或者（PATH 里已有 gcc 时）：

```
cd fvp
mingw32-make -f Makefile
```

- 编译器：w64devkit gcc 16.2.0（`tools\w64devkit\w64devkit\bin`），`build.ps1` 会自动把该目录加进 PATH。
- 编译选项：`-std=c99 -O2 -Wall -Wextra -Wpedantic -fwrapv`
- **当前 0 warning**。
- 产物：`fvp/bin/fvm.exe`（约 244 KB）。
- 依赖：只有 C 标准库 + libm，**不依赖 zlib / Win32 API / iconv**，可直接交叉编译到 Android。

`src/gen_cp932.h`、`src/gen_gbk.h`、`src/gen_cmd_table.h` 是生成文件，已随源码提供，
所以构建只需要 gcc。需要重新生成时：

```powershell
python fvp\tests\gen_tables.py "C:\Users\Mechrevo\Downloads\五彩斑斓的世界HD\iroseka.hcb"
```

生成器会用 Python 自己的 codec 做交叉验证（见 §6.3）。

---

## 3. 用法

```powershell
# 主验收 / 长跑
fvp\bin\fvm.exe "C:\Users\Mechrevo\Downloads\五彩斑斓的世界HD\iroseka.hcb" `
    --steps 900000 --click 600 --quiet --text-out fvp\out\c\c-text.txt
fvp\bin\fvm.exe "C:\Users\Mechrevo\Downloads\五彩斑斓的世界HD\iroseka.hcb" `
    --steps 2000000 --click 600 --quiet --text-out fvp\out\c\c-text-2000k.txt

# 中文脚本（GBK）
fvp\bin\fvm.exe "C:\Users\Mechrevo\Downloads\五彩斑斓的世界HD\.iroseka.hcb" `
    --enc gbk --steps 900000 --click 600 --quiet --text-out out.txt
```

| 参数 | 含义 |
|---|---|
| `--enc cp932\|gbk` | 文本编码，默认 cp932（对应 Python 的 `--enc`） |
| `--steps N` | 指令预算，默认 2000000（对应 Python） |
| `--click N` | 每 N 步模拟一次点击，默认 0；验收用 600 |
| `--cursor X,Y` | 合成光标位置，默认 `960,540` |
| `--text-out FILE` | 输出 `[层] 文本`，**CRLF**，与 Python 一致 |
| `--quiet` | 不向 stdout 回显文本 / debmess |
| `--eol crlf\|lf` | 输出换行符，默认 `crlf`（POSIX 上想用 LF 时显式指定） |
| `--pctrace FILE` | 调试：逐条记录执行的 pc |
| `--savebin FILE` | 启动前用 `save/save.bin` 播种全局变量（只灌 `type==2` 的 int，见 `docs/save-format.md` §7.1） |
| `--dump-globals FILE` | 运行后导出非零 int 全局变量，每行 `index value` |

退出码：`0` 成功；`2` 参数错误；`3` 脚本加载/解码失败；`4` 写输出文件失败；`5` 读 `--savebin` 失败。

---

## 4. 代码结构

| 文件 | 职责 |
|---|---|
| `src/fvp.h` / `fvp.c` | 值模型（`FvpValue`/`FvpStr`）、Python 语义的相等/比较/真值、arena 分配器、`str(value)` 与 float repr |
| `src/encoding.h` / `encoding.c` | CP932 / GBK → UTF-8 流式解码（`decode(enc,'replace')` 语义），内含生成的 `gen_cp932.h` / `gen_gbk.h` |
| `src/hcb.h` / `hcb.c` | `.hcb` 加载（`script_len` / `main_start`）、整段指令解码、pc → 指令索引表、字符串驻留 |
| `src/vm.h` / `vm.c` | 解释器：操作数栈、调用帧、全局/局部变量、temp、garr/larr 字典、线程调度、Python 算术语义 |
| `src/syscall.h` / `syscall.c` | 命令分发表（148 条 argc/名字/returns + stub 处理器，返回策略与 Python 完全一致） |
| `src/main.c` | CLI、文本输出（CRLF）、summary 打印 |
| `src/gen_*.h` | `gen_tables.py` 生成的数据表（编解码 2 张、命令表 1 张） |

设计原则：**不做“看起来合理”的重新发明**，所有可观察行为（含 Python 的怪癖）逐条照抄
`fvm_ref.py`；每一条都写在 §6。

---

## 5. 设计说明

### 5.1 加载与解码（`hcb.c`）

- 文件头：`u32 script_len` + 脚本体（偏移 4 起）+ `u32 main_start` + 附加数据。
  样本：`script_len = 4,273,221 (0x413445)`，`main_start = 143,176 (0x22f48)`，文件 4,275,282 字节。
- 从偏移 4 顺序解码到 `script_len`，共 **948,219** 条指令（与 Python `Code.instr` 数量一致）。
- 每条指令存成定长 `FvpInstr`（pc、u32 操作数、u16 操作数、两个 u8、size、字符串指针）。
- 额外建 `int32_t pcidx[script_len]`：字节偏移 → 指令下标（-1 = 不是指令边界）。
  跳转/返回时 O(1) 查表；落点非法时与 Python 的 `Code.at() -> None` 一样让线程结束。
- `pushstring`：`u8 n` + n 字节（末字节是 NUL）；解码时丢弃末字节后再按编码转 UTF-8，
  **一次性驻留**（55,461 条字符串，UTF-8 共 3,161,830 字节）。
- 未知 opcode / 截断：报错并退出（Python 此时抛 `KeyError` / `struct.error`），绝不越界读。

### 5.2 值模型

Python 只有三种值：`int`、`float`、`str`（`bool` 也是 int）。C 侧：

```c
typedef struct FvpValue { uint8_t type; int64_t i; double f; FvpStr *str; } FvpValue;
```

- 字符串不可变、NUL 结尾、带长度；由 arena 分配（解码期 + 运行期拼接共用）。
- 真值：`0` / `0.0` / `""` 为假（NaN 为真，与 Python 一致）。
- 相等 `==`：数字跨类型按数值比较，字符串按内容，字符串 vs 数字为 `False`（不抛异常）。
- 排序 `< <= > >=`：数字按值、字符串按码点（UTF-8 字节序等价于码点序）；
  字符串 vs 数字按 Python 的 `TypeError` 处理 → `0`。

### 5.3 帧、栈与线程调度（`vm.c`）

严格对应 `fvm_ref.py`：

- 操作数栈每线程一份，动态增长；空栈 `pop` 返回 int 0。
- 帧字段：`ret`、`locals`、`args`、`pending`。
  - `call`：压入 `ret = pc+5`、`args = 整个操作数栈的拷贝`、`pending = 1`，随后清空栈，跳到目标。
  - 被调函数第一条 `initstack` 看到 `pending` 帧 → 分配 `a` 个局部槽位（0 → int 0），清 `pending`。
  - 若 `initstack` 没有 pending 帧（线程入口 / main_start），则**新建**一个 `ret = 当前 initstack 的 pc` 的帧（照抄 Python 行为）。
  - `pushstack i`：`i < len(locals)` 读局部；否则 `idx = i - (255 - nargs)` 读参数窗口，越界给 0。
  - `ret/ret2`：弹帧回 `ret`；无帧则线程死亡、pc 置 `script_len`。
- 线程调度：每轮 `pop(0)` → 死线程丢弃 → 否则 `append` 到队尾 → 若 `blocked` 则 `wait++`，
  `wait < 2` 时跳过本轮，否则清除 blocked 后执行一条指令。
  因此 `ThreadNext`/`ThreadWait` 标记 blocked 后会**空转一轮**再恢复，与 Python 完全一致。
- 全局数组 8192 项（`pushglobal` 参数 ≥ 8192 时压 0，`popglobal` 参数 ≥ 8192 时丢弃）。
- `garr_*` / `larr_*`（0x11/0x12/0x17/0x18）= Python 里 `(id, index)` 字典；
  C 侧实现为开放寻址哈希表，键的哈希/相等遵循 Python（`3` 与 `3.0` 是同一个键）。
  这四个 opcode 在样本 900k/2M 步里**一次都没执行**（静态各 1507/21/145/79 处，动态 0），
  实现只是为了将来脚本用到时不偏离语义。
- `neg`：int/float 取负；对字符串 Python 会抛 `TypeError` 直接崩溃，C 返回 0 并在 stderr 警告一次（见 §9）。

### 5.4 syscall 契约（`syscall.c`）

- argc 来自内嵌的 `fvp_cmd_argc[]`（源自 `ref/cmd_argc.json`）；参数从栈顶弹出后再反序，
  使 `args[0]` 是**最先压栈**的参数（等价 Python 的 `args.insert(0, pop())`）。
- 返回值**不进操作数栈**，写入 `t->temp`；`pushtemp`(0x14) 再取出。
- 返回值写入规则（逐字照抄 `do_syscall`）：
  1. 处理器返回非 None → `temp = 返回值`；
  2. 否则若命令名在 `returns` 集合（`*Test`、`InputGet*`、`Rand`、`IntToText`、`FlagGet`、
     `HistoryGet`、`TimerGet`、`TextTest`、`PrimHit`、`GraphLoad`、`InputFlash`、`SoundLoad`、
     `GraphRGB`、`SaveData`、`HistorySet`）→ `temp = 0`；
  3. 否则 `temp` **保持不变**。
- 未实现命令：先计入 unknown 统计；若名字以 `Test` 结尾 → `temp = 1`（“动画已结束”兜底），
  否则按第 2/3 条规则。
- 已实现 stub（与 Python 完全一致）：
  `123 TextPrint`、`111 TextClear`、`31 InputGetDown`（`steps % click < 2` 返回 1）、
  `36 InputGetUp=0`、`29/30` 光标 960/540、`34 InputGetState=0`、`32 InputGetEvent=0`、
  `28 InputGetCursIn=1`、`15 DissolveWait=1`、`44/47/50/53/56/59 Motion*Test=1`、
  `145 V3DMotionTest=1`、`62 MovieState=1`、`131 TextTest=0`、`22 GraphLoad=1`、
  `133 ThreadNext` / `137 ThreadWait`（标记 blocked）、`13 Debmess`、
  `143/144/146/61/64/14`（返回 None）。

### 5.5 文本输出（`main.c`）

- 运行时把每次 `TextPrint(layer, text)` 追加到文本列表；`text` 非字符串时按 Python `str()` 转换。
- 结束时按 `[<layer>] <text>` 逐行写文件；`layer` 用 Python `%d` 语义（int 十进制；float 截断）；
  文本内容里的 `\n`（如果有）同样翻译成 CRLF。
- **默认 CRLF**：Python 在 Windows 上以文本模式写文件，`\n` 会变 `\r\n`；
  验收基准里 130/195/450 行全部是 CRLF，C 已对齐。Android/POSIX 若需要 LF 用 `--eol lf`。

### 5.6 编码表（`gen_tables.py` + `encoding.c`）

- CP932：55 个 lead byte、逐字节表 256 项 + 55×192 的 pair 表。
- GBK：126 个 lead byte、256 项 + 126×192 pair 表。
- 解码规则与 CPython `'replace'` 一致：非法/截断双字节序列输出 `U+FFFD` 并**只消费 1 字节**，
  后续字节继续按起始字节解析；单字节扩展（如 CP932 的 `0xA0=U+F8F0`、`0xFD=U+F8F1`）照抄。
- 生成器自检（每次生成都跑）：
  - 全部 256 个单字节、全部 65,536 个双字节序列、30,000 组随机 3–24 字节序列，
    与 `bytes.decode(enc,'replace')` **0 处不同**；
  - 对 `iroseka.hcb` 的 **55,461 条 pushstring** 逐条比对，**0 处不同**。
- 因此 C 的文本解码与 Python 逐字节等价，不依赖 iconv / Win32 / ICU。

---

## 6. 与 Python 的逐条对应（保真清单）

### 6.1 指令（40 个 opcode 全部实现）

| opcode | Python 行为 | C 实现 |
|---|---|---|
| 0x00 nop | **没有分支 → 线程死亡** | 同（照抄怪癖；样本里 0 条 nop） |
| 0x01 initstack | pending 帧复用，否则新建帧（ret=自身 pc） | 同 |
| 0x02 call | args=整个栈拷贝、清栈、pending=1 | 同 |
| 0x03 syscall | `do_syscall`，pc+=3 | 同 |
| 0x04/0x05 ret | 弹帧；无帧则死亡且 pc=script_len | 同 |
| 0x06 jmp | pc=目标 | 同 |
| 0x07 jmpcond | **条件为假时跳转** | 同 |
| 0x08/0x09 | 压 int 1 / 0 | 同 |
| 0x0a/0x0b/0x0c | 压 u32（无符号）/u16/u8 | 同 |
| 0x0d pushfloat | u32 位型按 float32 解释 | 同（memcpy 位型） |
| 0x0e pushstring | 解码字符串入栈，pc+=size | 同 |
| 0x0f pushglobal | `arg<8192 ? globals[arg] : 0` | 同 |
| 0x10 pushstack | 局部 / 参数窗口（`i-(255-nargs)`） | 同 |
| 0x11/0x12/0x17/0x18 | garr/larr 字典读写（索引从栈弹） | 同（哈希表） |
| 0x13 pushtop | 栈顶帧 locals[0] | 同 |
| 0x14 pushtemp | 压 temp | 同 |
| 0x15 popglobal | 写入 globals | 同 |
| 0x16 copystack | 弹值写 locals[i] | 同 |
| 0x19 neg | 取负 | 同（字符串差异见 §9） |
| 0x1a–0x27 | add/sub/mul/div/mod/test/logand/logor/eq/neq/gt/le/lt/ge | 同（Python 语义，见下） |

算术细节照 Python：
- `div`：除数为假值 → int 0；否则**真除法**（int/int 也返回 float）；
- `mod`：除数为假值 → int 0；int%int 取 Python 地板模（符号随除数）；float%float 用
  `fmod` + 符号修正 + `copysign(0, b)`（与 CPython `float_rem` 相同）；
- `add`：int+int→int；含 float→float；str+str→拼接；其它类型组合（Python `TypeError`）→ 0；
- `mul`：数字相乘；`str*int` / `int*str` 实现为重复；其它 → 0；
- 比较：数字跨类型按数值、字符串按内容。

### 6.2 调度与输入

- `run` 主循环、blocked/wait 空转一轮、死线程移除：照抄。
- 点击模型：`InputGetDown` 在 `steps % 600 < 2` 时返回 1（`steps` 是**当前 syscall 指令已计数**后的值），
  光标 960,540，`InputGetCursIn=1`，`InputGetUp/Event/State=0`。
- `DissolveWait` 与 `Motion*Test`、`V3DMotionTest`、`MovieState` 返回 1，让脚本越过等待循环。

### 6.3 为什么可信

- 900k、2M 逐字节一致（硬指标）。
- 额外步数 123457 / 777777 / 1500000 / 20,000,000 与 Python 输出逐字节一致
  （20M 的 Python 输出保存在 `fvp/tests/ref-py-20m.txt`，SHA256 `B482F032…64A7`）。
- 非 quiet 的 **stdout 也逐字节一致**（900k，4972 字节，含 `[debmess]` 与 summary）。
- 指令解码结果与 Python 完全同构：948,219 条指令、55,461 条字符串，字符串全部解码一致。

---

## 7. 性能对比

同一台机器、同一脚本、`--click 600 --quiet`，进程整体墙钟（含进程启动、脚本加载+解码）：

| 用例 | C（median） | Python 参照（median） | 加速比 |
|---|---:|---:|---:|
| 900k 步 | **60.8 ms**（min 57.4） | 2042.9 ms | **33.6×** |
| 2M 步 | **75.1 ms**（min 73.7） | 3181.5 ms | **42.3×** |
| 20M 步 | **254.0 ms** | 17224 ms | **67.8×** |
| 仅加载+解码（0 步） | **45.9 ms** | 1622.3 ms | **35.3×** |

扣除加载后，C 的纯执行速度约 **6000 万条指令/秒**（900k 步约 15 ms），
2M → 20M 呈线性（无泄漏/无退化）。

内存：10 亿步长跑时峰值工作集 **约 58 MB**（脚本 4.3 MB 全部载入 + 指令表 + pc 索引表），
运行期不随步数增长（文本列表与字符串拼接是仅有的增长项，实测 20M 步 40 KB 输出）。

复测：

```powershell
python fvp\tests\bench.py --steps 900000 --repeat 3
python fvp\tests\bench.py --steps 0 --repeat 3
```

---

## 8. 逐字节比对工具

`fvp/tests/compare.py`：默认跑主验收，也可跑长跑：

```powershell
python fvp\tests\compare.py --run              # 900k，对照 ref-900k.txt
python fvp\tests\compare.py --run --steps 2000000
python fvp\tests\compare.py --no-run --c my.txt --ref fvp\ref\ref-900k.txt
```

输出：两边字节数/行数/SHA256、CRLF 统计；不一致时给出**首个差异偏移、行号、上下文**，
退出码 0 = 逐字节一致。

---

## 9. 已知差异与限制（均不影响验收）

1. **整数范围**：Python 是任意精度 `int`，C 用 `int64_t`（编译加 `-fwrapv` 定义溢出行为）。
   样本实测 |int| 最大 8500，远未溢出；若未来脚本做 64 位以上运算，结果会不同。
2. **float → 文本**：`TextPrint` 收到 float 时 Python 用 `repr(float)`（最短往返表示），
   C 实现了“最短往返 + Python 风格定点/指数”的近似；样本 900k/2M 的 TextPrint 参数
   只有 `(int,str)` 与 `(int,int)`，**从未触发** float 文本。正式版如需严格一致，
   建议换用 Ryū/Grisu 或在测试里加 float 语料。
3. **字符串取负**：Python 抛 `TypeError` 直接终止整个解释器；C 返回 0 并在 stderr 警告一次。
   样本中 `neg` 只作用于 int。
4. **`str % 数字`（Python 字符串格式化）与 `str * str`**：C 按 `TypeError → 0` 处理
   （`str * int` 的重复已实现）。样本中未出现。
5. **TextPrint 的 layer 为字符串**：Python `'[%d]'` 会 `TypeError`；C 宽松地按原样写出。
6. **argc 表内嵌**：C 不支持 Python 的 `--cmds` 覆盖；表由 `ref/cmd_argc.json` 生成。
   若要换表，重新跑 `gen_tables.py` 并重编。
7. **线程创建**：参考实现没有实现任何创建线程的命令，因此实际只有 main 线程；
   C 的调度器按多线程语义实现，但同样没有创建入口（与 Python 等价）。
8. **非 quiet 的 debmess 输出**：C 用近似 `repr` 打印参数列表；900k 实测 stdout 与 Python 完全一致，
   但含引号/转义字符串参数时可能与 Python 的 `repr` 有细微差异。该输出不属于逐字节验收范围。
9. **中文脚本 `.iroseka.hcb`**：结构损坏（偏移 `0x7e8fc` 处 opcode `0x97`）。
   Python 在加载期抛 `KeyError: 151`；C 打印
   `fvm: unknown opcode 0x97 at offset 0x7e8fc` 并以退出码 3 结束，**不崩溃、不越界**。

---

## 10. 关于参考实现的两点观察（未改动 Python）

1. **`cmd_argc.json` 修订**：新表（`0ECAD110…`）与 `ref/cmd_table.json`（hcb 内命令表）**完全一致**；
   修订说明里 id 52 / 54 的“旧→新”列写反了（实际 52: 8→1、54: 1→8），但文件本身是对的。
   已验证：用旧表 `work/cmd_argc.json`（`8396FA95…`）跑 Python，900k 输出与基准**仍逐字节相同**
   —— 这次修订是输出中性的，只影响栈平衡的严格性。
   用新表后 C 端两个验收用例均 0 差异，说明栈平衡实现无出入。
2. **参考实现的“怪癖”是被刻意保留的**（不是 C 的 bug）：
   `nop` 没有处理分支（执行到就杀线程）、`initstack` 无 pending 帧时 ret 指向自身、
   未知 `*Test` 命令兜底返回 1、`temp` 不进栈、`jmpcond` 假时跳转等。
   本移植逐条照抄，以后 Python 若修正这些语义，C 侧需同步（差异会立刻体现在 diff 上）。

---

## 11. Android 移植注意

- 纯 C99 + libm，无 Win32/iconv/zlib 依赖；`encoding.c` 的表驱动解码可原样带到 NDK。
- 端序：所有多字节读取都显式按小端组装，不依赖宿主端序。
- 内存约 58 MB：4.3 MB 脚本 + 30 MB 指令表（948k × 32B）+ 17 MB pc 索引表（每字节 4B）。
  若 Android 内存紧张，可把 pc 索引改成“仅跳转落点”的开放寻址表或 24 位偏移表，能省约 15 MB。
- 加载+解码 46 ms（桌面）：手机上会慢几倍，可在首次启动时后台解码或缓存解码结果。
- `syscall.c` 的 stub 是**原型阶段占位**：接入渲染/音频/视频时替换对应 case 即可，
  VM 核心（栈/帧/调度/文本）不需要改；`MovieState` 正式实现要接真实视频播放。
- 输出 EOL 默认 CRLF（为了与 Windows 上的 Python 基准逐字节一致）；Android 侧如需 LF 用 `--eol lf`。

---

## 12. 复现命令汇总

```powershell
# 构建
powershell -ExecutionPolicy Bypass -File fvp\build.ps1

# 主验收 / 长跑（自动对比，退出码 0 = 一致）
python fvp\tests\compare.py --run
python fvp\tests\compare.py --run --steps 2000000

# 性能
python fvp\tests\bench.py --steps 900000 --repeat 3

# 重新生成编码/命令表（会做 Python codec 交叉验证）
python fvp\tests\gen_tables.py "C:\Users\Mechrevo\Downloads\五彩斑斓的世界HD\iroseka.hcb"

# 复现 Python 基准（与 fvp/ref/REFERENCE.md 一致）
python fvp\ref\fvm_ref.py "C:\Users\Mechrevo\Downloads\五彩斑斓的世界HD\iroseka.hcb" `
    --steps 900000 --click 600 --quiet --text-out out.txt
```