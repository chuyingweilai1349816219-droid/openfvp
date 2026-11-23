/*
 * snd 层自检：解码一个文件，把混音结果写成 WAV，并打印参数。
 * 用法: test_snd <输入(ogg/wav)> <输出.wav> [引擎采样率]
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <math.h>

#include "../src/snd.h"

static void wr32(FILE *f, uint32_t v)
{
    uint8_t b[4] = { (uint8_t)v, (uint8_t)(v >> 8), (uint8_t)(v >> 16), (uint8_t)(v >> 24) };
    fwrite(b, 1, 4, f);
}
static void wr16(FILE *f, uint16_t v)
{
    uint8_t b[2] = { (uint8_t)v, (uint8_t)(v >> 8) };
    fwrite(b, 1, 2, f);
}

int main(int argc, char **argv)
{
    Snd *s;
    int src_rate = 0, src_ch = 0, src_frames = 0;
    int rate = 44100, frames, i, peak = 0;
    int16_t *buf;
    double energy = 0.0;
    FILE *f;

    if (argc < 3) {
        fprintf(stderr, "usage: %s <input.ogg|input.wav> <out.wav> [engine_rate]\n", argv[0]);
        return 2;
    }
    if (argc > 3) rate = atoi(argv[3]);

    s = snd_create(rate, 2);
    if (!s) return 1;

    if (snd_play_file(s, SND_CH_BGM, argv[1], 0, 1.0f) != 0) {
        fprintf(stderr, "decode failed: %s\n", argv[1]);
        return 1;
    }
    snd_last_info(&src_rate, &src_ch, &src_frames);
    frames = snd_channel_frames(s, SND_CH_BGM);
    printf("source : rate=%d channels=%d frames=%d (%.2fs)\n",
           src_rate, src_ch, src_frames, src_rate > 0 ? (double)src_frames / src_rate : 0.0);
    printf("engine : rate=%d frames=%d (%.3fs)\n", rate, frames, (double)frames / rate);
    if (frames <= 0) return 1;

    buf = (int16_t *)malloc((size_t)frames * 2 * sizeof(int16_t));
    if (!buf) return 1;
    snd_mix(s, buf, frames);

    for (i = 0; i < frames * 2; i++) {
        int v = buf[i];
        energy += (double)v * v;
        if (abs(v) > peak) peak = abs(v);
    }
    printf("peak=%d rms=%.1f silent=%s\n", peak,
           sqrt(energy / (double)(frames * 2)), peak == 0 ? "YES" : "no");

    f = fopen(argv[2], "wb");
    if (!f) return 1;
    fwrite("RIFF", 1, 4, f);
    wr32(f, (uint32_t)(36 + (uint32_t)frames * 4));
    fwrite("WAVEfmt ", 1, 8, f);
    wr32(f, 16);
    wr16(f, 1);
    wr16(f, 2);
    wr32(f, (uint32_t)rate);
    wr32(f, (uint32_t)(rate * 4));
    wr16(f, 4);
    wr16(f, 16);
    fwrite("data", 1, 4, f);
    wr32(f, (uint32_t)((uint32_t)frames * 4));
    fwrite(buf, 2, (size_t)frames * 2, f);
    fclose(f);

    printf("wrote %s\n", argv[2]);
    free(buf);
    snd_destroy(s);
    return 0;
}