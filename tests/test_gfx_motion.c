/* gfx 侧动画补间测试：补间 → 图元字段的接线 + 「未启用时不动画面」。 */
#include <stdio.h>
#include <string.h>

#include "../src/gfx.h"
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

static int field(Gfx *g, int id, int f)
{
    int v = -12345;

    if (gfx_get_prim(g, id, f, &v) != 0) {
        return -12345;
    }
    return v;
}

int main(void)
{
    Gfx *g = gfx_create(64, 36, NULL);
    int id;

    CHECK(g != NULL, "gfx_create");
    if (g == NULL) {
        return 1;
    }

    /* 1. 未启用：时间轴前进但图元不动（既有像素验收因此不受影响）。 */
    gfx_prim_reset(g, 5);
    gfx_set_prim(g, 5, GFX_F_X, 0);
    gfx_set_prim(g, 5, GFX_F_Y, 0);
    id = gfx_motion_add(g, 5, FVP_TWEEN_MOVE_XY, 0, 0, 100, 200, 1000, 1);
    CHECK(id > 0, "add move tween id=%d", id);
    CHECK(gfx_motion_enabled(g) == 0, "motion is disabled by default");
    gfx_motion_advance(g, 500);
    CHECK(field(g, 5, GFX_F_X) == 0, "disabled: X stays 0 (got %d)",
          field(g, 5, GFX_F_X));
    CHECK(gfx_motion_active(g) == 1, "tween still active (active=%d)",
          gfx_motion_active(g));

    /* 2. 启用后：半程 → 中点；结束 → 终值，补间被回收。 */
    gfx_motion_set_enabled(g, 1);
    gfx_motion_advance(g, 0);
    CHECK(field(g, 5, GFX_F_X) == 50, "enabled at 50%%: X=%d (want 50)",
          field(g, 5, GFX_F_X));
    CHECK(field(g, 5, GFX_F_Y) == 100, "enabled at 50%%: Y=%d (want 100)",
          field(g, 5, GFX_F_Y));
    gfx_motion_advance(g, 600);
    CHECK(field(g, 5, GFX_F_X) == 100, "finish: X=%d (want 100)",
          field(g, 5, GFX_F_X));
    CHECK(field(g, 5, GFX_F_Y) == 200, "finish: Y=%d (want 200)",
          field(g, 5, GFX_F_Y));
    CHECK(gfx_motion_active(g) == 0, "no active tween after finish (%d)",
          gfx_motion_active(g));

    /* 3. 透明度补间：255 → 0，半程 128（四舍五入）。 */
    gfx_set_prim(g, 6, GFX_F_ALPHA, 255);
    CHECK(gfx_motion_add(g, 6, FVP_TWEEN_ALPHA, 255, 0, 0, 0, 1000, 1) > 0,
          "add alpha tween");
    gfx_motion_advance(g, 500);
    CHECK(field(g, 6, GFX_F_ALPHA) == 128, "alpha at 50%% = %d (want 128)",
          field(g, 6, GFX_F_ALPHA));
    gfx_motion_advance(g, 600);
    CHECK(field(g, 6, GFX_F_ALPHA) == 0, "alpha at end = %d (want 0)",
          field(g, 6, GFX_F_ALPHA));

    /* 4. 缩放补间：1000 → 2000，半程 1500（引擎单位 *1000）。 */
    gfx_set_prim(g, 7, GFX_F_SCALE_X, 1000);
    gfx_set_prim(g, 7, GFX_F_SCALE_Y, 1000);
    CHECK(gfx_motion_add(g, 7, FVP_TWEEN_SCALE, 1000, 1000, 2000, 2000, 1000,
                         1) > 0,
          "add scale tween");
    gfx_motion_advance(g, 500);
    CHECK(field(g, 7, GFX_F_SCALE_X) == 1500, "scale_x at 50%% = %d",
          field(g, 7, GFX_F_SCALE_X));
    CHECK(field(g, 7, GFX_F_SCALE_Y) == 1500, "scale_y at 50%% = %d",
          field(g, 7, GFX_F_SCALE_Y));
    gfx_motion_advance(g, 600);
    CHECK(field(g, 7, GFX_F_SCALE_X) == 2000, "scale_x at end = %d",
          field(g, 7, GFX_F_SCALE_X));

    /* 5. 非法图元 id：加不进去。 */
    CHECK(gfx_motion_add(g, -1, FVP_TWEEN_MOVE_XY, 0, 0, 1, 1, 100, 1) == 0,
          "negative prim id rejected");
    CHECK(gfx_motion_add(g, GFX_PRIM_MAX, FVP_TWEEN_MOVE_XY, 0, 0, 1, 1, 100,
                         1) == 0,
          "prim id out of range rejected");

    /* 6. clear 清空补间。 */
    gfx_motion_add(g, 8, FVP_TWEEN_MOVE_XY, 0, 0, 10, 10, 1000, 1);
    gfx_motion_clear(g);
    CHECK(gfx_motion_active(g) == 0, "clear emptied the table");

    /* 7. NULL 安全。 */
    gfx_motion_set_enabled(NULL, 1);
    gfx_motion_advance(NULL, 10);
    gfx_motion_clear(NULL);
    CHECK(gfx_motion_active(NULL) == 0, "NULL gfx safe");

    gfx_destroy(g);
    printf("\ntest_gfx_motion: %d passed, %d failed\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}