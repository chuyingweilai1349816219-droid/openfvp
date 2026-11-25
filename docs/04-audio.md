# 音频层

> 以下内容由 DeepSeek 生成,不一定代表作者个人意愿

## 素材格式（实测）

| 来源 | 格式 | 参数 |
|---|---|---|
| `bgm.bin` / `bgm2.bin` | Ogg Vorbis | 48 kHz 立体声（如 `001` 116.41 秒 / 2.8 MB） |
| `voice.bin` | Ogg Vorbis | 同上，18,612 条 |
| `se.bin` / `se_env.bin` / `se_sys.bin` | RIFF/WAVE PCM16 | **22.05 kHz** 立体声（如 `001` 5.64 秒） |

**采样率不统一**：BGM 是 48 kHz，SE 是 22.05 kHz。引擎内部统一跑 **48 kHz / 立体声 / PCM16**，非 48 kHz 的素材在加载时重采样。

## 实现（`src/snd.c`）

```c
Snd *snd_create(int rate, int channels);
int  snd_play_ogg (Snd*, int ch, const uint8_t *mem, size_t len, int loop, float vol);
int  snd_play_wav (Snd*, int ch, const uint8_t *mem, size_t len, int loop, float vol);
int  snd_play_file(Snd*, int ch, const char *path, int loop, float vol);
void snd_stop(Snd*, int ch);
void snd_set_vol(Snd*, int ch, float vol);
void snd_mix(Snd*, int16_t *out, int frames);   /* 交错立体声 */
```

- **Ogg**：内置 `stb_vorbis`（public domain），整段解码
- **WAV**：自己解析 RIFF，只支持 `fmt=1`（PCM16）；ADPCM 直接返回失败
- **重采样**：线性插值，在加载时做一次，播放时零开销
- **混音**：8 个通道（BGM=0、语音=1、SE=2 起），逐样本相加后钳制到 16 位

解码策略是「播放时整段解码进内存，停止时释放」。BGM 一首 116 秒 × 48 kHz × 2ch × 2B ≈ 22 MB，原型阶段可以接受；正式版要改成流式（接口不用变，只要把 `pcm` 换成环形缓冲）。

## 自检

```
make test
bin/test_snd.exe <输入.ogg|.wav> <输出.wav> [引擎采样率]
```

实测：

```
Ogg : source 48000 Hz 2ch 5,587,774 frames (116.41s) → engine 同, peak=19297 rms=4385
WAV : source 22050 Hz 2ch   124,457 frames ( 5.64s) → engine 48k 270,926 frames (5.644s), peak=22123
```

重采样后时长保持一致（5.64 → 5.644 秒），能量正常，无静音。

## 淡入淡出（`snd_fade`，2026-11-25）

原版切场景时 BGM 是**淡出淡入**，不是硬切。之前 `src/snd.c` 只有 `snd_set_vol()` 的
瞬时改变，这轮加了线性增益斜坡：

```c
void  snd_fade(Snd *s, int ch, float target, int ms);  /* 线性过渡到 target */
float snd_channel_gain(Snd *s, int ch);                /* 当前有效增益 */
int   snd_play_pcm(Snd *s, int ch, const int16_t *stereo, int frames,
                   int loop, float vol);               /* 直通 PCM（测试/流式） */
```

- 通道里 `vol` 是**目标音量**、`gain` 是**当前生效增益**，混音按帧推进 `gain`；
- `target == 0` 且淡出完成时**通道自动停止**（不必再调 `snd_stop`）；
- 淡出中途反向淡入是允许的（增益会拉回去，通道不停止）；
- `ms == 0` 等价于立即生效；
- 混音公式不变（仍是 `pcm * gain` 后饱和裁剪），冻结基线不受影响。

单测：`tests/test_snd_fade.c`（17 项）——用 8 kHz 的常数 PCM 验证：
淡出逐块下降、第 6 块（>500 ms）静音且通道自动停止；淡入逐块上升并收敛；
`ms=0` 立即生效；淡出中途反向淡入不停止通道。

**交叉淡入淡出已接线**（`snd_crossfade`）：

- 通道布局改成：`SND_CH_BGM=0`、**`SND_CH_BGM_ALT=1`**（BGM 的淡入伙伴）、
  `SND_CH_VOICE=2`、`SND_CH_SE=3`（SE 轮转 3..7，比原来少一条）；
