# 资源清单

`C:\Users\Mechrevo\Downloads\五彩斑斓的世界HD` 下的资源包构成。
所有包都是同一套 BIN 格式（见 `00-engine.md`），条目名是 Shift-JIS，**不带扩展名**，格式靠魔数判断。

## 包一览

| 包 | 条目数 | 解压后大小 | 内容 | 实际格式 |
|---|---:|---:|---|---|
| `graph.bin` | 1,112 | 492 MB | 事件 CG、特效、UI 图、外字、标题 | `hzc1`（44 字节头 + zlib） |
| `graph_bg.bin` | 332 | 786 MB | 背景 `BG*` | `hzc1` |
| `graph_bs.bin` | 428 | 675 MB | 立绘 `CHR_*` | `hzc1` |
| `graph_vis.bin` | 286 | 1.62 GB | 角色 CG `ETC_*` | `hzc1` |
| `graph_vis1.bin` | 256 | 1.99 GB | 角色 CG `KANA_*` | `hzc1` |
| `graph_vis2.bin` | 239 | 1.83 GB | 角色 CG `MIO_*` | `hzc1` |
| `bgm.bin` | 45 | 427 MB | BGM | **Ogg Vorbis**（`OggS`） |
| `bgm2.bin` | 42 | 604 MB | BGM（HD 重编曲版） | Ogg Vorbis |
| `voice.bin` | 18,612 | 990 MB | 全语音 | Ogg Vorbis |
| `se.bin` | 301 | 102 MB | 音效 | **RIFF/WAVE** |
| `se_env.bin` | 59 | 66 MB | 环境音 | RIFF/WAVE |
| `se_sys.bin` | 13 | 3 MB | 系统音 | RIFF/WAVE |

6 个图像包共 **2,653 个不重名条目**（实测：跨包零重名），所以解析时可以建一张全局「名字 → (包, 索引)」表，不必按前缀猜包。

合计约 **9.36 GB**。

## 命名规律（`graph.bin`）

```
000101W          事件 CG（6 位数字 + W/H = 宽/高版本）
000101W_F        同上，多帧版（F = frames，分表情/动作）
bu_effect*       演出特效（光、魔法阵、雨、雪）
sd_*             立绘差分
cut_*            过场切图
title*/title2_*  标题画面、菜单图标
fc_*             功能提示图标（存档/读档/快进/系统…）
menu_*           菜单文字图
opt_*            设置画面部件
gaiji_*          外字（特殊符号）
icon_*           小图标
logo_*           厂商 logo
x*.x             分辨率/缩放用贴图（x1.0 ~ x2.8）
```

## 音频格式细节

- **Ogg Vorbis**：可以直接用 `stb_vorbis`（单文件、MIT）在 Android 上解码，或交给平台解码器
- **RIFF/WAVE**：需要确认是 PCM 还是 IMA-ADPCM / MS-ADPCM，Android 原生只吃 PCM，非 PCM 要自己实现解码
- 引擎侧混音用 DirectSound 8，多路同时播放（BGM + 语音 + SE）

## 文本

- 日文版：`iroseka.hcb`，Shift-JIS
- 中文补丁：`.iroseka.hcb`，GBK
- 两者结构相同，见 `00-engine.md` 与 `cn-script-rebuild.md`

## 视频

- `movie\01.wmv`（234 MB，WMV 编码，开场动画）
- 引擎通过 `Movie` / `MovieState` / `MovieStop` 三个命令控制
- Android 没有 WMV 解码器 → 转码为 H.264/VP9 用 `MediaCodec` 播放，或直接跳过