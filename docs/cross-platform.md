# 跨平台（OS 抽象层）

> 以下内容由 DeepSeek 生成,不一定代表作者个人意愿
> 本项目由 **6amm** 辅助 DeepSeek 完成，其中所有代码由 DeepSeek 生成。

> 以下内容由 DeepSeek 生成,不一定代表作者个人意愿

本项目目前只有 **Win32 桌面外壳**。这一页说明"跨平台"到底缺什么、现在已经抽出
哪一层，以及新增一个平台要做什么。

## 1. 已经抽出的最小缝：`src/platform/platform.h`

引擎核心（`vm.c` / `syscall.c` / 存档 / 文本 / 纯逻辑）不应当碰 `<windows.h>`。
与操作系统打交道的最小集合只有四件事：

| 函数 | 用途 | 便携参考实现 |
|---|---|---|
| `fvp_plat_now_sec()` | 单调时钟（秒），帧间隔/计时 | `clock()` |
| `fvp_plat_ticks_ms()` | 单调时钟（毫秒） | `clock()` |
| `fvp_plat_log(const char *)` | 写一行 UTF-8 日志 | `stderr` |
| `fvp_plat_read_file(path, **data, *len)` | 读整个文件到 malloc 缓冲 | `fopen`/`fread` |

- 契约：`src/platform/platform.h`
- 参考实现：`src/platform/portable/platform_portable.c`（**只用 C 标准库**）
- 单测：`tests/test_platform.c`（16 项：时钟单调、日志空指针安全、文件读入的长度/
  内容、缺失文件与 NULL 参数的错误路径）。`make check` 里包含它。

这层是"结构上可移植"的证据：单测与便携实现都不含任何 Win32 头文件。

## 2. 还没抽出的部分（都是平台强相关，由外壳直接实现）

| 能力 | 现在在哪 | 新平台要做什么 |
|---|---|---|
| 窗口 / 消息循环 / 输入 | `src/platform/win32/win32_window.c` | 新建窗口与输入层，把鼠标/键盘映射成 `FvpWin32Input` 同构的结构 |
| 渲染输出（D3D9） | `src/platform/win32/main_win32.c` | 把 `gfx_canvas()` 的 1920×1080 RGBA 画布贴到自己的表面 |
| 音频输出（waveOut/WASAPI） | `src/platform/win32/win32_audio.c` | 接 `snd_mix()` 的输出，48 kHz 立体声 PCM16 |
| 缺字回退（GDI） | `src/platform/win32/win32_gdi_text.c` | 用本平台的字体栅格化产出覆盖率掩码（接口是 `GfxGlyphFallback`） |
| 日志落文件、路径拼接 | `src/platform/win32/win32_log.c` | 按平台习惯实现；`platform.h` 只定义"写一行"的语义 |

渲染层与音频层本身是平台无关的 C（`src/gfx.c`、`src/snd.c`），外壳只是消费者——
这也是为什么 `tests/stub_gfx.c` 能替换真实渲染层跑窗口自检。

## 3. 一个平台外壳的最小构成

```
核心（src/*.c，平台无关）
 ├── gfx.c / snd.c / motion.c        —— 纯逻辑与混音/合成
 ├── vm.c / syscall.c / save*.c      —— 脚本、命令、存档
 └── platform/platform.h             —— 四个系统能力（本页第 1 节）
平台外壳（src/platform/<平台>/）
 ├── 窗口 + 输入 + 帧循环
 ├── 画布上屏（gfx_canvas 的结果）
 ├── 音频输出（snd_mix 的结果）
 └── 字体栅格化（可选，缺字回退用）
```

## 4. 无窗口（headless）外壳：`make headless`

除了 Win32 桌面外壳，现在还有一个**不依赖任何 Win32 API** 的外壳：

```
make headless                     # → bin/fvp-headless.exe
bin/fvp-headless.exe <script.hcb> --steps 200000 \
    --screenshot out/headless/frame.bmp --text-out out/headless/text.txt
```

- 只用 `src/platform/platform.h` 的四个系统能力（`src/platform/portable/platform_portable.c`）；
- 加载 `.hcb` → 初始化 VM → **挂上可移植渲染层 `src/gfx.c`**（离屏 1920×1080）；
- 跑 N 条指令后把画布写成 24 位 BMP，文本行通过 `gfx_bridge->on_text` 回调落文件；
- 没有窗口、没有输入、没有音频——真正的桌面体验仍由 `src/platform/win32` 提供。

**门禁 `tests/verify_headless.py`**（`run_all` 第 6.6 步）断言：

| 检查 | 实测 |
|---|---|
| `make headless` 能构建 | exit 0 |
| 步数与 `fvm` 一致 | 200000 vs 200000 |
| 截图 1920×1080 且非全黑 | 平均亮度 34.8 |
| 文本回调有产出 | 32 行 |

它的意义不是"再做一个外壳"，而是把 **⑦ 跨平台** 从口号变成可验证的事实：核心 +
渲染层可以在没有窗口系统的情况下跑起来，CI/无桌面环境也能做冒烟。

## 4. 现状与下一步

- ✅ 最小 OS 缝有契约、有便携实现、有 16 项单测；
- ✅ 核心库 `make lib` 不依赖 Win32；
- 🚧 窗口/渲染/音频/字体仍只有 Win32 实现；**Android 方案**见 `docs/android-shell.md`
  （占位渲染，未接真实窗口）；
- ✅ 无窗口 headless 外壳已落地（`make headless` + `tests/verify_headless.py` 门禁）；
  可继续把这条路径接进 CI（下一步是写 CI 配置，本仓库暂未提交 CI 文件）。