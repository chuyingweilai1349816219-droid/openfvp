/* FVP engine -- 开场序列时间轴（见 openseq.h）。 */
#include <stddef.h>

#include "openseq.h"

static int clamp255(int v)
{
    return v < 0 ? 0 : (v > 255 ? 255 : v);
}

/* 平滑缓动（近似值，原版缓动模式未实测）：smoothstep 3t^2-2t^3。 */
static int ease(int elapsed, int dur, int from, int to)
{
    long long num;
    long long den;
    long long t;

    if (dur <= 0) {
        return to;
    }
    if (elapsed <= 0) {
        return from;
    }
    if (elapsed >= dur) {
        return to;
    }
    t = elapsed;
    den = (long long)dur * (long long)dur;
    num = t * t * (3 * (long long)dur - 2 * t);
    return from + (int)(((long long)(to - from) * num) / den);
}

void fvp_openseq_init(FvpOpenSeq *s)
{
    if (s != NULL) {
        s->elapsed_ms = 0;
    }
}

void fvp_openseq_advance(FvpOpenSeq *s, int ms)
{
    if (s == NULL) {
        return;
    }
    if (ms < 0) {
        ms = 0;
    }
    s->elapsed_ms += ms;
    if (s->elapsed_ms > FVP_OPEN_TOTAL_MS) {
        s->elapsed_ms = FVP_OPEN_TOTAL_MS;
    }
}

int fvp_openseq_elapsed(const FvpOpenSeq *s)
{
    return s != NULL ? s->elapsed_ms : 0;
}

FvpOpenPhase fvp_openseq_phase(const FvpOpenSeq *s)
{
    int t = fvp_openseq_elapsed(s);

    if (t < FVP_OPEN_WHITE_MS) {
        return FVP_OPEN_WHITE_HOLD;
    }
    t -= FVP_OPEN_WHITE_MS;
    if (t < FVP_OPEN_IN_MS) {
        return FVP_OPEN_LOGO_IN;
    }
    t -= FVP_OPEN_IN_MS;
    if (t < FVP_OPEN_HOLD_MS) {
        return FVP_OPEN_HOLD;
    }
    t -= FVP_OPEN_HOLD_MS;
    if (t < FVP_OPEN_OUT_MS) {
        return FVP_OPEN_LOGO_OUT;
    }
    t -= FVP_OPEN_OUT_MS;
    if (t < FVP_OPEN_FADE_MS) {
        return FVP_OPEN_TITLE_FADE;
    }
    return FVP_OPEN_DONE;
}

int fvp_openseq_logo_alpha(const FvpOpenSeq *s)
{
    int t = fvp_openseq_elapsed(s);

    if (t < FVP_OPEN_WHITE_MS) {
        return 0;
    }
    t -= FVP_OPEN_WHITE_MS;
    if (t < FVP_OPEN_IN_MS) {
        return clamp255(ease(t, FVP_OPEN_IN_MS, 0, 255));
    }
    t -= FVP_OPEN_IN_MS;
    if (t < FVP_OPEN_HOLD_MS) {
        return 255;
    }
    t -= FVP_OPEN_HOLD_MS;
    if (t < FVP_OPEN_OUT_MS) {
        return clamp255(ease(t, FVP_OPEN_OUT_MS, 255, 0));
    }
    return 0;
}

int fvp_openseq_veil_alpha(const FvpOpenSeq *s)
{
    int t = fvp_openseq_elapsed(s);
    int begin = FVP_OPEN_WHITE_MS + FVP_OPEN_IN_MS + FVP_OPEN_HOLD_MS +
                FVP_OPEN_OUT_MS;

    if (t < begin) {
        return 0;
    }
    t -= begin;
    if (t >= FVP_OPEN_FADE_MS) {
        return 0;
    }
    return clamp255(ease(t, FVP_OPEN_FADE_MS, 255, 0));
}

int fvp_openseq_done(const FvpOpenSeq *s)
{
    return fvp_openseq_elapsed(s) >= FVP_OPEN_TOTAL_MS;
}