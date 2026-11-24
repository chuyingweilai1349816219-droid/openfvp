# 变更记录

> 以下内容由 DeepSeek 生成,不一定代表作者个人意愿

格式参考 [Keep a Changelog](https://keepachangelog.com/zh-CN/1.1.0/)，版本号语义化。

## [未发布] — 协程槽落地（1.0.0 之前）

### 新增

- **纹理缓存 LRU 上限（默认 256 MiB）**：长跑 RSS 阶梯式增长的根因
  - 采样证据：0–95 s 稳定 194 MiB，之后随换场景阶跃 222.9 / 209.5 / 221.7 MiB，
    每一跳都精确对应缓存 75→78→80→81→83 张（108.7 MiB）的阶跃 —— **不是泄漏，是只进不出**
  - 语义：`tex_unref()` 到 0 引用不再立即释放，图像留在缓存等复用；新加 `tex_free()`
    只由淘汰和 `gfx_destroy` 调用。`refs` 只数外部持有者，**refs == 0 = 可淘汰**
  - 淘汰只挑 refs == 0 的条目（被图元/组槽/部件引用的永不动），因此像素输出不变：
    `verify_gfx.ps1 -SkipDump` 仍是 293 px / PSNR 49.94 / SSIM 0.99971
  - 接口：`gfx_tex_cap_bytes` / `gfx_tex_set_cap_bytes` / `gfx_tex_evictions`；
    外壳 `--tex-cache-mb N`（0 = 不限制），`tex:` 行打印上限与累计淘汰数
  - 测试：`tests/test_tex_cache.c` **26 项**（已并入 `run_all`，共 21 步）
- **32 个协程槽进 C，默认开启**（`ThreadStart`/`ThreadExit`/`ThreadNext`）
  - `src/vm.h`：`FVP_THREAD_SLOTS 32`、`thread_slots[32]`、`threads_on`、诊断计数
  - `src/vm.c`：`fvp_vm_thread_start()` / `fvp_vm_thread_exit()`，死线程回收时清槽
  - 语义按静态分析：`ThreadStart(0, entry)` 先重置 32 槽再在 0 号槽建线程；`ThreadExit(0)` 收掉
    除自己外所有槽；**主线程不在槽里**（否则 `ThreadStart(0, …)` 会把自己重置掉）
  - **实测行为中立**：900k / 2M 两档，开关两种模式的文本**逐字节相同**，且都等于冻结基准
    （`7f9837ff…` / `32d7d4e2…`）。原因见下一条修正
  - 新增门禁 `tests/verify_threads.py`（已并入 `run_all.py`）：`start == exit`、开/关同字节、
    900k 与 2M 都对冻结基准
  - 新增 `--no-threads`（fvm 与 Win32 外壳），用于 A/B 与回归

### 修复

- **栈底帧的返回地址指回了自己，导致 `ret` 无限自循环**
  `initstack` 在没有 `call` 建立帧时（线程/函数入口）会新建"栈底帧"；旧实现把返回地址写成
  `ins->pc`（`initstack` 自己），于是 `ret` 跳回入口再执行一次 —— 这正是 2026-11 Python 原型
  "58,908 次空转"和"开线程后主线程被饿死"的真因。改为指向 `script_len`（脚本末尾），
  `ret` 即结束线程。主线程不会从栈底帧返回，所以关闭协程时基线不变。
- **周期性能报告的 fps/slow/frame_avg 用了全程累计计数**（除以 5 s 窗口），数值随运行时间
  线性增长（实测 5 s 报 58.7、135 s 报 **1606.9**），不能当门禁。改为独立的窗口统计
  `wperf`，报告后清零；退出总结仍用全程累计。修好后连测四窗：**57.2 / 59.3 / 60.0 / 60.0 fps**。

### 诊断

- `tex:` 行（`gfx_tex_count()` / `gfx_tex_bytes()`）：把 RSS 增长归因到纹理缓存。
  20 分钟采样（每 10 s 记 RSS）结论：**不是内存泄漏，是纹理缓存只进不出**
  —— RSS 在 0–95 s 稳定 194 MiB，之后随换场景阶跃到 222.9 / 209.5 / 221.7 MiB，
  每一跳都精确对应缓存 75→78→80→81→83 张（108.7 MiB）的阶跃。
  → 已由上面的 LRU 上限解决。

### 修正（推翻旧结论）

- **「约 50 句台词缺失」不成立**。旧文档认为台词在协程里拼字符串、协程没实现所以缺失，
  且认为协程入口卡在等时间条件的循环里。实测（`work/probe_threads3.py` + `docs/05-threads.md` §10）：
  本作每个协程入口都是 `initstack; pushint8 0; jmpcond <ret>`，条件是**编译期常量 0**，
  协程建起来就退出 —— 原版引擎里这些协程体也不执行。900k 步 195 行、2M 步 450 行线性推进，
  没有断流。`jmpcond` 方向也做了反证：把方向反过来同一脚本只出 **0 行**。
- 相应改写 `README.md` / `README.en.md` / `docs/05-threads.md` / `docs/render-model.md` /
  `docs/roadmap.md` 里的旧说法。

## [0.8.0] — 未发布：完整测试版（动画/时间模型起步，目标版本）

### 新增

- **最小时间模型（`--timeline`，默认关闭）**：虚拟时钟 + `ThreadSleep`/`ThreadWait` 定时唤醒
  - 调度器在一轮全部定时阻塞时推到**最早** `wake_ms` 并叫醒到期线程；无目标则结束
  - 实测 900k 步：legacy `virtual ms 0` / `--timeline` `virtual ms 2000`，**两者文本逐字节相同**（6,496 B = `ref-900k.txt`）
  - → 诚实结论：时钟确实在走，但**没有**解决「约 50 句台词缺失」；那个问题在别的机制里
  - `tests/verify_timeline.ps1`：7 项断言（默认==冻结基准、timeline 不挂死、时钟前进、
    当前与 legacy 逐字节相同 —— 将来分歧会立刻报错）
- **`src/motion.c` / `src/motion.h`：补间核心数学**（`Motion*` 的时间轴 + 线性插值）
  - 只实现**已确证**部分：开始/持续/完成、线性插值、零时长、完成回收；
    `mode 1..5` 的缓动曲线**没有证据**，原样存下但不影响输出（文档标明未实现）
  - 未接 gfx / syscall，因此现有所有验收数字不变
  - 验收：`tests/test_motion.c` **31 项**（起点/中点/终点、超时钳制、零时长、
    负时长拒绝、多补间并存、reap、clear 保留时钟、NULL 安全）
- **`Motion*` 命令接入补间表**（`src/syscall.c`）：`MotionMove`(48) / `MotionAlpha`(42) / `MotionMoveS2`(54)
  在**动画开启**时改为「当前值 → 目标值 + 时长 + mode」的补间，关闭时仍是原来的静态赋值
  外壳新增 `--motion`（默认关）+ 每帧 `gfx_motion_advance()` + 日志 `motion: N active tween(s)`
  实测：开场 6 s / 15 s 运行里观察到 **1–2 条活跃补间**
  ⚠️ 像素 A/B 对比**不能作数**：默认模式两次 15 s 运行本身就不同（墙钟驱动的文本节奏），
  下一步需要用**步数确定**的 `render_vm` harness 做对照（已记入待办）
- **确定性动画 A/B（`tests/verify_motion.ps1`，新增）**：`render_vm` 加 `--motion` / `--motion-dt`，
  固定步数 + 固定 dt 做对照（外壳壁钟驱动不能做 A/B）。实测：
  关动画两次 60k 步完全一致 `76C16B46…`；**1,000 步开=关** `E46C17E3…`；
  **60,000 步开≠关**（`F3CA8F6F…`）—— 动画在真开场里确实改了像素
- **动画双门制已落实**：静态门 `verify_gfx.ps1 -SkipDump` 仍 **PASS**（图元 24/24
  位置误差 **0 px**、像素差 **293 (0.0141%)**、PSNR **49.94**、SSIM **0.99971**），
  证明动画集成没有动到静态渲染路径；动态门交给 `verify_motion.ps1`。两条写进 `docs/testing.md` 矩阵
- **`docs/animation-baseline.md`**：动画/时间模型的验收基线设计 ——
  哪些旧数字会失效（528k 参考帧、端到端帧）、改用哪三组自洽断言
  （t=0 不动 / t=END 到位 / 单调有界），以及「拿到原版实机对照前不默认开时间模型」的理由


### 新增

- **诊断能力**：`fvm --unknown-all`（完整未知命令直方图）与每线程台词计数；
  实测 900k 步：**195 行全部来自主线程**，`ThreadStart`(136) 调用 **5 次**、
  `ThreadExit`(132) 19 次、`TextSuspendChr`(130) 124 次、`TimerSet`(139) 259 次，全是桩
  → 台词缺失主因定位为 **协程线程未实现**（非时间模型）；
  `docs/05-threads.md` §9 记录了为何不能「顺手实现」（Python 原型加线程后 711 行但大量 `[0] 0`）
- **长跑验收 `tests/verify_soak.ps1`**：真跑外壳 N 秒，期间采样工作集，断言退出码、
  工作集增长、每 5 s 窗口 fps/slow、`underruns=0`、脚本有产出、日志无 error
  - 首次 120 s 实测：退出码 0、工作集 140.9 → 209.5 MiB（增长 68.6 MiB）、23 个窗口
    全部 fps ≥ 55（slow 均 0）、`underruns=0`、127 行台词、0 error
  - ⚠️ 工作集前 120 s 增长 68.6 MiB，**尚未判定**是资源缓存填充还是泄漏，
    需要更长时间的曲线（已安排 10 分钟对照）

### 验收（0.8.0 发布前实跑）

| 项 | 结果 |
|---|---|
| `tests/run_all.py` | **20/20 PASS**（含 VM 逐字节 / 资源 / 音频 / 存档 / flag / textattr / thumb / encode / wheel / motion / fuzz） |
| `verify_gfx.ps1 -SkipDump`（静态门） | PASS：24/24 图元 0 px、293 px 差 (0.0141%)、PSNR 49.94、SSIM 0.99971 |
| `verify_motion.ps1`（动态门） | PASS：关两次一致；1,000 步 开=关；60,000 步 开≠关 |
| `verify_timeline.ps1` | PASS：legacy `virtual ms 0` 且 == `ref-900k.txt`；`--timeline` 2000 ms 且文本相同 |
| `verify_save_preview.ps1` | PASS：真跑外壳 → 4 个槽全部读回，尾段 598,950 B 不变 |
| `verify_save_encoding.ps1` | PASS：CP932 `83 76 …` / GBK `a5 d7 …` 与 Python codecs 逐字节一致 |
| `verify_render_vm.ps1` / `verify_audio.ps1` | PASS：端到端帧一致；BGM 出声、混音非静音 |
| `compare.py --run` | **逐字节一致** 6,496 B / 195 行 / `7F9837FF…` |
| 编译 | `make -B all lib test … win32` **0 警告 0 错误** |

## [0.3.0] — 已打 tag（v0.3.0 → 55b8e6f）：存档写入闭环（模板路径）+ 结构分析文档

0.3 的主线只有一条：**把「存档写入」做成能重复验证的闭环**，并把槽内快照的结构知识固化成一篇文章。
不做的事：从零生成存档（缺字段语义）、游戏内 `SaveWrite`/`Load` 接线。

### 新增

- **`--save-encoding cp932|gbk`：存档字符串编码可选**（默认跟随脚本编码）
  - 实跑对照（同一句 `プロローグ`）：CP932 → `83 76 83 8d 83 8d 81 5b 83 4f`，
    GBK → `a5 d7 a5 ed a5 ed a9 60 a5 b0`，与 Python codecs 的输出**逐字节相同**
  - `test_save --dump-strs` 新开关：打印槽内三条字符串的 hex，便于离线比对
  - 新增 `tests/verify_save_encoding.ps1`：真跑外壳两次（默认 / `--save-encoding gbk`）+
    逐字节断言，**PASS**；这条补上了 0.3 测试盘里「GBK 路径端到端」的空缺
- **存档序列化的字节布局回归钉**：一份 52 字节合成槽（时间戳 + 三条字符串 + 2×2 缩略图 +
  5 字节尾段）序列化后 FNV-1a 64 固定为 `0x55bc2d006a29f2bf`，`make test_save` 会校验
  —— 以后改序列化顺序 / 字节序 / 漏字段会立刻失败（`test_save` 自检 50 → **58 项**）
- **鼠标滚轮接入 VM（`InputGetWheel`，命令 36）**：外壳把窗口滚轮增量累加进 VM，
  脚本取走即清零；120 = 一格，正负方向都支持
  - 顺手修掉一处 id 错位：原代码把 `case 36` 标成 `InputGetUp`，而表里 35 = `InputGetUp`、
    36 = `InputGetWheel` —— 现在两个 id 都按表处理
  - 验收：`tests/test_wheel.c` **8 项**（首帧为 0、单格、同帧累加 120+240=360、负方向、
    加 0 是 no-op、帧间无残留、NULL 安全）；`run_all.py` → **18/18 PASS / 76.7 s**
  - 与冻结基准不冲突：滚轮默认为 0，`compare.py --run` 仍逐字节一致（6,496 B）
- **`docs/save-snapshot.md`**：槽内「引擎状态快照」专文 —— 常量头 64 B、中段密度地标
  （`+0x19000` 共同分界 / 118,784 B 稀疏块）、常量尾 216×6B 表、17 字节记录流，
  以及「未解清单 + 继续研究的路线」。`docs/README.md` 索引已收录（27 份文档）
- **`tests/verify_save_preview.ps1`**：存档预览的端到端验收（新增，自动跑）
  - 真跑 `fvp.exe --run-seconds 6 --save-preview-every 2`，断言：退出码 0、
    ≥2 次写入、缩略图来自活画布、每个产出的槽都能被 `test_save` 读回（0 失败）、
    且**尾段全部保持模板的 598,950 字节**
  - 实测：4 个槽 `str 0/0/0 → 10/0/0 → 10/0/26 → 10/0/22`，45 项断言 0 失败
- **`--save-preview-every N`：运行中周期性写存档预览**（「按 F5 存档」的自动化版）
  - 实测 7 秒 / 每 2 秒：写出 4 个周期槽 + 1 个退出槽，文件名 `.1/.2/…`
  - **脚本进度会体现在内容里**：前两个 `str 0/0/0`，之后变成 `str 10/0/14` → `10/0/16` → `10/0/22`
    —— 这正是游戏自己的 `SaveCreate` 在喂数据，证明「存档数据从运行时到文件」这条链路是活的
  - 5 个槽全部被 `test_save` 读回：**52 项断言 0 失败**，每个槽的引擎状态尾段都是模板的 598,950 字节
- **118,784 B 稀疏块的实测结论（否掉了旧猜想）**：5 个槽里它几乎全零（31–112 个非零字节），
  只有「演出中途存的」s906 填了数据，而且是 **17 字节周期**的记录流
  （`f4 ff ff ff` = -12、`34 03 00 00` = 820、递增序号 6/7/8/9）
  → 它是「平时为空、演出时填」的表（像动画/事件队列），**不是 32 个文本窗口**
  - `docs/save-format.md` §2.4.2 新增，§2.4.1 里那条旧猜想同步改成「已否掉」
- **变长中段的密度结构（实测，6 个槽一致的地标）**：按 4 KiB 块 / 5% 非零密度分段后，
  6 个槽都出现同一串地标 —— `+0x19000`（102,400）是共同分界，之前是「前段 + 86,016 B 零区」，
  之后是随内容变化的 DENSE 区（229,376–331,776 B）；紧跟一个 **118,784 B 稀疏块**
  （与内容无关；118,784/32 = 3,712 B/窗口，与早先推定的 0xB1D=2,845 冲突，**待定论**），
  再之后是固定尾链 12,288 / 4,096 / 4,096–8,192 / 8,192 / 4,096 / 8,192 / 收尾 1.9–4.9 KB
  - 结论写进 `docs/save-format.md` §2.4.1
- **UTF-8 → CP932 / GBK 编码方向（`fvp_encode`）**：写存档需要（槽里的字符串是原编码）
  - 反查码表：单字节表 + 双字节行表；表里没有的码点写 `?`（对应 Python `encode(enc,'replace')`）
  - 验收：`tests/test_encode.c` **14 项** —— 含真实存档里出现过的字节序列往返
    （`83 76 83 8d 83 8d 81 5b 83 4f` = `プロローグ`、GBK `谁这样说道。`、全角空格 `A1A1`）
- **存档预览现在带脚本字符串**：`--save-preview` 会把 `SaveCreate` 准备好的字符串按脚本编码
  写进槽里。实测 6 秒运行写出的槽 `str 10/0/8`，其中
  `str0 = 83 76 83 8d 83 8d 81 5b 83 4f`（`プロローグ`）、`str2 = 81 40 81 63 81 63 81 42`（`　……。`）
  —— **与真实存档 s481.bin 的字节完全相同**；文件 689,381 B，`test_save` 读回 24 项断言全过
- **桌面外壳 `--save-template` / `--save-preview`：真实存档预览**（0.2 存档线第一次「写出一个能读的槽」）
  - 拿一个真实存档槽当模板，只替换**时间戳（本地时间）**与**缩略图（当前画面 box-filter 缩到 200×113）**，
    其余字节（含 500+ KB 引擎状态尾段）原样保留
  - 实测：`--save-template s485.bin --save-preview out/preview-s900.bin` →
    `689,363 bytes`（与模板同长）、`timestamp 2026-11-24 01:29`、缩略图是从活画布缩出来的
  - 再用 `test_save.exe --savebin ... out/preview-s900.bin` 读回：**24 项断言全过**
    （时间戳/缩略图/尾段 598,950 字节全部正确）
  - ⚠️ 仍未做：字符串段（存档里是 CP932/GBK，UTF-8 需转码）；槽内快照语义仍未解，所以没有 `SaveWrite`
- **保守写档路径的自动化验收**：`tests/test_save.c` 拿真实存档槽当模板，只改
  **已确证字段**（时间戳 / 章节名 / 缩略图），写盘后读回并断言
  **引擎状态尾段（500+ KB）逐字节不变**、新字段全部生效（真实样本 49 → **59 项**断言）
  - 这正是 `docs/save-format.md` §6「能安全做的」那条路径，现在有测试兜住
- **存档尾段常量区的内部结构（实测，`tools/tailstat.py` 的「常量区」小节）**
  - 1,438 B 常量尾 = **216 × 6B 定长表**（每条 `{u16 0, u16 100, u16 0}`，216 个 0x64 全部落在 +2）
    + 140 B 全零 + 末 2B `01 01`
  - 常量头 64 B = 24 个 0 + `02` + 23 个 0 + 之后每 4 字节一个非零（+48 / +52 / +56）
  - 结论写进 `docs/save-format.md` §2.4.1；写回时整段原样拷贝即可，不必解语义
- **桌面外壳 `--thumb-shot FILE`**：退出时把最后一帧按 200×113 缩成存档缩略图 BMP
  （`src/thumb.c` 的 box filter）。实测 `--run-seconds 4` 抓到的缩略图是**真实开场画面**
  （黑底 + 底部对话框），尺寸 200×113 / 90,400 字节
- **`tools/tailstat.py`：存档尾段统计/差分**（不猜字段语义，只量数据）
  - 6 个真实槽实测：尾段 502,020–598,984 B，非零只占 11–16%（其余零填充）；
    **前 64 字节完全相同**（24 个 0 后接 `02 00 00 00`）、**末 1,438–1,449 字节完全相同**
    （末两字节 `01 01`）
  - 由此确立：尾段 = `≥64B 常量头` + `变长中段` + `≥1438B 常量尾`；中段长度随内容变化 ~97 KB，
    所以写回时**必须整体保留原中段**，不能按固定偏移生成
  - 结论与复现方法写进 `docs/save-format.md` §2.4.0 / §5
- **存档缩略图缩放（`src/thumb.c` / `src/thumb.h`）**：把渲染层的 BGRA 画面 box-filter 缩到
  200×113（或任意尺寸），alpha 强制写成 0xFF —— 与引擎 `render_thumb()` 写进槽文件的格式一致
  - 面积平均而非双线性：1920×1080 → 200×113 这种大幅缩小时更接近原图整体亮度
  - 用 64 位整数算覆盖矩形，1920×1080 不会溢出
  - 验收：`tests/test_thumb.c` **14 项**（纯色保持、2×2 平均、真实 1920×1080→200×113 全像素检查、非法参数拒绝）
- **槽文件落盘（`fvp_save_slot_write_file`）**：序列化整块写文件（UTF-8 → 宽字符，失败退回 ANSI）；
  `test_save.c` 写盘回环 +6 项（共 56 项）
- **存档暂存区（`SaveCreate` 命令 93 / `SaveThumbSize` 命令 94）**：脚本要写的存档内容
  先记在 VM 里 —— id 0/1/2 = 章节名 / 未用 / 当前对话行，id 3 = 选槽号；
  缩略图尺寸按引擎范围钳制（20..200 × 15..150），初值 96×72
  - 真实脚本 900k 步实测：`save state slot=1 str=1/0/0 thumb=200x113`，
    **缩略图尺寸与真实存档样本的 200×113 完全一致**
  - `SaveWrite` 尚未实现（槽内引擎状态快照语义未解），所以只到「准备好数据」为止
  - `fvm` 汇总多一行 `save state ...`；`compare` 仍逐字节一致

- **文本窗口属性存储（`src/textattr.c` / `src/textattr.h`）**：`TextColor`（命令 112）、
  `TextSize`（126）、`TextSpeed`（129）不再落到兜底桩，而是按静态分析的范围钳制后存进
  VM 的 32 个窗口属性里（颜色 0..255、字号 12..64 × 8..32、速度 0..300000，窗口号 0..31）
  - 真实脚本 900k 步实测：`text attrs 4 window(s) configured`
  - 渲染层暂不消费这些值，所以**已验收的帧与逐字节文本基线都不变**（`compare` 仍 6,496 B 一致）
  - 验收：`tests/test_textattr.c` **29 项**（默认值、钳制两个方向、越界拒绝、configured 计数、reset）

### 变更

- `fvm` 汇总多一行 `text attrs N window(s) configured`；`make test_textattr`；
  `tests/run_all.py` 步数随新测试增加（0.3.0 收尾时 **18 步**）

### 验收（0.3.0 收尾实跑）

| 项 | 结果 |
|---|---|
| `tests/run_all.py` | **18/18 PASS / 57.0 s** |
| `compare.py --run` | **逐字节一致**（6,496 B / 195 行 / `7F9837FF…`） |
| `test_save` | 自检 **58 项** + 真实槽 **49 项**（含字节布局回归钉 `0x55bc2d006a29f2bf`） |
| `test_save_vm` | 合成 19 + 真实 13 |
| `test_flags` | 28 + 37 |
| `test_textattr` / `test_thumb` / `test_encode` / `test_wheel` | 29 / 14 / 14 / 8 |
| `verify_save_preview.ps1` | PASS（真跑外壳 → 4 个槽全部读回、尾段 598,950 B 不变） |
| `verify_save_encoding.ps1` | PASS（CP932 `83 76 …` / GBK `a5 d7 …` 与 Python codecs 一致） |
| `verify_render_vm.ps1` / `verify_audio.ps1` | PASS（端到端帧一致；BGM 出声、混音非静音） |
| 编译 | `make -B all lib test … win32` **0 警告 0 错误** |

## [0.2.0] — 已打 tag（v0.2.0 → `4c80b0e`）：音频接运行时 + 存档读写落地 + 全局 flag

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