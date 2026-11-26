/* 文本渲染模式的单元测试（2026-11-26 起默认是“图元驱动 + 版面兜底”）。
 *
 *   bin/test_textprims.exe
 *
 * 语义（依据 exe 静态分析 + 实机反馈，见 docs/render-model.md §1.3）：
 *   PrimSetText(86)（handler 0x43BDC0）把图元重建成 type 5 的「文本图元」：
 *   prim+0x32 = 文本窗口号、prim+0x14/0x16 = x/y、prim+0x07 = alpha 255。
 *   原版通过这个图元把文本窗口“抬”到任意位置（可以浮在 CG 上，也可以跟着组动）。
 *   没有绑定图元的窗口按窗口自己的版面画；但 8（人名）/9（章节标题）是覆盖层，
 *   原版只在脚本给它们建了文本图元时才上屏 —— 未绑定就不画（否则会出现实机
 *   看不到的「标题」）。合成对话框/名牌只属于旧路径 --text-fixed-layout。
 *
 * 本测试用内建 8x16 ASCII 字体做像素级断言：
 *   1. 默认：正文窗口按版面画、**没有合成框**；未绑定的 9 号窗口不画
 *   2. 建 type 5 文本图元 → 文本改画在图元绝对位置（含父链累加）
 *   3. 图元 alpha 乘进文字颜色（128 比 255 暗）；draw=0 不画
 *   4. 渲染确定；旧路径（fixed）有合成框 + 按版面画，可随时切回
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
    (void)gfx_set_prim(g, id, GFX_F_PARENT, 0);
}

int main(void)
{
    Gfx *g;
    long at_prim;
    long at_prim_again;
    long half;
    long fixed_prim_region;
    long fixed_layout_region;
    long fixed_box_region;

    g = gfx_create(400, 1000, NULL);
    CHECK(g != NULL, "gfx_create");
    if (g == NULL) {
        return 1;
    }
    /* 正文窗口 6：默认版面 x=120,y=880（size 30）；图元放在 (20,40)。
     * 9 号窗口（章节标题）在旧版面里是 y=380、居中。 */
    (void)gfx_text_set_color(g, 6, 200, 200, 190);
    gfx_text_append(g, 6, "ABC");
    (void)gfx_text_set_color(g, 9, 240, 240, 240);
    gfx_text_append(g, 9, "T");

    /* --- 1. 默认：正文按版面画、没有合成框、未绑定的 9 号不画 --- */
    CHECK(gfx_text_fixed_layout(g) == 0, "prim-driven by default");
    CHECK(gfx_text_prim_bindings(g) == 0, "no bindings yet (got %d)",
          gfx_text_prim_bindings(g));
    gfx_render(g);
    CHECK(region_light(g, 110, 870, 220, 930) > 0,
          "body text at its own layout (got %ld)",
          region_light(g, 110, 870, 220, 930));
    CHECK(region_light(g, 60, 860, 105, 1000) == 0,
          "no synthetic dialogue box (got %ld)",
          region_light(g, 60, 860, 105, 1000));
    CHECK(region_light(g, 0, 360, 400, 470) == 0,
          "unbound overlay window 9 is NOT drawn (got %ld)",
          region_light(g, 0, 360, 400, 470));

    /* --- 2. 正文窗口绑到文本图元 → 画在图元位置，版面位置不再画 --- */
    make_text_prim(g, 7, 6, 20, 40, 255);
    CHECK(gfx_text_prim_bindings(g) == 1, "binding counted (got %d)",
          gfx_text_prim_bindings(g));
    gfx_render(g);
    at_prim = region_light(g, 20, 40, 120, 80);
    CHECK(at_prim > 0, "text drawn at the prim position (got %ld)", at_prim);
    CHECK(region_light(g, 110, 870, 220, 930) == 0,
          "not drawn twice at the layout position (got %ld)",
          region_light(g, 110, 870, 220, 930));

    /* --- 3. 覆盖层窗口 9 绑定后才画，而且位置由图元决定 --- */
    make_text_prim(g, 8, 9, 200, 300, 255);
    gfx_render(g);
    CHECK(region_light(g, 200, 300, 300, 340) > 0,
          "bound title window draws at its prim (got %ld)",
          region_light(g, 200, 300, 300, 340));
    CHECK(region_light(g, 0, 380, 400, 460) == 0,
          "title no longer at the fitted layout (got %ld)",
          region_light(g, 0, 380, 400, 460));

    /* --- 4. 父链累加：组 (400,500) 里的图元跟着动 --- */
    (void)gfx_set_prim(g, 8, GFX_F_PARENT, 20);
    (void)gfx_set_prim(g, 8, GFX_F_X, 0);
    (void)gfx_set_prim(g, 8, GFX_F_Y, 0);
    (void)gfx_set_prim(g, 20, GFX_F_TYPE, 1);
    (void)gfx_set_prim(g, 20, GFX_F_X, 100);
    (void)gfx_set_prim(g, 20, GFX_F_Y, 300);
    (void)gfx_set_prim(g, 20, GFX_F_DRAW, 1);
    (void)gfx_set_prim(g, 20, GFX_F_PARENT, 0);
    gfx_render(g);
    CHECK(region_light(g, 100, 300, 200, 340) > 0,
          "parent chain offset applied (got %ld)",
          region_light(g, 100, 300, 200, 340));
    CHECK(region_light(g, 200, 300, 300, 340) == 0,
          "old position empty after the move (got %ld)",
          region_light(g, 200, 300, 300, 340));
    /* 拆掉组，恢复原位 */
    (void)gfx_set_prim(g, 20, GFX_F_PARENT, -1);
    (void)gfx_set_prim(g, 8, GFX_F_PARENT, 0);
    (void)gfx_set_prim(g, 8, GFX_F_X, 200);
    (void)gfx_set_prim(g, 8, GFX_F_Y, 300);

    /* --- 5. 确定性 --- */
    gfx_render(g);
    at_prim_again = region_light(g, 20, 40, 120, 80);
    CHECK(at_prim_again == at_prim, "deterministic (%ld vs %ld)", at_prim,
          at_prim_again);

    /* --- 6. 图元 alpha 乘进文字：128 明显比 255 暗；draw=0 不画 --- */
    (void)gfx_set_prim(g, 7, GFX_F_ALPHA, 128);
    gfx_render(g);
    half = region_light(g, 20, 40, 120, 80);
    CHECK(half > 0 && half < at_prim, "alpha 128 dims the text (%ld < %ld)",
          half, at_prim);
    (void)gfx_set_prim(g, 7, GFX_F_ALPHA, 255);
    (void)gfx_set_prim(g, 7, GFX_F_DRAW, 0);
    gfx_render(g);
    CHECK(region_light(g, 20, 40, 120, 80) == 0, "hidden prim draws nothing");
    (void)gfx_set_prim(g, 7, GFX_F_DRAW, 1);
    (void)gfx_set_prim(g, 7, GFX_F_TW, -1);
    CHECK(gfx_text_prim_bindings(g) == 1, "tw<0 not a binding (got %d)",
          gfx_text_prim_bindings(g));
    (void)gfx_set_prim(g, 7, GFX_F_TW, 6);

    /* --- 7. 旧路径（--text-fixed-layout）：合成框 + 版面表 ---
     * 合成框只在正文窗口 0 / 人名窗口 8 有文本时才画，所以先给 0 号一行。 */
    gfx_text_append(g, 0, "B");
    gfx_set_text_fixed_layout(g, 1);
    CHECK(gfx_text_fixed_layout(g) == 1, "fixed layout on");
    gfx_render(g);
    fixed_prim_region = region_light(g, 20, 40, 120, 80);
    fixed_layout_region = region_light(g, 110, 870, 220, 930);
    fixed_box_region = region_light(g, 60, 860, 105, 1000);
    CHECK(fixed_layout_region > 0,
          "legacy: text at the layout position (got %ld)",
          fixed_layout_region);
    CHECK(fixed_box_region > 0, "legacy: synthetic box drawn (got %ld)",
          fixed_box_region);
    CHECK(fixed_prim_region == 0,
          "legacy: prim position not used (got %ld)", fixed_prim_region);

    /* --- 8. 回到默认（图元驱动）--- */
    gfx_set_text_fixed_layout(g, 0);
    gfx_render(g);
    CHECK(region_light(g, 20, 40, 120, 80) > 0,
          "back to prim-driven (got %ld)", region_light(g, 20, 40, 120, 80));
    CHECK(region_light(g, 60, 860, 105, 1000) == 0,
          "box gone again (got %ld)", region_light(g, 60, 860, 105, 1000));

    /* --- 9. 兼容旧名与 NULL 安全 --- */
    gfx_set_text_prims(g, 0); /* 等价 fixed=1 */
    CHECK(gfx_text_fixed_layout(g) == 1, "legacy alias maps to fixed");
    gfx_set_text_prims(g, 1); /* 等价 fixed=0 */
    CHECK(gfx_text_fixed_layout(g) == 0, "legacy alias maps to prim mode");
    gfx_set_text_fixed_layout(NULL, 1);
    gfx_set_text_prims(NULL, 1);
    CHECK(gfx_text_fixed_layout(NULL) == 0, "fixed(NULL)");
    CHECK(gfx_text_prims_enabled(NULL) == 0, "enabled(NULL)");
    CHECK(gfx_text_prim_bindings(NULL) == 0, "bindings(NULL)");
    gfx_text_clear(g, 6);
    gfx_text_clear(g, 9);
    gfx_text_clear(g, 0);
    gfx_render(g);
    CHECK(1, "NULL calls survive");

    gfx_destroy(g);
    printf("\ntest_textprims: %d passed, %d failed\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}