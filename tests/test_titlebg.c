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

#include <string.h>

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
    st.angle_units = -1;
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
    CHECK(fvp_titlebg_total_ms(FVP_TITLEBG_GAME_IROSEKA) == 25000,
          "iroseka 25000 ms（四层 path A/B）");

    /* ---- 2. 星空：2000 → 1000，缓出 ---- */
    {
        FvpTitleBgState st = at(FVP_TITLEBG_GAME_HOSHIMEMO, 0);

        CHECK(st.animated == 1, "hoshimemo animated");
        CHECK(st.scale_x1000 == 2000 && st.scale_y1000 == 2000,
              "t=0 scale 2000（实际 %d/%d）", st.scale_x1000,
              st.scale_y1000);
        CHECK(st.done == 0, "t=0 not done");
        CHECK(st.angle_units == 0, "不旋转");
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

    /* ---- 3b. 五彩斑斓：四层逐层状态 ---- */
    {
        FvpTitleBgLayerState lst;
        int n = fvp_titlebg_layer_count(FVP_TITLEBG_GAME_IROSEKA);

        CHECK(n == 4, "iroseka 4 层（实际 %d）", n);
        CHECK(fvp_titlebg_layer_count(FVP_TITLEBG_GAME_HOSHIMEMO) == 0,
              "hoshimemo 不是逐层模式");
        CHECK(fvp_titlebg_uses_layers(FVP_TITLEBG_GAME_IROSEKA) == 1,
              "iroseka uses layers");
        CHECK(fvp_titlebg_uses_layers(FVP_TITLEBG_GAME_HOSHIMEMO) == 0,
              "hoshimemo 用单层接口");
        CHECK(fvp_titlebg_total_ms(FVP_TITLEBG_GAME_IROSEKA) == 25000,
              "iroseka 25000 ms");

        /* 层定义（顺序 = 绘制顺序 A → C → D → B；A 是唯一 path A） */
        {
            const char *want[4] = {"title_bg1_A", "title_bg1_C",
                                   "title_bg1_D", "title_bg1_B"};
            int want_tag[4] = {0, 2, 3, 1};
            int want_center[4] = {1, 0, 0, 0};
            int want_imgx[4] = {0, 0, 0, 518};
            int want_imgy[4] = {0, 0, 111, 0};
            int want_sf[4] = {1300, 2500, 4000, 1600};
            int want_st[4] = {900, 800, 800, 800};
            int want_af[4] = {150, 550, 1250, 250};
            int i;

            for (i = 0; i < 4; ++i) {
                const FvpTitleBgLayerDef *d =
                    fvp_titlebg_layer_def(FVP_TITLEBG_GAME_IROSEKA, i);

                CHECK(d != NULL, "层 %d 定义存在", i);
                if (d == NULL) {
                    continue;
                }
                CHECK(strcmp(d->name, want[i]) == 0, "层 %d 名字 %s（实际 %s）",
                      i, want[i], d->name);
                CHECK(d->slot_tag == want_tag[i], "层 %d slot_tag %d", i,
                      d->slot_tag);
                CHECK(d->anchor_center == want_center[i],
                      "层 %d anchor_center %d", i, want_center[i]);
                CHECK(d->img_x == want_imgx[i] && d->img_y == want_imgy[i],
                      "层 %d 图头 (%d,%d)", i, d->img_x, d->img_y);
                CHECK(d->op_x == 1200 && d->op_y == 675, "层 %d OP=(1200,675)",
                      i);
                CHECK(d->scale_from == want_sf[i] && d->scale_to == want_st[i],
                      "层 %d 缩放 %d->%d", i, d->scale_from, d->scale_to);
                CHECK(d->angle_from == want_af[i] && d->angle_to == 0,
                      "层 %d 角度 %d->0", i, d->angle_from);
            }
            CHECK(fvp_titlebg_layer_def(FVP_TITLEBG_GAME_IROSEKA, 4) == NULL,
                  "越界层返回 NULL");
            CHECK(fvp_titlebg_layer_def(FVP_TITLEBG_GAME_HOSHIMEMO, 0) == NULL,
                  "hoshimemo 没有逐层定义");
        }

        /* t=0：起点值 */
        CHECK(fvp_titlebg_layer_state(FVP_TITLEBG_GAME_IROSEKA, 0, 0, &lst) ==
                  1 && lst.scale_x1000 == 1300 && lst.angle_units == 150,
              "A t=0 s=1300 θ=150（实际 %d/%d）", lst.scale_x1000,
              lst.angle_units);
        CHECK(fvp_titlebg_layer_state(FVP_TITLEBG_GAME_IROSEKA, 3, 0, &lst) ==
                  1 && lst.scale_x1000 == 1600 && lst.angle_units == 250,
              "B t=0 s=1600 θ=250");

        /* t=12500（p=0.5, ease3=0.75） */
        CHECK(fvp_titlebg_layer_state(FVP_TITLEBG_GAME_IROSEKA, 0, 12500,
                                      &lst) == 1 &&
                  lst.scale_x1000 == 1000 && lst.angle_units == 37,
              "A t=12500 s=1000 θ=37（实际 %d/%d）", lst.scale_x1000,
              lst.angle_units);
        CHECK(fvp_titlebg_layer_state(FVP_TITLEBG_GAME_IROSEKA, 1, 12500,
                                      &lst) == 1 &&
                  lst.scale_x1000 == 1225 && lst.angle_units == 137,
              "C t=12500 s=1225 θ=137（实际 %d/%d）", lst.scale_x1000,
              lst.angle_units);
        CHECK(fvp_titlebg_layer_state(FVP_TITLEBG_GAME_IROSEKA, 2, 12500,
                                      &lst) == 1 &&
                  lst.scale_x1000 == 1600 && lst.angle_units == 312,
              "D t=12500 s=1600 θ=312（实际 %d/%d）", lst.scale_x1000,
              lst.angle_units);

        /* t=25000：终值 */
        CHECK(fvp_titlebg_layer_state(FVP_TITLEBG_GAME_IROSEKA, 0, 25000,
                                      &lst) == 1 &&
                  lst.scale_x1000 == 900 && lst.angle_units == 0,
              "A t=25000 s=900 θ=0");
        CHECK(fvp_titlebg_layer_state(FVP_TITLEBG_GAME_IROSEKA, 1, 25000,
                                      &lst) == 1 &&
                  lst.scale_x1000 == 800 && lst.angle_units == 0,
              "C t=25000 s=800 θ=0");

        /* 单调性 / 有界（全部四层） */
        {
            int i;

            for (i = 0; i < 4; ++i) {
                const FvpTitleBgLayerDef *d =
                    fvp_titlebg_layer_def(FVP_TITLEBG_GAME_IROSEKA, i);
                int t;
                int prev_s = 1 << 30;
                int prev_a = 1 << 30;

                for (t = 0; t <= 25000; t += 250) {
                    FvpTitleBgLayerState q;

                    (void)fvp_titlebg_layer_state(FVP_TITLEBG_GAME_IROSEKA, i,
                                                  t, &q);
                    CHECK(q.scale_x1000 <= prev_s, "层 %d 缩放单调 @%d", i, t);
                    CHECK(q.angle_units <= prev_a, "层 %d 角度单调 @%d", i, t);
                    CHECK(q.scale_x1000 >= d->scale_to &&
                              q.scale_x1000 <= d->scale_from,
                          "层 %d 缩放有界 @%d（%d）", i, t, q.scale_x1000);
                    CHECK(q.angle_units >= 0 && q.angle_units <= d->angle_from,
                          "层 %d 角度有界 @%d（%d）", i, t, q.angle_units);
                    prev_s = q.scale_x1000;
                    prev_a = q.angle_units;
                }
            }
        }
        CHECK(fvp_titlebg_layer_state(FVP_TITLEBG_GAME_IROSEKA, 0, 0, NULL) ==
                  0, "NULL state 返回 0");
        CHECK(fvp_titlebg_layer_state(FVP_TITLEBG_GAME_IROSEKA, 9, 0, &lst) ==
                  0, "越界层返回 0");
    }

    /* ---- 4. 五彩斑斓：单层接口仍是 static（逐层模式走 layer_state） ---- */
    {
        FvpTitleBgState st = at(FVP_TITLEBG_GAME_IROSEKA, 0);

        CHECK(st.animated == 0, "iroseka 未实现动画");
        CHECK(st.scale_x1000 == 1000 && st.scale_y1000 == 1000,
              "iroseka static scale 1000");
        CHECK(st.angle_units == 0 && st.done == 1, "iroseka static/done");
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