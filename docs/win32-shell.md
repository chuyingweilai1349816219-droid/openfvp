# Win32 桌面外壳（fvp.exe）

> 以下内容由 DeepSeek 生成,不一定代表作者个人意愿

给 C 版 FVP 引擎做的桌面运行器：Win32 窗口 + 固定 60 Hz 帧循环 + 鼠标/键盘输入 +
`waveOut` 音频输出。目标是「双击就能跑游戏」，不是调试工具。

外壳**一行都不改** `fvp/src/` 下的引擎代码：它只用 `hcb.h / vm.h / snd.h` 的公开接口，
以及渲染层 `gfx.h` 的 `gfx_create / gfx_render / gfx_canvas`。所有平台代码都在
`fvp/src/platform/win32/`。

```
fvp/src/platform/win32/
    main_win32.c    入口、命令行、脚本/编码探测、VM 时间片、帧循环、文本节奏、统计
    win32_window.c 窗口、DPI、输入、GDI 呈现（BGRA DIB + BitBlt/StretchBlt）
    win32_window.h
    win32_audio.c   waveOut 多缓冲 + 专用填充线程（snd_mix 或自检音）
    win32_audio.h
    win32_log.c     UTF-8 日志文件（+- 父控制台），线程安全
    win32_log.h
    gfx_iface.h     渲染层接口；有 src/gfx.h 时直接用，否则用契约镜像
fvp/tests/stub_gfx.c    渲染层未落地时的测试图案（只在 GFX_FORCE_STUB=1 时编译）
```

---

## 1. 构建

```powershell
# 把自带编译器的目录加进 PATH（w64devkit，自带 Win32 头文件，没有 zlib）
$env:PATH = "C:\...\tools\w64devkit\w64devkit\bin;$env:PATH"

cd fvp
mingw32-make win32          # 产物 bin\fvp.exe
```

- `src/gfx.c` 存在时自动编进壳里（当前状态）；不存在时自动退回
  `tests/stub_gfx.c` 的测试图案。
- 想看渲染层没落地时的测试图案：`mingw32-make -B win32 GFX_FORCE_STUB=1`
  （`-B` 是必须的，Make 不会因为变量变化重链接）。
- 只追加了 `win32` 目标，`make / make lib / make test / make gfx / make test_gfx`
  的语义和输出没有变化（已在 2026-11-23 全部重新构建验证）。

---

## 2. 用法

```
fvp.exe --game <游戏目录> [选项]
```

不带参数启动时会依次尝试：exe 所在目录、`<exe>\game`、`<exe>\..\game`、当前目录里
有没有 `iroseka.hcb`；都没有就弹出目录选择框。**把 `fvp.exe` 放进游戏目录后双击**即可
（不需要命令行参数）；别处双击时会让你选游戏目录。

