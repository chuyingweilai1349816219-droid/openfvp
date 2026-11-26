# openfvp

> **说明**
>
> 本项目由 **6amm** 辅助 DeepSeek 完成，其中所有代码由 DeepSeek 生成。
>
> 如果觉得项目还不错的话,可以通过 https://afdian.com/a/6ammm 向 6amm 赞助
>
> 以下内容由 DeepSeek 生成,不一定代表作者个人意愿

> 文档只描述**文件格式与可观察行为**（不带任何原版游戏数据）；措辞规范见 [CONTRIBUTING.md](CONTRIBUTING.md)。

**FVP（FAVORITE VIEW POINT SYSTEM）引擎的开源 C99 重实现。**

目标：让 FAVORITE 社的 FVP 视觉小说（《いろとりどりのセカイ HD》系列、《星空のメモリア》、
《アストラエアの白き永遠》等）能在现代平台运行，首要目标是 Android。

> **状态：1.0.0 正式版。** 引擎本体（C99）在桌面端跑得起来：窗口 / 渲染 / 音频 / 输入 /
> 存档文件层 / 协程槽 / 定时器 / 逐字显示都已落地，测试与文档齐备。
> 脚本虚拟机（VM）与冻结的 Python 基准逐字节对齐；
> 资源层（BIN / inflate / HZC1）、渲染层（gfx）都有全量验收数字；
> **已知限制（1.0.0 明确不做，不是"以后再说"的漂亮话）：** 游戏内存/读档（槽尾 500–597 KB
> 的引擎状态快照未解）、动画缓动曲线（`mode 1..5`）、WMV 视频、游戏内菜单/设置界面、
> 语音播放、Android 后端。逐条见下面的「能做什么 / 不能做什么」。

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
| 虚拟机指令集 | **40 个 opcode 全部实现**（栈、帧、全局/局部变量、temp）；**32 个协程槽已实现**（`ThreadStart`/`ThreadExit`/`ThreadNext`，默认开启），900k/2M 开关两档文本逐字节相同；虚拟时钟为可选最小实现（`--timeline`） | [`src/vm.c`](src/vm.c) · [`docs/c-vm.md`](docs/c-vm.md) §6.1 · [`docs/05-threads.md`](docs/05-threads.md) §10 |
| 命令表 | **148 条**命令的 id / 名字 / argc / 是否返回值，来自 `.hcb` 内嵌附加数据；本作实际用到 **131 条**（17 条未使用） | [`src/gen_cmd_table.h`](src/gen_cmd_table.h) · [`src/syscall.c`](src/syscall.c) · [`docs/00-engine.md`](docs/00-engine.md) · [`docs/vm-commands.md`](docs/vm-commands.md) |
| **C 版与 Python 版逐字节一致** | 900k 步：6,496 字节 / 195 行（`SHA256 7F9837FF…`）；2M 步：15,378 字节 / 450 行（`SHA256 32D7D4E2…`），完整哈希见 [`ref/REFERENCE.md`](ref/REFERENCE.md)；开发机上另跑过 20M 步与 123457 / 777777 / 1500000 步对照，均一致 | [`tests/compare.py`](tests/compare.py) · [`ref/ref-900k.txt`](ref/ref-900k.txt) · [`ref/REFERENCE.md`](ref/REFERENCE.md) |
| 资源层：BIN 包 / inflate / HZC1 | 12 个 `.bin` 包共 **21,725 条**索引/名字/偏移逐条一致；`graph.bin` **1,112/1,112** 个 zlib 流（**2,539,861,820 字节**）与 `zlib.decompress` 逐字节相同；**1,112 张图 / 3,654 帧 / 727,573,943 像素 0 差异**（fmt 0–4 全覆盖） | [`src/bin.c`](src/bin.c) · [`src/inflate.c`](src/inflate.c) · [`src/hzc1.c`](src/hzc1.c) · [`docs/c-assets.md`](docs/c-assets.md) · [`tests/verify_assets.ps1`](tests/verify_assets.ps1) |
| 音频层（**已接到 VM**） | 48 kHz 立体声 PCM16 统一格式；Ogg 实测 5,587,774 帧（116.41 s）、peak=19297；22.05 kHz WAV 重采样后时长 5.64→5.644 s。运行 900k 步触发 **4 次 BGM 播放**（`BGM2/002` 等），混音 peak **14004** / rms **2431.8**，解码失败 0 | [`src/snd.c`](src/snd.c) · [`docs/04-audio.md`](docs/04-audio.md) · [`tests/verify_audio.ps1`](tests/verify_audio.ps1) |
| 渲染层（Python 原型已验证，**未入库**） | 1920×1080 合成：背景 + 立绘 + 对话框 + 文字；90 万步里导出 12 帧 + 10 张剧情检查点 PNG。图层 / 图元 / 文本 / 合成模型与已知偏差成文 | [`docs/render-model.md`](docs/render-model.md)（原型在本地 `work/`，见下） |
| 渲染层（C，**已验收**） | 1920×1080 合成：图元 **24/24 位置误差 0 像素**；像素差异 **293 / 2,073,600 = 0.0141%**（PSNR 49.94 dB、SSIM 0.99971）；热帧 **9.0–9.5 ms** | [`src/gfx.c`](src/gfx.c) · [`docs/c-gfx.md`](docs/c-gfx.md) · [`tests/verify_gfx.ps1`](tests/verify_gfx.ps1) |
| 渲染 ↔ VM 接线（**已验收**） | `syscall` 直接驱动 gfx 后，跑 528,000 步 + 每帧渲染，与参考帧**每个数字与纯渲染基线完全相同**（0 像素 / 0.0141%）；同时 `fvm.exe` 输出**逐字节不变** | [`src/syscall.c`](src/syscall.c) · [`tests/render_vm.c`](tests/render_vm.c) · [`docs/c-gfx.md`](docs/c-gfx.md) §9 |
| Win32 桌面外壳（**已验收**） | 启动到窗口 **130–230 ms**、**60.00 fps**、GDI 上屏 avg 3.4–3.6 ms（慢帧 0）、waveOut 10 秒 underrun 0、点击注入被 `InputGetDown` 消费 | [`src/platform/win32/main_win32.c`](src/platform/win32/main_win32.c) · [`docs/win32-shell.md`](docs/win32-shell.md) |
| 存档层（**文件格式已实现**） | `save/sNNN.bin` 与 `save/save.bin` 的解析 + 重写：**6 个真实存档槽逐字节回环一致**；`save.bin` 解出 **1471 条**全局变量（**145 int + 846 未解 + 480 未设**）、200×113 缩略图尺寸、字体名 `メイリオ`；**全局变量桥**把 145 条 int 灌进 VM（`fvm --savebin`），**桌面外壳启动时自动读档**并播种全局变量与 256 B flag 位图；50 + 49 + 19 + 13 项断言含畸形输入拒绝 | [`src/save.c`](src/save.c) · [`src/save_vm.c`](src/save_vm.c) · [`docs/save-format.md`](docs/save-format.md) §7 · [`tests/verify_save.ps1`](tests/verify_save.ps1) · [`tests/verify_save_vm.ps1`](tests/verify_save_vm.ps1) |
| 全局 flag（**已实现**） | `FlagGet`/`FlagSet`（命令 18/19）读写 2048 bit / 256 B 位图，索引 0..0x7FF，越界安全；与 `save.bin` 里那段位图是同一张图（28 + 37 项断言） | [`src/flags.c`](src/flags.c) · [`docs/vm-commands.md`](docs/vm-commands.md) · [`tests/test_flags.c`](tests/test_flags.c) |
| **跨作品验证** | 第二部作品《星空のメモリア HD》同样跑通：命令表 **148 条逐条完全相同**、opcode 同套、VM **零修改**输出 164 行正确台词 | [`docs/06-cross-validation.md`](docs/06-cross-validation.md) · [`tools/fvpprobe.py`](tools/fvpprobe.py) |
| **原版真值对照**（部分完成） | 能启动官方 `*_HD.exe` 并用 `PrintWindow(PW_RENDERFULLCONTENT)` 抓真值画面；标题菜单几何量出：右对齐 x=1769、右边距 151 px、行距 100±1 px、墨迹高 40–42 px。**2026-11-25：绘制协议不再靠实拍** —— 从 exe 静态提取出 `CreateFontA(SHIFTJIS_CHARSET)` + `GetGlyphOutlineA(GGO_BITMAP 1bpp)` + 单位 `MAT2`、exe 内置的 4 个字体名，冻结为 `ref/exe-gdi-params.json`；缺字回退可用 `--glyph-original` 按同协议栅格化 | [`docs/original-verification.md`](docs/original-verification.md) · [`scripts/extract-exe-gdi.py`](scripts/extract-exe-gdi.py) |
| 健壮性测试 | **27,300 次** HCB 变异（0 崩溃 / 0 越界）、**339 个**资产畸形用例、**43 个** VM 边界用例；`tests/run_all.py` 一键 **45 PASS / 0 FAIL / 1 SKIP**（约 275 s；SKIP 是缺 numpy 的 `verify_assets`），共 **46 步**；`make check` **30 组**单测全 PASS（含原版协议 28 项） | [`docs/testing.md`](docs/testing.md) · [`tests/run_all.py`](tests/run_all.py) |
| 中文脚本重建（方法已验证） | 被汉化补丁打坏的 GBK 脚本重建为 3,712,944 字节、948,219 条指令、命令表与冻结表一致的结构合法 `.hcb`（产物不入库） | [`docs/cn-script-rebuild.md`](docs/cn-script-rebuild.md) |
| Android 外壳（设计，进行中） | APK 流水线 / JNI / EGL 原生窗口 / 触摸 / 资产目录的设计（非当前主线）；外壳不在本仓库 | [`docs/android-shell.md`](docs/android-shell.md) · [`docs/roadmap.md`](docs/roadmap.md) |

