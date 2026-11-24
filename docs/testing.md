# FVP 健壮性（畸形输入）测试

> 以下内容由 DeepSeek 生成,不一定代表作者个人意愿

本目录文档说明 `fvp/tests` 下新增的畸形输入测试套件：它回答的问题是
**「一个坏文件能不能把引擎搞崩？」**——现有验收（`compare.py`、
`verify_assets.py`）保证的是「正常文件逐字节正确」，这里保证的是
「畸形文件不崩溃、不越界、不挂死，且错误可诊断」。

- 引擎源码、`Makefile`、已有测试文件、`fvp/work/` 与游戏目录均**未修改**。
- 新增内容全部在 `fvp/tests/`（新建文件）、本文档、以及运行产物 `fvp/out/fuzz/`。
- 所有用例可复现（固定种子），失败时可从 journal 找回并重建那一个变异体。

---

## 1. 测试矩阵

| 层 | 测试 | 驱动方式 | 输入 / 规模 | 断言 |
|---|---|---|---|---|
| HCB 加载器 + VM | `fuzz_hcb.c`（→ `fuzz_hcb.exe`） | 进程内，链接 `src/{fvp,encoding,hcb,vm,syscall}.c` | 20,000 个变异脚本：真实 `iroseka.hcb` 32 KiB 切片 + 24 个手写极小脚本 | 每个变异体要么被加载器拒绝并给出原因，要么有界执行（步数上限）；0 崩溃 |
| HCB 内存安全 | `fuzz_hcb_guard.exe`（同一源码，`-DFUZZ_ALLOC_GUARD`） | 进程内，页保护分配器 | 3,000 个变异 | 堆上越界读/写立即触发访问违例；0 次 |
| HCB 真实 CLI | `bin/fvm.exe` | 子进程（每例独立进程 + 20 s 墙钟超时） | 4,000 个已落盘变异体，`--steps 2048` | 退出码 0（执行且打印 summary）或 3（`fvm: <原因>` 诊断）；无负退出码（崩溃）、无超时 |
| HCB CLI 内存安全 | `fvm_guard.exe`（`src/main.c` + 核心，页保护） | 子进程 | 300 个变异体 | 同上，且不允许 71/72（内存故障） |
| VM 边界 | `test_vm_edge.py` | `bin/fvm.exe` + `fvm_guard.exe` | 43 个手写脚本（空文件、自跳转、栈下溢/溢出、10 万层递归、`pushglobal 65535`、`syscall 200/65535`、字符串重复溢出……） | 安全报错或有界退出；1 个对照脚本必须逐字节输出 `[0] 12345`（证明构造的 HCB 有效） |
| BIN 包 | `c_assets_guard.exe list/dump` | 子进程 | 74 个变异包：名字偏移越界、`data_off+size` 越界、条目数超大、名字表无 NUL、截断、随机翻转、真实 `se_sys.bin` 头字段伪造 | 拒绝（退出 1 + `c_assets:` 诊断）或安全接受；无崩溃/超时/内存故障 |
| HZC1 图像 | `c_assets_guard.exe hdr/rgba` | 子进程 | 139 个变异：头截断、magic 破坏、fmt 非法、宽高乘积溢出、帧数超大、`uncompressed_size` 不符、zlib 流截断/翻转/替换、裸 deflate 流 | 同上 |
| INFLATE | `c_assets_guard.exe inflate/rawinflate` + `c_assets fuzz` | 子进程/进程内 | 输出缓冲边界（expect 小于/大于真实长度）+ 40,000 次随机/变异流（带前后守卫页） | 输出缓冲过小必须报错而不是写越界；`guard_violations=0` |
| 音频 | `fuzz_snd.c`（→ `fuzz_snd.exe`，页保护） | 子进程 | 126 个变异：截断 WAV/Ogg、非法 fmt chunk、bits/channels/rate 伪造、data 长度说谎、Ogg 页头破坏、前缀/全文件翻转、随机垃圾 | 解码成功或拒绝；解码后混音固定 2048 帧；0 崩溃/越界/挂死 |
| 既有验收（回归） | `compare.py --run`、`verify_assets.py --quick`、`bin/test_snd.exe` | 原样调用 | 真实脚本与资源 | 与现状一致（PASS） |
| **动画（静态门）** | `verify_gfx.ps1 -SkipDump` | `bin/test_gfx.exe` 回放 528k 步场景 | 真实资产 | 动画默认关闭时参考帧指标必须**一分不变**：24/24 图元位置误差 **0 px**、像素差 **293 (0.0141%)**、PSNR **49.94 dB**、SSIM **0.99971**（2026-11 实测仍 PASS） |
| **长跑（soak）** | `verify_soak.ps1` | 真跑 `bin\fvp.exe --run-seconds N`，期间 2 s 一次采样工作集 | 默认 180 s | 退出码 0、工作集增长 ≤ 96 MiB、
每个 5 s 窗口 fps ≥ 55 且 slow ≤ 2、`underruns=0`、脚本有产出、日志无 error/fatal |
| **动画（动态门）** | `verify_motion.ps1` | `render_vm --motion`（固定步数 + 固定 dt） | 真实脚本 | ① 关动画两次同参数帧一致（harness 确定）② 1,000 步 开=关（t≈0 不漂移）③ 60,000 步 开≠关（动画可见） |
| **协程（开关中立）** | `verify_threads.py` | `bin/fvm.exe` 跑 900k / 2M 两档，各跑「默认（协程开）」与 `--no-threads` | 真实脚本 | ① 默认模式 `start == exit`（每个协程都退出，没有空转）② 开/关文本**逐字节相同** ③ 两档都与 `ref/ref-900k.txt` / `ref/ref-2m.txt` 逐字节相同 |
| **命中测试** | `test_prim_hit` | 假 host 造固定尺寸图 + 合成图元树，直接调 `gfx_prim_hit()` | 47 项 | ① `primId` 必须 1..0xFFF ② `parent == -1` 不可命中 ③ 起始图元 `draw==0`：flag=0 不命中、flag=1 命中 ④ 祖先 `draw==0` 一律不命中 ⑤ 父链 x/y 累加 ⑥ 矩形**左闭右开**（右/下边界外不命中） ⑦ 写过多 WH 取 `min(prim, 图)`、显式 WH=0 时矩形为空 ⑧ UV 后源坐标越界不命中 ⑨ type 2 看 alpha、type 5/0 恒不命中 ⑩ 组递归命中且**不加组自己的 x/y** ⑪ 悬空子树（链没到根）不命中 ⑫ 环状父子关系安全返回 0 ⑬ NULL 安全 |
| **随机数/重画** | `test_vmmisc` | 直接构造 `FvpVM`，调 `fvp_vm_rand()` | 2069 项 | ① 2002 次取值全部落在 0..0x7FFF（对齐 MSVC RAND_MAX）② 序列不是常数 ③ **可复现**：两个同初始状态的 VM 前 64 个值逐一相同 ④ NULL vm 返回 0 ⑤ `TextRepaint` 只计数（逐次 +1） |
| **外字渲染** | `test_gaiji_render` | 假 `GfxHost` 返回 8×8 纯红图 + 400×1000 画布、层 0 默认排版 | 17 项 | ① 没登记外字时字符「X」的位置**没有**纯红像素 ② `gfx_graph_load` 到 0x0F00 + `gfx_gaiji_add('X', 34, slot)` 后同一位置**出现**纯红 ③ 重复登记同码点=覆盖、条目数不变 ④ 码点 0 / 槽号 ≥ `GFX_GRAPH_MAX` / -1 / NULL gfx 全拒绝 ⑤ 空槽（登记了但没装图）什么都不画 ⑥ `gfx_gaiji_count(NULL) == 0` | ⑧ **A/B**：`render_vm --no-gaiji` + `gfx_gaiji_set_enabled()` 可把同一帧开/关外字各渲一次做逐像素对比；`render_vm` 汇总会打印每条外字（码点/字号/槽/图是否加载） |
| **外字/3D 补间参数** | `test_gaiji` | 直接构造 `FvpVM`（不跑脚本） | 33 项 | ① `GaijiLoad` 两个字符串 + size 12..64：11 / 65 拒绝且不改旧值，int / float / NULL 参数拒绝 ② 记录 name1/name2（arena 拷贝）与调用计数 ③ `V3DMotion` 收 time 1..300000、mode 1..5：0 / 300001 / 0 / 6 / float 全拒绝 ④ 边界值 1 与 300000 合法 ⑤ NULL vm 安全 |
| **命令覆盖审计** | `scripts/check-commands.py` | 静态扫 `src/syscall.c` 的四个 `switch (cid)` + `gen_cmd_table.h` 的 148 个名字 | 148 条 id | ① 分母固定 148 ② `fvp_gfx_argc` 里的 case **不算**实现（只改 argc）③ `*Test` 归到通用规则 ④ `MISSING_BASELINE = 28` 当棘轮：缺口变大就 REGRESSION ⑤ 输出 json 便于以后画曲线 |
| **文本缓冲/窗口模式** | `test_textbuff` | 直接构造 `FvpVM`（不跑脚本） | 44 项 | ① `TextBuff` 窗口号 0..31：32 / -1 / 非 int / NULL 拒绝且不计数 ② 正常设置写进对应窗口，别的窗口不动 ③ a/b 缺省或非 int / 负数都回落 8/8 ④ 0 与 0x400 合法 ⑤ **0x401 让整条命令放弃**（连计数都不加、旧值保留） ⑥ `WindowMode` 收 -1..6，0/1 才改 state、其它只记录；7 / -2 / float / NULL 拒绝 ⑦ `ExitMode` 收 0..4，5 / -1 / float / NULL 拒绝 ⑧ NULL vm 安全 |
| **图形槽颜色/工程目录** | `test_graphcolor` | 直接构造 `FvpVM`（不跑脚本） | 24 项 | ① `GraphRGB` 槽号 0..0xFFF：4096 / -1 / 非 int / NULL 全部拒绝且不计数 ② 分量"是 int 且在 0..200"才采用，201 / -5 / float 各自回落到 100 ③ 边界 0 与 200 合法 ④ 不同槽互不干扰、`graph_rgb_set` 标记 ⑤ `SysProjFolder` 只接受字符串（空串也算），int / float / NULL 拒绝 ⑥ 两次设置互相覆盖 ⑦ NULL vm 安全 |
| **控制/光标状态** | `test_control` | 直接构造 `FvpVM`（不跑脚本），调用 4 个状态函数 | 42 项 | ① 初始值（`cursor_shape == -1`）② `ControlMask` **看类型不看值**：int 0/1、字符串、NULL 全是 mask=0，同时记下参数类型 ③ `ControlPulse` 只置 1、重复调用仍为 1 ④ `CursorChange` 只收 int 0..3，4 / -1 / float / string / NULL 全被拒且不改 shape ⑤ `CursorMove` 要求 x/y 是 int，第三参数只记录类型；NULL x/y 在计数前返回 ⑥ 每条命令的调用计数 ⑦ NULL vm 安全 |
| **补间撤销** | `test_motion` | 直接构造 `FvpMotion`，不经过 gfx / VM | 47 项 | ① 时间轴推进/清零 ② `add` 参数非法（dur<0 / NULL）返回 0 ③ 到点给终值、线性插值中点 ④ 句柄自增唯一 ⑤ `reap` 只回收跑完的 ⑥ **`fvp_motion_cancel`：按图元+种类精确撤、`kind<0` 撤全部、空表/未知图元/NULL 返回 0、时间轴不受影响** ⑦ NULL 安全（2026-11 补上 Makefile 规则前，`run_all.py` 跑的是手编旧二进制） |
| **台词历史** | `test_history` | 直接构造 `FvpHistory`，不经过 VM | 164 项 | ① init：count/pos=0、每条 value=-1 ② 字符串整串替换（不是追加）、`kind` 只收 0/1、value 只收 ≥0 ③ 空串/NULL 也写进去 ④ count=0 时任何 idx 都取不到 ⑤ "开新的一条"推进 pos、清该槽、count 上限 100 ⑥ **槽位 = pos − idx，idx 0 是最近一条** ⑦ 写满 100 条后 pos 回绕、count 停在上限 ⑧ 超长串截断到 `FVP_HISTORY_TEXT_MAX` ⑨ free 可重复、NULL 安全 |
| **多作品兼容** | `verify_multigame.py` | 对**三部**目标作品（`iroseka.hcb`、`Hoshimemo_HD.hcb`、`Sakura.hcb`）各跑两遍 `bin/fvm.exe --steps 900000` | 真实脚本 | ① 两个 HCB 都能解码：`script_len` / 指令数 / `main_start` 落在合理范围 ② 三部作品的**内嵌命令表逐项相同**（148 条，名字+argc+顺序） ③ 每部作品两次运行输出**逐字节相同**（同一 SHA-256）③ 900k 步时的基准文本行数（iroseka 195 行 / 6,496 B、hoshimemo 164 行 / 6,644 B）不得漂移 ④ 0 个 U+FFFD、日文字符占比 > 50%。**这是「第二部作品也能跑」的硬门禁**；步数不足 900k 时只断言「有文本产出」 |
| **纹理缓存上限** | `test_tex_cache` | 假 host 造固定 100×100 图，`gfx_graph_load` 反复装载 | 26 项 | ① cap=0 时全缓存 ② 设上限后 `tex_bytes` 不超上限且按 LRU 淘汰 ③ **被引用的图（refs>0）永不被淘汰** ④ 引用释放后可淘汰 ⑤ 淘汰后重新解码、缓存命中不重复解码 ⑥ NULL 安全 |
| **定时器** | `test_timer` | 直接构造 `FvpTimer[16]`，给不同 `now_ms` | 37 项 | ① set 记起点、ms 钳到 1..100000 ② elapsed = now-start，时钟早于起点夹 0 ③ `opt` 换算 = elapsed*opt/duration（含取整与 INT32 夹紧）④ 未激活/越界 id 返回 0 ⑤ 重新 set 重置起点 ⑥ 16 个互不干扰 ⑦ NULL/0 计数安全 |
| **文本窗口属性** | `test_textattr` | 直接构造 `FvpTextAttr[32]`，逐命令调 setter | 82 项 | ① TextColor/TextSize 钳制 ② TextSpeed 超范围**整条忽略**且 -1 合法 ③ TextShadowDist(125) 单个 0..12 **钳制**（99→12、-7→0） ④ TextSpace(128) 两分量各 -32..32、超范围的**只跳过该分量** ⑤ TextFont(113) 两个字体号 -5..31、同样逐分量 ⑥ TextSkip(127) 0..3、超范围整条忽略 ⑦ 窗口号越界/NULL 拒绝 ⑧ TextFormat(118) 六字段各自判范围（前两个就是 TextSpace 的那对字段） ⑨ TextFunction(119) 三开关 0..1/0..2/0..2 ⑩ configured_count 只数设过的窗口 |
| **字形兜底** | `test_glyph_fallback` | 造两个临时 GFXF 图集（34px 只有 B、16px 只有 A），走 `gfx_text_append` + `gfx_render` | 16 项 | ① 目标图集有的字直接命中、不算兜底 ② 目标图集缺、别的图集有 → `fallback_glyphs=1`、`tofu_glyphs=0`（按字号缩放） ③ 都缺 → `tofu_glyphs=1` ④ 计数每次 render 归零 |
| **逐字显示（打字机）** | `test_typewriter` | `Gfx` + `gfx_text_append/advance`，不比对像素 | 31 项 | ① 默认关闭：整行一次可见、`advance` 是空操作 ② 打开后从 0 字开始、按 TextSpeed 的每字毫秒推进、余数不丢 ③ `speed 0` = 立刻显示完 ④ ASCII/2 字节/3 字节混排的 UTF-8 码点计数 ⑤ 关掉时立刻拉满 ⑥ 多窗口各自推进 ⑦ 负速度/NULL/越界安全。**默认关闭时像素基线与 `verify_gfx` 一致**（静态门另测） |
| 检测器自检 | `fuzz_selftest.c` | 两个分配器构建 | 故意在 32 字节块后写 1 字节 | 页保护构建必须以访问违例退出，金丝雀构建必须以 72 退出——否则「0 故障」没有意义 |