| 选项 | 说明 |
|---|---|
| `--game DIR` | 游戏数据目录（找 `iroseka.hcb`，没有就找目录里第一个 `.hcb`） |
| `--script NAME` | 脚本文件名，默认 `iroseka.hcb`；相对路径按游戏目录解析 |
| `--enc MODE` | `cp932` / `gbk` / `auto`（默认 auto） |
| `--scale auto\|1\|2` | 窗口逻辑尺寸：auto/1 = 1280×720，2 = 2560×1440（超桌面工作区会自动缩） |
| `--filter nearest\|bilinear` | 缩放过滤。默认 nearest 保 60fps；bilinear 更平滑但 GDI 会慢很多 |
| `--fullscreen` | 启动即全屏（游戏中 F11 切换） |
| `--vm-steps N` | 每帧 VM 指令预算，默认 40000（只在没有等文本/等点击时用满） |
| `--text-pace auto\|off\|毫秒` | 每句文本停留时间；auto 按字数估算，off = 全速 |
| `--no-audio` | 不打开音频设备 |
| `--audio-tone HZ` | 自检正弦音（如 440），用来验证 waveOut 通路 |
| `--audio-buffers N` | waveOut 缓冲区数，默认 3 |
| `--run-seconds S` | 运行 S 秒后自动退出（回归测试用） |
| `--screenshot FILE` | 退出时把最后一帧画布写成 32 位 BMP |
| `--thumb-shot FILE` | 退出时把最后一帧按 **200×113** 缩成存档缩略图 BMP（`src/thumb.c` 的 box filter，真存档里是裸 BGRA） |
| `--save-template FILE` | 拿一个**真实存档槽**当模板（与 `--save-preview` 配对使用） |
| `--save-preview FILE` | 退出时写出「模板 + 当前本地时间 + 当前画面缩略图」的存档槽（字符串段沿用模板） |
| `--save-preview-every N` | 运行中每 N 秒写一次预览，文件名追加 `.1/.2/…`（相当于「按 F5 存档」的自动化版） |
| `--save-encoding ENC` | 存档字符串编码：`cp932` / `gbk`（默认跟随脚本编码）。同一句日文写成 `83 76 83 8d …` 或 `a5 d7 a5 ed …` |
| `--font FILE` | 加载 GFXF 字体图集，可重复；默认自动找 exe 目录的 `gfx-font-*.bin` |
| `--text-prims` | 文本窗口画在 `PrimSetText(86)` 绑定的**图元绝对位置**上（原版做法），不再按层号固定版式画。默认关 —— 关时与冻结像素基线一致。汇总多一行 `text prims N drawn, M bound` |
| `--save-template <槽文件>` | 写档模板：SaveWrite 请求用它当底，替换时间戳/字符串/缩略图后写 `<game_dir>\save\sNNN.bin` |
| `--debug-save-request N` | 调试钩子：发一个 `SaveWrite(N)` 请求，验证"请求 → 外壳写档"链路 |
| `--debug-load-request N` | 调试钩子：发一个 `Load(N)` 请求（目前只记日志，快照恢复未实现） |
| `--title-font <图集>` | 标题菜单文字用的字库（如 Palatino Bold 61 的
  `bin/gfx-font-61.bin`）；不传则用内建位图字体 |
| `--title-fade N` | 点 New Game 后的白场过渡时长（ms，默认 800；0 = 直接进） |
| `--pick N` | 把标题菜单第 N 项当成"已被选中"（截图/门禁用） |
| `--menu-pick N` | 菜单打开后自动激活第 N 项（截图/门禁用） |
| `--menu-at N` | 脚本开始跑 N 毫秒后自动打开游戏内菜单（截图/门禁用） |
| `--load-slot N` | 启动就进入第 N 号槽的"部分恢复"视图（整屏背景+台词+标注） |
| `--preview-slot N` | 存档列表预选第 N 号槽并显示预览（槽尾记的背景图 + 台词） |
| `--no-input` | 忽略真实鼠标/键盘事件：门禁/截图只由程序状态决定（消除偶发） |
| `--opening` | 启动先跑开场序列（白底 → FAVORITE logo 淡入淡出 → 标题从白色淡入，5.4 s） |
| `--shot-at N` | 跑到 N 毫秒时存一张截图（开场/淡入的中间态；结尾不再覆盖它） |
| `--title-menu` | 启动后先显示**引擎侧标题菜单**（`New Game/Continue/Album/Option/Exit`），点 `New Game`（或回车）才开始跑脚本，`Exit` 关窗口。默认关 —— 关时行为与之前完全一致。菜单几何按原版实测（右对齐 x=1769、行距 100、字号 56），见 [`docs/title-menu.md`](title-menu.md)。汇总多一行 `title menu enabled (profile=..., align=..., continue=..., picked ...)`；`continue=` 由 `<game_dir>\save\sNNN.bin` 的个数决定 |
| `--no-feed-text` | 关闭壳的临时文本桥（等 syscall 层接好 gfx 后应关闭） |
| `--log FILE` | 日志路径，默认 `<exe目录>\fvp-win32.log`（不可写时退到 %TEMP%） |
| `--console` | 同时输出到启动它的控制台（GUI 子系统程序默认没有 stdout） |
| `--startup-trace FILE` | 把启动各阶段（entry / dpi / com / 命令行 / 日志+脚本+VM / 音频 / gfx / 窗口 / ready）写进 FILE 并立即 flush。**日志是在解析参数之后才打开的**，所以程序若在"进程起来但还没开日志"之间卡住（驱动钩子、注入 DLL 等），只有这个开关能看到卡在哪一步 |
| `--verbose` | 更详细日志（参数、每 5 秒鼠标/等待状态等） |

