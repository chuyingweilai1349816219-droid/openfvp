/* 开场 FAVORITE logo 序列（openseq）单测：时间 → 每层 alpha / 缩放 / 旋转 / 位移 / 轴心。
 *
 *   bin/test_openseq.exe
 *
 * 时间轴来源是脚本（src/openinganim.h、ref/opening-scene.json）：
 *   0..1200   logo_favo(252) alpha 0→255、缩放 750→1000
 *   0..1400   logo_favo_view(254) + p1/p2/p3 alpha 0→255；254 缩放 750→1000、
 *             y +50→0；p1..p3 缩放 10000→1000、旋转 3600→0、位移 (±200,200)→(0,0)
 *   1200..    淡出：logo 层 2500 ms、白底 3500 ms
 *   3700..3800 全部层 100 ms 收尾
 *   3800 结束（之后标题自己做 500 ms 淡入）
 *
 * 期望值都是"手算 + 脚本常量"：alpha 是线性，缩放/旋转/位移是 mode 3 缓出
 * （1-(1-p)^2），坐标是逻辑像素 × HD/逻辑 的整数乘除。
 */
#include <stdio.h>

#include "../src/motion.h"
#include "../src/openseq.h"

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

static FvpOpenSeq at(FvpOpenGame game, int ms)
{
    FvpOpenSeq q;

    fvp_openseq_init(&q, game);
    fvp_openseq_set_elapsed(&q, ms);
    return q;
}

static int alpha_at(FvpOpenGame game, int slot, int ms)
{
    FvpOpenSeq q = at(game, ms);
    FvpOpenLayerState st;
    int idx = fvp_openseq_layer_index(slot);

    if (idx < 0 || !fvp_openseq_layer_state(&q, idx, &st)) {
        return -1;
    }
    return st.alpha;
}

static FvpOpenLayerState state_at(FvpOpenGame game, int slot, int ms)
{
    FvpOpenSeq q = at(game, ms);
    FvpOpenLayerState st;

    st.alpha = -1;
    st.scale_x1000 = -1;
    st.scale_y1000 = -1;
    st.angle_x10 = -1;
    st.dx = 0;
    st.dy = 0;
    st.op_x = -1;
    st.op_y = -1;
    {
        int idx = fvp_openseq_layer_index(slot);

        if (idx >= 0) {
            (void)fvp_openseq_layer_state(&q, idx, &st);
        }
    }
    return st;
}

