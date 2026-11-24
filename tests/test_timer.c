/* 定时器单元测试：TimerSet / TimerGet / TimerSuspend 的存储与换算。
 *
 * 语义来自 docs/vm-commands.md §5 的静态分析：
 *   TimerSet(id 0..15, ms 1..100000) → {active=1, elapsed=0, duration=ms}
 *   TimerGet(id, opt)                → 已过毫秒；给了 opt 返回 elapsed*opt/duration
 *
 * 这一层不持有时钟，now_ms 由调用方给（VM 传 vm.virtual_ms）。默认没开
 * --timeline 时它恒为 0，所以 TimerGet 恒返回 0 —— 与「未实现返回 0」的旧
 * 行为一致，这是基线不被改动的原因。
 */
#include <stdio.h>
#include <string.h>

#include "../src/timer.h"

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

int main(void)
{
    FvpTimer t[FVP_TIMER_COUNT];

    /* --- 1. 复位 --- */
    memset(t, 0xFF, sizeof t);
    fvp_timer_reset(t, FVP_TIMER_COUNT);
    CHECK(fvp_timer_active_count(t, FVP_TIMER_COUNT) == 0,
          "reset clears every timer");
    CHECK(fvp_timer_get(t, FVP_TIMER_COUNT, 0, 0, 1000) == 0,
          "reset timer reads 0");

    /* --- 2. set：激活、记起点、钳制时长 --- */
    CHECK(fvp_timer_set(t, FVP_TIMER_COUNT, 3, 500, 100) == 1, "set id 3");
    CHECK(t[3].active == 1, "id 3 active");
    CHECK(t[3].start_ms == 100, "start_ms recorded (%llu)",
          (unsigned long long)t[3].start_ms);
    CHECK(t[3].duration_ms == 500, "duration recorded");
    CHECK(fvp_timer_active_count(t, FVP_TIMER_COUNT) == 1, "one active");

    /* --- 3. elapsed / get 的基本换算 --- */
    CHECK(fvp_timer_elapsed(t, FVP_TIMER_COUNT, 3, 100) == 0,
          "no time passed yet");
    CHECK(fvp_timer_elapsed(t, FVP_TIMER_COUNT, 3, 350) == 250,
          "elapsed = now - start (%llu)",
          (unsigned long long)fvp_timer_elapsed(t, FVP_TIMER_COUNT, 3, 350));
    CHECK(fvp_timer_get(t, FVP_TIMER_COUNT, 3, 0, 350) == 250,
          "get with opt 0 returns elapsed");
    CHECK(fvp_timer_get(t, FVP_TIMER_COUNT, 3, 100, 350) == 50,
          "get with opt = elapsed*100/duration (250*100/500)");
    CHECK(fvp_timer_get(t, FVP_TIMER_COUNT, 3, 1000, 350) == 500,
          "opt 1000 = permille of duration");
    CHECK(fvp_timer_get(t, FVP_TIMER_COUNT, 3, 0, 50) == 0,
          "clock before start clamps to 0");

    /* --- 4. opt 换算的取整：不因整除顺序而偏 --- */
    CHECK(fvp_timer_get(t, FVP_TIMER_COUNT, 3, 33, 200) == 6,
          "elapsed 100, opt 33, duration 500 -> 6 (got %d)",
          fvp_timer_get(t, FVP_TIMER_COUNT, 3, 33, 200));

    /* --- 5. 未激活 / 越界 id --- */
    CHECK(fvp_timer_get(t, FVP_TIMER_COUNT, 4, 0, 9999) == 0,
          "unset timer reads 0");
    CHECK(fvp_timer_set(t, FVP_TIMER_COUNT, -1, 100, 0) == 0, "id -1 rejected");
    CHECK(fvp_timer_set(t, FVP_TIMER_COUNT, 16, 100, 0) == 0, "id 16 rejected");
    CHECK(fvp_timer_get(t, FVP_TIMER_COUNT, 16, 0, 9999) == 0,
          "get id 16 rejected");
    CHECK(fvp_timer_elapsed(t, FVP_TIMER_COUNT, -1, 9999) == 0,
          "elapsed id -1 rejected");

    /* --- 6. ms 钳制 --- */
    fvp_timer_reset(t, FVP_TIMER_COUNT);
    CHECK(fvp_timer_set(t, FVP_TIMER_COUNT, 0, 0, 0) == 1, "ms 0 accepted");
    CHECK(t[0].duration_ms == FVP_TIMER_MS_MIN,
          "ms 0 clamped up to %d (got %d)", FVP_TIMER_MS_MIN, t[0].duration_ms);
    CHECK(fvp_timer_set(t, FVP_TIMER_COUNT, 1, 999999, 0) == 1,
          "ms 999999 accepted");
    CHECK(t[1].duration_ms == FVP_TIMER_MS_MAX,
          "ms clamped down to %d (got %d)", FVP_TIMER_MS_MAX, t[1].duration_ms);

    /* --- 7. 重新 set 会重置起点（不是累加） --- */
    CHECK(fvp_timer_set(t, FVP_TIMER_COUNT, 2, 1000, 5000) == 1, "set at 5000");
    CHECK(fvp_timer_elapsed(t, FVP_TIMER_COUNT, 2, 5000) == 0,
          "restart resets elapsed");
    CHECK(fvp_timer_elapsed(t, FVP_TIMER_COUNT, 2, 5300) == 300,
          "elapsed counts from the restart (%llu)",
          (unsigned long long)fvp_timer_elapsed(t, FVP_TIMER_COUNT, 2, 5300));

    /* --- 8. 16 个定时器互不干扰 --- */
    fvp_timer_reset(t, FVP_TIMER_COUNT);
    {
        int i;
        int ok = 1;

        for (i = 0; i < FVP_TIMER_COUNT; ++i) {
            if (!fvp_timer_set(t, FVP_TIMER_COUNT, i, 100 * (i + 1),
                               (uint64_t)i)) {
                ok = 0;
            }
        }
        CHECK(ok, "all 16 timers can be set");
        CHECK(fvp_timer_active_count(t, FVP_TIMER_COUNT) == FVP_TIMER_COUNT,
              "16 active (%d)", fvp_timer_active_count(t, FVP_TIMER_COUNT));
        for (i = 0; i < FVP_TIMER_COUNT; ++i) {
            if (fvp_timer_elapsed(t, FVP_TIMER_COUNT, i, (uint64_t)i + 10) != 10) {
                ok = 0;
            }
        }
        CHECK(ok, "each timer counts independently");
    }

    /* --- 9. 溢出夹到 INT32_MAX --- */
    {
        uint64_t huge = 0xFFFFFFF0ULL;

        fvp_timer_reset(t, FVP_TIMER_COUNT);
        CHECK(fvp_timer_set(t, FVP_TIMER_COUNT, 0, 1, 0) == 1, "set for overflow");
        CHECK(fvp_timer_get(t, FVP_TIMER_COUNT, 0, 0, huge) == 0x7FFFFFFF,
              "elapsed clamps to INT32_MAX (got %d)",
              fvp_timer_get(t, FVP_TIMER_COUNT, 0, 0, huge));
    }

    /* --- 10. NULL / 0 计数安全 --- */
    CHECK(fvp_timer_set(NULL, FVP_TIMER_COUNT, 0, 100, 0) == 0, "set(NULL)");
    CHECK(fvp_timer_get(NULL, FVP_TIMER_COUNT, 0, 0, 0) == 0, "get(NULL)");
    CHECK(fvp_timer_elapsed(NULL, FVP_TIMER_COUNT, 0, 0) == 0, "elapsed(NULL)");
    CHECK(fvp_timer_active_count(NULL, FVP_TIMER_COUNT) == 0,
          "active_count(NULL)");
    fvp_timer_reset(NULL, FVP_TIMER_COUNT); /* 不应崩 */
    CHECK(1, "reset(NULL) survives");
    CHECK(fvp_timer_set(t, 0, 0, 100, 0) == 0, "count 0 rejects");

    printf("\n%d passed, %d failed\n", g_pass, g_fail);
    if (g_fail == 0) {
        printf("test_timer: PASS\n");
        return 0;
    }
    printf("test_timer: FAIL\n");
    return 1;
}