一次完整运行（默认参数）：

```
27,300 次 HCB 变异执行（20,000 进程内金丝雀 + 3,000 页保护 + 4,000 fvm.exe + 300 fvm_guard.exe）
   339 个资产/音频畸形用例（BIN 74 / HZC1 139 / AUDIO 126）
    43 个 VM 边界用例
40,000 次 inflate 随机流（另有 verify_assets 自带的 20,000+5,000 次）
     0 崩溃 · 0 越界 · 0 挂死 · 0 未诊断拒绝
```

---

## 2. 怎么跑

### 一键

```bat
cd fvp
python tests\run_all.py            :: 全量（约 80–90 s）
python tests\run_all.py --fast     :: 快速冒烟（约 60 s，HCB 3,000 + 600 E2E）
python tests\run_all.py --list     :: 列出步骤
python tests\run_all.py --only fuzz_hcb,vm_edge
python tests\run_all.py --keep     :: 保留变异文件/临时音频（默认清理）
```

输出统一汇总表（步骤 / PASS-FAIL / 耗时 / 关键指标）并以退出码表示
结果；机器可读结果写到 `fvp/out/fuzz/run_all.json`。
`run_all.py` **不调用 make**，也不会改 `Makefile`：它直接调用编译器把
新 harness 编到 `fvp/out/fuzz/`，再用 python 调现有测试。

