/* 文本图元定位（PrimSetText 86 / --text-prims）的单元测试。
 *
 *   bin/test_textprims.exe
 *
 * 语义来自静态分析（见 docs/render-model.md §1.3）：
 *   PrimSetText(86) 把图元重建成 type 5 的「文本图元」——prim+0x32 = 文本窗口号、
 *   prim+0x14/0x16 = x/y、prim+0x07 = alpha 255。也就是说文本窗口是**挂在图元上**
 *   画的：位置由图元的（含父链的）绝对坐标决定，alpha 跟着图元走。
 *
 * 这里用内建 8x16 ASCII 字体（不需要字体图集）在画布上做像素级断言：
 *   1. 默认（--text-prims 关）：文本画在 gfx_layout_default(层号) 的位置
 *   2. 打开：文本画在文本图元的绝对位置，且**不再**画在默认位置
 *   3. 图元 alpha 真的乘进文字颜色（128 比 255 暗）
 *   4. 渲染是确定的（同一状态连渲两次，像素和相同）
 *   5. 只统计 type==5 且 tw>=0 的图元；draw=0 的图元不画
 */
#include <stdio.h>
#include <string.h>

#include "../src/gfx.h"

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

/* 区域亮度总和：背景是不透明黑，文字的"墨"会把和抬起来。 */
static long region_light(Gfx *g, int x0, int y0, int x1, int y1)
{
    const uint8_t *c;
    int w = 0;
    int h = 0;
    long sum = 0;
    int y;

    c = gfx_canvas_rgba(g, &w, &h);
    if (c == NULL) {
        return -1;
    }
    if (x1 > w) {
        x1 = w;
    }
    if (y1 > h) {
        y1 = h;
    }
    if (x0 < 0) {
        x0 = 0;
    }
    if (y0 < 0) {
        y0 = 0;
    }
    for (y = y0; y < y1; y++) {
        int x;

        for (x = x0; x < x1; x++) {
            const uint8_t *p =
                c + ((size_t)y * (size_t)w + (size_t)x) * 4u;

            sum += (long)p[0] + (long)p[1] + (long)p[2];
        }
    }
    return sum;
}

/* 做一个文本图元：type 5 + 文本窗口号 + 位置 + alpha。 */
static void make_text_prim(Gfx *g, int id, int tw, int x, int y, int alpha)
{
    (void)gfx_set_prim(g, id, GFX_F_TYPE, 5);
    (void)gfx_set_prim(g, id, GFX_F_TW, tw);
    (void)gfx_set_prim(g, id, GFX_F_X, x);
    (void)gfx_set_prim(g, id, GFX_F_Y, y);
    (void)gfx_set_prim(g, id, GFX_F_ALPHA, alpha);
    (void)gfx_set_prim(g, id, GFX_F_DRAW, 1);
}

int main(void)
{
    Gfx *g;
    long prim_off;
    long prim_on;
    long prim_on_again;
    long prim_half;
    long layout_on;

    g = gfx_create(400, 1000, NULL);
    CHECK(g != NULL, "gfx_create");
    if (g == NULL) {
        return 1;
    }
    /* 层 6 不进对话框分支（只有层 0/8 会画对话框底），默认版面 x=120,y=880；
     * 文本图元放在 (20,40)，两块区域不重叠，断言干净。 */
    (void)gfx_text_set_color(g, 6, 200, 200, 190);
    gfx_text_append(g, 6, "ABC");

    /* --- 1. 默认关：文本在默认版面位置，图元区域干净 --- */
    CHECK(gfx_text_prims_enabled(g) == 0, "text prims off by default");
    CHECK(gfx_text_prim_bindings(g) == 0, "no bindings yet (got %d)",
          gfx_text_prim_bindings(g));
    gfx_render(g);
    CHECK(region_light(g, 20, 40, 120, 80) == 0,
          "prim region empty before the command (got %ld)",
          region_light(g, 20, 40, 120, 80));
    CHECK(region_light(g, 120, 880, 220, 920) > 0,
          "text drawn at the default layout (got %ld)",
          region_light(g, 120, 880, 220, 920));

    /* --- 2. 建文本图元后仍然按默认版面画（开关没开） --- */
    make_text_prim(g, 7, 6, 20, 40, 255);
    CHECK(gfx_text_prim_bindings(g) == 1, "binding counted (got %d)",
          gfx_text_prim_bindings(g));
    gfx_render(g);
    CHECK(region_light(g, 20, 40, 120, 80) == 0,
          "still nothing at the prim pos while the flag is off (got %ld)",
          region_light(g, 20, 40, 120, 80));

    /* --- 3. 打开开关：文本挪到图元位置，默认位置不再画 --- */
    gfx_set_text_prims(g, 1);
    CHECK(gfx_text_prims_enabled(g) == 1, "flag on");
    gfx_render(g);
    prim_on = region_light(g, 20, 40, 120, 80);
    layout_on = region_light(g, 120, 880, 220, 920);
    CHECK(prim_on > 0, "text drawn at the prim position (got %ld)", prim_on);
    CHECK(layout_on == 0, "default layout no longer drawn twice (got %ld)",
          layout_on);

    /* --- 4. 确定性：连渲两次结果一样 --- */
    gfx_render(g);
    prim_on_again = region_light(g, 20, 40, 120, 80);
    CHECK(prim_on_again == prim_on, "deterministic (%ld vs %ld)", prim_on,
          prim_on_again);

    /* --- 5. 图元 alpha 乘进文字：128 明显比 255 暗 --- */
    (void)gfx_set_prim(g, 7, GFX_F_ALPHA, 128);
    gfx_render(g);
    prim_half = region_light(g, 20, 40, 120, 80);
    CHECK(prim_half > 0 && prim_half < prim_on,
          "alpha 128 dims the text (%ld < %ld)", prim_half, prim_on);

    /* --- 6. 关掉开关回到默认版面 --- */
    (void)gfx_set_prim(g, 7, GFX_F_ALPHA, 255);
    gfx_set_text_prims(g, 0);
    gfx_render(g);
    prim_off = region_light(g, 20, 40, 120, 80);
    CHECK(prim_off == 0, "back to the layout position (got %ld)", prim_off);
    CHECK(region_light(g, 120, 880, 220, 920) > 0,
          "layout text is back (got %ld)",
          region_light(g, 120, 880, 220, 920));

    /* --- 7. draw=0 的文本图元不画；tw<0 的不算绑定 --- */
    gfx_set_text_prims(g, 1);
    (void)gfx_set_prim(g, 7, GFX_F_DRAW, 0);
    gfx_render(g);
    CHECK(region_light(g, 20, 40, 120, 80) == 0, "hidden prim draws nothing");
    (void)gfx_set_prim(g, 7, GFX_F_DRAW, 1);
    CHECK(gfx_text_prim_bindings(g) == 1, "binding still counted (got %d)",
          gfx_text_prim_bindings(g));
    (void)gfx_set_prim(g, 7, GFX_F_TW, -1);
    CHECK(gfx_text_prim_bindings(g) == 0, "tw<0 not a binding (got %d)",
          gfx_text_prim_bindings(g));

    /* --- 8. NULL 安全 --- */
    gfx_set_text_prims(NULL, 1);
    CHECK(gfx_text_prims_enabled(NULL) == 0, "enabled(NULL)");
    CHECK(gfx_text_prim_bindings(NULL) == 0, "bindings(NULL)");
    gfx_text_clear(g, 6);
    gfx_render(g);
    CHECK(1, "NULL calls survive");

    gfx_destroy(g);
    printf("\ntest_textprims: %d passed, %d failed\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}