### 按键

| 输入 | 行为 |
|---|---|
| 鼠标左键 | 确定 / 推进对话（按住会以 ~8 次/秒连发，等同 VN 的快速推进） |
| Enter / 空格 | 同鼠标左键 |
| 标题菜单里的 ↑ / ↓ | 上下选择菜单项（跳过置灰项、到边界环绕） |
| 标题菜单里的 Enter | 确认当前项（等价于点击） |
| 存档列表里的 ↑ / ↓ / Enter | 选择槽 / 确认（只读，目前只记日志） |
| 存档列表里的 Esc | 回到标题菜单（标题菜单里 Esc 仍是退出） |
| 鼠标移动 | 换算回 1920×1080 画布坐标，写入 `InputGetCursX/Y` |
| 滚轮 | 记录到日志，**并累加到 VM**；脚本用 `InputGetWheel`（命令 36）取走（120 = 一格） |
| F11 | 无边框全屏 / 还原（记住窗口位置） |
| ESC | 退出（干净关闭：停音频线程 → 销毁 gfx/窗口 → 释放 VM/HCB） |
| 关闭按钮 | 同 ESC |

---

## 3. 实测数据（本机：2560×1600 @150% DPI，Windows 11）

游戏：`C:\Users\Mechrevo\Downloads\五彩斑斓的世界HD\iroseka.hcb`（cp932，4.27 MB，
948,219 条指令）。

| 指标 | 结果 |
|---|---|
| 启动到窗口就绪 | **~130–230 ms**（脚本加载 40–60 ms、waveOut 打开 ~20 ms、窗口 ~50 ms；机器在跑大文件解压时会慢一些） |
| 启动到第一帧上屏 | ~140–240 ms（加载 3 个字体图集时再 +60 ms 左右） |
| 帧率 | **60.00 fps**（4 s / 240 帧；5 s / 300 帧；6 s / 360 帧都稳在 60.0；10 s 那次因后台解压有 1 个 193 ms 尖峰，平均 58.3） |
| 帧间隔 | avg 16.60–16.62 ms，max 17.57 ms（1 ms 定时器 + 最后 1.5 ms 自旋等待） |
| GDI 上屏耗时 | **avg 3.4–3.6 ms，p95 4.5–5.5 ms，max ~9 ms，>20 ms 的慢帧 0 个** |
| 画布拷贝（RGBA→BGRA） | ~2.4–3.0 ms/帧（1920×1080 32 位整数循环） |
| 音频 | `waveOut` 48000 Hz / 2ch / PCM16 / 3×480 帧（30 ms），10 秒提交 759 个缓冲、完成 756、underrun 0 |
| 退出 | ESC / 关闭按钮 / `--run-seconds` 都是退出码 0，无残留进程 |

### 启动时读取全局存档

外壳在 `fvp_vm_start()` 之前会找 `<game>/save/save.bin`（退一步找 `<game>/save.bin`），
命中就解析并把其中**已确证的 `type==2` 整型全局变量**灌进 VM；其余段落只报数不解释
（`docs/save-format.md` §7.1）。实测日志：

```
[    0.025] save: C:\Users\Mechrevo\Downloads\五彩斑斓的世界HD/save/save.bin ->
            145/1471 globals applied (145 int, 846 unknown type1, 480 unset),
            0 flags bit(s) set, thumb 200x113, readmap 1048576 bytes
```

找不到存档时打一行 `save: no save/save.bin under the game directory (starting from
defaults)`，不阻塞启动；文件损坏时打 `save: ... is not a valid save.bin (ignored)`。

> 测试机当时在后台解压一个 15 GB 压缩包，偶尔出现 100–200 ms 的整帧尖峰；同一时刻
> `gdi present` 的 max 仍只有 ~9 ms，说明尖峰来自系统负载/调度，不是壳里的 GDI 抖动。
> 空闲桌面下连续 4–6 秒的跑分都是 60.00 fps、0 个慢帧。