### 单独

```bat
python tests\fuzz_hcb.py                      :: 20k 变异 + 4k E2E
python tests\fuzz_hcb.py --fast               :: 3k + 600
python tests\fuzz_hcb.py --iters 50000 --e2e 8000
python tests\fuzz_hcb.py --script D:\game\iroseka.hcb --slice 65536

python tests\fuzz_assets.py                   :: ~340 例
python tests\fuzz_assets.py --fast
python tests\fuzz_assets.py --scale 2.5       :: 更多随机用例
python tests\fuzz_assets.py --no-build        :: 用现存 bin/c_assets.exe（无内存插桩）

python tests\test_vm_edge.py
python tests\verify_threads.py                :: 协程开/关逐字节一致（900k + 2M）
python tests\verify_multigame.py              :: 三部作品 × 两次运行（900k）
python tests\verify_multigame.py --steps 300000  :: 冒烟档
```

前置条件：

- 编译器：`tools\w64devkit\w64devkit\bin\gcc.exe`（脚本会自动找到；也可
  `--cc PATH`）。没有 gcc 时 `fuzz_assets.py --no-build` 仍可跑（但失去
  页保护插桩），`run_all.py` 会把依赖 harness 的步骤标 SKIP/FAIL。
- Python 3（`verify_assets.py` 需要 numpy）。
- 真实脚本 / 资源：`iroseka.hcb` 与 `graph/`、`*.bin`、`bgm001.ogg`、
  `se001.wav` 通过 `$FVP_GAME` / `$FVP_SCRIPT` 或自动在 `~/Downloads`
  下寻找含 `iroseka.hcb` 的目录；找不到时 HCB 用合成语料、资产用合成包，
  并在输出中注明。

