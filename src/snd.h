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
    SND_CH_VOICE = 1,
    SND_CH_SE = 2,
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