窗口尺寸：逻辑 1280×720 在 150% DPI 下是物理 1920×1080，画布正好 **1:1 上屏**
（无缩放、无采样损失）；F11 全屏时 1920×1080 → 2560×1600 用最近邻放大，仍 60 fps
（bilinear 全屏会掉到 ~50 fps，故不是默认）。

---

## 4. 帧循环做了什么

```
每帧（固定 1/60 s）：
  PeekMessage/DispatchMessage          # 窗口/输入消息
  vm.cx/cy = 画布坐标；按键/滚轮 → 待处理点击
  若在等文本停留 → 本帧不推进 VM
  否则推进 VM（最多 --vm-steps 条）：
      逐条 fvp_vm_run()，遇到 TextPrint 立即停（保证一次只出一句）
      遇到 InputGetDown 轮询且有待处理点击 → 只在那一条指令里把
      vm->click_period 置 2（syscall 桩返回"按下"），然后立刻复位
      连续 96 次 InputGetDown 轮询 → 认定在等点击，提前结束本帧
      TextPrint → 记录日志 + 可选喂给 gfx（临时文本桥）+ 设置停留时间
  gfx_render()
  gfx_canvas() → 转成 BGRA DIB → BitBlt/StretchBlt 到窗口
  每 0.5 s 更新窗口标题 FPS；每 5 s 写一行 perf/vm/audio 日志
```

要点：

- **VM 是成段时间片推进的**，不是跑到底：脚本里大量 `*Test` / `InputGetDown` 等待，
  一次跑完会瞬间冲到脚本末尾。
- **点按是边沿触发**：引擎的输入 syscall 目前是桩（`InputGetDown` 只在
  `vm->click_period` 非 0 时返回 1），壳在"下一条指令就是 InputGetDown"时精确置位一条
  指令，等于把真实鼠标按下变成脚本能看到的单次点击，不会因为忙等而连点。
- **文本节奏**：卡在 `TextPrint` 后按字数停留（层 8 人名 0.35 s、层 9 标题 1.8 s、
  正文 700 ms + 26 ms/字，钳 0.9–4.5 s）。这是壳层的近似，因为引擎的
  `TextSpeed/TextPause` 等排版命令还没实现；`--text-pace off` 可关掉。
- **文本桥（临时）**：`vm->text[]` 里新出现的行会 `gfx_text_clear + gfx_text_append`。
  它只是让画面在 syscall 层接好之前能显示当前台词；等 `GraphLoad/PrimSet*/Text*`
  接到 gfx setter 之后应加 `--no-feed-text`（或删掉这段）。
- **音频线程**：专用线程等 `CALLBACK_EVENT`，对完成的 `WAVEHDR` 反准备→`snd_mix`
  →再准备→再入队；`snd` 只在这个线程里被调用，避免与 VM 线程竞争。

---

## 5. 已知限制

1. **游戏画面还没接上**：`syscall.c` 里 `GraphLoad / PrimSet* / Parts* / Text*` 仍是桩，
   没人调用 gfx 的 setter，所以真渲染层下画布只有背景 + 文本窗口（喂 `--font` 后可见台词），
   没有背景图/立绘。这是渲染→syscall 接线的问题，不是壳的问题；壳每帧该做的都做了。