各脚本都会打印机器可读的 `RESULT: PASS|FAIL`，并在 `fvp/out/fuzz/*.json`
写明细（分类统计、失败列表、复现信息）。

---

## 3. HCB 变异器（`fuzz_hcb.c` / `fuzz_hcb.py`）

### 语料

1. **真实脚本切片**：读取 `iroseka.hcb` 头部 `script_len`，从第一条指令
   （偏移 4）取 32 KiB，重写成
   `[u32 4+len][指令流][u32 main_start=4]`；如果尾部指令跨出切片导致
   解码失败，就逐字节缩短切片直到加载器接受（保证变异从「合法输入」出发）。
2. **24 个手写极小脚本**：覆盖全部 0x00–0x27 操作码、整数/浮点/字符串
   运算、自跳转/条件跳转/自递归、`initstack/pushstack/copystack` 边界、
   字符串长度 0/1/255、`syscall` 的 0/13/123/131/133/137/147/200/65535、
   `pushglobal/popglobal` 的 0/8191/8192/65535、`garr/larr` 的 int/str/float
   键、无帧 `ret/ret2`、`main_start` 非法值等。

每次迭代按权重选一个语料（默认 70% 真实切片），再施加一个变异。

### 变异类别与实测拒绝率（20,000 次金丝雀运行）

