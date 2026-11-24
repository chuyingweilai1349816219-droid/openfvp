/* 动画补间核心数学测试（时间轴 + 线性插值 + 回收）。 */
#include <stdio.h>
#include <string.h>

#include "../src/motion.h"

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
    FvpMotion m;
    int id;
    double x = 0.0;
    double y = 0.0;

    fvp_motion_init(&m);

    /* 1. 初始状态。 */
    CHECK(fvp_motion_active_count(&m) == 0, "fresh active=%zu",
          fvp_motion_active_count(&m));
    CHECK(fvp_motion_done_count(&m) == 0, "fresh done=%zu",
          fvp_motion_done_count(&m));

    /* 2. 位置补间：0 ms 在起点，中点插值，结束给终值。 */
    id = fvp_motion_add(&m, 12, FVP_TWEEN_MOVE_XY, 100.0, 200.0, 300.0, 400.0,
                        1000.0, 1, 1);
    CHECK(id > 0, "add move id=%d", id);
    CHECK(fvp_motion_value(&m, id, &x, &y) == 1, "value at t0");
    CHECK(x == 100.0 && y == 200.0, "start (%g,%g)", x, y);

    fvp_motion_advance(&m, 500.0);
    CHECK(fvp_motion_value(&m, id, &x, &y) == 1, "value at t=500");
    CHECK(x == 200.0 && y == 300.0, "midpoint (%g,%g)", x, y);
    CHECK(fvp_motion_active_count(&m) == 1, "still active at half way");
    CHECK(fvp_motion_done_count(&m) == 0, "not done at half way");

    fvp_motion_advance(&m, 600.0); /* 超过总时长 */
    CHECK(fvp_motion_value(&m, id, &x, &y) == 1, "value after end");
    CHECK(x == 300.0 && y == 400.0, "clamped to target (%g,%g)", x, y);
    CHECK(fvp_motion_done_count(&m) == 1, "done=%zu", fvp_motion_done_count(&m));

    /* 3. 回收：done 的条数清掉后表变空。 */
    fvp_motion_reap(&m);
    CHECK(m.n == 0, "reap left %zu entries", m.n);
    CHECK(fvp_motion_value(&m, id, &x, &y) == 0, "reaped id is gone");

    /* 4. dur = 0：立刻到位。 */
    id = fvp_motion_add(&m, 3, FVP_TWEEN_ALPHA, 0.0, 0.0, 255.0, 0.0, 0.0, 0, 1);
    CHECK(id > 0, "add zero-duration");
    CHECK(fvp_motion_value(&m, id, &x, &y) == 1, "zero-duration value");
    CHECK(x == 255.0, "zero-duration gives target immediately (%g)", x);
    CHECK(fvp_motion_done_count(&m) == 1, "zero-duration counts as done");
    fvp_motion_reap(&m);

    /* 5. 负数时长被拒。 */
    CHECK(fvp_motion_add(&m, 1, FVP_TWEEN_SCALE, 0, 0, 1, 1, -5.0, 1, 1) == 0,
          "negative duration rejected");
    CHECK(fvp_motion_add(NULL, 1, FVP_TWEEN_SCALE, 0, 0, 1, 1, 10.0, 1, 1) == 0,
          "NULL motion rejected");

    /* 6. 多条补间并存 + 各自独立。 */
    {
        int a = fvp_motion_add(&m, 1, FVP_TWEEN_MOVE_XY, 0, 0, 100, 100, 100.0,
                               1, 1);
        int b = fvp_motion_add(&m, 2, FVP_TWEEN_MOVE_XY, 0, 0, 0, 50, 200.0, 1,
                               1);

        CHECK(a > 0 && b > 0 && a != b, "two ids a=%d b=%d", a, b);
        fvp_motion_advance(&m, 100.0);
        CHECK(fvp_motion_value(&m, a, &x, &y) == 1 && x == 100.0,
              "a finished (%g)", x);
        CHECK(fvp_motion_value(&m, b, &x, &y) == 1 && y == 25.0,
              "b half way (%g)", y);
        CHECK(fvp_motion_active_count(&m) == 1, "one active");
        fvp_motion_reap(&m);
        CHECK(m.n == 1, "reap kept %zu", m.n);
        CHECK(fvp_motion_value(&m, b, &x, &y) == 1, "b survived the reap");
    }

    /* 7. clear 只清补间，时间轴保留。 */
    {
        double t = m.now_ms;

        fvp_motion_clear(&m);
        CHECK(m.n == 0, "clear emptied the table");
        CHECK(m.now_ms == t, "clear kept the clock (%g)", m.now_ms);
    }

    /* 8. 推进 0 / 负数不改变时间轴。 */
    {
        double t = m.now_ms;

        fvp_motion_advance(&m, 0.0);
        fvp_motion_advance(&m, -10.0);
        CHECK(m.now_ms == t, "clock unchanged (%g vs %g)", m.now_ms, t);
    }

    /* 9. NULL 安全。 */
    fvp_motion_advance(NULL, 10.0);
    fvp_motion_clear(NULL);
    fvp_motion_reap(NULL);
    CHECK(fvp_motion_active_count(NULL) == 0, "NULL active");
    CHECK(fvp_motion_value(NULL, 1, &x, &y) == 0, "NULL value");

    /* 10. Motion*Stop 的语义：按图元 + 种类撤补间。 */
    {
        int a1;
        int a2;
        int mv;
        int other;

        fvp_motion_clear(&m);
        a1 = fvp_motion_add(&m, 5, FVP_TWEEN_ALPHA, 0, 0, 0, 128, 100.0, 1, 1);
        a2 = fvp_motion_add(&m, 5, FVP_TWEEN_ALPHA, -1, 0, 0, 255, 300.0, 1, 1);
        mv = fvp_motion_add(&m, 5, FVP_TWEEN_MOVE_XY, 0, 0, 10, 20, 90.0, 1, 1);
        other = fvp_motion_add(&m, 6, FVP_TWEEN_ALPHA, 0, 0, 0, 64, 100.0, 1, 1);
        CHECK(a1 > 0 && a2 > 0 && mv > 0 && other > 0, "add 4 tweens");
        CHECK(m.n == 4, "table has 4 (%zu)", m.n);

        /* MotionAlphaStop(5)：只撤 prim 5 的 alpha，两条都撤掉。 */
        CHECK(fvp_motion_cancel(&m, 5, FVP_TWEEN_ALPHA) == 2, "cancel 2 alpha");
        CHECK(m.n == 2, "2 left (%zu)", m.n);
        CHECK(fvp_motion_value(&m, a1, &x, &y) == 0, "a1 gone");
        CHECK(fvp_motion_value(&m, a2, &x, &y) == 0, "a2 gone");
        CHECK(fvp_motion_value(&m, mv, &x, &y) == 1, "move kept");
        CHECK(fvp_motion_value(&m, other, &x, &y) == 1, "other prim kept");

        /* kind < 0：撤 prim 5 的全部种类。 */
        CHECK(fvp_motion_cancel(&m, 5, -1) == 1, "cancel rest of prim 5");
        CHECK(m.n == 1, "only prim 6 left (%zu)", m.n);
        CHECK(fvp_motion_value(&m, other, &x, &y) == 1, "prim 6 survived");

        /* 没有这类补间 / 没有这个图元 / NULL：都返回 0，不改表。 */
        CHECK(fvp_motion_cancel(&m, 5, FVP_TWEEN_ALPHA) == 0, "nothing to cancel");
        CHECK(fvp_motion_cancel(&m, 99, -1) == 0, "unknown prim");
        CHECK(fvp_motion_cancel(NULL, 5, -1) == 0, "NULL table");
        CHECK(m.n == 1, "table untouched (%zu)", m.n);

        /* 撤掉之后时间轴不受影响（Stop 只动补间表）。 */
        fvp_motion_advance(&m, 50.0);
        CHECK(fvp_motion_active_count(&m) == 1, "clock still drives the survivor");
        fvp_motion_clear(&m);
    }

    fvp_motion_free(&m);
    printf("\ntest_motion: %d passed, %d failed\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}