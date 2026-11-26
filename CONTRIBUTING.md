# 参与贡献 / Contributing

> 以下内容由 DeepSeek 生成,不一定代表作者个人意愿

谢谢愿意帮忙。当前版本 [`VERSION`](VERSION) = 1.0.0：引擎本体与工程已完整，
**但"能完整通关"还没做到**（游戏内存读档、动画缓动、菜单/设置仍是缺口，
逐条见 [`README.md`](README.md) 的「现在还不能做什么」与 [`CHANGELOG.md`](CHANGELOG.md)）。
**最有价值的贡献依次是**：报出「不兼容 / 输出不一致」的可复现问题 > 补命令语义与渲染 >
补文档与工具 > 清理代码。

只想先看看的话：[`docs/README.md`](docs/README.md) 是文档索引，
[`docs/roadmap.md`](docs/roadmap.md) 列了还没做的阶段。

---

## 0. 先明确边界

| 可以做 | 不要做 |
|---|---|
| 用正版游戏的数据做本地验证 | 提交、上传、或在 issue 里附上任何游戏数据（`.bin` / `.hcb` / 音频 / 图像 / 台词） |
| 报告「某版本游戏跑出来不对」 | 索要或分发游戏的非授权版本 / 汉化补丁本体 |
| 提交代码、文档、工具 | 在提交里带上 `work/`、`out/`、`extract/`、`bin/`、`tools-src/` 里的东西 |
| 修改 `src/`、`tests/`、`docs/` | 手改 `src/gen_*.h`（那是生成文件）、修改冻结的 [`ref/`](ref/REFERENCE.md) 基准 |

提交即表示你同意以 **GPL-2.0-only** 授权你的贡献（见 [`LICENSE`](LICENSE)）。

---

## 1. 报告 FVP 版本兼容问题

FVP 是闭源引擎，**每部作品、每个版本都可能不一样**。只写「跑不起来」没法修；
请按下面的模板把「可核对的数字」给出来。没有游戏数据也能提供大部分信息。

### 1.1 报告模板

```markdown
### 环境
- 操作系统 / 编译器（如 Windows 11 + w64devkit gcc 16.2.0）
- 本仓库 commit（`git rev-parse --short HEAD`）
- 构建命令：`make` / `make CC=gcc` / `build.ps1`

### 游戏
- 作品名：中文 / 日文原名
- 发布形态：通常版 / HD / 重制版，版本号（exe 属性里的文件版本）
- 数据目录里有哪些 `*.bin`、`*.hcb`（只列文件名 + 每个文件的字节数与 SHA256，不要传文件）

### 复现命令
fvm <script.hcb> --enc cp932 --steps 900000 --click 600 --quiet --text-out c.txt

### 期望 / 实际
- 期望：第 N 行应该是 …
- 实际：第 N 行是 …
- 或：`fvm` 退出码、stderr 最后几行

### 已有统计（见 §2 的命令输出，贴关键行）
- 指令数、字符串数、命令表条数、unknown 命令 top 5
- 若是渲染/音频问题：出问题的资源名（脚本里的字符串即可）与期望的画面/时长
```

### 1.2 「你的版本」和「已验证版本」的差别怎么定位

已验证的版本是 `いろとりどりのセカイ HD Re:GENERATION`（`iroseka_HD.exe` 1.0.0.95）。
换作品/换版本时，优先看这四处，它们决定了要不要改代码：

1. **`.hcb` 头**：`script_len`（u32 @0）、`main_start`（u32 @script_len）能不能读出、
   命令表条数是不是 148 —— 见 [`docs/00-engine.md`](docs/00-engine.md)。
2. **命令表**：把新脚本导出的命令名/argc 与 [`ref/cmd_table.json`](ref/cmd_table.json) 对比，
   多出来的名字或 argc 不同 = 引擎 API 变了。
3. **opcode**：如果出现 40 个之外的 opcode，说明指令集变了（会直接解码失败）。
4. **资源格式**：`graph*.bin` 里的图像是否仍是 `hzc1` + `NVSG` 魔数；
   音频是否仍是 Ogg / RIFF WAVE —— 见 [`docs/hzc1-format.md`](docs/hzc1-format.md)、
   [`docs/01-assets.md`](docs/01-assets.md)。

---

## 2. 等价性验证（本项目的核心验收手段）

C 实现不是「看起来对」，而是**和冻结的 Python 基准逐字节比对**。
基准在 [`ref/`](ref/REFERENCE.md)：