| 类别 | 内容 | 次数 | 加载器拒绝率 |
|---|---|---:|---:|
| `byteflip` | 1–8 个随机位翻转 | 2,525 | 65.4% |
| `bytewrite` | 1–8 个随机字节覆写 | 1,797 | 78.0% |
| `truncate` | 截断到 0–8 字节 / 随机长度 / 脚本区内 / 尾部 | 1,786 | 98.0% |
| `script_len` | 改写 `script_len` 为 0/1/3/4/5/8/0x7FFFFFFF/0xFFFFFFFF/±偏移 | 1,384 | 74.0% |
| `main_start` | 改写入口为 0/4/文件尾/越界/随机 | 1,432 | 0.0%（本类只应影响执行） |
| `jump_target` | 把 call/jmp/jmpcond 目标改为合法指令/指令中间/文件外/随机 | 1,667 | 0.0% |
| `string_len` | 把 pushstring 长度字节改为 0/1/2/3/16/128/254/255 | 1,127 | 50.9% |
| `opcode` | 把操作码改为 0x28/0x29/0x80/0xFF（非法）等 | 931 | 79.3% |
| `insert` | 插入 1–8 字节（指令流失步） | 954 | 79.2% |
| `delete` | 删除 1–8 字节 | 918 | 100.0% |
| `zero_run` | 8–512 字节清零 | 891 | 32.9% |
| `splice` | 从自身另一位置覆盖一段 | 947 | 56.3% |
| `append` | 追加 1–64 字节（超出 `script_len` 的尾部数据） | 761 | 0.0% |
| `align_main` | 入口指向某条指令的第二个字节 | 685 | 0.0% |
| `header_garbage` | 随机化前 8 字节 | 745 | 90.7% |
| `combined` | 两个变异叠加 | 1,450 | 93.6% |
| **合计** | | **20,000** | **58.4%**（11,672 拒绝 / 7,973 正常结束 / 355 步数封顶） |

页保护确认运行（3,000 次，步数 512）：拒绝 1,760、正常结束 1,164、封顶 76，
`corrupt=0`，各类别比例与上表一致。

### 分类语义

- **rejected**：加载器拒绝并打印具体原因（`script too short`、`bad script
  length`、`unknown opcode 0x.. at offset 0x..` 等），`fvm.exe` 以 3 退出。
- **finished**：加载成功，线程全部自然结束（含各种非法控制流安全退出）。
- **capped**：加载成功但进入死循环/深递归，`--steps` 预算耗尽后正常退出；
  这是引擎自带的挂死保护。
- **crash / memfault / timeout**：失败。出现时脚本会把 journal 里最后一条
  用例用 `--repro` 重建（写入 `fvp/out/fuzz/repro/`）并用 `fvm.exe` 复跑，
  输出最小复现信息。
- E2E 阶段 `fvm.exe` 的退出码 2（`fvm: string repeat too large` 等受控
  fatal、OOM 路径）单独计为 `fatal`：打印原因、有界退出、内存安全，
  **不算失败**（真实脚本也可能触发，属于设计上的 fail-closed）。

---

## 4. 资产层变异（`fuzz_assets.py`）

真实语料：`se_sys.bin`（真实 BIN 包）、`fvp/extract/graph/` 里最小的真实
hzc1（243 字节，102×102 fmt4）、`fvp/work/bgm001.ogg`、`fvp/work/se001.wav`。
缺少真实资源时退化为合成包（合法 BIN / zlib 压缩的小图 / 合成 WAV）。

实测（默认参数：BIN 74、HZC1 139、AUDIO 126）：

| 段 | 覆盖类别（拒绝率） | 合计 |
|---|---|---|
| BIN | 名字偏移越界 100%、数据偏移越界 100%、`data_off+size` 越界 100%、条目数超大 100%、名字表长度异常 80%、截断 100%、无 NUL 名字 0%（安全接受）、名字中间偏移 0%、头部清零 0%、随机翻转 51.6%、`dump` 浅路径 0%、真实包伪造 100% | 74 例，拒绝 54 |
| HZC1 | 头截断 62.5%、magic 100%、fmt 非法 100%、宽高为 0 100%、宽高乘积溢出 100%、帧数超大 100%、`usize` 不符 100%、其他头字段 0%（安全接受）、payload 截断 100%、payload 翻转 100%、payload 替换 100%、随机头 100%、inflate 输出缓冲边界 100%、裸 deflate 合法 0%/截断 100%/翻转 58.3% | 139 例，拒绝 118 |
| AUDIO | WAV 截断 87.5%、fmt 非法 100%、bits 非法 100%、channels 伪造 40%、rate 伪造 50%、fmt chunk 过短 100%、data 长度说谎 33.3%、无 data 100%、RIFF 奇数 chunk 0%、chunk 越界 100%、真实 WAV 截断/翻转 17.6%；Ogg 截断 55.6%、magic 50%、前缀翻转 12.5%、追加垃圾 0%、随机前缀 100%、全文件翻转 0% | 126 例，拒绝 49 |

三段合计 339 例：**接受 118 / 拒绝 221 / 崩溃 0 / 超时 0 / 内存故障 0 /
未诊断拒绝 0**。另外 `c_assets fuzz 40000`（带前后守卫页的 inflate
随机流）`guard_violations=0`；`verify_assets.py --quick` 自带的 20,000+5,000
次 fuzz 也全部通过。

说明：拒绝率不是越高越好——例如「名字表无 NUL」「`header_fields`」这类
输入在格式上是允许的，引擎应当安全接受；测试记录的是*实际*结果与安全性，
而不是强行要求拒绝。JSON 里的 `expected_mismatch` 汇总了「我预期拒绝、
实际安全接受」的用例，默认配置下共 12 条，已逐条确认都是合法/可安全
忽略的输入，不是漏洞：

- BIN `name_len+1`：名字表多算了 1 字节（该字节其实属于数据区），名字
  解码到表尾即止，安全；
- BIN `header_zeros`：`count=0, name_len=0`，就是一个合法的空包；
- WAV 截掉最后 1 字节 / 真实 WAV 的某些截断前缀：data 长度按文件尾
  截断后仍能解码；
