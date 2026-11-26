/* FVP engine -- 开场 FAVORITE logo 序列的时间轴（见 openseq.h / openinganim.h）。 */
#include <ctype.h>
#include <stddef.h>
#include <string.h>

#include "motion.h"
#include "openseq.h"

typedef struct FvpOpenGameDef {
    const char *name;
    int logical_w; /* 脚本坐标系的宽（1024 / 800） */
    int logical_h; /* 脚本坐标系的高（640 / 600） */
    int op_view_x; /* p1..p3 的缩放/旋转轴心（HD 像素，脚本常量） */
    int op_view_y;
} FvpOpenGameDef;

static const FvpOpenGameDef GAMES[] = {
    /* hcb 0x580E2..0x5813E：helper(prim, 1024/2, 640/2) → OP=(960,540)；
     * p1..p3 直接 PrimSetOP(960,628)（hcb 0x58171/0x581A7/0x581DD）。 */
    {"iroseka", FVP_OPEN_LOGICAL_W_IROSEKA, FVP_OPEN_LOGICAL_H_IROSEKA,
     FVP_OPEN_OP_VIEW_X, FVP_OPEN_OP_VIEW_Y_IROSEKA},
    /* hcb 0x6F03A..0x6F086：helper(prim, 800/2, 600/2) → OP=(960,540)；
     * p1..p3 helper(prim, 400, 339) → OP=(960,610)（hcb 0x6F0BB…）。 */
    {"hoshimemo", FVP_OPEN_LOGICAL_W_HOSHIMEMO, FVP_OPEN_LOGICAL_H_HOSHIMEMO,
     FVP_OPEN_OP_VIEW_X, FVP_OPEN_OP_VIEW_Y_HOSHIMEMO},
};

typedef struct FvpOpenLayerDef {
    int slot;
    const char *name;
    int alpha_in_ms; /* 0 = 不淡入（一开始就是 255） */
    int scale_from;  /* per-mille */
    int scale_to;
    int scale_ms;
    int angle_from; /* 角度单位：1 = 0.05°（7200 = 360°） */
    int angle_to;
    int angle_ms;
    int move_from_x; /* 逻辑像素（脚本原值） */
    int move_from_y;
    int move_to_x;
    int move_to_y;
    int move_ms;
    int op_kind; /* 0 = 不做缩放/旋转（轴心无关）；1 = FAVO 中心；2 = VIEW 轴心 */
} FvpOpenLayerDef;

#define OP_KIND_NONE 0
#define OP_KIND_FAVO 1
#define OP_KIND_VIEW 2

