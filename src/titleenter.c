/* FVP engine -- 标题进场的逐元素时间轴（见 titleenter.h / titleanim.h）。 */
#include <stddef.h>

#include "titleenter.h"

static int clampi(int v, int lo, int hi)
{
    return v < lo ? lo : (v > hi ? hi : v);
}

/* 线性补间（MotionAlpha 的 handler 0x43A8B0 没有缓动 mode，是整数线性）。 */
static int lerp_linear(int from, int to, int now, int dur)
{
    if (dur <= 0 || now >= dur) {
        return to;
    }
    if (now <= 0) {
        return from;
    }
    return from + (int)(((long long)(to - from) * now) / dur);
}

/* 部件 → (起始 ms, 时长 ms)；start/dur 来自 ref/title-enter.json。 */
static void part_window(int part, int *start_ms, int *dur_ms)
{
    if (part == FVP_TITLEENTER_PART_LOGO) {
        *start_ms = FVP_TITLEENTER_LOGO_START_MS;
        *dur_ms = FVP_TITLEENTER_LOGO_MS;
        return;
    }
    if (part == FVP_TITLEENTER_PART_COPY) {
        *start_ms = FVP_TITLEENTER_COPY_START_MS;
        *dur_ms = FVP_TITLEENTER_COPY_MS;
        return;
    }
    {
        int i = part - FVP_TITLEENTER_PART_MENU0;

        *start_ms = FVP_TITLEENTER_MENU_START_MS +
                    i * FVP_TITLEENTER_MENU_STEP_MS;
        *dur_ms = (i == 0) ? FVP_TITLEENTER_MENU_FIRST_MS
                           : FVP_TITLEENTER_MENU_MS;
    }
}

void fvp_titleenter_init(FvpTitleEnter *s)
{
    if (s != NULL) {
        s->elapsed_ms = 0;
    }
}

void fvp_titleenter_set_elapsed(FvpTitleEnter *s, int ms)
{
    if (s != NULL) {
        s->elapsed_ms = clampi(ms, 0, FVP_TITLEENTER_TOTAL_MS);
    }
}

int fvp_titleenter_elapsed(const FvpTitleEnter *s)
{
    return s != NULL ? s->elapsed_ms : 0;
}

int fvp_titleenter_done(const FvpTitleEnter *s)
{
    return fvp_titleenter_elapsed(s) >= FVP_TITLEENTER_TOTAL_MS;
}

int fvp_titleenter_alpha(const FvpTitleEnter *s, int part)
{
    int start_ms;
    int dur_ms;

    if (s == NULL || part < 0 || part >= FVP_TITLEENTER_PART_COUNT) {
        return 0;
    }
    part_window(part, &start_ms, &dur_ms);
    if (s->elapsed_ms <= start_ms) {
        return 0;
    }
    return clampi(lerp_linear(0, 255, s->elapsed_ms - start_ms, dur_ms), 0,
                  255);
}

void fvp_titleenter_offset(const FvpTitleEnter *s, int part, int *dx, int *dy)
{
    int start_ms;
    int dur_ms;

    if (dx != NULL) {
        *dx = 0;
    }
    if (dy != NULL) {
        *dy = 0;
    }
    if (s == NULL || part < 0 || part >= FVP_TITLEENTER_PART_COUNT) {
        return;
    }
    part_window(part, &start_ms, &dur_ms);
    if (part == FVP_TITLEENTER_PART_COPY) {
        if (dx != NULL) {
            *dx = lerp_linear(FVP_TITLEENTER_COPY_DX, 0,
                              s->elapsed_ms - start_ms, dur_ms);
        }
        return;
    }
    if (part >= FVP_TITLEENTER_PART_MENU0) {
        if (dy != NULL) {
            *dy = lerp_linear(FVP_TITLEENTER_MENU_DY, 0,
                              s->elapsed_ms - start_ms, dur_ms);
        }
    }
}