- Ogg 截断前缀、只改动 magic 中后 3 字节的输入：仍构成可解码的流。

---

## 5. VM 边界用例（`test_vm_edge.py`）

49 个用例，全部 PASS，且都在 `fvm_guard.exe`（页保护）下复跑通过。
关键结果：

| 用例 | 断言 | 实测 |
|---|---|---|
| 空文件 / 1 字节 / 7 字节 | 加载器拒绝，exit 3 + 诊断 | exit 3 |
| `script_len` 为 0/3/`0xFFFFFFFF`/超文件 | 拒绝 | exit 3 |
| 只有 `script_len` 头（8 字节） | 可加载，0 指令，安全退出 | steps 0 |
| `main_start` = 0 / 越界 / 指令中间 | 安全退出 | steps 0 |
| `jmp` 指向自身 | `--steps 1000` 封顶 | steps 1000 |
| `jmpcond` 自循环 | 封顶 | steps 1000 |
| `call` 自递归 | 封顶 | steps 20,000 / 100,000 |
| 只有 `add` 没有 push（栈下溢） | 弹空栈得到 0，不崩溃 | steps 2 |
| 全部二元运算 + neg + pushtop + 空栈 TextPrint | 不崩溃 | steps 19 |
| `pushstack/copystack` 无帧、索引 255 | 不崩溃 | steps 4 |
| 连续 push 500,000 / 1,000,000 次 | 有界、不 OOM | 200,000 / 1,000,000 步封顶 |
| `pushglobal/popglobal 65535`、`syscall 200/65535/147` | 不崩溃 | 有界 |
| `ThreadWait` 自循环 | 步数封顶（调度器不饿死） | 500 |
| `pushstring` 声明 200 字节但文件只剩 2 字节 | 安全加载/退出 | steps 1 |
| 字符串重复溢出（`"ab" * 0x7FFFFFFF`） | 受控 fatal：exit 2 + 原因 | exit 2 |
| 除法/取模 0、NaN 比较、GBK 字符串 | 不崩溃 | 有界 |
| 对照：`pushint32 12345` + `TextPrint` | 文本输出逐字节 `[0] 12345` | PASS |

---

## 6. 内存安全检查（为什么不是 ASan）

本机工具链是 w64devkit 的 gcc 16.2，**没有 ASan/UBSan 运行库**：

```
$ gcc -fsanitize=address ...
ld.exe: cannot find -lasan
$ gcc -print-file-name=libubsan   ->  libubsan（原样返回，未找到）
```

因此套件用两个自建插桩（仅作用于 harness，不改 `src/`）：

1. **页保护分配器**（`tests/fuzz_alloc.h`，`-DFUZZ_ALLOC_GUARD`）
   每个堆块结束位置精确对齐到一张 `PAGE_NOACCESS` 页（Electric Fence 式）。
   越界**读**或**写**会在发生的那条指令立刻触发访问违例，而不是静默踩内存。
   单块上限 192 MiB（伪造的巨大尺寸字段只会得到 NULL，走引擎既有的 OOM 分支），
   进程退出时校验全部存活块的 magic，能顺带发现 use-after-free/泄漏。
2. **金丝雀分配器**（默认模式）
   每个块尾部有 16 字节 `0xA5` 哨兵 + 头部 magic；`free` 时校验，能廉价地
   发现堆写越界/双重释放/野指针。用于 20,000 次的大批量运行。
3. **`-fstack-protector-all`**：所有 harness 构建自动尝试该编译选项
   （工具链支持时启用）。
4. **检测器自检**（`tests/fuzz_selftest.c`，`run_all.py` 第 1 步）
   故意在 32 字节块后写 1 字节：页保护构建必须以访问违例退出
   （实测 NTSTATUS 0xC0000005），金丝雀构建必须以 72 退出。
   若分配器被优化成空操作（例如 `-O1` 下 gcc 把越界写当死存储删掉），
   自检会失败——所以自检以 `-O0 -fno-builtin` 构建。

**局限（重要，报告结论时须一并说明）**：

- 不是完整 ASan：栈溢出未插桩（靠 `-fstack-protector-all`），
  金丝雀模式不检测越界**读**，页保护模式不检测块**前**的下溢，
  且只覆盖经过 `malloc/free` 的引擎分配（CRT 内部私有堆不在内）。
- 页保护模式对每次分配做 `VirtualAlloc/VirtualFree`，故用较小步数
  （512）跑确认子集，而不是全部 20,000 例。
- 结论应表述为「在上述插桩覆盖范围内未发现越界/崩溃」，而不是
  「已证明绝对内存安全」。
- 引擎里少数路径是**受控 fatal**：`fvp_xmalloc` 失败与
  `string repeat/concat too large` 会打印原因并 `exit(2)`，这是设计选择
  （fail-closed），不是内存违规；`test_vm_edge.py` 把它作为合法退出接受。
- 退出时 `ALLOC ... live=N` 是 CRT/stdio 启动分配（`--iters 0` 与
  `--iters 20000` 都是 10 个块，不随用例数增长），不是引擎泄漏。

---

## 7. 已知不可达 / 未覆盖的路径

这些不是 bug，但在评估「测试覆盖率」时应知道：

