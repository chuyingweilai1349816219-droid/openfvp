/*
 * 音频通道淡入淡出（snd_fade）单元测试。
 *
 * 背景：原版切场景时 BGM 是淡出淡入，不是硬切。snd.c 原来只有 snd_set_vol 的
 * 瞬间改变，这轮加了 snd_fade(ch, target, ms) 的线性增益斜坡，这里用合成的
 * 常数 PCM 验证包络：淡出逐块衰减、结束自动停；淡入逐块增强、到点收敛到目标。
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../src/snd.h"

static int g_fail = 0;
static int g_pass = 0;

#define CHECK(cond, ...)                                                      \
    do {                                                                      \
        if (cond) {                                                           \
            ++g_pass;                                                         \
        } else {                                                              \
            ++g_fail;                                                         \
            printf("FAIL %s:%d: ", __FILE__, __LINE__);                       \
            printf(__VA_ARGS__);                                              \
            printf("\n");                                                     \
        }                                                                     \
    } while (0)

#define RATE 8000
#define FRAMES 8000

/* 常数幅度 PCM（立体声交错），方便用块峰值看包络。 */
static void fill_const(int16_t *pcm, int frames, int amp)
{
    int i;

    for (i = 0; i < frames; ++i) {
        pcm[i * 2] = (int16_t)amp;
        pcm[i * 2 + 1] = (int16_t)amp;
    }
}

static int block_peak(const int16_t *buf, int frames)
{
    int peak = 0;
    int i;

    for (i = 0; i < frames * 2; ++i) {
        int v = buf[i] < 0 ? -buf[i] : buf[i];

        if (v > peak) {
            peak = v;
        }
    }
    return peak;
}

int main(void)
{
    Snd *s;
    int16_t *pcm;
    int16_t buf[800 * 2];
    int blk;
    int peaks[16];

    s = snd_create(RATE, 2);
    CHECK(s != NULL, "snd_create");
    if (s == NULL) {
        return 1;
    }
    pcm = (int16_t *)malloc((size_t)FRAMES * 2 * sizeof(int16_t));
    CHECK(pcm != NULL, "pcm buffer");
    if (pcm == NULL) {
        snd_destroy(s);
        return 1;
    }
    fill_const(pcm, FRAMES, 10000);

    /* --- 1. 淡出：500 ms（4000 帧），每块 800 帧，峰值必须逐块下降 --- */
    CHECK(snd_play_pcm(s, SND_CH_BGM, pcm, FRAMES, 1, 1.0f) == 0,
          "snd_play_pcm accepted");
    CHECK(snd_is_playing(s, SND_CH_BGM) == 1, "channel is playing");
    snd_fade(s, SND_CH_BGM, 0.0f, 500);
    /* 500 ms @ 8 kHz = 4000 帧 = 5 个 800 帧块；第 6 块应当已经静音且通道停止 */
    for (blk = 0; blk < 6; ++blk) {
        snd_mix(s, buf, 800);
        peaks[blk] = block_peak(buf, 800);
    }
    CHECK(peaks[0] > peaks[1] && peaks[1] > peaks[2] && peaks[2] > peaks[3] &&
              peaks[3] > peaks[4],
          "fade-out envelope decreases (%d %d %d %d %d)", peaks[0], peaks[1],
          peaks[2], peaks[3], peaks[4]);
    CHECK(peaks[4] > 0, "tail of the fade is still audible (%d)", peaks[4]);
    CHECK(peaks[5] == 0, "fade-out reaches silence after 500 ms (%d)",
          peaks[5]);
    CHECK(snd_is_playing(s, SND_CH_BGM) == 0,
          "fade-out stops the channel automatically");
    CHECK(snd_channel_gain(s, SND_CH_BGM) <= 0.0001f,
          "gain is zero after fade-out (%f)", snd_channel_gain(s, SND_CH_BGM));

    /* --- 2. 淡入：从 0 到 1（500 ms），峰值必须逐块上升并收敛 --- */
    CHECK(snd_play_pcm(s, SND_CH_BGM, pcm, FRAMES, 1, 0.0f) == 0,
          "replay at volume 0");
    snd_fade(s, SND_CH_BGM, 1.0f, 500);
    for (blk = 0; blk < 5; ++blk) {
        snd_mix(s, buf, 800);
        peaks[blk] = block_peak(buf, 800);
    }
    CHECK(peaks[0] < peaks[1] && peaks[1] < peaks[2] && peaks[2] < peaks[3],
          "fade-in envelope increases (%d %d %d %d)", peaks[0], peaks[1],
          peaks[2], peaks[3]);
    CHECK(peaks[4] >= 9900, "fade-in reaches full volume (%d)", peaks[4]);
    CHECK(snd_is_playing(s, SND_CH_BGM) == 1,
          "fade-in keeps the channel alive");

    /* --- 3. 立即淡变（ms = 0）等价于 snd_set_vol --- */
    snd_fade(s, SND_CH_BGM, 0.5f, 0);
    CHECK(snd_channel_gain(s, SND_CH_BGM) > 0.49f &&
              snd_channel_gain(s, SND_CH_BGM) < 0.51f,
          "zero-ms fade applies immediately (%f)", snd_channel_gain(s,
                                                                   SND_CH_BGM));
    snd_mix(s, buf, 800);
    peaks[0] = block_peak(buf, 800);
    CHECK(peaks[0] > 4800 && peaks[0] < 5200,
          "0.5 gain halves the amplitude (%d)", peaks[0]);

    /* --- 4. 淡出的中途打断：反向淡入应把增益拉回去，不停止通道 --- */
    snd_fade(s, SND_CH_BGM, 0.0f, 500);
    snd_mix(s, buf, 800);
    snd_mix(s, buf, 800);
    snd_fade(s, SND_CH_BGM, 1.0f, 500);
    snd_mix(s, buf, 800);
    CHECK(snd_is_playing(s, SND_CH_BGM) == 1,
          "reversing a fade-out keeps the channel alive");
    CHECK(snd_channel_gain(s, SND_CH_BGM) > 0.0f, "gain is climbing again");

    free(pcm);
    snd_destroy(s);
    printf("test_snd_fade: %d passed, %d failed\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}