「逐字节一致」的含义：C 实现不是「看起来跑得对」，而是把脚本跑 N 步后导出的每一行文本
（`[层] 文本`，含 CRLF）与冻结的 Python 基准输出做 **raw byte** 比较，第一个不同字节的偏移会被打印出来。
没有游戏数据时这个测试会显式输出 `SKIPPED`，而不是假装通过 —— 见
[`scripts/ci-compare.py`](scripts/ci-compare.py) 与 [`.github/workflows/ci.yml`](.github/workflows/ci.yml)。

## 现在还不能做什么

按影响从大到小，这些都是**已知未完成**，不是待办里的漂亮话：

1. **游戏内 UI 还是占位。** 渲染层与桌面外壳都通过了像素级验收，但对话框仍是画的圆角框
   （游戏自己的 `main_window_mask` 贴图已解出、脚本也把它挂在 36 号图元上，但没有证据表明
   它就是对话框外框，所以没强行接）；没有菜单、没有存档界面、没有设置页。
   动画（`Motion*`）有补间骨架（`--motion` 可开）；`mode 1..5` 的缓动曲线**已从静态分析确认**
   并落地（`fvp_motion_ease()`，`mode 4/5` 的后半段是推导，见
   [`docs/animation-baseline.md`](docs/animation-baseline.md) §2.3）。
