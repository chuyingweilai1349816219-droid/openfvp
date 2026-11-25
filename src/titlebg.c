/* FVP engine -- 标题背景的动画状态（见 titlebg.h / ref/title-bg-motion.json）。 */
#include <stddef.h>
#include <string.h>

#include "motion.h"
#include "titlebg.h"

/* 星空：MotionMoveS2(258, 2000, 1000, 2000, 1000, 40000, 3)（hcb 0x6FD73..0x6FD87，
 * 时长是 0x6FD67 的 pushint32 40000 字面量）。背景 1920x1080、无 PrimSetOP，
 * 所以轴心是默认 (0,0)（左上锚），也不旋转。 */
#define HOSHIMEMO_BG_SCALE_FROM 2000
#define HOSHIMEMO_BG_SCALE_TO 1000
#define HOSHIMEMO_BG_MS 40000
#define HOSHIMEMO_BG_EASE_MODE 3

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
    return game == FVP_TITLEBG_GAME_HOSHIMEMO ? HOSHIMEMO_BG_MS : 0;
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
        /* 五彩斑斓：4 层超尺寸图 + OP 轴心 + 旋转，尚未实现（见 titlebg.h）。 */
        return;
    }
    out->animated = 1;
    out->done = 0;
    if (elapsed_ms <= 0) {
        out->scale_x1000 = HOSHIMEMO_BG_SCALE_FROM;
        out->scale_y1000 = HOSHIMEMO_BG_SCALE_FROM;
        return;
    }
    if (elapsed_ms >= HOSHIMEMO_BG_MS) {
        out->scale_x1000 = HOSHIMEMO_BG_SCALE_TO;
        out->scale_y1000 = HOSHIMEMO_BG_SCALE_TO;
        out->done = 1;
        return;
    }
    {
        double p = (double)elapsed_ms / (double)HOSHIMEMO_BG_MS;
        double e = fvp_motion_ease(HOSHIMEMO_BG_EASE_MODE, p);
        int s = (int)((double)HOSHIMEMO_BG_SCALE_FROM +
                      ((double)HOSHIMEMO_BG_SCALE_TO -
                       (double)HOSHIMEMO_BG_SCALE_FROM) * e);

        out->scale_x1000 = clampi(s, HOSHIMEMO_BG_SCALE_TO,
                                  HOSHIMEMO_BG_SCALE_FROM);
        out->scale_y1000 = out->scale_x1000;
    }
}