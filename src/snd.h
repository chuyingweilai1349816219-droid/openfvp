#ifndef FVP_SND_H
#define FVP_SND_H

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* 通道用途 */
enum {
    SND_CH_BGM = 0,
    /* BGM 的交叉淡入伙伴：切曲时新曲先在这里淡入，旧曲在 SND_CH_BGM 上淡出，
     * 完成后再交换角色（见 snd_crossfade）。SE 的轮转范围不含这两个通道。 */
    SND_CH_BGM_ALT = 1,
    SND_CH_VOICE = 2,
    SND_CH_SE = 3,
    SND_CH_COUNT = 8
};

typedef struct Snd Snd;

Snd *snd_create(int rate, int channels);
void snd_destroy(Snd *s);

/* 播放（旧通道上的声音会被替换）。loop!=0 时循环。返回 0 成功。 */
int snd_play_ogg(Snd *s, int ch, const uint8_t *mem, size_t len, int loop, float vol);
int snd_play_wav(Snd *s, int ch, const uint8_t *mem, size_t len, int loop, float vol);
int snd_play_file(Snd *s, int ch, const char *path, int loop, float vol);

void snd_stop(Snd *s, int ch);
void snd_stop_all(Snd *s);
void snd_set_vol(Snd *s, int ch, float vol);
/* 线性淡入淡出：在 ms 毫秒内把通道的**有效增益**从当前值线性过渡到 target。
 * target == 0 且淡出完成时通道自动停止（原版切场景是淡出淡入，不是硬切）。 */
void snd_fade(Snd *s, int ch, float target, int ms);
/* 当前有效增益（0..1+）；没有淡变时等于 snd_set_vol 设的值。 */
float snd_channel_gain(Snd *s, int ch);
/* 交叉淡入淡出：在 ms 内把 from 通道淡到 0（结束自动停止）、
 * to 通道淡到它自己的目标音量（播放时给的 vol，或用 snd_set_vol 设的值）。
 * 调用前 to 通道应当已经装好新音频。 */
void snd_crossfade(Snd *s, int from, int to, int ms);
/* 测试/流式播放用的直通入口：拷一份交错立体声 PCM 到通道。 */
int snd_play_pcm(Snd *s, int ch, const int16_t *stereo, int frames, int loop,
                 float vol);

int snd_is_playing(Snd *s, int ch);
/* 该通道当前（重采样后）的总帧数与播放位置 */
int snd_channel_frames(Snd *s, int ch);
int snd_channel_pos(Snd *s, int ch);

/* 混音出 frames 帧交错立体声 PCM16；内部会推进播放位置。 */
void snd_mix(Snd *s, int16_t *out, int frames);

/* 诊断：返回最近一次解码的采样率/声道/帧数，失败返回 -1 */
int snd_last_info(int *rate, int *channels, int *frames);

#ifdef __cplusplus
}
#endif

#endif