int main(void)
{
    FvpOpenSeq s;
    int i;

    /* ---- 1. 层表 = 脚本 GraphLoad 顺序 ---- */
    CHECK(fvp_openseq_layer_count() == 6, "6 layers (got %d)",
          fvp_openseq_layer_count());
    CHECK(fvp_openseq_layer_slot(0) == 250, "layer 0 = logo_bg (250)");
    CHECK(fvp_openseq_layer_slot(1) == 252, "layer 1 = logo_favo (252)");
    CHECK(fvp_openseq_layer_slot(2) == 254, "layer 2 = logo_favo_view (254)");
    CHECK(fvp_openseq_layer_slot(3) == 255, "layer 3 = p1 (255)");
    CHECK(fvp_openseq_layer_slot(4) == 256, "layer 4 = p2 (256)");
    CHECK(fvp_openseq_layer_slot(5) == 257, "layer 5 = p3 (257)");
    CHECK(fvp_openseq_layer_name(0) != NULL &&
              fvp_openseq_layer_name(0)[0] == 'l',
          "layer name is the script asset name");
    CHECK(fvp_openseq_layer_index(250) == 0, "slot 250 -> index 0");
    CHECK(fvp_openseq_layer_index(253) == -1, "slot 253 has no layer");
    CHECK(fvp_openseq_layer_slot(-1) == -1, "negative index");
    CHECK(fvp_openseq_layer_name(6) == NULL, "out of range name");

    /* ---- 2. 总时长 / 相位边界（脚本 1200 + 2500 + 100）---- */
    CHECK(FVP_OPEN_TOTAL_MS ==
              FVP_OPEN_OUT_START_MS + FVP_OPEN_LOGO_OUT_MS + FVP_OPEN_CUT_MS,
          "total = out_start + logo_out + cut (%d)", FVP_OPEN_TOTAL_MS);
    CHECK(FVP_OPEN_CUT_START_MS ==
              FVP_OPEN_OUT_START_MS + FVP_OPEN_LOGO_OUT_MS,
          "cut starts when the 2500 ms logo fade ends");
    CHECK(FVP_OPEN_TOTAL_MS == 3800, "total 3800 ms (got %d)",
          FVP_OPEN_TOTAL_MS);
    CHECK(fvp_openseq_phase(&(FvpOpenSeq){0, FVP_OPEN_GAME_IROSEKA}) ==
              FVP_OPEN_IN,
          "t=0 IN");
    CHECK(fvp_openseq_phase(&(FvpOpenSeq){1199, FVP_OPEN_GAME_IROSEKA}) ==
              FVP_OPEN_IN,
          "t=1199 IN");
    CHECK(fvp_openseq_phase(&(FvpOpenSeq){1200, FVP_OPEN_GAME_IROSEKA}) ==
              FVP_OPEN_OUT,
          "t=1200 OUT");
    CHECK(fvp_openseq_phase(&(FvpOpenSeq){3699, FVP_OPEN_GAME_IROSEKA}) ==
              FVP_OPEN_OUT,
          "t=3699 OUT");
    CHECK(fvp_openseq_phase(&(FvpOpenSeq){3700, FVP_OPEN_GAME_IROSEKA}) ==
              FVP_OPEN_CUT,
          "t=3700 CUT");
    CHECK(fvp_openseq_phase(&(FvpOpenSeq){3799, FVP_OPEN_GAME_IROSEKA}) ==
              FVP_OPEN_CUT,
          "t=3799 CUT");
    CHECK(fvp_openseq_phase(&(FvpOpenSeq){3800, FVP_OPEN_GAME_IROSEKA}) ==
              FVP_OPEN_DONE,
          "t=3800 DONE");
    CHECK(!fvp_openseq_done(&(FvpOpenSeq){3799, FVP_OPEN_GAME_IROSEKA}),
          "not done at 3799");
    CHECK(fvp_openseq_done(&(FvpOpenSeq){3800, FVP_OPEN_GAME_IROSEKA}),
          "done at 3800");

    /* ---- 3. alpha：线性淡入 + 线性淡出（MotionAlpha 没有缓动 mode）---- */
    CHECK(alpha_at(FVP_OPEN_GAME_IROSEKA, 252, 0) == 0, "252 alpha 0 at t=0");
    CHECK(alpha_at(FVP_OPEN_GAME_IROSEKA, 252, 600) == 127, "252 alpha 127 at 600");
    CHECK(alpha_at(FVP_OPEN_GAME_IROSEKA, 252, 1200) == 255,
          "252 alpha 255 at 1200 (fade-in ends)");
    CHECK(alpha_at(FVP_OPEN_GAME_IROSEKA, 252, 2450) == 128,
          "252 alpha 128 at 2450 (halfway through the 2500 ms fade-out)");
    CHECK(alpha_at(FVP_OPEN_GAME_IROSEKA, 252, 3700) == 0,
          "252 alpha 0 at 3700");
    CHECK(alpha_at(FVP_OPEN_GAME_IROSEKA, 252, 3800) == 0, "252 alpha 0 at end");

    CHECK(alpha_at(FVP_OPEN_GAME_IROSEKA, 250, 0) == 255,
          "logo_bg has no fade-in: 255 at t=0");
    CHECK(alpha_at(FVP_OPEN_GAME_IROSEKA, 250, 1200) == 255,
          "logo_bg 255 when the fade-out starts");
    CHECK(alpha_at(FVP_OPEN_GAME_IROSEKA, 250, 2950) == 128,
          "logo_bg 128 at 2950 (half of 3500 ms)");
    CHECK(fvp_openseq_layer_index(2950) == -1, "slot 2950 is not a layer");
    /* 3700 ms 时白底还剩 255-255*2500/3500 = 73（整数向零截断），
     * 再 100 ms 收尾到 0。 */
    CHECK(alpha_at(FVP_OPEN_GAME_IROSEKA, 250, 3700) == 73,
          "logo_bg 73 at 3700 (cut start; got %d)",
          alpha_at(FVP_OPEN_GAME_IROSEKA, 250, 3700));
    CHECK(alpha_at(FVP_OPEN_GAME_IROSEKA, 250, 3750) == 37,
          "logo_bg 37 halfway through the cut");
    CHECK(alpha_at(FVP_OPEN_GAME_IROSEKA, 250, 3800) == 0, "logo_bg 0 at end");

    /* 254 的淡入被 1200 ms 的淡出打断：那一刻是 255*1200/1400 = 218 */
    CHECK(alpha_at(FVP_OPEN_GAME_IROSEKA, 254, 1200) == 218,
          "254 alpha 218 at 1200 (interrupted fade-in; got %d)",
          alpha_at(FVP_OPEN_GAME_IROSEKA, 254, 1200));
    CHECK(alpha_at(FVP_OPEN_GAME_IROSEKA, 257, 1400) > 0,
          "p3 still visible at 1400");

    /* ---- 4. 缩放：mode 3 缓出 1-(1-p)^2 ---- */
    CHECK(state_at(FVP_OPEN_GAME_IROSEKA, 252, 0).scale_x1000 == 750,
          "252 scale 750 at t=0");
    CHECK(state_at(FVP_OPEN_GAME_IROSEKA, 252, 1200).scale_x1000 == 1000,
          "252 scale 1000 at 1200");
    CHECK(state_at(FVP_OPEN_GAME_IROSEKA, 252, 600).scale_x1000 == 937,
          "252 scale 937 at 600 (= 750 + 250*0.75; got %d)",
          state_at(FVP_OPEN_GAME_IROSEKA, 252, 600).scale_x1000);
    CHECK(state_at(FVP_OPEN_GAME_IROSEKA, 255, 0).scale_x1000 == 10000,
          "p1 scale 10000 at t=0");
    CHECK(state_at(FVP_OPEN_GAME_IROSEKA, 255, 700).scale_x1000 == 3250,
          "p1 scale 3250 at 700 (ease-out mid; got %d)",
          state_at(FVP_OPEN_GAME_IROSEKA, 255, 700).scale_x1000);
    CHECK(state_at(FVP_OPEN_GAME_IROSEKA, 255, 1400).scale_x1000 == 1000,
          "p1 scale 1000 at 1400");
    CHECK(state_at(FVP_OPEN_GAME_IROSEKA, 255, 1000).scale_y1000 ==
              state_at(FVP_OPEN_GAME_IROSEKA, 255, 1000).scale_x1000,
          "p1 scale is uniform on both axes");

    /* ---- 5. 旋转：3600 -> 0（0.1°） ---- */
    CHECK(state_at(FVP_OPEN_GAME_IROSEKA, 255, 0).angle_x10 == 3600,
          "p1 angle 3600 at t=0");
    CHECK(state_at(FVP_OPEN_GAME_IROSEKA, 255, 700).angle_x10 == 900,
          "p1 angle 900 at 700 (got %d)",
          state_at(FVP_OPEN_GAME_IROSEKA, 255, 700).angle_x10);
    CHECK(state_at(FVP_OPEN_GAME_IROSEKA, 255, 1400).angle_x10 == 0,
          "p1 angle 0 at 1400");
    CHECK(state_at(FVP_OPEN_GAME_IROSEKA, 252, 700).angle_x10 == 0,
          "logo_favo never rotates");

    /* ---- 6. 位移：逻辑 (0,-200) → (0,0)，HD = ×1080/640 = -337 ---- */
    CHECK(state_at(FVP_OPEN_GAME_IROSEKA, 257, 0).dy == -337,
          "p3 dy -337 at t=0 (got %d)",
          state_at(FVP_OPEN_GAME_IROSEKA, 257, 0).dy);
    CHECK(state_at(FVP_OPEN_GAME_IROSEKA, 257, 1400).dy == 0, "p3 dy 0 at 1400");
    CHECK(state_at(FVP_OPEN_GAME_IROSEKA, 255, 0).dx == -375,
          "p1 dx -375 at t=0 (got %d)",
          state_at(FVP_OPEN_GAME_IROSEKA, 255, 0).dx);
    CHECK(state_at(FVP_OPEN_GAME_IROSEKA, 256, 0).dx == 375,
          "p2 dx +375 at t=0");
    CHECK(state_at(FVP_OPEN_GAME_IROSEKA, 254, 0).dy == 84,
          "logo_favo_view dy 84 at t=0 (logical +50 -> HD 84)");
    CHECK(state_at(FVP_OPEN_GAME_IROSEKA, 254, 1400).dy == 0,
          "logo_favo_view dy 0 at 1400");

    /* ---- 7. OP 轴心（脚本 PrimSetOP / helper 的 HD 像素值）---- */
    CHECK(state_at(FVP_OPEN_GAME_IROSEKA, 252, 0).op_x == 960 &&
              state_at(FVP_OPEN_GAME_IROSEKA, 252, 0).op_y == 540,
          "iroseka logo_favo OP = (960,540)");
    CHECK(state_at(FVP_OPEN_GAME_IROSEKA, 257, 0).op_y == 628,
          "iroseka p1..p3 OP y = 628");
    CHECK(state_at(FVP_OPEN_GAME_HOSHIMEMO, 257, 0).op_y == 610,
          "hoshimemo p1..p3 OP y = 610");
    CHECK(state_at(FVP_OPEN_GAME_HOSHIMEMO, 252, 0).op_y == 540,
          "hoshimemo logo_favo OP y = 540");

    /* ---- 8. 星空：逻辑画布 800x600 → 同样的 10x/旋转，位移是 ±480/±360 ---- */
    CHECK(state_at(FVP_OPEN_GAME_HOSHIMEMO, 255, 0).dx == -480,
          "hoshimemo p1 dx -480 at t=0 (got %d)",
          state_at(FVP_OPEN_GAME_HOSHIMEMO, 255, 0).dx);
    CHECK(state_at(FVP_OPEN_GAME_HOSHIMEMO, 257, 0).dy == -360,
          "hoshimemo p3 dy -360 at t=0");
    CHECK(state_at(FVP_OPEN_GAME_HOSHIMEMO, 254, 0).dy == 90,
          "hoshimemo logo_favo_view dy 90 at t=0");

    /* ---- 9. 单调 / 有界：整条时间轴上所有层 ---- */
    for (i = 0; i <= FVP_OPEN_TOTAL_MS; i += 37) {
        FvpOpenSeq q = at(FVP_OPEN_GAME_IROSEKA, i);
        int li;

        for (li = 0; li < fvp_openseq_layer_count(); ++li) {
            FvpOpenLayerState st;

            CHECK(fvp_openseq_layer_state(&q, li, &st), "state at %d layer %d",
                  i, li);
            CHECK(st.alpha >= 0 && st.alpha <= 255, "alpha bounded");
            CHECK(st.scale_x1000 >= 750 && st.scale_x1000 <= 10000,
                  "scale bounded (%d)", st.scale_x1000);
            CHECK(st.angle_x10 >= 0 && st.angle_x10 <= 3600, "angle bounded");
        }
    }
    /* 淡入阶段 alpha 单调不减；淡出阶段单调不增（252） */
    {
        int prev = -1;

        for (i = 0; i <= 1200; i += 60) {
            int a = alpha_at(FVP_OPEN_GAME_IROSEKA, 252, i);

            CHECK(a >= prev, "252 alpha monotonic up at %d (a=%d)", i, a);
            prev = a;
        }
        prev = 256;
        for (i = 1200; i <= FVP_OPEN_TOTAL_MS; i += 60) {
            int a = alpha_at(FVP_OPEN_GAME_IROSEKA, 252, i);

            CHECK(a <= prev, "252 alpha monotonic down at %d (a=%d)", i, a);
            prev = a;
        }
    }
    /* 终点：所有层 alpha 0 */
    {
        FvpOpenSeq q = at(FVP_OPEN_GAME_IROSEKA, FVP_OPEN_TOTAL_MS);

        for (i = 0; i < fvp_openseq_layer_count(); ++i) {
            FvpOpenLayerState st;

            CHECK(fvp_openseq_layer_state(&q, i, &st) && st.alpha == 0,
                  "layer %d alpha 0 at end", i);
        }
    }

    /* ---- 10. 边界 / 鲁棒性 ---- */
    fvp_openseq_init(&s, FVP_OPEN_GAME_IROSEKA);
    CHECK(fvp_openseq_elapsed(&s) == 0, "starts at 0");
    fvp_openseq_advance(&s, -100);
    CHECK(fvp_openseq_elapsed(&s) == 0, "negative ms ignored");
    fvp_openseq_advance(&s, 3800 * 3);
    CHECK(fvp_openseq_elapsed(&s) == 3800, "clamped at total");
    fvp_openseq_set_elapsed(&s, -5);
    CHECK(fvp_openseq_elapsed(&s) == 0, "set_elapsed clamps low");
    fvp_openseq_set_elapsed(&s, 999999);
    CHECK(fvp_openseq_elapsed(&s) == 3800, "set_elapsed clamps high");
    CHECK(fvp_openseq_elapsed(NULL) == 0, "NULL elapsed");
    CHECK(!fvp_openseq_done(NULL), "NULL not done (elapsed 0)");
    CHECK(fvp_openseq_phase(NULL) == FVP_OPEN_IN, "NULL phase IN");
    CHECK(!fvp_openseq_layer_state(NULL, 0, NULL), "NULL state");
    fvp_openseq_init(NULL, FVP_OPEN_GAME_IROSEKA);  /* 不该崩 */
    fvp_openseq_advance(NULL, 1);                   /* 不该崩 */
    fvp_openseq_set_elapsed(NULL, 1);               /* 不该崩 */

    /* 游戏名映射 */
    CHECK(fvp_openseq_game_from_name("iroseka") == FVP_OPEN_GAME_IROSEKA,
          "iroseka by name");
    CHECK(fvp_openseq_game_from_name("hoshimemo") == FVP_OPEN_GAME_HOSHIMEMO,
          "hoshimemo by name");
    CHECK(fvp_openseq_game_from_name(NULL) == FVP_OPEN_GAME_IROSEKA,
          "NULL -> iroseka");
    CHECK(fvp_openseq_game_from_name("other") == FVP_OPEN_GAME_IROSEKA,
          "unknown -> iroseka");

    /* 缓动公共实现与时间轴一致：252 缩放 600 ms 必须等于直接调 fvp_motion_ease */
    {
        double p = 600.0 / 1200.0;
        int want = (int)(750.0 + 250.0 * fvp_motion_ease(FVP_OPEN_EASE_MODE, p));

        CHECK(state_at(FVP_OPEN_GAME_IROSEKA, 252, 600).scale_x1000 == want,
              "scale uses fvp_motion_ease(mode 3)");
    }

    printf("test_openseq: %d passed, %d failed\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}