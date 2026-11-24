/* 标题菜单的**像素级渲染验证**：把布局模块 + 绘制 API 串起来，量画出来的墨迹。
 *
 *   bin/test_titlemenu_draw.exe
 *
 * 验收目标（来自原版实测，见 docs/title-menu.md）：
 *   * 5 项**右对齐**到 x = 1769（客户区 1920 宽）
 *   * 行距 100 px
 *   * 墨迹上沿从 y = 511 起
 *   * 墨迹高 40..42 px —— 这一条**只在内建字体下无法完全复现**（内建是 8x16 位图，
 *     按 56 px 放大后墨迹高由位图自身的行数决定），所以这里只断言"在合理区间内"，
 *     并把这一点写进文档，不假装等价于原版的衬线字体。
 *
 * 量法：canvas 是不透明黑，墨迹把 R+G+B 抬起来；按行/列扫描非零像素得到墨迹包围盒。
 */
#include <stdio.h>
#include <string.h>

#include "../src/gfx.h"
#include "../src/titlemenu.h"

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

/* 测宽回调：走 gfx 的同一套字体量宽，保证"量"和"画"用的是同一个字体。 */
static int measure_cb(void *ctx, const char *utf8, int font_px, int *width)
{
    Gfx *g = (Gfx *)ctx;
    int w = gfx_text_width(g, utf8, font_px);

    if (w <= 0) {
        return 0;
    }
    *width = w;
    return 1;
}

/* 在 [x0,x1) x [y0,y1) 里找墨迹包围盒；没有墨迹返回 0。 */
static int ink_box(Gfx *g, int x0, int y0, int x1, int y1, int *bx0, int *by0,
                   int *bx1, int *by1)
{
    const uint8_t *c;
    int w = 0;
    int h = 0;
    int x;
    int y;
    int found = 0;
    int minx = 0, miny = 0, maxx = 0, maxy = 0;

    c = gfx_canvas_rgba(g, &w, &h);
    if (c == NULL) {
        return 0;
    }
    if (x1 > w) {
        x1 = w;
    }
    if (y1 > h) {
        y1 = h;
    }
    for (y = y0; y < y1; ++y) {
        for (x = x0; x < x1; ++x) {
            const uint8_t *p = c + ((size_t)y * (size_t)w + (size_t)x) * 4u;

            if ((int)p[0] + (int)p[1] + (int)p[2] > 12) {
                if (!found) {
                    minx = maxx = x;
                    miny = maxy = y;
                    found = 1;
                } else {
                    if (x < minx) minx = x;
                    if (x > maxx) maxx = x;
                    if (y < miny) miny = y;
                    if (y > maxy) maxy = y;
                }
            }
        }
    }
    if (!found) {
        return 0;
    }
    if (bx0 != NULL) *bx0 = minx;
    if (by0 != NULL) *by0 = miny;
    if (bx1 != NULL) *bx1 = maxx;
    if (by1 != NULL) *by1 = maxy;
    return 1;
}

