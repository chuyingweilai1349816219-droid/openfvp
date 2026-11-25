/* 标题背景动画（titlebg）单测。
 *
 *   bin/test_titlebg.exe
 *
 * 参数来自脚本（`ref/title-bg-motion.json`，星空 hcb 0x6FD73..0x6FD87）：
 *   星空：256 槽 258 的 1920x1080 背景，MotionMoveS2(2000→1000 per-mille,
 *   40000 ms, mode 3)，轴心默认 (0,0)（左上锚），不旋转。
 *   五彩斑斓：4 层超尺寸图 + OP 轴心 + 旋转，尚未实现 → animated=0、scale 1000。
 *
 * 期望值手算：mode 3 = 缓出 1-(1-p)^2
 *   t=10000  p=1/4 → e=0.4375 → 2000-437.5 = 1562.5 → 截断 1562
 *   t=20000  p=1/2 → e=0.75   → 1250
 *   t=30000  p=3/4 → e=0.9375 → 1062.5 → 截断 1062
 */
#include <stdio.h>

#include "../src/titlebg.h"

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

static FvpTitleBgState at(FvpTitleBgGame game, int ms)
{
    FvpTitleBgState st;

    st.animated = -1;
    st.scale_x1000 = -1;
    st.scale_y1000 = -1;
    st.angle_x10 = -1;
    st.done = -1;
    fvp_titlebg_state(game, ms, &st);
    return st;
}

int main(void)
{
    int i;

    /* ---- 1. 游戏名映射与总时长 ---- */
    CHECK(fvp_titlebg_game_from_name("hoshimemo") ==
              FVP_TITLEBG_GAME_HOSHIMEMO, "hoshimemo by name");
    CHECK(fvp_titlebg_game_from_name("iroseka") ==
              FVP_TITLEBG_GAME_IROSEKA, "iroseka by name");
    CHECK(fvp_titlebg_game_from_name(NULL) == FVP_TITLEBG_GAME_IROSEKA,
          "NULL -> iroseka");
    CHECK(fvp_titlebg_total_ms(FVP_TITLEBG_GAME_HOSHIMEMO) == 40000,
          "hoshimemo 40000 ms");
    CHECK(fvp_titlebg_total_ms(FVP_TITLEBG_GAME_IROSEKA) == 0,
          "iroseka 0（尚未实现）");

    /* ---- 2. 星空：2000 → 1000，缓出 ---- */
    {
        FvpTitleBgState st = at(FVP_TITLEBG_GAME_HOSHIMEMO, 0);

        CHECK(st.animated == 1, "hoshimemo animated");
        CHECK(st.scale_x1000 == 2000 && st.scale_y1000 == 2000,
              "t=0 scale 2000（实际 %d/%d）", st.scale_x1000,
              st.scale_y1000);
        CHECK(st.done == 0, "t=0 not done");
        CHECK(st.angle_x10 == 0, "不旋转");
    }
    {
        FvpTitleBgState st = at(FVP_TITLEBG_GAME_HOSHIMEMO, 10000);

        CHECK(st.scale_x1000 == 1562, "t=10000 scale 1562（实际 %d）",
              st.scale_x1000);
    }
    {
        FvpTitleBgState st = at(FVP_TITLEBG_GAME_HOSHIMEMO, 20000);

        CHECK(st.scale_x1000 == 1250, "t=20000 scale 1250（实际 %d）",
              st.scale_x1000);
    }
    {
        FvpTitleBgState st = at(FVP_TITLEBG_GAME_HOSHIMEMO, 30000);

        CHECK(st.scale_x1000 == 1062, "t=30000 scale 1062（实际 %d）",
              st.scale_x1000);
    }
    {
        FvpTitleBgState st = at(FVP_TITLEBG_GAME_HOSHIMEMO, 40000);

        CHECK(st.scale_x1000 == 1000 && st.scale_y1000 == 1000,
              "t=40000 scale 1000");
        CHECK(st.done == 1, "t=40000 done");
    }
    {
        FvpTitleBgState st = at(FVP_TITLEBG_GAME_HOSHIMEMO, 999999);

        CHECK(st.scale_x1000 == 1000 && st.done == 1, "超过总时长按终值");
    }
    {
        FvpTitleBgState st = at(FVP_TITLEBG_GAME_HOSHIMEMO, -5);

        CHECK(st.scale_x1000 == 2000 && st.done == 0, "负时间按 0");
    }

    /* ---- 3. 单调不增、有界 ---- */
    {
        int prev = 100000;

        for (i = 0; i <= 40000; i += 250) {
            FvpTitleBgState st = at(FVP_TITLEBG_GAME_HOSHIMEMO, i);

            CHECK(st.scale_x1000 <= prev, "scale 单调不增 @%d（%d > %d）", i,
                  st.scale_x1000, prev);
            CHECK(st.scale_x1000 >= 1000 && st.scale_x1000 <= 2000,
                  "scale 有界 @%d（%d）", i, st.scale_x1000);
            CHECK(st.scale_x1000 == st.scale_y1000, "两轴同值 @%d", i);
            prev = st.scale_x1000;
        }
    }

    /* ---- 4. 五彩斑斓：尚未实现 → static ---- */
    {
        FvpTitleBgState st = at(FVP_TITLEBG_GAME_IROSEKA, 0);

        CHECK(st.animated == 0, "iroseka 未实现动画");
        CHECK(st.scale_x1000 == 1000 && st.scale_y1000 == 1000,
              "iroseka static scale 1000");
        CHECK(st.angle_x10 == 0 && st.done == 1, "iroseka static/done");
    }

    /* ---- 5. 边界 / NULL ---- */
    fvp_titlebg_state(FVP_TITLEBG_GAME_HOSHIMEMO, 1000, NULL); /* 不该崩 */
    {
        FvpTitleBgState st = at((FvpTitleBgGame)99, 1000);

        CHECK(st.animated == 0 && st.scale_x1000 == 1000,
              "非法 game 当五彩斑斓（static）");
    }

    printf("test_titlebg: %d passed, %d failed\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}