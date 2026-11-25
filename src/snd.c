/*
 * FVP 音频层：Ogg Vorbis（stb_vorbis）+ RIFF/WAVE 解码与混音。
 *
 * 解码策略：播放时整段解码到内存，停止时释放。BGM 一首约 30 MB，
 * 原型阶段够用；后续要改成流式，接口不变。
 */
#include "snd.h"

#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "third_party/stb_vorbis.c"

typedef struct {
    int16_t *pcm;      /* 交错立体声，引擎统一 2 声道 */
    int frames;        /* 帧数（每声道采样数） */
    int pos;           /* 当前帧 */
    int loop;
    float vol;        /* 目标音量（snd_set_vol / 播放时给的值） */
    float gain;       /* 当前生效增益（淡变时在 vol 之间过渡） */
    float gain_step;  /* 每个采样帧的增益增量；0 = 不淡变 */
    float gain_left;  /* 还剩多少帧到达目标 */
    int fade_to_zero; /* 淡出到 0 时结束后自动停止 */
    int active;
} Chan;

struct Snd {
    int rate;
    int channels;
    Chan ch[SND_CH_COUNT];
};

static int g_last_rate = -1;
static int g_last_ch = -1;
static int g_last_frames = -1;

Snd *snd_create(int rate, int channels)
{
    Snd *s = (Snd *)calloc(1, sizeof(Snd));
    if (!s) return NULL;
    s->rate = rate > 0 ? rate : 44100;
    s->channels = channels > 0 ? channels : 2;
    return s;
}

void snd_destroy(Snd *s)
{
    int i;
    if (!s) return;
    for (i = 0; i < SND_CH_COUNT; i++) free(s->ch[i].pcm);
    free(s);
}

/* 把任意声道数的交错 PCM 转成引擎统一的立体声 */
static int16_t *to_stereo(const int16_t *src, int frames, int src_ch, int *out_frames)
{
    int16_t *dst;
    int i;
    if (src_ch <= 0) return NULL;
    dst = (int16_t *)malloc((size_t)frames * 2 * sizeof(int16_t));
    if (!dst) return NULL;
    for (i = 0; i < frames; i++) {
        if (src_ch == 1) {
            dst[i * 2] = src[i];
            dst[i * 2 + 1] = src[i];
        } else {
            dst[i * 2] = src[(size_t)i * src_ch];
            dst[i * 2 + 1] = src[(size_t)i * src_ch + 1];
        }
    }
    *out_frames = frames;
    return dst;
}

static int chan_assign(Snd *s, int ch, int16_t *pcm, int frames, int loop, float vol)
{
    if (ch < 0 || ch >= SND_CH_COUNT) {
        free(pcm);
        return -1;
    }
    free(s->ch[ch].pcm);
    s->ch[ch].pcm = pcm;
    s->ch[ch].frames = frames;
    s->ch[ch].pos = 0;
    s->ch[ch].loop = loop;
    s->ch[ch].vol = vol;
    s->ch[ch].gain = vol;
    s->ch[ch].gain_step = 0.0f;
    s->ch[ch].gain_left = 0.0f;
    s->ch[ch].fade_to_zero = 0;
    s->ch[ch].active = 1;
    return 0;
}

/* 重采样参数的硬限制：
 *   - 采样率必须落在 1000..384000 Hz（真实素材是 22.05k/44.1k/48k）；
 *   - 输出时长不得超过 30 分钟。
 * 没有这两条限制时，一个 1 Hz 的 WAV（1000 帧）会被放大成 4800 万帧、
 * 单次 malloc 约 190 MB，足够让 32 位进程直接失败（fuzz 用例
 * out/fuzz/repro/wav_rate1_amplification.wav 就是这个）。超出限制时
 * 拒绝加载，调用方拿到失败返回值。 */
#define SND_RATE_MIN 1000
#define SND_RATE_MAX 384000
#define SND_MAX_SECONDS (30 * 60)