/* 顺序 = 脚本 GraphLoad 顺序：250 / 252 / 254 / 255 / 256 / 257（hcb 0x580AA…）。 */
static const FvpOpenLayerDef LAYERS[] = {
    /* logo_bg：纯白 1920x1080，不淡入、不缩放；只做 3.5 s 淡出。 */
    {FVP_OPEN_SLOT_BG, "logo_bg", 0, 1000, 1000, 0, 0, 0, 0,
     0, 0, 0, 0, 0, OP_KIND_NONE},
    /* logo_favo：alpha 0→255 / 1200 ms，缩放 750→1000 / 1200 ms。 */
    {FVP_OPEN_SLOT_FAVO, "logo_favo", FVP_OPEN_ALPHA_MS_FAVO,
     FVP_OPEN_SCALE_FROM_FAVO, FVP_OPEN_SCALE_TO_FAVO, FVP_OPEN_SCALE_MS_FAVO,
     0, 0, 0, 0, 0, 0, 0, 0, OP_KIND_FAVO},
    /* logo_favo_view：alpha 0→255 / 1400，缩放 750→1000 / 1400，y +50→0。 */
    {FVP_OPEN_SLOT_VIEW, "logo_favo_view", FVP_OPEN_ALPHA_MS_VIEW,
     FVP_OPEN_SCALE_FROM_FAVO, FVP_OPEN_SCALE_TO_FAVO, FVP_OPEN_SCALE_MS_VIEW,
     0, 0, 0, 0, FVP_OPEN_MOVE_LOGICAL_Y_254, 0, 0, FVP_OPEN_MOVE_VIEW_MS,
     OP_KIND_FAVO},
    /* p1：从 (-200,+200) 收拢到 (0,0)，10×→1×，360°→0°。 */
    {FVP_OPEN_SLOT_P1, "logo_favo_view_p1", FVP_OPEN_ALPHA_MS_VIEW,
     FVP_OPEN_VIEW_SCALE_FROM, FVP_OPEN_VIEW_SCALE_TO, FVP_OPEN_VIEW_MS,
     FVP_OPEN_VIEW_ANGLE_FROM, FVP_OPEN_VIEW_ANGLE_TO, FVP_OPEN_VIEW_MS,
     -FVP_OPEN_MOVE_VIEW_LOGICAL, FVP_OPEN_MOVE_VIEW_LOGICAL_Y, 0, 0,
     FVP_OPEN_MOVE_VIEW_MS, OP_KIND_VIEW},
    /* p2：从 (+200,+200) 收拢到 (0,0)。 */
    {FVP_OPEN_SLOT_P2, "logo_favo_view_p2", FVP_OPEN_ALPHA_MS_VIEW,
     FVP_OPEN_VIEW_SCALE_FROM, FVP_OPEN_VIEW_SCALE_TO, FVP_OPEN_VIEW_MS,
     FVP_OPEN_VIEW_ANGLE_FROM, FVP_OPEN_VIEW_ANGLE_TO, FVP_OPEN_VIEW_MS,
     FVP_OPEN_MOVE_VIEW_LOGICAL, FVP_OPEN_MOVE_VIEW_LOGICAL_Y, 0, 0,
     FVP_OPEN_MOVE_VIEW_MS, OP_KIND_VIEW},
    /* p3：从 (0,-200) 收拢到 (0,0)。 */
    {FVP_OPEN_SLOT_P3, "logo_favo_view_p3", FVP_OPEN_ALPHA_MS_VIEW,
     FVP_OPEN_VIEW_SCALE_FROM, FVP_OPEN_VIEW_SCALE_TO, FVP_OPEN_VIEW_MS,
     FVP_OPEN_VIEW_ANGLE_FROM, FVP_OPEN_VIEW_ANGLE_TO, FVP_OPEN_VIEW_MS,
     0, -FVP_OPEN_MOVE_VIEW_LOGICAL_Y, 0, 0, FVP_OPEN_MOVE_VIEW_MS,
     OP_KIND_VIEW},
};

#define LAYER_COUNT ((int)(sizeof(LAYERS) / sizeof(LAYERS[0])))

static int clampi(int v, int lo, int hi)
{
    return v < lo ? lo : (v > hi ? hi : v);
}

/* 线性补间（MotionAlpha 的通道；原版 handler 0x43A8B0 没有缓动 mode）。 */
static int lerp_linear(int from, int to, int now, int dur)
{
    if (dur <= 0 || now >= dur) {
        return to;
    }
    if (now <= 0) {
        return from;
    }
    /* 原版是整数乘除、向零截断（docs/animation-baseline.md §2.3）。 */
    return from + (int)(((long long)(to - from) * now) / dur);
}

/* mode 1..5 的缓动补间（缩放/旋转/位移；脚本这几条都用 mode 3）。 */
static int lerp_ease(int from, int to, int now, int dur, int mode)
{
    double p;

    if (dur <= 0 || now >= dur) {
        return to;
    }
    if (now <= 0) {
        return from;
    }
    p = (double)now / (double)dur;
    return (int)((double)from + ((double)(to - from)) * fvp_motion_ease(mode, p));
}

/* 逻辑像素 → HD 像素：整数乘除（与原版 helper 的 float32 结果一致；见
 * openinganim.h 的说明）。C 的整数除法向零截断，正好对负数也成立。 */
