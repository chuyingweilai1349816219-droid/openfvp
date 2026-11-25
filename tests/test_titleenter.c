/* 标题进场逐元素时间轴（titleenter）单测。
 *
 *   bin/test_titleenter.exe
 *
 * 时间轴来自脚本（iroseka hcb 0x5933E..0x5961A，冻结在 ref/title-enter.json）：
 *    logo(265)   t=2000..4000   alpha 0→255 / 2000 ms
 *    menu0(1501) t=2400..3300   alpha 0→255 / 900 ms，纵向 +23 px → 0
 *    menu1..4    t=2440/2480/2520/2560 起，各 820 ms，纵向 +23 px → 0
 *    copy(278)   t=2560..3380   alpha 0→255 / 820 ms，横向 +30 px → 0
 *    t=4000 全部结束（= 静态画面）
 *
 * 期望值都是手算：alpha 线性（MotionAlpha 没有缓动 mode，整数向零截断）。
 */
#include <stdio.h>

#include "../src/titleenter.h"

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

static FvpTitleEnter at(FvpTitleEnterGame game, int ms)
{
    FvpTitleEnter s;

    fvp_titleenter_init(&s, game);
    fvp_titleenter_set_elapsed(&s, ms);
    return s;
}

static int alpha(FvpTitleEnterGame game, int ms, int part)
{
    FvpTitleEnter s = at(game, ms);

    return fvp_titleenter_alpha(&s, part);
}

static int hoshimemo_alpha(int ms, int part)
{
    return alpha(FVP_TITLEENTER_GAME_HOSHIMEMO, ms, part);
}

static void offset(FvpTitleEnterGame game, int ms, int part, int *dx,
                   int *dy)
{
    FvpTitleEnter s = at(game, ms);

    fvp_titleenter_offset(&s, part, dx, dy);
}