static int16_t *resample_to_rate(const int16_t *src, int frames, int src_rate, int dst_rate, int *out_frames)
{
    int16_t *dst;
    double step;
    double limit;
    double dn;
    int n, i;

    if (src_rate <= 0 || dst_rate <= 0 || frames <= 0) return NULL;
    if (src_rate < SND_RATE_MIN || src_rate > SND_RATE_MAX) return NULL;
    if (dst_rate < SND_RATE_MIN || dst_rate > SND_RATE_MAX) return NULL;
    limit = (double)dst_rate * (double)SND_MAX_SECONDS;
    if ((double)frames > limit) return NULL; /* 源本身已超过 30 分钟 */

    if (src_rate == dst_rate) {
        dst = (int16_t *)malloc((size_t)frames * 2 * sizeof(int16_t));
        if (!dst) return NULL;
        memcpy(dst, src, (size_t)frames * 2 * sizeof(int16_t));
        *out_frames = frames;
        return dst;
    }

    step = (double)src_rate / (double)dst_rate;
    dn = (double)frames / step;
    if (!(dn >= 1.0) || dn > limit) return NULL; /* 放大后超过 30 分钟 */
    n = (int)dn;
    dst = (int16_t *)malloc((size_t)n * 2 * sizeof(int16_t));
    if (!dst) return NULL;

    for (i = 0; i < n; i++) {
        double sp = i * step;
        int i0 = (int)sp;
        double frac = sp - i0;
        int i1 = i0 + 1 < frames ? i0 + 1 : frames - 1;
        int c;
        for (c = 0; c < 2; c++) {
            double a = src[(size_t)i0 * 2 + c];
            double b = src[(size_t)i1 * 2 + c];
            double v = a + (b - a) * frac;
            if (v > 32767.0) v = 32767.0;
            if (v < -32768.0) v = -32768.0;
            dst[(size_t)i * 2 + c] = (int16_t)v;
        }
    }
    *out_frames = n;
    return dst;
}

int snd_play_ogg(Snd *s, int ch, const uint8_t *mem, size_t len, int loop, float vol)
{
    int channels = 0, rate = 0, frames = 0;
    short *out = NULL;
    int16_t *stereo, *resampled;
    int n = 0, rn = 0;

    if (!s || !mem || len == 0) return -1;
    frames = stb_vorbis_decode_memory(mem, (int)len, &channels, &rate, &out);
    if (frames < 0 || !out) return -1;

    g_last_rate = rate;
    g_last_ch = channels;
    g_last_frames = frames;

    stereo = to_stereo(out, frames, channels, &n);
    free(out);
    if (!stereo) return -1;

    resampled = resample_to_rate(stereo, n, rate, s->rate, &rn);
    free(stereo);
    if (!resampled) return -1;

    return chan_assign(s, ch, resampled, rn, loop, vol);
}

/* 最小 WAV 解析：支持 PCM16、单/双声道 */
int snd_play_wav(Snd *s, int ch, const uint8_t *mem, size_t len, int loop, float vol)
{
    size_t p = 12;
    int channels = 0, bits = 0, rate = 0;
    const uint8_t *data = NULL;
    size_t data_len = 0;
    int frames;
    int16_t *stereo;

    if (!s || !mem || len < 44) return -1;
    if (memcmp(mem, "RIFF", 4) != 0 || memcmp(mem + 8, "WAVE", 4) != 0) return -1;

    while (p + 8 <= len) {
        const uint8_t *id = mem + p;
        uint32_t sz = (uint32_t)mem[p + 4] | ((uint32_t)mem[p + 5] << 8) |
                      ((uint32_t)mem[p + 6] << 16) | ((uint32_t)mem[p + 7] << 24);
        p += 8;
        if (p + sz > len) sz = (uint32_t)(len - p);
        if (memcmp(id, "fmt ", 4) == 0 && sz >= 16) {
            uint16_t fmt = (uint16_t)(mem[p] | (mem[p + 1] << 8));
            channels = mem[p + 2] | (mem[p + 3] << 8);
            rate = mem[p + 4] | (mem[p + 5] << 8) | (mem[p + 6] << 16) | (mem[p + 7] << 24);
            bits = mem[p + 14] | (mem[p + 15] << 8);
            if (fmt != 1) return -1; /* 只支持 PCM */
            if (bits != 16) return -1;
        } else if (memcmp(id, "data", 4) == 0) {
            data = mem + p;
            data_len = sz;
        }
        p += sz + (sz & 1);
    }
    if (!data || channels <= 0) return -1;
    /* int 帧数上限：data 长度来自 u32，2 GB 的 data 除以 2 会溢出 int */
    if (data_len / (size_t)(2 * channels) > (size_t)INT_MAX) return -1;

    frames = (int)(data_len / (size_t)(2 * channels));
    if (frames <= 0) return -1;

    g_last_rate = rate;
    g_last_ch = channels;
    g_last_frames = frames;

    stereo = to_stereo((const int16_t *)data, frames, channels, &frames);
    if (!stereo) return -1;

    {
        int rn = 0;
        int16_t *resampled = resample_to_rate(stereo, frames, rate, s->rate, &rn);
        free(stereo);
        if (!resampled) return -1;
        return chan_assign(s, ch, resampled, rn, loop, vol);
    }
}

int snd_play_file(Snd *s, int ch, const char *path, int loop, float vol)
{
    FILE *f;
    long n;
    uint8_t *buf;
    int rc;

    f = fopen(path, "rb");
    if (!f) return -1;
    fseek(f, 0, SEEK_END);
    n = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (n <= 0) { fclose(f); return -1; }
    buf = (uint8_t *)malloc((size_t)n);
    if (!buf) { fclose(f); return -1; }
    if (fread(buf, 1, (size_t)n, f) != (size_t)n) { free(buf); fclose(f); return -1; }
    fclose(f);

    if (n > 4 && memcmp(buf, "OggS", 4) == 0)
        rc = snd_play_ogg(s, ch, buf, (size_t)n, loop, vol);
    else
        rc = snd_play_wav(s, ch, buf, (size_t)n, loop, vol);
    free(buf);
    return rc;
}