static int logical_to_hd_x(const FvpOpenGameDef *g, int v)
{
    return (int)(((long long)v * FVP_OPEN_HD_W) / g->logical_w);
}

static int logical_to_hd_y(const FvpOpenGameDef *g, int v)
{
    return (int)(((long long)v * FVP_OPEN_HD_H) / g->logical_h);
}

/* 脚本的淡出是"从当前值降到 0"（MotionAlpha 的 from 实参是 true = 保持当前值），
 * 所以先算 1200 ms 那一刻的 alpha，再线性降。最后一组 100 ms 收尾从 3700 ms
 * 的当前值降到 0。 */
static int open_alpha(int t, const FvpOpenLayerDef *L)
{
    int base;
    int out_ms;

    if (t < FVP_OPEN_OUT_START_MS) {
        if (L->alpha_in_ms <= 0) {
            return 255;
        }
        return lerp_linear(0, 255, t, L->alpha_in_ms);
    }
    if (L->alpha_in_ms <= 0) {
        base = 255;
    } else {
        base = lerp_linear(0, 255, clampi(FVP_OPEN_OUT_START_MS, 0,
                                          L->alpha_in_ms),
                           L->alpha_in_ms);
    }
    out_ms = (L->slot == FVP_OPEN_SLOT_BG) ? FVP_OPEN_BG_OUT_MS
                                           : FVP_OPEN_LOGO_OUT_MS;
    if (t >= FVP_OPEN_CUT_START_MS) {
        /* 100 ms 收尾：从 3700 ms 的当前值（白底还没降完）降到 0。 */
        int cut_base = 0;

        if (FVP_OPEN_CUT_START_MS < FVP_OPEN_OUT_START_MS + out_ms) {
            cut_base = lerp_linear(base, 0,
                                   FVP_OPEN_CUT_START_MS -
                                       FVP_OPEN_OUT_START_MS,
                                   out_ms);
        }
        return lerp_linear(cut_base, 0, t - FVP_OPEN_CUT_START_MS,
                           FVP_OPEN_CUT_MS);
    }
    if (t < FVP_OPEN_OUT_START_MS + out_ms) {
        return lerp_linear(base, 0, t - FVP_OPEN_OUT_START_MS, out_ms);
    }
    return 0;
}

/* 名字既可能是 per-title profile 的短名（"hoshimemo"），也可能是 --game 给的
 * 目录路径（"C:\\...\\Hoshimemo_HD"）：按不区分大小写的子串匹配，两种都能认。 */
static int name_has_token(const char *name, const char *token)
{
    size_t n = strlen(token);
    size_t i;

    if (name == NULL || n == 0) {
        return 0;
    }
    for (i = 0; name[i] != '\0'; ++i) {
        size_t k = 0;

        while (k < n && name[i + k] != '\0' &&
               tolower((unsigned char)name[i + k]) ==
                   tolower((unsigned char)token[k])) {
            ++k;
        }
        if (k == n) {
            return 1;
        }
    }
    return 0;
}

FvpOpenGame fvp_openseq_game_from_name(const char *name)
{
    if (name_has_token(name, GAMES[1].name)) {
        return FVP_OPEN_GAME_HOSHIMEMO;
    }
    return FVP_OPEN_GAME_IROSEKA;
}

const char *fvp_openseq_game_name(FvpOpenGame game)
{
    if (game < 0 || game >= (int)(sizeof(GAMES) / sizeof(GAMES[0]))) {
        return GAMES[0].name;
    }
    return GAMES[game].name;
}

void fvp_openseq_init(FvpOpenSeq *s, FvpOpenGame game)
{
    if (s == NULL) {
        return;
    }
    s->elapsed_ms = 0;
    s->game = (game == FVP_OPEN_GAME_HOSHIMEMO) ? FVP_OPEN_GAME_HOSHIMEMO
                                                : FVP_OPEN_GAME_IROSEKA;
}

