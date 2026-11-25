/* FVP engine -- 标题进场的逐元素时间轴（见 titleenter.h / titleanim.h）。 */
#include <stddef.h>
#include <string.h>

#include "titleenter.h"

/* 每部作品的进门时刻/位移（时间轴其余部分——时长与步进——两作相同）。
 * 出处：ref/title-enter.json（五彩斑斓 0x5933E..0x59650，星空 0x6FEDE..0x701E2）。 */
typedef struct FvpTitleEnterGameDef {
    const char *name;
    int logo_start_ms;
    int menu_start_ms;
    int copy_start_ms;
    int total_ms;
    int menu_dy;  /* HD px */
    int copy_dx;  /* HD px */
} FvpTitleEnterGameDef;

static const FvpTitleEnterGameDef GAMES[] = {
    /* 五彩斑斓：等 1000 → 装饰层 264 → 再等 1000 → logo。菜单初位移 14 逻辑像素
     * ×1.6875 = 23 px；版权行 16×1.875 = 30 px。 */
    {"iroseka", FVP_TITLEENTER_IROSEKA_LOGO_START_MS,
     FVP_TITLEENTER_IROSEKA_MENU_START_MS,
     FVP_TITLEENTER_IROSEKA_COPY_START_MS,
     FVP_TITLEENTER_IROSEKA_TOTAL_MS, FVP_TITLEENTER_IROSEKA_MENU_DY,
     FVP_TITLEENTER_IROSEKA_COPY_DX},
    /* 星空：只等一次 1000 ms（装饰层 259/260 与 logo 同时），菜单 1400 起。
     * 初位移 28 逻辑像素 ×1.8 = 50 px；版权行 32×2.4 = 76 px。 */
    {"hoshimemo", FVP_TITLEENTER_HOSHIMEMO_LOGO_START_MS,
     FVP_TITLEENTER_HOSHIMEMO_MENU_START_MS,
     FVP_TITLEENTER_HOSHIMEMO_COPY_START_MS,
     FVP_TITLEENTER_HOSHIMEMO_TOTAL_MS, FVP_TITLEENTER_HOSHIMEMO_MENU_DY,
     FVP_TITLEENTER_HOSHIMEMO_COPY_DX},
};

static int clampi(int v, int lo, int hi)
{
    return v < lo ? lo : (v > hi ? hi : v);
}

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

static const FvpTitleEnterGameDef *game_of(int game)
{
    if (game < 0 || game >= (int)(sizeof(GAMES) / sizeof(GAMES[0]))) {
        return &GAMES[0];
    }
    return &GAMES[game];
}

/* 部件 → (起始 ms, 时长 ms)。 */
static void part_window(const FvpTitleEnterGameDef *g, int part,
                        int *start_ms, int *dur_ms)
{
    if (part == FVP_TITLEENTER_PART_LOGO) {
        *start_ms = g->logo_start_ms;
        *dur_ms = FVP_TITLEENTER_LOGO_MS;
        return;
    }
    if (part == FVP_TITLEENTER_PART_COPY) {
        *start_ms = g->copy_start_ms;
        *dur_ms = FVP_TITLEENTER_COPY_MS;
        return;
    }
    {
        int i = part - FVP_TITLEENTER_PART_MENU0;

        *start_ms = g->menu_start_ms + i * FVP_TITLEENTER_MENU_STEP_MS;
        *dur_ms = (i == 0) ? FVP_TITLEENTER_MENU_FIRST_MS
                           : FVP_TITLEENTER_MENU_MS;
    }
}

FvpTitleEnterGame fvp_titleenter_game_from_name(const char *name)
{
    if (name != NULL && strcmp(name, GAMES[1].name) == 0) {
        return FVP_TITLEENTER_GAME_HOSHIMEMO;
    }
    return FVP_TITLEENTER_GAME_IROSEKA;
}

const char *fvp_titleenter_game_name(FvpTitleEnterGame game)
{
    return game_of((int)game)->name;
}

void fvp_titleenter_init(FvpTitleEnter *s, FvpTitleEnterGame game)
{
    if (s == NULL) {
        return;
    }
    s->elapsed_ms = 0;
    s->game = (game == FVP_TITLEENTER_GAME_HOSHIMEMO)
                  ? FVP_TITLEENTER_GAME_HOSHIMEMO
                  : FVP_TITLEENTER_GAME_IROSEKA;
}

void fvp_titleenter_set_elapsed(FvpTitleEnter *s, int ms)
{
    if (s == NULL) {
        return;
    }
    s->elapsed_ms = clampi(ms, 0, game_of((int)s->game)->total_ms);
}

int fvp_titleenter_elapsed(const FvpTitleEnter *s)
{
    return s != NULL ? s->elapsed_ms : 0;
}

int fvp_titleenter_done(const FvpTitleEnter *s)
{
    if (s == NULL) {
        return 0;
    }
    return s->elapsed_ms >= game_of((int)s->game)->total_ms;
}

int fvp_titleenter_total_ms(const FvpTitleEnter *s)
{
    return game_of(s != NULL ? (int)s->game : 0)->total_ms;
}

int fvp_titleenter_alpha(const FvpTitleEnter *s, int part)
{
    const FvpTitleEnterGameDef *g;
    int start_ms;
    int dur_ms;

    if (s == NULL || part < 0 || part >= FVP_TITLEENTER_PART_COUNT) {
        return 0;
    }
    g = game_of((int)s->game);
    part_window(g, part, &start_ms, &dur_ms);
    if (s->elapsed_ms <= start_ms) {
        return 0;
    }
    return clampi(lerp_linear(0, 255, s->elapsed_ms - start_ms, dur_ms), 0,
                  255);
}

void fvp_titleenter_offset(const FvpTitleEnter *s, int part, int *dx, int *dy)
{
    const FvpTitleEnterGameDef *g;
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
    g = game_of((int)s->game);
    part_window(g, part, &start_ms, &dur_ms);
    if (part == FVP_TITLEENTER_PART_COPY) {
        if (dx != NULL) {
            *dx = lerp_linear(g->copy_dx, 0, s->elapsed_ms - start_ms, dur_ms);
        }
        return;
    }
    if (part >= FVP_TITLEENTER_PART_MENU0) {
        if (dy != NULL) {
            *dy = lerp_linear(g->menu_dy, 0, s->elapsed_ms - start_ms, dur_ms);
        }
    }
}