- 播放桥（`src/syscall.c`）里的 BGM 选择改为**两条通道轮换**：当前 BGM 通道在响时，
  新曲先装到另一个 BGM 通道，然后 `snd_crossfade(旧, 新, 600 ms)`，
  统计里记 `bgm_crossfades`，日志 `snd: bgm crossfade A -> B (600 ms)`；
- 默认 600 ms，可用 `fvp_vm_snd_set_bgm_xfade(vm, ms)` 调整（0 = 硬切）；
- `snd_crossfade` 在 `syscall.c` 里是**弱符号**（`#pragma weak`），因为 `fvm`/部分单测
  只链接 `syscall.o` 不链接 `snd.c`，调用点有 NULL 保护。

单测（`tests/test_snd_fade.c`，24 项）新增交叉淡变用例：两条通道同时有声（和值明显高于
单条）、过渡结束后旧通道停止、新通道保持满音量。

**真实运行里的观察**：iroseka 前 25 秒没有 BGM 切换（脚本早期还没起 BGM），所以日志里
暂时看不到 `bgm crossfade` 行；要观察到它需要一段"场景切换"的脚本区间。这条如实记录，
不假装已经验证过。

## 三级音量（槽 × 主 × 类型）+ 静音（2026-11-25）

命令表里的音量分三层，任意一层都能把声音压下去：

| 层 | 命令 | 存哪 |
|---|---|---|
| 槽音量 | `SoundVol(107)` / `AudioVol(6)` | 每槽 `volume`（0..100） |
| 主音量 | `SoundMasterVol(101)` | 桥 `master_vol` |
| 类型音量 | `SoundTypeVol(106)` | 桥 `type_vol[10]`（BGM/语音/SE…） |
| 通道静音 | `AudioSilentOn` / `SoundSilentOn` | 槽 `silent` |

有效增益 = `槽/100 × 主/100 × 类型/100`，夹到 `[0,1]`；`silent` 恒为 0。
这轮把这条链从 `syscall.c` 的静态函数抽成**纯函数**
`float fvp_snd_gain_calc(slot_vol, master_vol, type_vol, silent)`，
`fvp_snd_gain()` 只负责取三层当前值再调它。

**单测 `tests/test_snd_gain.c`（40 项）**：

* 默认 (100,100,100) → 1.0；单层减半 → 0.5；`50×50×50 → 0.125`、`25×80×50 → 0.1`；
* 任一层为 0 → 0；**静音优先于任何音量**；
* 越界夹紧：`200 → 1.0`、负数 → 0、两层越界 → 1.0；
* 单调性：槽音量/主音量各扫 0..100，增益单调不减；
* 分档语义：主音量影响所有类型，类型音量只影响自己那组。

命令处理里三层音量改动后都会调 `fvp_snd_refresh_gains()`，把新增益推到正在播放的
通道上（`snd_set_vol`），所以运行中改音量立即生效。

## 待办

1. **流式解码**：现在是整段解码，一首 BGM 占 22 MB，语音多了会爆内存
2. **BGM 交叉淡入淡出**：已实现（两条 BGM 通道轮换 + `snd_crossfade`），还差"真实运行里观察到一次切换"的验证样本
3. **语音与文字同步**：`LipSync`（argc=2）在脚本里只用了 3 次，暂未处理
4. ~~**音量体系**：三级音量已接进混音并有 40 项单测（见上）~~
5. **平台输出**：Windows 用 waveOut，Android 用 AAudio（48 kHz 原生，正好）

## 与脚本命令的对应

| 命令 | id | 参数 | 说明 |
|---|---:|---|---|
| `AudioLoad` | 0 | (2) | 记录槽位 → 资源名（不解码） |
| `AudioPlay` | 1 | (2) | 播放槽位里的音频（延迟解码） |
| `AudioStop` | 4 | (2) | 停止 |
| `AudioVol` | 6 | (3) | 设置音量 |
| `AudioType` | 5 | (2) | 指定通道类型（BGM/语音/SE） |
| `AudioState` | 3 | (1) | 返回播放状态 |
| `AudioSilentOn` | 2 | (1) | 通道静音 |
| `SoundLoad` / `SoundMasterVol` / `SoundPlay` | 100/101/102 | — | 旧接口 |
| `SoundSilentOn` / `SoundStop` / `SoundType` | 103/104/105 | — | 旧接口 |
| `SoundTypeVol` / `SoundVol` | 106/107 | — | 旧接口 |

资源名形如 `bgm/001`、`voice/01001530`、`se/201`，前缀表示类别，去掉前缀就是包内条目名。