2. **文本需要字体图集**：gfx 内置只有 8×16 ASCII 字体，CJK 会画成缺字方框（豆腐块）。
   图集必须按**目标作品用到的字**生成：`scripts/make-font-atlas.py` 用
   `ref/fvm_ref.py` 精确解出 HCB 里所有 `pushstring` 操作数、取两作并集，再调用
   `tests/gfx_dump.py::write_font()`（同一 TTF、同一 margin、glyph/stroke1/stroke2
   三层覆盖图）写出 `bin\gfx-font-<size>.bin`：

   ```
   python fvp/scripts/make-font-atlas.py \
       --hcb "<五彩斑斓的世界HD>\iroseka.hcb" \
       --hcb "<hoshimemo_installed>\Hoshimemo_HD.hcb" \
       --size 30 --size 32 --size 34 --size 56 --out fvp/bin
   ```

   尺寸不是随便挑的：`gfx_layout_default()` 里正文层 34、人名层 34、标题层 56，
   其它层默认 **30**，正文层还会用 `TextSize` 给的高度（本作实测 32）。
   **缺任何一档，那一层就会退回内置 8×16 字体 → 该层的日文全是豆腐块**
   （2026-11-24 实测：《星空的记忆 HD》的层 6 用 30 px，当时只生成了 32/34/56，
   开场就有 8 个豆腐块；补上 30 之后两作都是 `glyphs 0 tofu`）。
   图集只包含生成时给的字符集，换作品/换语言要重新生成；`bin\` 与 `out\` 都在
   `.gitignore` 里，所以**图集不进仓库**，只进生成脚本。
   排查缺字时设环境变量 `FVP_DEBUG_TOFU=1`：日志会多出
   `glyphs missing U+XXXX …`，还能在 stderr 看到每处豆腐块用的是哪套字体
   （`builtin=1` 表示整层退回了内置字体）。
3. **滚轮已接**：壳把累加量交给 VM，`InputGetWheel`（36）取走并清零（`tests/test_wheel.c` 有 8 项用例）；缺的是「原版引擎取走时机的对照验证」，需要脚本真的调用它。
   同理键盘只有 Enter/空格（= 左键）有语义，其它键不转发。
4. **音频目前是静音的**：waveOut 通路、缓冲循环、`snd_mix` 都验证过在跑
   （`--audio-tone 440` 能出声），但 `syscall.c` 还没有调用 `snd_play_*`，所以游戏声音
   要等音频命令接进来才会响。没有声卡时 `waveOutOpen` 失败会记一行日志并静音继续，不崩。
5. **缩放质量**：默认最近邻（60 fps 优先）。`--filter bilinear` 用 GDI HALFTONE，
   2/3 缩小约 14 ms/帧、1080p→1600p 放大约 40–60 ms/帧，会掉帧，只在 1:1 或
   配置很高的机器上建议用。
6. **只跑一个脚本**：没有标题菜单/存档/读档/多脚本选择，也不处理 `.wmv` 电影
   （引擎里 Movie 是空实现）。存档目录、字体安装等原版行为都没有。
7. **一次只能开一个实例**：没有单实例互斥，双击两次会开两个窗口。
8. **双击不带参数**时依赖目录探测或目录选择框；放在游戏目录里最省事。
9. 启动日志/截图写在 exe 目录或 `--screenshot` 指定处；游戏目录只读，不写任何文件
   （`save/` 存档等由引擎 syscall 负责，目前是空实现）。

---

## 6. 验证方法（可复现）

```powershell
$env:PATH = "<w64devkit>\bin;$env:PATH"
cd fvp
mingw32-make -B win32
$game = "C:\Users\Mechrevo\Downloads\五彩斑斓的世界HD"

# 60 秒稳定性 + 汇总
.\bin\fvp.exe --game $game --run-seconds 60 --log run.log
# 看 run.log 末尾的 "=== win32 shell summary ==="

# 音频自检（听 440 Hz，同时日志里 submitted/completed 持续增长）
.\bin\fvp.exe --game $game --audio-tone 440 --run-seconds 5 --log tone.log

# 测试图案（渲染层未接 syscall 时验证窗口/输入/缩放）
mingw32-make -B win32 GFX_FORCE_STUB=1
.\bin\fvp.exe --game $game --screenshot canvas.bmp

# 带字体看台词
.\bin\fvp.exe --game $game --font out\c\gfx-font-32.bin --font out\c\gfx-font-30.bin `
              --font out\c\gfx-font-34.bin --run-seconds 6 --screenshot canvas.bmp
```

日志里值得关注的几行：

