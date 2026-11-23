# 音频层

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

## 待办

1. **流式解码**：现在是整段解码，一首 BGM 占 22 MB，语音多了会爆内存
2. **BGM 交叉淡入淡出**：原版切场景时 BGM 是淡出淡入，不是硬切
3. **语音与文字同步**：`LipSync`（argc=2）在脚本里只用了 3 次，暂未处理
4. **音量体系**：`SoundVol` / `SoundTypeVol` / `SoundMasterVol` 三级，需要接进混音
5. **平台输出**：Windows 用 waveOut，Android 用 AAudio（48 kHz 原生，正好）

## 与脚本命令的对应

| 命令 | id | 参数 | 说明 |
|---|---:|---|---|
| `AudioLoad` | 0 | (2) | 预加载音频到槽位 |
| `AudioPlay` | 1 | (2) | 播放槽位里的音频 |
| `AudioStop` | 4 | (2) | 停止 |
| `AudioVol` | 6 | (3) | 设置音量 |
| `AudioType` | 5 | (2) | 指定通道类型（BGM/语音/SE） |
| `AudioState` | 3 | (1) | 返回播放状态 |
| `SoundPlay` / `SoundLoad` / `SoundStop` | 103/101/105 | — | 旧接口，本作少量使用 |

资源名形如 `bgm/001`、`voice/01001530`、`se/201`，前缀表示类别，去掉前缀就是包内条目名。