id 以 `fvp/ref/cmd_table.json`（hcb 命令表）为准；旧接口是 100..107，**不是** exe 注册下标。

## 运行时接线（§ 运行时接线）

本节记录把已验证的 `src/snd.c` 接到 VM 命令分发（`src/syscall.c`）上的做法与实测数字。

### 接线 API（`src/vm.h` / `src/vm.c`）

```c
void fvp_vm_attach_snd(FvpVM *vm, Snd *snd);       /* NULL = 脱离 */
int  fvp_vm_snd_add_archive(FvpVM *vm, const char *path);
int  fvp_vm_snd_add_audio_dir(FvpVM *vm, const char *dir);  /* 注册 6 个音频包 */
int  fvp_snd_bank_read(FvpSndBridge *b, const char *name,
                       uint8_t **data, size_t *size, const char **archive);
```

- 照 gfx 的范式：`vm.c` / `syscall.c` 里所有 `snd_*`、`bin_*` 符号都是 `#pragma weak`。`bin/fvm.exe` 既不链接 `src/snd.c` 也不链接 `src/bin.c`，所以 `fvp_vm_attach_snd()` 在它里面不可达，所有音频命令仍走原来的空实现（unknown 计数、temp 规则不变）。
- `fvp_vm_snd_add_audio_dir(dir)` 注册 `bgm.bin bgm2.bin voice.bin se.bin se_env.bin se_sys.bin`；包在第一次按名字查找时才 `bin_open()` 并缓存索引。
- bridge 带一个可选观察者 `on_event(FvpSndEvent)`（命令 id / 槽位 / 类别 / SND 通道 / 名字 / 命中包 / loop / 音量），测试靠它打印播放列表。

### 通道映射

| 类别 | SND 通道 |
|---|---|
| BGM | `SND_CH_BGM`（0） |
| 语音 | `SND_CH_VOICE`（1） |
| SE | `SND_CH_SE`（2）起，6 路轮转 |

每个 SND 通道记录当前 owner 槽位；Stop / 被新播放顶替时清 owner。实现时这里有一个真实 bug：旧的 `AudioStop` 会因为槽位残留的通道号把**新** owner 停掉（表现为 BGM 只响一拍），已修（`fvp_snd_detach_chan()` 清 `snd_chan`）。

### 类别推断

1. **资源名前缀是权威**：`bgm/`、`bgm2/` → BGM；`voice/`（及 `voice2/`）→ 语音；`se/`、`se_env/`、`se_sys/`、`sound/`、`env/` → SE。大小写不敏感（脚本里 `BGM/050` 与 `bgm/001` 都出现）。
2. 前缀认不出来时才用 `AudioType` / `SoundType` 的 type。3M 步动态跟踪显示：`AudioType(ch,1)` 用于 BGM 通道，`AudioType(ch,0)` 用于语音通道（0x265000 处 `AudioType(ch,0)` 之后马上 `AudioLoad(ch, "voice/" + …)`）；legacy `SoundType` 的 0/2/3/4 是 SE 分组。因此取：type 1 → BGM、type 0 → 语音、2..9 → SE。
3. 兜底：Audio 槽 0..3 映射到同号 SND 通道，legacy Sound 槽落 SE 池。

### 资源解析