| 文件 | 作用 |
|---|---|
| [`ref/fvm_ref.py`](ref/fvm_ref.py) | 冻结的 Python 虚拟机原型（**不能改**，改了就失去基准） |
| [`ref/ref-900k.txt`](ref/ref-900k.txt) | 900,000 步的期望输出（195 行 / 6,496 字节） |
| [`ref/ref-2m.txt`](ref/ref-2m.txt) | 2,000,000 步的期望输出（450 行 / 15,378 字节） |
| [`ref/cmd_table.json`](ref/cmd_table.json) · [`ref/cmd_argc.json`](ref/cmd_argc.json) | 命令名 / argc 的冻结快照 |

### 2.1 跑一次

```bash
python tests/compare.py --run --script /path/to/iroseka.hcb
```

期望最后一行：

```
RESULT: PASS -- byte-identical (6496 bytes, 195 lines)
```

退出码：`0` = 逐字节一致；`1` = 有差异（会打印第一个不同字节的偏移、行号与上下文）；
`2` = 缺输入（exe 没构建、脚本路径不对）。长跑用例加 `--steps 2000000`。

> **换行符注意**：`ref/*.txt` 是 Windows 文本模式生成的 **CRLF**，而验收是「逐字节」比较。
> 本仓库有逐字节的等价性基准（`ref/`），克隆后请确认 `git config core.autocrlf false`，
> 否则比对会在每个行尾假失败。仓库根的 [`.gitattributes`](.gitattributes) 已声明
> `* -text`（提交/检出不做换行转换，但仍按文本出 diff），提交时不要绕过它。
> 也可以用 `python scripts/ci-compare.py --check-refs-only` 快速确认基准文件
> 是否被改坏 —— 它会用 [`ref/REFERENCE.md`](ref/REFERENCE.md) 里的 SHA256 校验。

### 2.2 没有游戏数据时

CI 和大多数贡献者都没有游戏数据，此时必须**明确跳过**而不是假装通过：

```bash
python scripts/ci-compare.py            # 无数据 -> 打印 SKIPPED，退出码 0
python scripts/ci-compare.py --script /path/to/iroseka.hcb   # 有数据 -> 真跑 tests/compare.py
```

即使没有数据，`scripts/ci-compare.py` 仍会做一件有意义的检查：
按 [`ref/REFERENCE.md`](ref/REFERENCE.md) 里的 SHA256 校验 5 个冻结基准文件。

### 2.3 改了行为怎么办

「输出变了」不一定是 bug，但**不能悄悄改基准来让测试变绿**：

1. 先开 issue，附上「为什么原行为是错的」的证据（文档、静态分析、原版截图/录屏）。
2. 结论若改变语义，**新增**一个基准（文件名形如 `ref/ref-<N>k.txt`）并在
   [`ref/REFERENCE.md`](ref/REFERENCE.md) 里登记 SHA256；旧的基准保留。
3. 代码与文档一起改，否则下一个人会再次踩坑。

C 侧验收之外，资源层有自己的验收脚本（需要游戏目录，Windows）：

```powershell
powershell -ExecutionPolicy Bypass -File tests\verify_assets.ps1 -Quick
```

---

## 3. 代码风格

### C（`src/`）

- **C99**，编译选项固定为 `-std=c99 -O2 -Wall -Wextra -Wpedantic -fwrapv`，**要求 0 warning**。
- **无外部依赖**：只用 C 标准库 + `libm`；不引入 zlib / iconv / libvorbis。
  唯一例外是 [`src/third_party/stb_vorbis.c`](src/third_party/stb_vorbis.c)（见 [`THIRD-PARTY.md`](THIRD-PARTY.md)）。
- **引擎核心不调用平台 API**：`src/` 下的引擎核心只用 C 标准库 + `libm`，
  可移植性靠 `#if defined(_WIN32)` 的小分支解决（如 `_fseeki64`），不引入 `<windows.h>`，
  这样同一份代码能交叉编译到 Android。**平台相关代码集中在 `src/platform/<平台>/`**
  （例如 `src/platform/win32/`），那一层允许使用原生 API（Win32 / GDI / waveOut / JNI 等），
  但不得被引擎核心反向依赖。
- **语义对齐 Python 基准**：C 版存在的意义是与 [`ref/fvm_ref.py`](ref/fvm_ref.py) 行为一致。
  改语义时，注释里写清「对应 Python 的哪一段 / 为什么这样等价」。
  注意 [`ref/fvm_ref.py`](ref/fvm_ref.py) 的整数是 Python 大整数，C 侧用 `int64_t` + `-fwrapv` 近似，
  额外差异记录在 [`docs/c-vm.md`](docs/c-vm.md) 的「已知差异」小节。
- **内存**：分配走 `fvp_xmalloc` / `fvp_xcalloc`；字符串是不可变的，从 arena 分配
  （`fvp_arena_str`），不要单独 free；错误用 `fvp_fatal` 而不是静默返回。
- **命名**：对外符号统一 `fvp_` / `fvm_` 前缀，snake_case；头文件 include guard 用 `FVP_<FILE>_H`。
- **格式**：4 空格缩进，**LF**，UTF-8（注释里可以写中文）；不要顺手格式化无关代码。