2. **命令语义仍有缺口，但清单是准确的。** [`src/syscall.c`](src/syscall.c) 里渲染相关的命令已接 gfx，
   动画接补间表，音频接 snd，定时器接 `src/timer.c`，台词历史接 `src/history.c`
   （`HistorySet`/`HistoryGet` 的 100 条环形缓冲，`fvm --dump-history` 可导出）。外壳 90 秒 /
   实测**只剩 15 种**未实现命令（45 s / 240,201 步），且都不再是文本/几何核心项 ——
   具体表在 [`docs/roadmap.md`](docs/roadmap.md)，
   `PrimHit` 的完整算法已解出但**还没落地**（[`docs/render-model.md`](docs/render-model.md) §8）。
   `Movie*`（视频）与 `V3D*` 仍是桩。`MenuMessSkip`/`SysAtSkipName`/`TitleMenu` 已证明原版
   就是空操作（handler 0x437950 = `ret $0x4`）。
3. **协程已实现，但本作的协程体被脚本自己关掉了。** 32 个协程槽
   （`ThreadStart`/`ThreadExit`/`ThreadNext`）已按静态分析语义接上（默认开启，`--no-threads` 可关），
   900k / 2M 两档的开/关文本**逐字节相同**，也与冻结基准相同 —— 因为这份脚本里每个协程入口
   都是 `initstack; pushint8 0; jmpcond <ret>`，条件是**编译期常量 0**，协程建起来就退出。
   所以「约 50 句台词缺失」这个旧结论**不成立**（已据此改正文档）。
   16 个定时器（`TimerSet`/`TimerGet`/`TimerSuspend`）已实现，elapsed 由虚拟时钟推导
   （`--timeline` 默认关闭，所以计时恒 0 —— 与旧行为一致）。
   细节与实测表在 [`docs/05-threads.md`](docs/05-threads.md) §10。