void fvp_openseq_advance(FvpOpenSeq *s, int ms)
{
    if (s == NULL) {
        return;
    }
    if (ms < 0) {
        ms = 0;
    }
    s->elapsed_ms = clampi(s->elapsed_ms + ms, 0, FVP_OPEN_TOTAL_MS);
}

void fvp_openseq_set_elapsed(FvpOpenSeq *s, int ms)
{
    if (s == NULL) {
        return;
    }
    s->elapsed_ms = clampi(ms, 0, FVP_OPEN_TOTAL_MS);
}

int fvp_openseq_elapsed(const FvpOpenSeq *s)
{
    return s != NULL ? s->elapsed_ms : 0;
}

FvpOpenPhase fvp_openseq_phase(const FvpOpenSeq *s)
{
    int t = fvp_openseq_elapsed(s);

    if (t < FVP_OPEN_OUT_START_MS) {
        return FVP_OPEN_IN;
    }
    if (t < FVP_OPEN_CUT_START_MS) {
        return FVP_OPEN_OUT;
    }
    if (t < FVP_OPEN_TOTAL_MS) {
        return FVP_OPEN_CUT;
    }
    return FVP_OPEN_DONE;
}

int fvp_openseq_done(const FvpOpenSeq *s)
{
    return fvp_openseq_elapsed(s) >= FVP_OPEN_TOTAL_MS;
}

int fvp_openseq_layer_count(void)
{
    return LAYER_COUNT;
}

const char *fvp_openseq_layer_name(int index)
{
    if (index < 0 || index >= LAYER_COUNT) {
        return NULL;
    }
    return LAYERS[index].name;
}

int fvp_openseq_layer_slot(int index)
{
    if (index < 0 || index >= LAYER_COUNT) {
        return -1;
    }
    return LAYERS[index].slot;
}

int fvp_openseq_layer_index(int slot)
{
    int i;

    for (i = 0; i < LAYER_COUNT; ++i) {
        if (LAYERS[i].slot == slot) {
            return i;
        }
    }
    return -1;
}

int fvp_openseq_layer_state(const FvpOpenSeq *s, int index,
                            FvpOpenLayerState *out)
{
    const FvpOpenLayerDef *L;
    const FvpOpenGameDef *g;
    int t;
    int game;

    if (s == NULL || out == NULL || index < 0 || index >= LAYER_COUNT) {
        return 0;
    }
    L = &LAYERS[index];
    game = (s->game >= 0 && s->game < (int)(sizeof(GAMES) / sizeof(GAMES[0])))
               ? (int)s->game
               : 0;
    g = &GAMES[game];
    t = clampi(s->elapsed_ms, 0, FVP_OPEN_TOTAL_MS);

    out->alpha = open_alpha(t, L);
    out->scale_x1000 = lerp_ease(L->scale_from, L->scale_to, t, L->scale_ms,
                                 FVP_OPEN_EASE_MODE);
    out->scale_y1000 = out->scale_x1000;
    out->angle_units = lerp_ease(L->angle_from, L->angle_to, t, L->angle_ms,
                               FVP_OPEN_EASE_MODE);
    out->dx = lerp_ease(logical_to_hd_x(g, L->move_from_x),
                        logical_to_hd_x(g, L->move_to_x), t, L->move_ms,
                        FVP_OPEN_EASE_MODE);
    out->dy = lerp_ease(logical_to_hd_y(g, L->move_from_y),
                        logical_to_hd_y(g, L->move_to_y), t, L->move_ms,
                        FVP_OPEN_EASE_MODE);
    if (L->op_kind == OP_KIND_FAVO) {
        out->op_x = FVP_OPEN_OP_FAVO_X;
        out->op_y = FVP_OPEN_OP_FAVO_Y;
    } else if (L->op_kind == OP_KIND_VIEW) {
        out->op_x = g->op_view_x;
        out->op_y = g->op_view_y;
    } else {
        out->op_x = 0;
        out->op_y = 0;
    }
    return 1;
}