### Python（`tests/`、`scripts/`）

- Python **3.8+**，只允许标准库（CI 固定 3.12）。
- 脚本必须能 `python -m py_compile` 通过，且不接受需要联网的行为。
- 输出格式保持一致：结论行用 `RESULT: PASS` / `RESULT: FAIL` / `RESULT: SKIPPED`，
  退出码 `0` 成功、`1` 失败、`2` 缺输入。

### 文档（`docs/`、根目录 Markdown）

- 中文为主，UTF-8 + **LF**。
- 只写验证过的结论；推测单独标出来（现有文档都这么做）。
- **文档里提到的仓库内文件路径必须真实存在** —— 提交前跑
  `python scripts/check-docs.py`。
- 新增文档后，在 [`docs/README.md`](docs/README.md) 的对应分组里加一行。

---

## 4. 提交前必须跑的检查

```bash
# 1. 构建（0 warning）—— 生成 bin/fvm.exe、bin/libfvp.a（含资源层 + 音频层）与 bin/test_snd.exe
make
make lib
make test

# 2. 单元测试：9 个不需要游戏数据的 self-test 一次跑完（CI 也跑这个）
make check

# 3. 生成的码表必须与入库版本一致（改了 ref 或生成器才需要重跑）
python tests/gen_tables.py
git diff --ignore-cr-at-eol --exit-code src/gen_cp932.h src/gen_gbk.h src/gen_cmd_table.h

# 4. 文档里的路径必须都在
python scripts/check-docs.py

# 5. 语法检查（tests/ 与 scripts/ 下的全部 Python）
python -m compileall -q tests scripts

# 6. 有游戏数据时：逐字节等价 + 基准文件完整性
python scripts/ci-compare.py --script /path/to/iroseka.hcb
```

CI（[`.github/workflows/ci.yml`](.github/workflows/ci.yml)）在 Ubuntu 与 Windows 上跑 1–5
（Windows 上额外跑 `make win32` 确认桌面外壳能构建），第 6 步在无数据时显式输出 `SKIPPED`。

> **桌面运行器（Win32）**：`make win32` → `bin/fvp.exe`，只在 Windows 上有意义。
> 它能构建（0 warning），但**还没有验收**（画面 / 输入 / 音频都没验证），
> 所以目前**不作为提交门槛**；贡献这部分时请附上可复核的证据（截图、日志、性能数字）。

提交信息建议：一行说清「改了什么、为什么」，正文里贴关键数字（步数/字节数/哈希）。
一次提交只做一件事；只改行为、不改生成文件。

---

## 5. 适合上手的任务

按难度从低到高，全部都有对应文档可依：

1. **补命令语义**：从 [`docs/vm-commands.md`](docs/vm-commands.md) 里挑一条当前是桩的命令
   （`src/syscall.c` 的 `default:` 分支），先写「Python 基准里它返回什么」的说明，再实现。
2. **音频接进 VM**：`src/snd.c` 已能解码 Ogg/WAV 并混音，Win32 外壳也已有 waveOut 输出，
   但脚本命令还没接上（syscall 里没有音频命令）；接 `BGMPlay` / `SEPlay` / `VoicePlay` 一类命令，
   并保证「没有音频设备也能跑」。
3. **渲染层对齐**：C 渲染层正在移植（`src/gfx.c`，仍会变），目标是和 Python 原型逐像素一致；
   未实现的语义清单见 [`docs/render-model.md`](docs/render-model.md) 第 7 节。
4. **线程 / 时间模型**：[`docs/05-threads.md`](docs/05-threads.md) 记了已确认的线程槽布局、
   失败尝试和下一步（虚拟时钟 + 32 线程槽调度），这是补回缺失台词的关键路径。
5. **Android 后端**：设计见 [`docs/android-shell.md`](docs/android-shell.md)（外壳本身在另一个项目里）。
6. **第二部作品兼容**：拿另一部 FVP 作品跑 §1.2 的四项对照，把差异写成文档 + issue。

参与讨论时，请尽量附上「命令 + 输出 + 退出码」，而不是截图 —— 这个项目的进展全靠在数字上对齐。

## 文档与提交信息的表述规范

- 只描述**数据格式、常量、接口约束与可观察行为**（例如"槽文件第 20 段是 100 条
  历史记录""缓动 mode 3 = ease-out"），不描述获取这些信息的过程或工具链；
- 涉及原版程序时统一用"原版程序 / 静态分析 / 结构分析"这类中性说法；
- 不提交游戏数据、汉化补丁或任何原版资源；issue 里贴文件名/大小/SHA256 即可。
- `scripts/check-docs.py` 只检查路径与声明，措辞规范靠评审与这条约定维持。