1. ~~**`hcb.c` 的 `truncated instruction` 诊断当前不可达。**~~
   **已于 0.1.0 修复**：边界检查从 `data_len`（文件长度）改为 `script_len`（脚本区长度），
   该诊断恢复生效。用 `out/fuzz/repro/hcb_operand_past_script_len.hcb` 复现，现在输出
   `fvm: truncated instruction 0x2 at 0x4`；两条逐字节基准（195 行 / 450 行）仍 PASS。
   以下为修复前的描述，留作记录：
   加载器要求 `script_len + 4 <= data_len`，而最长操作数只有 4 字节
   （`d`）；`pos <= script_len` 时 `pos + 4 <= script_len + 4 <= data_len`
   恒成立，所以「操作数越界」分支永远不触发。见第 8 节第 2 条：操作数
   反而会读到 `main_start` 那 4 个字节。
2. **`bin.c` 的两个「implausible」上限**（`count > 16,777,216`、
   `name_len > 256 MiB`）需要文件本身足够大才能越过前面的文件长度检查，
   小体积 fuzz 输入到不了；测试通过真实包截断/伪造覆盖相邻分支。
3. **`hzc1.c` 的 `raw_len > SIZE_MAX` 分支**在 64 位上不可达；
   `raw_len > 2 GiB` 的分支会被提前拒绝，所以「接近 2 GiB 的合法几何 +
   畸形 zlib」只在 `hdr` 层面测，不做实际解码（避免真的分配 GB 级内存；
   页保护模式的 192 MiB 上限也会把它变成 OOM 拒绝）。
4. **Ogg/Vorbis 的深度码流路径**：`stb_vorbis` 是第三方代码，套件只在
   容器层做截断/magic/页头/追加/翻转；没有对 Vorbis 熵解码做定向 fuzz。
5. **GUI（`fvp.exe` / win32 shell）与 gfx 层不在套件内**：它们依赖窗口/
   音频设备，本套件只覆盖 CLI 可达的加载器、VM、资源与音频解码路径。
6. `fvm` 的 syscall 实现是桩（无文件/网络 I/O），因此 VM 层面没有
   文件系统或网络攻击面；资产/音频路径才是文件格式攻击面。
7. `verify_assets.py` 的 `--quick` 只抽样（BIN 全量索引、20 张图、40 个
   payload）；`--full` 会覆盖全部 1,112 个 hzc1，属于现有验收工具的选择。

---

## 8. 发现的问题（未改引擎代码）

### 8.1 低危：WAV 重采样放大（资源耗尽）

`src/snd.c` 的 `resample_to_rate()` 没有约束 `dst_rate / src_rate` 比例：

- 复现文件：`fvp/out/fuzz/repro/wav_rate1_amplification.wav`（2,044 字节，
  合法 PCM16 单声道 1,000 帧，只把 `fmt ` 的采样率改成 **1 Hz**）。
- 复现命令：

  ```bat
  fvp\out\fuzz\fuzz_snd.exe fvp\out\fuzz\repro\wav_rate1_amplification.wav
  ```

- 实测：`RESULT OK frames=44100000`，峰值单次分配 **176,408,616 字节
  （168 MiB）**，耗时约 **320 ms**（2 KB 输入 → 1,000 秒音频 → 4,400 万帧）。
  这是典型的「小输入放大成大分配/长 CPU」问题，可被恶意资源包用来造成
  卡顿或内存压力（不是内存越界）。
- 建议（归引擎作者）：在 `snd_play_wav`/`resample_to_rate` 里限制
  `src_rate` 的合理范围（例如 8 kHz–192 kHz）和输出帧数上限，或对
  `n = frames * dst_rate / src_rate` 做溢出与上限检查。
- 套件中对应类别：AUDIO / `wav_rate`（`max 407 ms`），已记录在
  `fuzz_assets.json`。

### 8.2 低危：HCB 指令操作数可越过 `script_len`

`src/hcb.c` 解码时用 **文件长度** `hcb->data_len` 做操作数边界检查
（`pos + N > data_len`），而不是脚本区长度 `script_len`。因为文件末尾
总有 4 字节 `main_start`，一条位于脚本区末尾的指令可以把操作数读到
`main_start` 字段里，并被加载器接受。

- 复现文件：`fvp/out/fuzz/repro/hcb_operand_past_script_len.hcb`（9 字节）：
  `05 00 00 00 02 04 00 00 00` = `script_len=5`、代码 `call`、
  `main_start=4`。`call` 的 4 字节目标恰好落在 `main_start` 字段上，
  读出 4 → 线程自我递归。
- 复现命令与实测：

  ```bat
  fvp\bin\fvm.exe fvp\out\fuzz\repro\hcb_operand_past_script_len.hcb --steps 1000
  :: steps 1000 / threads 1 —— 仅靠 --steps 预算停住，加载器没有拒绝
  fvp\out\fuzz\fvm_guard.exe fvp\out\fuzz\repro\hcb_operand_past_script_len.hcb --steps 1000
  :: 同样有界，无内存故障
  ```

- 影响：内存安全（读取仍在 `data` 缓冲区内），但格式校验不严格：
  畸形脚本会被接受而不是以 `truncated instruction` 拒绝。若加上
  `pos + N <= script_len`，第 7 节第 1 条的诊断也会重新变得可达。
- 建议（归引擎作者）：把三个操作数检查的上界从 `data_len` 改为
  `script_len`（`'s'` 的长度字节同理），并按需保留「容忍尾部填充」的开关。

### 8.3 无其它问题