int main(void)
{
    Gfx *g;
    FvpTitleMenu m;
    int i;
    int prev_cy = 0;
    int pitches_ok = 1;

    g = gfx_create(1920, 1080, NULL);
    CHECK(g != NULL, "gfx_create 1920x1080");
    if (g == NULL) {
        return 1;
    }
    fvp_title_init(&m);

    /* --- 1. 量宽：内建字体下 5 个标签都能量出正宽度 --- */
    for (i = 0; i < FVP_TITLE_ITEMS; ++i) {
        int w = gfx_text_width(g, fvp_title_label(i), m.font_px);

        CHECK(w > 0, "measure '%s' = %d", fvp_title_label(i), w);
    }
    CHECK(gfx_text_width(g, "ABC", 0) == 0, "width px=0 -> 0");
    CHECK(gfx_text_width(g, NULL, 56) == 0, "width NULL text -> 0");
    CHECK(gfx_text_width(NULL, "ABC", 56) == 0, "width NULL gfx -> 0");

    /* --- 2. 按布局右对齐画 5 项，逐项量墨迹 ---
     * gfx_render() 会把画布按图元/文本窗口重建成"当前帧"，所以引擎侧 UI 的
     * 叠加文字必须在 render **之后**画（外壳里也是这个顺序：先合成场景，
     * 再画 UI 覆盖层）。 */
    {
        int rights[FVP_TITLE_ITEMS];
        int lefts[FVP_TITLE_ITEMS];

        gfx_render(g);
        for (i = 0; i < FVP_TITLE_ITEMS; ++i) {
            int x0 = 0, y0 = 0, x1 = 0, y1 = 0;
            int w;

            CHECK(fvp_title_item_rect(&m, i, measure_cb, g, &x0, &y0, &x1,
                                      &y1) == 1,
                  "rect %d", i);
            w = gfx_text_width(g, fvp_title_label(i), m.font_px);
            CHECK(w == x1 - x0, "rect width %d == measured %d", x1 - x0, w);
            CHECK(gfx_draw_text_line(g, fvp_title_label(i), x0, y0, m.font_px,
                                     235, 235, 240, 255) > 0,
                  "draw item %d", i);
            rights[i] = x1;
            lefts[i] = x0;
        }

        for (i = 0; i < FVP_TITLE_ITEMS; ++i) {
            int bx0 = 0, by0 = 0, bx1 = 0, by1 = 0;
            int row_top = m.y_first + i * m.pitch;
            int cy;

            CHECK(ink_box(g, 0, row_top - 8, 1920, row_top + m.pitch,
                          &bx0, &by0, &bx1, &by1) == 1,
                  "ink found for item %d (box %d,%d..%d,%d)", i, bx0, by0, bx1,
                  by1);
            /* 右边界：原版 1769；我们右对齐到同一列，允许 ±1 px（字形的右 bearing） */
            CHECK(bx1 <= rights[i] && bx1 >= rights[i] - 2,
                  "item %d ink right %d (layout %d)", i, bx1, rights[i]);
            CHECK(bx1 >= 1766 && bx1 <= 1769,
                  "item %d ink right %d should be ~1769", i, bx1);
            /* 左边界与布局给的位置一致（±2 px 右 bearing） */
            CHECK(bx0 >= lefts[i] - 1 && bx0 <= lefts[i] + 2,
                  "item %d ink left %d (layout %d)", i, bx0, lefts[i]);
            /* 墨迹上沿：内建 8x16 位图的字形本身在 cell 顶部留了约 2 行空白，
             * 放大到 56 px 后就是 7 px 的 bearing。布局给的是"文本原点"，
             * 所以墨迹会比它低 0..10 px —— 这是字体度量差异，不是布局错。 */
            CHECK(by0 >= row_top && by0 <= row_top + 10,
                  "item %d ink top %d (layout %d, builtin bearing)", i, by0,
                  row_top);
            /* 墨迹高：内建 8x16 放大到 56 px 后按位图行数决定，40..56 都算合理 */
            CHECK(by1 - by0 + 1 >= 30 && by1 - by0 + 1 <= 60,
                  "item %d ink height %d", i, by1 - by0 + 1);
            cy = (by0 + by1) / 2;
            if (i > 0) {
                int d = cy - prev_cy;

                if (d < 98 || d > 102) {
                    pitches_ok = 0;
                    CHECK(0, "pitch %d -> %d = %d (want ~100)", i - 1, i, d);
                }
            }
            prev_cy = cy;
        }
        CHECK(pitches_ok, "row pitch is 100 +/- 2 for all items");
    }

    /* --- 3. 命中判定用的是同一套布局：点每一项的中心都应命中 --- */
    for (i = 0; i < FVP_TITLE_ITEMS; ++i) {
        int x0 = 0, y0 = 0, x1 = 0, y1 = 0;
        int cx;

        CHECK(fvp_title_item_rect(&m, i, measure_cb, g, &x0, &y0, &x1, &y1) == 1,
              "rect %d again", i);
        cx = (x0 + x1) / 2;
        CHECK(fvp_title_hit(&m, cx, (y0 + y1) / 2, measure_cb, g) == i,
              "hit item %d at (%d,%d)", i, cx, (y0 + y1) / 2);
    }

    /* --- 4. 确定性与重画：render 之后再画，右边界仍然是 1769 --- */
    gfx_destroy(g);
    g = gfx_create(1920, 1080, NULL);
    CHECK(g != NULL, "gfx_create again");
    if (g != NULL) {
        int bx0 = 0, by0 = 0, bx1 = 0, by1 = 0;
        int x0 = 0, y0 = 0, x1 = 0, y1 = 0;

        gfx_render(g);
        (void)fvp_title_item_rect(&m, 0, measure_cb, g, &x0, &y0, &x1, &y1);
        (void)gfx_draw_text_line(g, fvp_title_label(0), x0, y0, m.font_px, 235,
                                 235, 240, 255);
        CHECK(ink_box(g, 0, 500, 1920, 580, &bx0, &by0, &bx1, &by1) == 1,
              "second render has ink (box %d,%d..%d,%d)", bx0, by0, bx1, by1);
        CHECK(bx1 >= 1766 && bx1 <= 1769, "second render right edge %d", bx1);
        gfx_destroy(g);
    }

    printf("test_titlemenu_draw: %d passed, %d failed\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}