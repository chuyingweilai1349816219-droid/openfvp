/* 开场序列（openseq）单测：时间 → 相位 / logo alpha / 白色遮罩 alpha。
 *
 *   bin/test_openseq.exe
 *
 * 时间轴（见 src/openseq.h）：白底 300 → logo 淡入 1200 → 停留 2100 →
 * 淡出 1200 → 标题从白色淡入（遮罩 255→0）600，合计 5400 ms。
 */
#include <stdio.h>

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

static FvpOpenSeq at(int ms)
{
    FvpOpenSeq q;

    fvp_openseq_init(&q);
    fvp_openseq_advance(&q, ms);
    return q;
}

static FvpOpenPhase phase_at(int ms)
{
    FvpOpenSeq q = at(ms);

    return fvp_openseq_phase(&q);
}

static int logo_at(int ms)
{
    FvpOpenSeq q = at(ms);

    return fvp_openseq_logo_alpha(&q);
}

static int veil_at(int ms)
{
    FvpOpenSeq q = at(ms);

    return fvp_openseq_veil_alpha(&q);
}

static int done_at(int ms)
{
    FvpOpenSeq q = at(ms);

    return fvp_openseq_done(&q);
}

int main(void)
{
    FvpOpenSeq s;
    int prev;
    int i;

    /* --- 1. 初始化 / 总时长 --- */
    fvp_openseq_init(&s);
    CHECK(fvp_openseq_elapsed(&s) == 0, "starts at 0");
    CHECK(fvp_openseq_phase(&s) == FVP_OPEN_WHITE_HOLD, "starts in WHITE_HOLD");
    CHECK(fvp_openseq_logo_alpha(&s) == 0, "logo alpha 0 at start");
    CHECK(fvp_openseq_veil_alpha(&s) == 0, "veil alpha 0 at start");
    CHECK(!fvp_openseq_done(&s), "not done at start");
    CHECK(FVP_OPEN_TOTAL_MS == 300 + 1200 + 2100 + 1200 + 600,
          "total %d ms", FVP_OPEN_TOTAL_MS);

    /* --- 2. 相位边界（累计时间） --- */
    CHECK(phase_at(0) == FVP_OPEN_WHITE_HOLD, "t=0 WHITE_HOLD");
    CHECK(phase_at(299) == FVP_OPEN_WHITE_HOLD, "t=299 WHITE_HOLD");
    CHECK(phase_at(300) == FVP_OPEN_LOGO_IN, "t=300 LOGO_IN");
    CHECK(phase_at(1500) == FVP_OPEN_HOLD, "t=1500 HOLD");
    CHECK(phase_at(3600) == FVP_OPEN_LOGO_OUT, "t=3600 LOGO_OUT");
    CHECK(phase_at(4800) == FVP_OPEN_TITLE_FADE, "t=4800 TITLE_FADE");
    CHECK(phase_at(FVP_OPEN_TOTAL_MS) == FVP_OPEN_DONE, "t=TOTAL DONE");
    CHECK(done_at(FVP_OPEN_TOTAL_MS), "done at TOTAL");
    CHECK(!done_at(FVP_OPEN_TOTAL_MS - 1), "not done before TOTAL");

    /* --- 3. logo alpha：淡入单调增 → 停留 255 → 淡出单调减 --- */
    prev = -1;
    for (i = 0; i <= 1200; i += 100) {
        int a = logo_at(300 + i);

        CHECK(a >= prev, "logo alpha monotonic at +%d (a=%d prev=%d)", i, a,
              prev);
        prev = a;
    }
    CHECK(logo_at(1499) > 200, "logo nearly full before HOLD");
    CHECK(logo_at(1500) == 255, "logo 255 at HOLD");
    CHECK(logo_at(2500) == 255, "logo stays 255 during HOLD");
    CHECK(logo_at(3600) == 255, "logo 255 at OUT start");
    CHECK(logo_at(4800) == 0, "logo 0 when the title fade starts");
    prev = 256;
    for (i = 0; i <= 1200; i += 100) {
        int a = logo_at(3600 + i);

        CHECK(a <= prev, "logo alpha monotonic down at +%d (a=%d)", i, a);
        prev = a;
    }

    /* --- 4. 标题上的白色遮罩：255 → 0 --- */
    CHECK(veil_at(4799) == 0, "veil 0 before TITLE_FADE");
    CHECK(veil_at(4800) == 255, "veil 255 at TITLE_FADE start");
    CHECK(veil_at(4800 + 10) > 0, "veil >0 shortly after the start");
    CHECK(veil_at(FVP_OPEN_TOTAL_MS) == 0, "veil 0 when done");
    prev = 256;
    for (i = 0; i <= 600; i += 50) {
        int a = veil_at(4800 + i);

        CHECK(a <= prev, "veil monotonic down at +%d (a=%d)", i, a);
        prev = a;
    }

    /* --- 5. 边界与鲁棒性 --- */
    fvp_openseq_init(&s);
    fvp_openseq_advance(&s, -100);
    CHECK(fvp_openseq_elapsed(&s) == 0, "negative ms ignored");
    fvp_openseq_advance(&s, FVP_OPEN_TOTAL_MS * 3);
    CHECK(fvp_openseq_elapsed(&s) == FVP_OPEN_TOTAL_MS,
          "clamped at total (got %d)", fvp_openseq_elapsed(&s));
    fvp_openseq_advance(&s, 100);
    CHECK(fvp_openseq_elapsed(&s) == FVP_OPEN_TOTAL_MS, "stays clamped");
    CHECK(fvp_openseq_phase(NULL) == FVP_OPEN_WHITE_HOLD,
          "NULL phase WHITE_HOLD");
    CHECK(fvp_openseq_logo_alpha(NULL) == 0, "NULL logo alpha");
    CHECK(fvp_openseq_veil_alpha(NULL) == 0, "NULL veil alpha");
    CHECK(fvp_openseq_elapsed(NULL) == 0, "NULL elapsed");
    CHECK(!fvp_openseq_done(NULL), "NULL not done");
    fvp_openseq_init(NULL);       /* 不该崩 */
    fvp_openseq_advance(NULL, 1); /* 不该崩 */

    /* --- 6. 全区间 alpha 有界；白底阶段两个 alpha 都是 0 --- */
    for (i = 0; i <= FVP_OPEN_TOTAL_MS; i += 37) {
        FvpOpenSeq q = at(i);
        int a = fvp_openseq_logo_alpha(&q);
        int v = fvp_openseq_veil_alpha(&q);

        CHECK(a >= 0 && a <= 255, "logo alpha in range at %d (a=%d)", i, a);
        CHECK(v >= 0 && v <= 255, "veil alpha in range at %d (v=%d)", i, v);
        if (fvp_openseq_phase(&q) == FVP_OPEN_WHITE_HOLD) {
            CHECK(a == 0 && v == 0, "WHITE_HOLD has both alphas 0 at %d", i);
        }
        if (fvp_openseq_phase(&q) == FVP_OPEN_TITLE_FADE) {
            CHECK(a == 0, "TITLE_FADE has logo alpha 0 at %d", i);
        }
    }

    printf("test_openseq: %d passed, %d failed\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}