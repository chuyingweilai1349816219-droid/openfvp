/* FVP engine -- 标题背景的动画状态（见 titlebg.h / ref/title-bg-motion.json）。
 *
 * 数值来源（全部是脚本事实，门禁 tests/verify_title_bg_motion.py 会重提取比对）：
 *   五彩斑斓 hcb 0x59143..0x591D3：
 *     MotionMoveS2(258, 1300, 900, 1300, 900, local7, 3)  + MotionMoveR(258, 150, 0, ...)
 *     MotionMoveS2(259, 1600, 800, ...)                   + MotionMoveR(259, 250, 0, ...)
 *     MotionMoveS2(260, 2500, 800, ...)                   + MotionMoveR(260, 550, 0, ...)
 *     MotionMoveS2(261, 4000, 800, ...)                   + MotionMoveR(261, 1250, 0, ...)
 *   local7 = 25000 ms（正常分支）；mode 3 = 缓出 1-(1-p)^2。
 *   星空 hcb 0x6FD87：MotionMoveS2(258, 2000, 1000, 2000, 1000, 40000, 3)。
 */
#include <stddef.h>
#include <string.h>

#include "motion.h"
#include "titlebg.h"

#define HOSHIMEMO_BG_SCALE_FROM 2000
#define HOSHIMEMO_BG_SCALE_TO 1000
#define HOSHIMEMO_BG_MS 40000
#define HOSHIMEMO_BG_EASE_MODE 3

/* 五彩斑斓四层：绘制顺序与静态路径一致（A 底 → C → D → B 顶）。 */
static const FvpTitleBgLayerDef IROSEKA_LAYERS[FVP_TITLEBG_MAX_LAYERS] = {
    {"title_bg1_A", 0, 0, 0, 1200, 675, 1, 1300, 900, 150, 0},
    {"title_bg1_C", 2, 0, 0, 1200, 675, 0, 2500, 800, 550, 0},
    {"title_bg1_D", 3, 0, 111, 1200, 675, 0, 4000, 800, 1250, 0},
    {"title_bg1_B", 1, 518, 0, 1200, 675, 0, 1600, 800, 250, 0},
};

#define IROSEKA_BG_MS 25000

static int clampi(int v, int lo, int hi)
{
    return v < lo ? lo : (v > hi ? hi : v);
}

FvpTitleBgGame fvp_titlebg_game_from_name(const char *name)
{
    if (name != NULL && strcmp(name, "hoshimemo") == 0) {
        return FVP_TITLEBG_GAME_HOSHIMEMO;
    }
    return FVP_TITLEBG_GAME_IROSEKA;
}

int fvp_titlebg_total_ms(FvpTitleBgGame game)
{
    if (game == FVP_TITLEBG_GAME_HOSHIMEMO) {
        return HOSHIMEMO_BG_MS;
    }
    return IROSEKA_BG_MS;
}

int fvp_titlebg_uses_layers(FvpTitleBgGame game)
{
    return game == FVP_TITLEBG_GAME_IROSEKA;
}

int fvp_titlebg_layer_count(FvpTitleBgGame game)
{
    return game == FVP_TITLEBG_GAME_IROSEKA ? FVP_TITLEBG_MAX_LAYERS : 0;
}

const FvpTitleBgLayerDef *fvp_titlebg_layer_def(FvpTitleBgGame game, int idx)
{
    if (game != FVP_TITLEBG_GAME_IROSEKA || idx < 0 ||
        idx >= FVP_TITLEBG_MAX_LAYERS) {
        return NULL;
    }
    return &IROSEKA_LAYERS[idx];
}

/* 0..1 的缓动进度 */
static double ease_progress(int elapsed_ms, int dur_ms)
{
    double p;

    if (dur_ms <= 0 || elapsed_ms >= dur_ms) {
        return 1.0;
    }
    if (elapsed_ms <= 0) {
        return 0.0;
    }
    p = (double)elapsed_ms / (double)dur_ms;
    return fvp_motion_ease(HOSHIMEMO_BG_EASE_MODE, p);
}

int fvp_titlebg_layer_state(FvpTitleBgGame game, int idx, int elapsed_ms,
                            FvpTitleBgLayerState *out)
{
    const FvpTitleBgLayerDef *def;

    if (out == NULL) {
        return 0;
    }
    out->scale_x1000 = 1000;
    out->scale_y1000 = 1000;
    out->angle_units = 0;
    def = fvp_titlebg_layer_def(game, idx);
    if (def == NULL) {
        return 0;
    }
    {
        double e = ease_progress(elapsed_ms, IROSEKA_BG_MS);
        int s = (int)((double)def->scale_from +
                      ((double)def->scale_to - (double)def->scale_from) * e);
        int a = (int)((double)def->angle_from +
                      ((double)def->angle_to - (double)def->angle_from) * e);

        out->scale_x1000 = s;
        out->scale_y1000 = s;
        out->angle_units = a;
    }
    return 1;
}

void fvp_titlebg_state(FvpTitleBgGame game, int elapsed_ms,
                       FvpTitleBgState *out)
{
    if (out == NULL) {
        return;
    }
    out->animated = 0;
    out->scale_x1000 = 1000;
    out->scale_y1000 = 1000;
    out->angle_units = 0;
    out->done = 1;
    if (game != FVP_TITLEBG_GAME_HOSHIMEMO) {
        /* 五彩斑斓走逐层接口（fvp_titlebg_layer_state）。 */
        return;
    }
    out->animated = 1;
    out->done = 0;
    if (elapsed_ms >= HOSHIMEMO_BG_MS) {
        out->scale_x1000 = HOSHIMEMO_BG_SCALE_TO;
        out->scale_y1000 = HOSHIMEMO_BG_SCALE_TO;
        out->done = 1;
        return;
    }
    {
        double e = ease_progress(elapsed_ms, HOSHIMEMO_BG_MS);
        int s = (int)((double)HOSHIMEMO_BG_SCALE_FROM +
                      ((double)HOSHIMEMO_BG_SCALE_TO -
                       (double)HOSHIMEMO_BG_SCALE_FROM) * e);

        out->scale_x1000 = clampi(s, HOSHIMEMO_BG_SCALE_TO,
                                  HOSHIMEMO_BG_SCALE_FROM);
        out->scale_y1000 = out->scale_x1000;
    }
}