```
window: created 1920x1080 physical client (1280 x 720 logical, canvas 1920x1080, dpi 144, filter=0)
startup: script+vm+gfx+window ready in 131.2 ms
startup: first frame presented 140.6 ms after process start (render 4.37 ms, present 3.81 ms = fill 2.37 + stretch 1.24 + blit 0.00)
input: click edge at canvas (900,200), steps=41603
input: click consumed by InputGetDown at steps=43177
run            5.84 s, 351 frames (60.10 fps)
frame pacing   avg 16.62 ms, max 31.99 ms (target 16.67 ms)
gdi present    avg 3.55 ms, p95 9.00 ms, max 11.44 ms, slow(>20ms) 0
audio          ok=1 submitted=381 completed=378 underruns=0
exit           window/ESC
```

`perf: slow frame dt=...` 会在帧间隔 >30 ms 时单独打印，便于确认卡顿是不是壳造成的。

截图（本次验证产物）：

- `fvp/out/win32-window-0-idle.png` — 测试图案 + 窗口缩放 + 标题 FPS
- `fvp/out/win32-window-1-click.png` — 鼠标点击后准星移到画布 (900,200)
- `fvp/out/win32-window-2-fullscreen.png` — F11 全屏 2560×1600，16:9 画布上下留黑边
- `fvp/out/win32-real-canvas-text.png` — 真渲染层 + 文本桥 + 字体图集：对话框里是
  VM 实际打印的台词 `……羽根だ。`

---

## 7. 排错

| 现象 | 查什么 |
|---|---|
| 双击没反应/弹"找不到 .hcb" | 游戏目录里要有 `iroseka.hcb`；或命令行加 `--game` |
| 文本是方框/空白 | 没加载字体图集：加 `--font ...`，并确认字号 32/34/30 |
| 画面全黑 | 正常：syscall 层还没接 gfx setter（见限制 1）；用 `GFX_FORCE_STUB=1` 看测试图案 |
| 没有声音 | 引擎还没调用 `snd_play_*`；用 `--audio-tone 440` 验证设备通路 |
| 日志在哪 | 默认 `bin\fvp-win32.log`（exe 目录）；`--log` 可改 |
| 想接控制台看输出 | 加 `--console`（从控制台启动时有效） |
| `make win32` 没反应 | `bin\fvp.exe` 已是最新；改编译宏时用 `-B` |
| 全屏掉帧 | 用默认 `--filter nearest`；或让窗口尺寸与画布 1:1 |

## 存档/读档请求（Load 40 / SaveWrite 96）怎么走

`Load`/`SaveWrite` **本身不碰文件**（原版也只是置 `engine+0xfcb/fca` 两个标志 +
槽号）。本站照这个模型：VM 记录请求，**外壳**在执行帧里消费：

| 请求 | 外壳行为 |
|---|---|
| `SaveWrite(slot)` + 给了 `--save-template` | 用模板当底，把**当前画布缩略图**、脚本暂存的 3 条字符串（`SaveCreate`）、**当前时间**写进 `<game_dir>\save\sNNN.bin`；日志 `save: wrote slot N -> <路径>` |
| `SaveWrite(slot)` + 没给模板 | **拒绝写**：`save: slot N requested but --save-template was not given (refusing to invent a snapshot)`（槽尾语义未解，硬造只会写坏档） |
| `Load(slot)` | 只记日志：`load: slot N requested; snapshot restore is not implemented yet`（`docs/save-snapshot.md` §4） |

汇总行会多一句 `save/load … shell did N write(s), refused M load(s)`。

**实测**（2026-11-25，`out/savetest` 临时目录）：
`--debug-save-request 12 --save-template <真槽>` → 写出 616,572 B 的 `s012.bin`，
时间戳更新为当前时间、缩略图换成当前画面、尾段与模板一致（`work/probe_slot.exe` 可读懂）。
`--debug-save-request 7` 不带模板 → 只打拒绝日志、**不落任何文件**。
这条"不写坏档"的红线也进了多游戏门禁。

> **2026-11-25 行为变化**：`ESC` 不再直接退出窗口（`capture_nav` 常开）。游戏中按 ESC 打开游戏内菜单，退出请用菜单里的 `QUIT`；标题菜单里 ESC 仍然回退/关闭列表。