4. **音频播放已经通了，但音画同步与音量体系还没做。** 脚本里的 `AudioPlay` 现在会真的解码并混音
   （900k 步触发 4 次 BGM），但语音路径依赖存档层给的 voice id（原型里没有），
   音量主控/分组的配置层未接，也没有 A/V 同步与淡入淡出。
5. **视频（WMV 开场）直接跳过。** `Movie` / `MovieState` 被实现成「立即播放完毕」，
   否则脚本会卡在等待循环里。
6. **中文版不能当成品跑。** 重建脚本结构合法、VM 可执行，但没有渲染/音频，也没有逐场景校对。
7. **只验证过两部作品。** `いろとりどりのセカイ HD Re:GENERATION` 与
   《星空のメモリア HD》都跑通了（命令表 148 条零差异），并且有**自动化门禁**
   [`tests/verify_multigame.py`](tests/verify_multigame.py)：两部作品各跑两遍 90 万步，
   要求同一次运行输出逐字节相同、文本行数不漂移（iroseka 195 行 / 6,496 B、
   星空のメモリア 164 行 / 6,644 B）、0 个替换字符。但更老的 FVP 版本
   （2004–2008 年的作品）**未测试**，格式是否仍然一致未知。
8. **4 个 opcode 语义未确认**（`0x11` / `0x12` / `0x17` / `0x18`，占本作指令 0.18%），
   按数组读写实现，未做定向验证 —— 见 [`docs/roadmap.md`](docs/roadmap.md)。
9. **存档只做到文件层。** `save/sNNN.bin` 与 `save/save.bin` 能完整解析并原样重写
   （6 个真实存档逐字节回环一致），但槽文件里那段引擎状态快照（500–597 KB）的字段语义
   还没解完，所以**游戏内还不能真的存档/读档**。2026-11 查明：脚本的 `SaveData`(95)
   只打三个已读标记（`kind=2 idx=900/903/904`），快照是 `0x43DE90` 直接序列化引擎内部结构
   —— 见 [`docs/save-format.md`](docs/save-format.md) §2.5。
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
make win32      # 构建 bin/fvp.exe（Win32 桌面运行器）
make check      # 构建并运行 23 个不需要游戏数据的单元测试（CI 也跑这个）
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
├── VERSION                版本号（1.0.0）
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
4. 补 `Timer*` 与真正的帧级调度（协程槽已实现，见 [`docs/05-threads.md`](docs/05-threads.md) §10；缺的是计时器与 `ThreadWait` 的时间语义）；
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

What works today (v1.0.0):

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