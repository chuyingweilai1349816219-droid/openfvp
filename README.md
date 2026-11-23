# openfvp

> **说明**
>
> 本项目由 **6amm** 辅助 DeepSeek 完成，其中所有代码由 DeepSeek 生成。
>
> 以下 README 由 DeepSeek 生成，不一定代表作者个人意愿。

**FVP（FAVORITE VIEW POINT SYSTEM）引擎的开源 C99 重实现。**

目标：让 FAVORITE 社的 FVP 视觉小说（《いろとりどりのセカイ HD》系列、《星空のメモリア》、
《アストラエアの白き永遠》等）能在现代平台运行，首要目标是 Android。

> **状态：0.1.0 原型。** 脚本虚拟机（VM）与冻结的 Python 基准逐字节对齐；
> 资源层（BIN / inflate / HZC1）、渲染层（gfx）、音频层（Ogg / WAV / 混音）都已有验收数字；
> Win32 桌面外壳能跑出 60 fps 并显示真实游戏画面；
> 命令语义（动画/存档/设置）、线程与时间模型、Android 后端**仍未完成**。
> 下面的「能做什么 / 不能做什么」是逐条可核对的清单，不含宣传词。

本仓库**不含任何游戏数据**：没有 `.bin`、`.hcb`、音频、图像、台词。要跑起来必须自备正版游戏，
见文末[免责声明](#免责声明)。

---

## 这是什么

| | |
|---|---|
| 引擎 | FVP（FAVORITE VIEW POINT SYSTEM），闭源，PE i386 + Direct3D 9 + DirectSound |
| 本项目 | 从零写的 C99 运行时：`.hcb` 脚本解释器 + 资源层 + 音频层 |
| 产物 | `bin/fvm.exe`（脚本虚拟机，可跑真实脚本并导出文本；产物名默认带 `.exe`）· `bin/libfvp.a`（引擎核心 + 资源层 + 音频层，给外壳链接）· `bin/fvp.exe`（Win32 桌面运行器，开发中） |
| 对标 | [OpenBGI](https://openbgi.net)（同为 C99 从零重写的闭源视觉小说引擎运行时，BGI/Ethornell 引擎）——同样是「给闭源引擎补一个开源运行时」 |

三个明确的不：

- **不是模拟器**：不加载、不执行原游戏的 `iroseka_HD.exe`，没有任何 x86 解释层。
- **不是 Wine / 兼容层封装**：不依赖原程序的 PE、D3D9、DirectSound。
- **不修改原游戏文件**：只读原目录，产物全部写在自己的 `out/` 里。

## 为什么做这个

1. **引擎闭源，运行时也没有开源替代。** FVP 被用于 FAVORITE 的多部作品，但社区工具链
   （HCB 解包/重打包、BIN 归档器、图像转换器）都停在「能看、能改」，**没有能执行脚本的引擎**。
   想在新平台跑，只能靠原版 exe。
2. **BGI 有 openbgi，FVP 没有对等物。** openbgi 证明了「社区从零重写闭源视觉小说引擎」是可行的；
   FVP 这边接口面还小得多：**40 个 opcode + 148 条命令**（而 openbgi 要覆盖 BGI 的上千个操作码），
   见 [`docs/roadmap.md`](docs/roadmap.md)。
3. **Wine / 虚拟机解决不了 Android。** 目标是手机和平板；这要求引擎本身可移植、无 Win32 依赖。
4. **把格式知识固化成可验证的代码。** 结构分析结论散落在帖子和脚本里容易失传；
   这个仓库把结论写成文档 + 用「与 Python 基准逐字节一致」把实现钉住。

---

## 现在能做什么

每一条都给出数字和仓库内对应的代码/文档。

| 能力 | 现在到什么程度 | 代码 / 文档 |
|---|---|---|
| `.hcb` 脚本加载与解码 | `iroseka.hcb` **948,219 条指令**、**55,461 条字符串**全部解析，跳转目标零错位 | [`src/hcb.c`](src/hcb.c) · [`docs/c-vm.md`](docs/c-vm.md) §5.1 |
| 虚拟机指令集 | **40 个 opcode 全部实现**（栈、帧、全局/局部变量、temp）；线程调度是简化版，还没有时间模型 | [`src/vm.c`](src/vm.c) · [`docs/c-vm.md`](docs/c-vm.md) §6.1 · [`docs/05-threads.md`](docs/05-threads.md) |
| 命令表 | **148 条**命令的 id / 名字 / argc / 是否返回值，来自 `.hcb` 内嵌附加数据；本作实际用到 **131 条**（17 条未使用） | [`src/gen_cmd_table.h`](src/gen_cmd_table.h) · [`src/syscall.c`](src/syscall.c) · [`docs/00-engine.md`](docs/00-engine.md) · [`docs/vm-commands.md`](docs/vm-commands.md) |
| **C 版与 Python 版逐字节一致** | 900k 步：6,496 字节 / 195 行（`SHA256 7F9837FF…`）；2M 步：15,378 字节 / 450 行（`SHA256 32D7D4E2…`），完整哈希见 [`ref/REFERENCE.md`](ref/REFERENCE.md)；开发机上另跑过 20M 步与 123457 / 777777 / 1500000 步对照，均一致 | [`tests/compare.py`](tests/compare.py) · [`ref/ref-900k.txt`](ref/ref-900k.txt) · [`ref/REFERENCE.md`](ref/REFERENCE.md) |
| 资源层：BIN 包 / inflate / HZC1 | 12 个 `.bin` 包共 **21,725 条**索引/名字/偏移逐条一致；`graph.bin` **1,112/1,112** 个 zlib 流（**2,539,861,820 字节**）与 `zlib.decompress` 逐字节相同；**1,112 张图 / 3,654 帧 / 727,573,943 像素 0 差异**（fmt 0–4 全覆盖） | [`src/bin.c`](src/bin.c) · [`src/inflate.c`](src/inflate.c) · [`src/hzc1.c`](src/hzc1.c) · [`docs/c-assets.md`](docs/c-assets.md) · [`tests/verify_assets.ps1`](tests/verify_assets.ps1) |
| 音频层（**已接到 VM**） | 48 kHz 立体声 PCM16 统一格式；Ogg 实测 5,587,774 帧（116.41 s）、peak=19297；22.05 kHz WAV 重采样后时长 5.64→5.644 s。运行 900k 步触发 **4 次 BGM 播放**（`BGM2/002` 等），混音 peak **14004** / rms **2431.8**，解码失败 0 | [`src/snd.c`](src/snd.c) · [`docs/04-audio.md`](docs/04-audio.md) · [`tests/verify_audio.ps1`](tests/verify_audio.ps1) |
| 渲染层（Python 原型已验证，**未入库**） | 1920×1080 合成：背景 + 立绘 + 对话框 + 文字；90 万步里导出 12 帧 + 10 张剧情检查点 PNG。图层 / 图元 / 文本 / 合成模型与已知偏差成文 | [`docs/render-model.md`](docs/render-model.md)（原型在本地 `work/`，见下） |
| 渲染层（C，**已验收**） | 1920×1080 合成：图元 **24/24 位置误差 0 像素**；像素差异 **293 / 2,073,600 = 0.0141%**（PSNR 49.94 dB、SSIM 0.99971）；热帧 **9.0–9.5 ms** | [`src/gfx.c`](src/gfx.c) · [`docs/c-gfx.md`](docs/c-gfx.md) · [`tests/verify_gfx.ps1`](tests/verify_gfx.ps1) |
| 渲染 ↔ VM 接线（**已验收**） | `syscall` 直接驱动 gfx 后，跑 528,000 步 + 每帧渲染，与参考帧**每个数字与纯渲染基线完全相同**（0 像素 / 0.0141%）；同时 `fvm.exe` 输出**逐字节不变** | [`src/syscall.c`](src/syscall.c) · [`tests/render_vm.c`](tests/render_vm.c) · [`docs/c-gfx.md`](docs/c-gfx.md) §9 |
| Win32 桌面外壳（**已验收**） | 启动到窗口 **130–230 ms**、**60.00 fps**、GDI 上屏 avg 3.4–3.6 ms（慢帧 0）、waveOut 10 秒 underrun 0、点击注入被 `InputGetDown` 消费 | [`src/platform/win32/main_win32.c`](src/platform/win32/main_win32.c) · [`docs/win32-shell.md`](docs/win32-shell.md) |
| 存档层（**文件格式已实现**） | `save/sNNN.bin` 与 `save/save.bin` 的解析 + 重写：**6 个真实存档槽逐字节回环一致**；`save.bin` 解出 **1471 条**全局变量、200×113 缩略图尺寸、字体名 `メイリオ`；50 项合成用例含畸形输入拒绝。槽内部的引擎状态快照按不透明字节原样透传（语义未解，见文档 §4） | [`src/save.c`](src/save.c) · [`docs/save-format.md`](docs/save-format.md) §7 · [`tests/verify_save.ps1`](tests/verify_save.ps1) |
| **跨作品验证** | 第二部作品《星空のメモリア HD》同样跑通：命令表 **148 条逐条完全相同**、opcode 同套、VM **零修改**输出 164 行正确台词 | [`docs/06-cross-validation.md`](docs/06-cross-validation.md) · [`tools/fvpprobe.py`](tools/fvpprobe.py) |
| 健壮性测试 | **27,300 次** HCB 变异（0 崩溃 / 0 越界）、**339 个**资产畸形用例、**43 个** VM 边界用例；`tests/run_all.py` 一键 **10/10 PASS**（含存档层 50 + 44 项检查） | [`docs/testing.md`](docs/testing.md) · [`tests/run_all.py`](tests/run_all.py) |
| 中文脚本重建（方法已验证） | 被汉化补丁打坏的 GBK 脚本重建为 3,712,944 字节、948,219 条指令、命令表与冻结表一致的结构合法 `.hcb`（产物不入库） | [`docs/cn-script-rebuild.md`](docs/cn-script-rebuild.md) |
| Android 外壳（设计，进行中） | APK 流水线 / JNI / EGL 原生窗口 / 触摸 / 资产目录的设计（非当前主线）；外壳不在本仓库 | [`docs/android-shell.md`](docs/android-shell.md) · [`docs/roadmap.md`](docs/roadmap.md) |

「逐字节一致」的含义：C 实现不是「看起来跑得对」，而是把脚本跑 N 步后导出的每一行文本
（`[层] 文本`，含 CRLF）与冻结的 Python 基准输出做 **raw byte** 比较，第一个不同字节的偏移会被打印出来。
没有游戏数据时这个测试会显式输出 `SKIPPED`，而不是假装通过 —— 见
[`scripts/ci-compare.py`](scripts/ci-compare.py) 与 [`.github/workflows/ci.yml`](.github/workflows/ci.yml)。

## 现在还不能做什么

按影响从大到小，这些都是**已知未完成**，不是待办里的漂亮话：

1. **只有开场到第一章的画面能看。** 渲染层与桌面外壳都通过了像素级验收，
   但**游戏内 UI 还是占位**：对话框是画的圆角框（真实的 `main_window` 贴图已解出但没接），
   没有菜单、没有存档界面、没有设置页。动画（`Motion*`）仍是空实现，所以立绘不会动。
2. **命令语义基本是桩。** [`src/syscall.c`](src/syscall.c) 里渲染相关的 30 条已接 gfx，
   但动画/视频/存档/设置类命令仍是兜底策略（`*Test` 返回 1、声明返回值的返回 0）。
   **没有移动、没有淡入淡出、没有转场、没有存档。**
3. **协程 / 时间模型没实现。** `ThreadNext` / `ThreadWait` 只是「让出」桩，
   `Timer*` / `ThreadSleep` 没有虚拟时钟，也没有 32 线程槽的调度。
   直接后果是一部分台词（协程里拼好、由文本线程打印）拿不到，90 万步里约 50 句缺失。
   已确认的事实、失败尝试与下一步在 [`docs/05-threads.md`](docs/05-threads.md)。
4. **音频播放已经通了，但音画同步与音量体系还没做。** 脚本里的 `AudioPlay` 现在会真的解码并混音
   （900k 步触发 4 次 BGM），但语音路径依赖存档层给的 voice id（原型里没有），
   音量主控/分组的配置层未接，也没有 A/V 同步与淡入淡出。
5. **视频（WMV 开场）直接跳过。** `Movie` / `MovieState` 被实现成「立即播放完毕」，
   否则脚本会卡在等待循环里。
6. **中文版不能当成品跑。** 重建脚本结构合法、VM 可执行，但没有渲染/音频，也没有逐场景校对。
7. **只验证过两部作品。** `いろとりどりのセカイ HD Re:GENERATION` 与
   《星空のメモリア HD》都跑通了（命令表 148 条零差异），但更老的 FVP 版本
   （2004–2008 年的作品）**未测试**，格式是否仍然一致未知。
8. **4 个 opcode 语义未确认**（`0x11` / `0x12` / `0x17` / `0x18`，占本作指令 0.18%），
   按数组读写实现，未做定向验证 —— 见 [`docs/roadmap.md`](docs/roadmap.md)。
9. **存档只做到文件层。** `save/sNNN.bin` 与 `save/save.bin` 能完整解析并原样重写
   （6 个真实存档逐字节回环一致），但槽文件里那段引擎状态快照的字段语义还没解完，
   所以**游戏内还不能真的存档/读档**；也没有选项菜单与 Android 后端。
10. **`work/` 里的 Python 原型没有入库**（体积大、含大量一次性侦查脚本）。
    因此「重新生成渲染帧」「重跑 HZC1 参考解码」这类操作在克隆出来的仓库里做不了，
    只能用仓库内的 C 工具（[`tests/verify_assets.ps1`](tests/verify_assets.ps1)）做验收。

---

## 构建

依赖只有 **C99 编译器 + GNU make**（Windows 上 `mingw32-make` 亦可用）。没有 zlib / iconv /
libvorbis / SDL 等外部依赖；唯一第三方源码是 `stb_vorbis`（见 [`THIRD-PARTY.md`](THIRD-PARTY.md)）。

```bash
make            # 构建 bin/fvm.exe（脚本 VM）
make lib        # 构建 bin/libfvp.a（引擎核心 + 资源层 + 音频层，给外壳链接）
make test       # 构建 bin/test_snd.exe（音频解码自检）
make win32      # 构建 bin/fvp.exe（Win32 桌面运行器，未验收）
make clean
```

- 默认编译选项：`-std=c99 -O2 -Wall -Wextra -Wpedantic -fwrapv`，**要求 0 warning**。
- 产物名默认带 `.exe`（`Makefile` 里 `EXE ?= .exe`，不区分平台）；POSIX 上想要无后缀的
  `bin/fvm`，用 `make EXE=`。
- `src/gen_cp932.h`、`src/gen_gbk.h`、`src/gen_cmd_table.h` 是**已入库的生成文件**，
  所以构建不需要 Python；要重新生成见 [`tests/gen_tables.py`](tests/gen_tables.py)。
- 如果 `cc` 不在 PATH 上（某些 Windows 发行版只有 `gcc`），用 `make CC=gcc`。
- `make win32` 只在 Windows 上有意义（链接 `gdi32` / `user32` / `winmm` / `shell32`）；
  它能构建，但属于[「还不能做什么」](#现在还不能做什么)第 1 条：**没有验收**。

**Windows**：仓库根目录的 [`build.ps1`](build.ps1) 是开发机自用脚本，它把
`..\tools\w64devkit\w64devkit\bin` 硬编码进了 PATH；在干净克隆里请直接用 `make`
（或改 `$DevkitBin` 指向你自己的 w64devkit / MSYS2 gcc）。

## 运行

```bash
# 跑 90 万步，导出「[层] 文本」到文件；--click 600 是验收用的合成点击节奏
./bin/fvm.exe /path/to/iroseka.hcb --steps 900000 --click 600 --quiet --text-out c-text.txt

# 看全部参数
./bin/fvm.exe --help
```

> 产物名默认是 `bin/fvm.exe`（见构建一节）；如果你用 `make EXE=` 构建，则命令里的
> `./bin/fvm.exe` 换成 `./bin/fvm`。

| 参数 | 含义 |
|---|---|
| `--enc cp932\|gbk` | 文本编码，默认 `cp932`（日文原版）；汉化版用 `gbk` |
| `--steps N` | 指令预算，默认 2,000,000 |
| `--click N` | 每 N 步模拟一次点击，默认 0（验收用 600） |
| `--cursor X,Y` | 合成光标位置，默认 `960,540` |
| `--text-out FILE` | 输出 `[层] 文本` 行（默认 CRLF，与 Python 基准一致） |
| `--eol crlf\|lf` | 改 `--text-out` 的换行符 |
| `--quiet` | 不向 stdout 回显文本 / debmess |
| `--pctrace FILE` | 调试：逐条记录执行的 pc |

退出码：`0` 成功 · `2` 参数错误 · `3` 脚本加载/解码失败 · `4` 写输出失败。

### 与 Python 基准比对

```bash
python tests/compare.py --run --script /path/to/iroseka.hcb
# -> RESULT: PASS -- byte-identical (6496 bytes, 195 lines)
```

没有游戏数据时：

```bash
python scripts/ci-compare.py        # -> RESULT: SKIPPED（仍会校验冻结基准的 SHA256）
```

> 注意：`ref/*.txt` 是 CRLF 的**逐字节**基准。克隆仓库时请保持
> `git config core.autocrlf false`，否则换行被转换会让比对假失败。
> 细节见 [`CONTRIBUTING.md`](CONTRIBUTING.md) §2.1。

---

## 目录结构

```
.
├── README.md              本文件（中文）
├── README.en.md           英文说明
├── VERSION                版本号（0.1.0）
├── LICENSE                GPL-2.0-only 全文
├── THIRD-PARTY.md         第三方代码（stb_vorbis）与生成文件的来源
├── CONTRIBUTING.md        报兼容问题 / 等价性验证 / 代码风格 / 提交前检查
├── .gitattributes         逐字节基准的换行策略（* -text）
├── .gitignore             产物 / 本地工作区不入库
├── Makefile               make / make lib / make test / make win32 / make clean
├── build.ps1              开发机用的 Windows 一键构建（硬编码 w64devkit 路径）
├── docs/                  结构分析与实现文档（14 份 + 索引 docs/README.md）
├── src/                   C99 引擎核心
│   ├── fvp.h  fvp.c       值模型、Python 语义的相等/比较、arena、float repr
│   ├── encoding.h/.c      CP932 / GBK → UTF-8 解码（语义 = Python decode('replace')）
│   ├── hcb.h  hcb.c       .hcb 加载、指令解码、pc→指令索引
│   ├── vm.h   vm.c        解释器：栈、帧、线程、变量、40 个 opcode
│   ├── syscall.h/.c       148 条命令的分发表（语义见「不能做什么」第 2 条）
│   ├── main.c             CLI、文本输出、summary
│   ├── bin.h  bin.c       BIN 资源包索引与读取
│   ├── inflate.h/.c       自写的 zlib / raw deflate 解压（不依赖 zlib）
│   ├── hzc1.h hzc1.c      HZC1 / NVSG 图像解码（fmt 0–4，多帧）
│   ├── gfx.h  gfx.c       渲染层（C 移植中）：图元表、图层合成、文本排版
│   ├── snd.h  snd.c       音频：stb_vorbis + RIFF WAVE 解码、重采样、8 通道混音
│   ├── gen_*.h            生成的数据表（编码 2 张 + 命令表 1 张，已入库）
│   ├── platform/win32/    Win32 桌面外壳（开发中）：窗口 / GDI / 输入 / waveOut / 日志
│   └── third_party/
│       └── stb_vorbis.c   Ogg Vorbis 解码（v1.25，public domain / MIT）
├── tests/                 等价性、基准、码表生成、资源层验收、渲染导出工具
├── ref/                   冻结的 Python 基准 + 期望输出（SHA256 见 ref/REFERENCE.md）
├── scripts/               check-docs.py / ci-compare.py / release.ps1
└── .github/workflows/     CI（ubuntu + windows）
```

**不入库**（见 [`.gitignore`](.gitignore)）：

| 目录 | 是什么 | 为什么不入库 |
|---|---|---|
| `work/` | Python 结构分析脚本、VM 原型、渲染原型 | 一次性侦查脚本多、体积大；结论已整理进 `docs/` |
| `out/` `extract/` `bin/` | 构建产物、解包产物、导出帧 | 可重建 |
| `tools-src/` | 下载来的第三方工具源码（见 [`THIRD-PARTY.md`](THIRD-PARTY.md)） | 不是本项目组成部分，许可归属他人 |

---

## 如何参与

欢迎任何形式的帮忙，最有价值的是**带数字的可复现问题**。先读
[`CONTRIBUTING.md`](CONTRIBUTING.md)，里面给了报告模板、等价性验证流程和提交前检查清单。

几条原则：

- **不要提交游戏数据**，也不要上传汉化补丁 / 非授权版本；issue 里贴文件名、大小、SHA256 即可。
- **不要为了让测试变绿而改基准**：[`ref/`](ref/REFERENCE.md) 是冻结的验收锚点，
  行为要变就新增基准并说明证据。
- **文档里的路径要真实存在**：提交前跑 `python scripts/check-docs.py`（CI 也会跑）。
- **C 侧 0 warning**：`-Wall -Wextra -Wpedantic`，不引入新的第三方依赖。

适合上手的任务（按难度）：

1. 给 [`src/syscall.c`](src/syscall.c) 里某条桩命令补语义（先对照 [`docs/vm-commands.md`](docs/vm-commands.md)）；
2. 把 `src/snd.c` 接进 syscall（脚本命令目前无法触发播放），并保证「没有音频设备也能跑」；
3. 参与 C 渲染层移植 / 验收：把 `src/gfx.c` 的输出和 Python 原型逐像素对比（模型见 [`docs/render-model.md`](docs/render-model.md)）；
4. 实现线程调度与虚拟时钟（已确认事实与失败尝试见 [`docs/05-threads.md`](docs/05-threads.md)）；
5. 拿第二部 FVP 作品做兼容对照（[`CONTRIBUTING.md`](CONTRIBUTING.md) §1.2 的四项检查）。

## 许可证

本项目以 **GPL-2.0-only** 发布，全文见 [`LICENSE`](LICENSE)；与 openbgi 的许可选择对齐。
唯一的第三方代码 `stb_vorbis`（public domain / MIT 双许可）与 GPL 兼容，
来源与版本记录在 [`THIRD-PARTY.md`](THIRD-PARTY.md)。

## 免责声明

- 本仓库**不包含任何游戏数据**，也不提供下载。运行需要**自备正版游戏**。
- 引擎代码是**独立重写**的：不含、也不链接原游戏的任何二进制代码；
  文件格式与命令语义来自对可执行文件与数据文件的**结构分析**，目的是互操作性（interoperability）与学习研究。
- 项目名与代码中的 `FVP`、作品名、公司名等归各自权利人所有；本项目与 FAVORITE 社无关，未获其授权或背书。
- 请勿将本项目用于分发游戏数据、绕过授权或任何违反当地法律与游戏 EULA 的用途。
- 代码按「现状」提供，不附带任何担保；用坏存档、跑坏流程的风险自负。

---

## English

**openfvp** is a from-scratch C99 reimplementation of the runtime for FVP
(FAVORITE VIEW POINT SYSTEM), the closed-source visual-novel engine behind
FAVORITE's titles such as *Irotoridori no Sekai HD*. It is not an emulator, not a
Wine wrapper, and it never modifies the original game files.

What works today (v0.1.0 prototype):

- the whole `.hcb` script format is parsed — **948,219 instructions** and
  **55,461 strings** in the reference title — and all **40 VM opcodes** are implemented;
- the **148-entry** command table (id / name / argc / returns-value) is decoded from the
  script's embedded metadata;
- the C VM is **byte-identical** to the frozen Python reference: 6,496 bytes / 195 lines at
  900k steps and 15,378 bytes / 450 lines at 2M steps, verified raw-byte by
  [`tests/compare.py`](tests/compare.py);
- the asset layer decodes BIN packages, its own zlib inflate, and HZC1 images with
  **0 differing pixels** over 1,112 images / 3,654 frames / 727,573,943 pixels;
- the audio layer decodes Ogg Vorbis and RIFF WAVE, resamples to 48 kHz, and mixes 8 channels;
- a C renderer (`src/gfx.c`) is being ported from the validated Python prototype
  (1920×1080 compositing of backgrounds, sprites, dialogue box and text) and is
  **not yet accepted**; a Win32 desktop shell builds (`make win32` → `bin/fvp.exe`) but
  has **no acceptance run** either.

Not done yet: renderer acceptance, only stub command semantics (no drawing, saves, settings or
video), audio is not wired to syscalls, no thread/time model, no Android backend in-tree, and
only one title version has been tested. See [`docs/roadmap.md`](docs/roadmap.md) and the
"不能做什么" list above.

Build with `make` (C99 + GNU make, no external dependencies), run `./bin/fvm.exe game.hcb
--steps 900000 --click 600 --quiet --text-out c-text.txt`, and compare against the frozen
reference with `python tests/compare.py`. Licensed **GPL-2.0-only**; no game data is
included — bring your own legally purchased copy.