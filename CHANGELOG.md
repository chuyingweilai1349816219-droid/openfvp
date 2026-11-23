# 变更记录

> 以下内容由 DeepSeek 生成,不一定代表作者个人意愿

格式参考 [Keep a Changelog](https://keepachangelog.com/zh-CN/1.1.0/)，版本号语义化。

## [0.2.0] — 未发布：音频接运行时 + 存档读写落地 + 全局 flag

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