- 取最后一个 `/`（或 `\`）之后的 basename，去掉扩展名（包内条目名没有扩展名）。
- **前缀必须参与解析**：音频包之间有大量同名条目（`001` 同时存在于 bgm/se/se_env/se_sys；6 个音频包共 67 个 basename 重名），只按 basename 全局查会放错声音。这里纠正 `01-assets.md` 的"跨包零重名"——那只对 6 个图像包成立。
- 已知前缀优先查对应包；查不到（或前缀未知）再顺序扫其余已注册包，并记录实际命中的包。实测 `BGM2/002` 命中 `bgm2.bin`（16.8 MB），不会误取 `bgm.bin` 的同名 11.6 MB 条目。
- **解码严格延迟到 Play**：Load 只记录"槽位 → 名字"。命中后按魔数分派：`OggS` → `snd_play_ogg()`，其余 → `snd_play_wav()`（只吃 PCM16）；包数据在解码后立即释放，BGM 的 20+ MB 解码内存只在播放期间存在。

### 音量模型

- 按引擎的三级结构实现：`SoundMasterVol`（主音量）、`SoundTypeVol`/`AudioType`（10 个分组）、`AudioVol`/`SoundVol`（单源），增益 = 三级归一化相乘（对应 0x436080 的算式）。`AudioSilentOn` 记为该通道静音，下一次 Play 时清除（对应 4410b0 写 +0x14 = 0）。
- **原型配置层缺失的回退**：脚本的音量全局变量由存档/配置层填充，本原型里 `SoundMasterVol`/`SoundTypeVol` 运行时收到的永远是 0，`AudioVol`/`SoundVol` 是 0/1——这些不是用户设置，照字面应用会整机静音。因此：主/分组音量在收到第一个非零值之前保持默认 100；单源音量只在主或分组已配置后才参与计算。shell 若解析了真实选项，在跑 VM 之前用正常命令写入即可，回退规则自动失效。副作用：原型里"音量 0/1"的静音意图不生效。
- 音量/类型命令执行后立即对所有在播通道重算并 `snd_set_vol()`；淡变时长忽略。

### 验证（`fvp/tests/audio_vm.c`）

测试程序不接声卡，只把 `snd_mix()` 的输出当"时间轴"统计：

```
gcc -std=c99 -O2 -Isrc -o bin/audio_vm.exe tests/audio_vm.c \
    src/fvp.c src/encoding.c src/hcb.c src/vm.c src/syscall.c \
    src/snd.c src/bin.c -lm
bin/audio_vm.exe <iroseka.hcb> --steps 900000 --wav-out out/c/audio-900k.wav
```

实测（引擎 48 kHz / 立体声，click=600，每 1000 步混 1024 帧）：

| 步数 | 音频命令 | BGM 播放 | 解析 / 失败 | 解码失败 | 混音 peak / rms | 非静音块 |
|---:|---|---:|---|---:|---:|---:|
| 900,000 | loads=21 plays=8 stops=137 vol=70 type=12 silent=119 | 4（`BGM2/002`、`024`、`025`、`005`，均命中 `bgm2.bin`） | 4 / 4 | 0 | **14004 / 2431.8** | 757 / 908 |
| 3,000,000 | loads=57 plays=42 stops=665 vol=223 type=46 silent=407 | 10（`002 024 025 005 013 006 011 008`） | 10 / 32 | 0 | **14164 / 2144.4** | 2203 / 3008 |

- 首个成功播放：step 30,000（`BGM2/002`，解码 43,007,768 帧）；其余 41.5M–49.2M 帧。
- 失败播放全部是 legacy `SoundPlay` 打在从未 `SoundLoad` 的槽位 0 上（脚本的 SE 资源名全局变量来自存档层，本原型为空）。引擎对空 source 的播放同样无声，所以它们不是解码失败。
- 语音（`AudioLoad(ch, "voice/" + IntToText(...))` 路径）在 3M 步内没有被执行到，因此 VOICE 播放为 0；接线路径已通，缺的是存档层给出的 voice id。

### 重采样放大防护（`src/snd.c` 修复）

`resample_to_rate()` 现在拒绝：采样率不在 1000..384000 Hz、或输出超过 30 分钟（`dst_rate × 1800` 帧）、或 WAV data 帧数会溢出 int。复现用例 `out/fuzz/repro/wav_rate1_amplification.wav`（1 Hz、1000 帧）；修复前会生成 4410 万帧、单次申请约 168 MB，修复后：

```
RESULT REJECT kind=wav rc=-1
ALLOC mode=canary allocs=6 ... peak=6470 oom=0
```

普通素材不受影响：`work/se001.wav` 22.05k → 44.1k，248,914 帧，`RESULT OK`。

### 仍不确定 / 未实现

1. `AudioSilentOn` 的静态分析只看到写通道 +0x14；这里当作"该通道静音，Play 时解除"，语义未在真机验证。
2. Stop / Vol 的淡变毫秒数被忽略（硬停、立即变音量）。
3. `AudioPlay(ch, arg2)` 在引擎里按类型判空（null → playflag 0，其余 → 1），这里解释为 loop 标志；C VM 里 int 0 与 false 无法区分，统一按真值处理，BGM 另强制 loop=1。
4. 语音/SE 资源名依赖存档/配置层；接入存档层或由 shell 注入名字之后，同一条 Play 路径即可出声。
5. 流式解码、BGM 交叉淡入淡出、`LipSync` 仍未做（见上面的待办）。
6. 音量回退规则是"配置层缺失"的原型补丁；接入真实配置后应删掉对 0/1 的特殊处理并复测。