int main(void)
{
    FvpTitleEnter s;
    int i;

    /* ---- 1. 部件表 ---- */
    CHECK(FVP_TITLEENTER_PART_COUNT == 7, "7 parts (logo + copy + 5 menu)");
    CHECK(FVP_TITLEENTER_PART_LOGO == 0 && FVP_TITLEENTER_PART_COPY == 1 &&
              FVP_TITLEENTER_PART_MENU0 == 2,
          "part ids 0/1/2..6");
    CHECK(FVP_TITLEENTER_IROSEKA_TOTAL_MS == 4000,
          "iroseka total 4000 ms");
    CHECK(FVP_TITLEENTER_HOSHIMEMO_TOTAL_MS == 3000,
          "hoshimemo total 3000 ms");

    /* ---- 2. logo：2000 起、2000 ms ---- */
    CHECK(alpha(FVP_TITLEENTER_GAME_IROSEKA, 0, FVP_TITLEENTER_PART_LOGO) == 0, "logo alpha 0 @0");
    CHECK(alpha(FVP_TITLEENTER_GAME_IROSEKA, 1999, FVP_TITLEENTER_PART_LOGO) == 0, "logo alpha 0 @1999");
    CHECK(alpha(FVP_TITLEENTER_GAME_IROSEKA, 2000, FVP_TITLEENTER_PART_LOGO) == 0, "logo alpha 0 @2000");
    CHECK(alpha(FVP_TITLEENTER_GAME_IROSEKA, 2500, FVP_TITLEENTER_PART_LOGO) == 63,
          "logo alpha 63 @2500 (=255*500/2000，实际 %d)",
          alpha(FVP_TITLEENTER_GAME_IROSEKA, 2500, FVP_TITLEENTER_PART_LOGO));
    CHECK(alpha(FVP_TITLEENTER_GAME_IROSEKA, 3000, FVP_TITLEENTER_PART_LOGO) == 127,
          "logo alpha 127 @3000（实际 %d）",
          alpha(FVP_TITLEENTER_GAME_IROSEKA, 3000, FVP_TITLEENTER_PART_LOGO));
    CHECK(alpha(FVP_TITLEENTER_GAME_IROSEKA, 3999, FVP_TITLEENTER_PART_LOGO) == 254,
          "logo alpha 254 @3999（实际 %d）",
          alpha(FVP_TITLEENTER_GAME_IROSEKA, 3999, FVP_TITLEENTER_PART_LOGO));
    CHECK(alpha(FVP_TITLEENTER_GAME_IROSEKA, 4000, FVP_TITLEENTER_PART_LOGO) == 255, "logo alpha 255 @4000");
    CHECK(alpha(FVP_TITLEENTER_GAME_IROSEKA, 99999, FVP_TITLEENTER_PART_LOGO) == 255, "logo alpha 255 clamped");

    /* ---- 3. 菜单：第一项 900 ms、其余 820 ms、逐项 +40 ms ---- */
    CHECK(alpha(FVP_TITLEENTER_GAME_IROSEKA, 2400, FVP_TITLEENTER_PART_MENU0) == 0, "menu0 alpha 0 @2400");
    CHECK(alpha(FVP_TITLEENTER_GAME_IROSEKA, 2850, FVP_TITLEENTER_PART_MENU0) == 127,
          "menu0 alpha 127 @2850（实际 %d）",
          alpha(FVP_TITLEENTER_GAME_IROSEKA, 2850, FVP_TITLEENTER_PART_MENU0));
    CHECK(alpha(FVP_TITLEENTER_GAME_IROSEKA, 3300, FVP_TITLEENTER_PART_MENU0) == 255, "menu0 alpha 255 @3300");
    CHECK(alpha(FVP_TITLEENTER_GAME_IROSEKA, 2439, FVP_TITLEENTER_PART_MENU0 + 1) == 0,
          "menu1 还没开始 @2439");
    CHECK(alpha(FVP_TITLEENTER_GAME_IROSEKA, 2440, FVP_TITLEENTER_PART_MENU0 + 1) == 0, "menu1 alpha 0 @2440");
    CHECK(alpha(FVP_TITLEENTER_GAME_IROSEKA, 2850, FVP_TITLEENTER_PART_MENU0 + 1) == 127,
          "menu1 alpha 127 @2850（实际 %d）",
          alpha(FVP_TITLEENTER_GAME_IROSEKA, 2850, FVP_TITLEENTER_PART_MENU0 + 1));
    CHECK(alpha(FVP_TITLEENTER_GAME_IROSEKA, 3260, FVP_TITLEENTER_PART_MENU0 + 1) == 255,
          "menu1 alpha 255 @3260");
    CHECK(alpha(FVP_TITLEENTER_GAME_IROSEKA, 2560, FVP_TITLEENTER_PART_MENU0 + 4) == 0,
          "menu4 alpha 0 @2560");
    CHECK(alpha(FVP_TITLEENTER_GAME_IROSEKA, 3380, FVP_TITLEENTER_PART_MENU0 + 4) == 255,
          "menu4 alpha 255 @3380");
    /* 每一项的时长：第一项 900、其余 820（用"半程 alpha=127"验证） */
    for (i = 0; i < 5; ++i) {
        int start = 2400 + i * 40;
        int dur = (i == 0) ? 900 : 820;

        CHECK(alpha(FVP_TITLEENTER_GAME_IROSEKA, start + dur / 2,
                    FVP_TITLEENTER_PART_MENU0 + i) == 127,
              "menu%d 半程 alpha 127", i);
    }

    /* ---- 4. 版权行：2560 起、820 ms、横向 +30 → 0 ---- */
    CHECK(alpha(FVP_TITLEENTER_GAME_IROSEKA, 2559, FVP_TITLEENTER_PART_COPY) == 0, "copy alpha 0 @2559");
    CHECK(alpha(FVP_TITLEENTER_GAME_IROSEKA, 2970, FVP_TITLEENTER_PART_COPY) == 127,
          "copy alpha 127 @2970（实际 %d）",
          alpha(FVP_TITLEENTER_GAME_IROSEKA, 2970, FVP_TITLEENTER_PART_COPY));
    CHECK(alpha(FVP_TITLEENTER_GAME_IROSEKA, 3380, FVP_TITLEENTER_PART_COPY) == 255, "copy alpha 255 @3380");

    /* ---- 5. 位移 ---- */
    {
        int dx = -1;
        int dy = -1;

        offset(FVP_TITLEENTER_GAME_IROSEKA, 0, FVP_TITLEENTER_PART_COPY, &dx, &dy);
        CHECK(dx == 30 && dy == 0, "copy offset (30,0) @0（实际 %d,%d）", dx, dy);
        offset(FVP_TITLEENTER_GAME_IROSEKA, 2560, FVP_TITLEENTER_PART_COPY, &dx, &dy);
        CHECK(dx == 30 && dy == 0, "copy offset still (30,0) @2560");
        offset(FVP_TITLEENTER_GAME_IROSEKA, 2970, FVP_TITLEENTER_PART_COPY, &dx, &dy);
        CHECK(dx == 15 && dy == 0, "copy dx 15 halfway（实际 %d,%d）", dx, dy);
        offset(FVP_TITLEENTER_GAME_IROSEKA, 3380, FVP_TITLEENTER_PART_COPY, &dx, &dy);
        CHECK(dx == 0 && dy == 0, "copy offset (0,0) @3380");

        offset(FVP_TITLEENTER_GAME_IROSEKA, 0, FVP_TITLEENTER_PART_MENU0, &dx, &dy);
        CHECK(dx == 0 && dy == 23, "menu0 dy 23 @0（实际 %d,%d）", dx, dy);
        offset(FVP_TITLEENTER_GAME_IROSEKA, 2400, FVP_TITLEENTER_PART_MENU0, &dx, &dy);
        CHECK(dy == 23, "menu0 dy 23 @2400");
        offset(FVP_TITLEENTER_GAME_IROSEKA, 2850, FVP_TITLEENTER_PART_MENU0, &dx, &dy);
        CHECK(dy == 12, "menu0 dy 12 halfway（实际 %d）", dy);
        offset(FVP_TITLEENTER_GAME_IROSEKA, 3300, FVP_TITLEENTER_PART_MENU0, &dx, &dy);
        CHECK(dx == 0 && dy == 0, "menu0 offset (0,0) @3300");

        offset(FVP_TITLEENTER_GAME_IROSEKA, 4000, FVP_TITLEENTER_PART_LOGO, &dx, &dy);
        CHECK(dx == 0 && dy == 0, "logo never moves");
        offset(FVP_TITLEENTER_GAME_IROSEKA, 4000, FVP_TITLEENTER_PART_MENU0 + 3, &dx, &dy);
        CHECK(dx == 0 && dy == 0, "menu3 offset (0,0) at the end");
    }

    /* ---- 5b. 星空：更早（只等一次 1000 ms）、位移更大 ---- */
    CHECK(hoshimemo_alpha(999, FVP_TITLEENTER_PART_LOGO) == 0,
          "hoshimemo logo 还没开始 @999");
    CHECK(hoshimemo_alpha(1000, FVP_TITLEENTER_PART_LOGO) == 0,
          "hoshimemo logo alpha 0 @1000");
    CHECK(hoshimemo_alpha(2000, FVP_TITLEENTER_PART_LOGO) == 127,
          "hoshimemo logo alpha 127 @2000（实际 %d）",
          hoshimemo_alpha(2000, FVP_TITLEENTER_PART_LOGO));
    CHECK(hoshimemo_alpha(3000, FVP_TITLEENTER_PART_LOGO) == 255,
          "hoshimemo logo alpha 255 @3000");
    CHECK(hoshimemo_alpha(1399, FVP_TITLEENTER_PART_MENU0) == 0,
          "hoshimemo menu0 还没开始 @1399");
    CHECK(hoshimemo_alpha(1850, FVP_TITLEENTER_PART_MENU0) == 127,
          "hoshimemo menu0 alpha 127 @1850（900 ms 半程）");
    CHECK(hoshimemo_alpha(1400 +
                          FVP_TITLEENTER_MENU_STEP_MS * 4,
                          FVP_TITLEENTER_PART_MENU0 + 4) == 0,
          "hoshimemo menu4 alpha 0 @1560");
    CHECK(hoshimemo_alpha(1970, FVP_TITLEENTER_PART_COPY) == 127,
          "hoshimemo copy alpha 127 @1970（实际 %d）",
          hoshimemo_alpha(1970, FVP_TITLEENTER_PART_COPY));
    {
        int dx = -1;
        int dy = -1;

        offset(FVP_TITLEENTER_GAME_HOSHIMEMO, 0,
               FVP_TITLEENTER_PART_MENU0, &dx, &dy);
        CHECK(dy == 50, "hoshimemo menu dy 50 @0（实际 %d）", dy);
        offset(FVP_TITLEENTER_GAME_HOSHIMEMO, 0,
               FVP_TITLEENTER_PART_COPY, &dx, &dy);
        CHECK(dx == 76, "hoshimemo copy dx 76 @0（实际 %d）", dx);
        offset(FVP_TITLEENTER_GAME_HOSHIMEMO, 2999,
               FVP_TITLEENTER_PART_MENU0, &dx, &dy);
        CHECK(dx == 0 && dy == 0, "hoshimemo menu 归零 @2999");
    }
    CHECK(fvp_titleenter_game_from_name("hoshimemo") ==
              FVP_TITLEENTER_GAME_HOSHIMEMO,
          "hoshimemo by name");
    CHECK(fvp_titleenter_game_from_name("iroseka") ==
              FVP_TITLEENTER_GAME_IROSEKA, "iroseka by name");
    CHECK(fvp_titleenter_game_from_name(NULL) ==
              FVP_TITLEENTER_GAME_IROSEKA, "NULL -> iroseka");

    /* ---- 6. 结束状态 = 静态画面（全部 255 / 0 偏移）---- */
    {
        FvpTitleEnter q;

        fvp_titleenter_init(&q, FVP_TITLEENTER_GAME_IROSEKA);
        fvp_titleenter_set_elapsed(&q, 3999);
        CHECK(fvp_titleenter_done(&q) == 0, "iroseka not done @3999");
        fvp_titleenter_set_elapsed(&q, 4000);
        CHECK(fvp_titleenter_done(&q) == 1, "iroseka done @4000");
        fvp_titleenter_init(&q, FVP_TITLEENTER_GAME_HOSHIMEMO);
        fvp_titleenter_set_elapsed(&q, 2999);
        CHECK(fvp_titleenter_done(&q) == 0, "hoshimemo not done @2999");
        fvp_titleenter_set_elapsed(&q, 3000);
        CHECK(fvp_titleenter_done(&q) == 1, "hoshimemo done @3000");
    }
    for (i = 0; i < FVP_TITLEENTER_PART_COUNT; ++i) {
        FvpTitleEnter q = at(FVP_TITLEENTER_GAME_IROSEKA,
                                          FVP_TITLEENTER_IROSEKA_TOTAL_MS);
        int dx = -1;
        int dy = -1;

        CHECK(fvp_titleenter_alpha(&q, i) == 255, "part %d alpha 255 at end",
              i);
        fvp_titleenter_offset(&q, i, &dx, &dy);
        CHECK(dx == 0 && dy == 0, "part %d offset 0 at end", i);
    }

    /* ---- 7. 单调性 / 有界 ---- */
    for (i = 0; i <= FVP_TITLEENTER_IROSEKA_TOTAL_MS; i += 25) {
        FvpTitleEnter q = at(FVP_TITLEENTER_GAME_IROSEKA, i);
        int p;

        for (p = 0; p < FVP_TITLEENTER_PART_COUNT; ++p) {
            int a = fvp_titleenter_alpha(&q, p);
            int dx = 0;
            int dy = 0;

            CHECK(a >= 0 && a <= 255, "alpha bounded @%d part %d", i, p);
            fvp_titleenter_offset(&q, p, &dx, &dy);
            CHECK(dx <= FVP_TITLEENTER_HOSHIMEMO_COPY_DX && dx >= 0,
                  "dx bounded @%d part %d", i, p);
            CHECK(dy <= FVP_TITLEENTER_HOSHIMEMO_MENU_DY && dy >= 0,
                  "dy bounded @%d part %d", i, p);
        }
    }
    {
        int prev = -1;

        for (i = 2000; i <= 4000; i += 40) {
            int a = alpha(FVP_TITLEENTER_GAME_IROSEKA, i,
                          FVP_TITLEENTER_PART_LOGO);

            CHECK(a >= prev, "logo alpha monotonic @%d (a=%d)", i, a);
            prev = a;
        }
    }

    /* ---- 8. 边界 / NULL ---- */
    fvp_titleenter_init(&s, FVP_TITLEENTER_GAME_IROSEKA);
    CHECK(fvp_titleenter_elapsed(&s) == 0, "starts at 0");
    fvp_titleenter_set_elapsed(&s, -5);
    CHECK(fvp_titleenter_elapsed(&s) == 0, "clamps low");
    fvp_titleenter_set_elapsed(&s, 99999);
    CHECK(fvp_titleenter_elapsed(&s) == FVP_TITLEENTER_IROSEKA_TOTAL_MS,
          "clamps high (iroseka)");
    CHECK(fvp_titleenter_elapsed(NULL) == 0, "NULL elapsed");
    CHECK(fvp_titleenter_done(NULL) == 0, "NULL not done");
    CHECK(fvp_titleenter_alpha(NULL, 0) == 0, "NULL alpha");
    CHECK(fvp_titleenter_alpha(&s, -1) == 0, "negative part");
    CHECK(fvp_titleenter_alpha(&s, FVP_TITLEENTER_PART_COUNT) == 0,
          "part out of range");
    {
        int dx = -1;
        int dy = -1;

        fvp_titleenter_offset(NULL, 0, &dx, &dy);
        CHECK(dx == 0 && dy == 0, "NULL offset zeroed");
        fvp_titleenter_offset(&s, -1, &dx, &dy);
        CHECK(dx == 0 && dy == 0, "bad part offset zeroed");
        fvp_titleenter_offset(&s, 0, NULL, NULL); /* 不该崩 */
    }
    fvp_titleenter_init(NULL, FVP_TITLEENTER_GAME_IROSEKA); /* 不该崩 */
    fvp_titleenter_set_elapsed(NULL, 10);  /* 不该崩 */

    printf("test_titleenter: %d passed, %d failed\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}