20,000 + 3,000 + 4,000 + 300 次 HCB 变异、339 个资产/音频畸形用例、
43 个 VM 边界用例、40,000 次 inflate 随机流：**0 崩溃、0 越界
（页保护检测）、0 挂死、0 未诊断拒绝**。没有发现可导致内存破坏的问题。

---

## 9. 文件与产物

新增文件（均在 `fvp/tests/`）：

| 文件 | 作用 |
|---|---|
| `fuzz_hcb.c` | 进程内 HCB 变异 fuzzer（语料构建、16 类变异、结果分类、journal、`--repro`） |
| `fuzz_hcb.py` | 构建两种 harness、跑批量/页保护/E2E、汇总 `fvp/out/fuzz/fuzz_hcb.json` |
| `fuzz_assets.py` | BIN/HZC1/inflate/WAV/Ogg 畸形输入驱动与拒绝率统计 |
| `fuzz_snd.c` | 音频解码/混音 harness（`snd_play_file` + 有界 `snd_mix`） |
| `test_vm_edge.py` | 43 个 VM 边界脚本（自带迷你汇编器） |
| `run_all.py` | 一键串起既有验收 + 新 fuzz，输出 PASS/FAIL 汇总表 |
| `harness_build.py` | 共享构建逻辑（gcc 发现、`out/fuzz` 产物、自检） |
| `fuzz_alloc.h` | 页保护/金丝雀分配器（仅 harness 使用） |
| `fuzz_alloc_link.c` | 把分配器链接进 `c_assets.c` 等已有 main 的工具 |
| `fuzz_selftest.c` | 分配器自检（证明越界检测真的会触发） |

运行时产物（`fvp/out/fuzz/`，可随时删除）：

```
fuzz_hcb.exe / fuzz_hcb_guard.exe / fvm_guard.exe
c_assets_guard.exe / fuzz_snd.exe
fuzz_selftest_guard.exe / fuzz_selftest_canary.exe
fuzz_hcb.json fuzz_assets.json vm_edge.json run_all.json
fuzz_hcb.journal fuzz_hcb_cases.csv
repro/   ← 失败时的最小复现文件（目前仅第 8 节手工复现的两个）
tmp/     ← 临时变异体/输出
```

复现单个可疑变异（`(seed, idx)` 唯一确定一个用例，默认种子 `20261123`；
`--repro` 与批量运行使用同一套 PRNG 派生，结果可重复）：

```bat
:: 让 fuzzer 重建第 12345 号变异体并落盘
out\fuzz\fuzz_hcb.exe <iroseka.hcb> --repro 12345 --out out\fuzz\repro\case_12345.hcb

:: 再用真实 CLI 复跑
bin\fvm.exe out\fuzz\repro\case_12345.hcb --steps 2048

:: 页保护版本（若关心越界）
out\fuzz\fvm_guard.exe out\fuzz\repro\case_12345.hcb --steps 2048
```

如果批量运行出现崩溃/超时，`fuzz_hcb.py` 会自动读取 journal 里的最后
一个用例、重建并用 `fvm.exe` 复跑，把最小复现信息写进
`fvp/out/fuzz/repro/` 和 `fuzz_hcb.json`。

---

## 10. 验收快照（本次数字对应哪一版引擎）

套件每次运行都会按 mtime 重新编译 harness，所以引擎源码一变，下一次
`run_all.py` 自动用新源码重建（不会测试陈旧二进制）。本次验收记录：

- 结果：`run_all.py` 8/8 PASS，**80.6 s**（日志 `fvp/out/fuzz/final_run4.log`）；
  运行前后引擎源码哈希完全一致（`src_hashes_before.txt` 与
  `src_hashes_after.txt` 相同，24 个 `src/*.c|*.h` 全部纳入快照）。
- 引擎源码快照（SHA-256，前 16 位；`gfx.*` 与 `gen_*.h` 未逐条列出，
  完整清单见 `fvp/out/fuzz/src_hashes_before.txt`）：

  | 文件 | 哈希 | 文件 | 哈希 |
  |---|---|---|---|
  | `fvp.c` | `F0276FEC426E1D61` | `fvp.h` | `27AEFFCE89FD1C33` |
  | `hcb.c` | `80319C2715D65BCA` | `hcb.h` | `B90028AFDB5EB944` |
  | `vm.c` | `857DB261565E99FD` | `vm.h` | `E11A2CE163226B65` |
  | `syscall.c` | `F620BB4584725C73` | `syscall.h` | `ACA964C7E309E9C7` |
  | `encoding.c` | `54C3B7B84056E271` | `encoding.h` | `50EF3FA54026F84F` |
  | `bin.c` | `90C8B5D1F741A719` | `bin.h` | `917678F2873D07DD` |
  | `hzc1.c` | `56F1B33C26ED80CE` | `hzc1.h` | `2CE720F5A9D73B6D` |
  | `inflate.c` | `13A33FA28523B0C3` | `inflate.h` | `681317C7502021C7` |
  | `snd.c` | `AA83081414B6C690` | `snd.h` | `9394F048F9160E03` |
  | `main.c` | `8C69F49AFBE47D9E` | | |

- 如果引擎代码在验收之后还有改动（本仓库当前有并行开发），请重跑
  `python tests/run_all.py`；文档中的统计数字只对这一份快照负责。
  固定种子 `20261123` 保证同一份源码 + 同一份输入下用例集完全可复现。