void snd_stop(Snd *s, int ch)
{
    if (!s || ch < 0 || ch >= SND_CH_COUNT) return;
    free(s->ch[ch].pcm);
    s->ch[ch].pcm = NULL;
    s->ch[ch].active = 0;
}

void snd_stop_all(Snd *s)
{
    int i;
    for (i = 0; i < SND_CH_COUNT; i++) snd_stop(s, i);
}

void snd_set_vol(Snd *s, int ch, float vol)
{
    if (!s || ch < 0 || ch >= SND_CH_COUNT) return;
    s->ch[ch].vol = vol;
    s->ch[ch].gain = vol;
    s->ch[ch].gain_step = 0.0f;
    s->ch[ch].gain_left = 0.0f;
    s->ch[ch].fade_to_zero = 0;
}

void snd_fade(Snd *s, int ch, float target, int ms)
{
    float frames;

    if (!s || ch < 0 || ch >= SND_CH_COUNT) return;
    frames = (float)ms * (float)s->rate / 1000.0f;
    s->ch[ch].vol = target;
    if (frames <= 0.0f) {
        s->ch[ch].gain = target;
        s->ch[ch].gain_step = 0.0f;
        s->ch[ch].gain_left = 0.0f;
        s->ch[ch].fade_to_zero = (target <= 0.0f);
        return;
    }
    s->ch[ch].gain_step = (target - s->ch[ch].gain) / frames;
    s->ch[ch].gain_left = frames;
    s->ch[ch].fade_to_zero = (target <= 0.0f);
}

float snd_channel_gain(Snd *s, int ch)
{
    if (!s || ch < 0 || ch >= SND_CH_COUNT) return 0.0f;
    return s->ch[ch].gain;
}

int snd_play_pcm(Snd *s, int ch, const int16_t *stereo, int frames, int loop,
                 float vol)
{
    int16_t *copy;

    if (!s || !stereo || frames <= 0) return -1;
    copy = (int16_t *)malloc((size_t)frames * 2 * sizeof(int16_t));
    if (!copy) return -1;
    memcpy(copy, stereo, (size_t)frames * 2 * sizeof(int16_t));
    return chan_assign(s, ch, copy, frames, loop, vol);
}

int snd_is_playing(Snd *s, int ch)
{
    if (!s || ch < 0 || ch >= SND_CH_COUNT) return 0;
    return s->ch[ch].active;
}

int snd_channel_frames(Snd *s, int ch)
{
    if (!s || ch < 0 || ch >= SND_CH_COUNT) return 0;
    return s->ch[ch].frames;
}

int snd_channel_pos(Snd *s, int ch)
{
    if (!s || ch < 0 || ch >= SND_CH_COUNT) return 0;
    return s->ch[ch].pos;
}

void snd_mix(Snd *s, int16_t *out, int frames)
{
    int i, c;
    memset(out, 0, (size_t)frames * 2 * sizeof(int16_t));
    if (!s) return;

    for (c = 0; c < SND_CH_COUNT; c++) {
        Chan *ch = &s->ch[c];
        if (!ch->active || !ch->pcm) continue;
        for (i = 0; i < frames; i++) {
            int l, r;
            if (ch->pos >= ch->frames) {
                if (ch->loop) {
                    ch->pos = 0;
                } else {
                    ch->active = 0;
                    break;
                }
            }
            if (ch->gain_left > 0.0f) {
                ch->gain += ch->gain_step;
                ch->gain_left -= 1.0f;
                if (ch->gain_left <= 0.0f) {
                    ch->gain = ch->vol;
                    ch->gain_step = 0.0f;
                    if (ch->fade_to_zero) {
                        ch->active = 0;
                        break;
                    }
                }
            }
            l = (int)(ch->pcm[(size_t)ch->pos * 2] * ch->gain);
            r = (int)(ch->pcm[(size_t)ch->pos * 2 + 1] * ch->gain);
            l += out[i * 2];
            r += out[i * 2 + 1];
            if (l > 32767) l = 32767;
            if (l < -32768) l = -32768;
            if (r > 32767) r = 32767;
            if (r < -32768) r = -32768;
            out[i * 2] = (int16_t)l;
            out[i * 2 + 1] = (int16_t)r;
            ch->pos++;
        }
    }
}

int snd_last_info(int *rate, int *channels, int *frames)
{
    if (rate) *rate = g_last_rate;
    if (channels) *channels = g_last_ch;
    if (frames) *frames = g_last_frames;
    return g_last_rate > 0 ? 0 : -1;
}