/* test_gfx_xform.c -- 标题进场动画的两块底层：缩放直贴 + 脚本补间。
 *
 *   bin/test_gfx_xform.exe
 *
 * 为什么有这个测试：标题进场动画的参数是从**脚本**里读出来的（`src/titleanim.h`，
 * 出处 `ref/title-scene.json`），时间轴复用已经单测过的 `src/motion.c`
 * （mode 1..5 的缓动曲线来自静态分析）。这里把两件事焊在一起验证：
 *
 *   1. `gfx_draw_image_xform()` 在 scale=1000 / alpha=255 时与静态绘制
 *      **字节等价** —— 所以动画结束后标题与不做动画完全一样（冻结基线不受影响）；
 *   2. scale / alpha 真的生效（画出包围盒 = round(w*scale)，alpha=0 不画）；
 *   3. 用脚本里的参数建两条补间（alpha 0→255、scale 10000→1000，1400 ms，mode 3），
 *      验证 t=0 / t=700 / t=1400 的取值符合 mode 3 的缓出曲线，并且
 *      **t>=1400 后画出的画布与静态绘制逐字节相同**。
 */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../src/gfx.h"
#include "../src/motion.h"
#include "../src/titleanim.h"

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

/* 40x30 的不透明方块：R=200 G=100 B=50 A=255。 */
#define IMG_W 40
#define IMG_H 30

static int fake_load(void *ud, const char *name, GfxImage *out)
{
    size_t n;
    size_t i;

    (void)ud;
    (void)name;
    n = (size_t)IMG_W * (size_t)IMG_H;
    out->rgba = (uint8_t *)malloc(n * 4u);
    if (out->rgba == NULL) {
        return 1;
    }
    for (i = 0; i < n; ++i) {
        out->rgba[i * 4u + 0u] = 200;
        out->rgba[i * 4u + 1u] = 100;
        out->rgba[i * 4u + 2u] = 50;
        out->rgba[i * 4u + 3u] = 255;
    }
    out->w = IMG_W;
    out->h = IMG_H;
    out->x = 0;
    out->y = 0;
    out->frames = 1;
    return 0;
}

static void fake_free(void *ud, GfxImage *img)
{
    (void)ud;
    free(img->rgba);
    img->rgba = NULL;
}

static const uint8_t *canvas_of(Gfx *g, int *w, int *h)
{
    return gfx_canvas_rgba(g, w, h);
}

/* 画布抄一份，用来比较两次绘制是否逐字节相同。 */
static uint8_t *canvas_snapshot(Gfx *g, size_t *len)
{
    const uint8_t *c;
    uint8_t *copy;
    int w = 0;
    int h = 0;

    c = canvas_of(g, &w, &h);
    if (c == NULL) {
        return NULL;
    }
    *len = (size_t)w * (size_t)h * 4u;
    copy = (uint8_t *)malloc(*len);
    if (copy != NULL) {
        memcpy(copy, c, *len);
    }
    return copy;
}

/* 非背景像素的包围盒（背景是不透明黑）。 */
static int ink_box(Gfx *g, int *bx0, int *by0, int *bx1, int *by1, long *count)
{
    const uint8_t *c;
    int w = 0;
    int h = 0;
    int x;
    int y;
    int found = 0;
    int minx = 0, miny = 0, maxx = 0, maxy = 0;
    long n = 0;

    c = canvas_of(g, &w, &h);
    if (c == NULL) {
        return 0;
    }
    for (y = 0; y < h; ++y) {
        for (x = 0; x < w; ++x) {
            const uint8_t *p = c + ((size_t)y * (size_t)w + (size_t)x) * 4u;

            if ((int)p[0] + (int)p[1] + (int)p[2] > 0) {
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
                ++n;
            }
        }
    }
    if (!found) {
        return 0;
    }
    *bx0 = minx;
    *by0 = miny;
    *bx1 = maxx;
    *by1 = maxy;
    *count = n;
    return 1;
}

static Gfx *make_canvas(void)
{
    GfxHost host;
    Gfx *g;

    memset(&host, 0, sizeof host);
    host.load_image = fake_load;
    host.free_image = fake_free;
    g = gfx_create(200, 150, &host);
    if (g == NULL) {
        return NULL;
    }
    if (gfx_graph_load(g, 1, "graph/xform") != 0) {
        gfx_destroy(g);
        return NULL;
    }
    return g;
}

/* 同一块画布上重画：把 canvas 清成不透明黑。 */
static void clear_canvas(Gfx *g)
{
    gfx_fill_canvas(g, 0, 0, 0, 255);
}

int main(void)
{
    Gfx *g;
    uint8_t *base;
    uint8_t *other;
    size_t len;
    size_t len2;
    int w = 0;
    int h = 0;
    int bx0 = 0, by0 = 0, bx1 = 0, by1 = 0;
    long count = 0;
    FvpMotion m;
    int alpha_id;
    int scale_id;
    double av = 0.0;
    double sx = 0.0;
    double sy = 0.0;

    printf("== 1. scale=1000 / alpha=255 时与静态绘制字节等价 ==\n");
    g = make_canvas();
    CHECK(g != NULL, "gfx_create + graph_load");
    if (g == NULL) {
        return 1;
    }
    clear_canvas(g);
    (void)gfx_draw_image_at(g, 1, 10, 10);
    base = canvas_snapshot(g, &len);
    CHECK(base != NULL, "snapshot A");
    clear_canvas(g);
    CHECK(gfx_draw_image_xform(g, 1, 10, 10, 1000, 1000, 255) == 1,
          "identity xform 返回 1");
    other = canvas_snapshot(g, &len2);
    CHECK(other != NULL && len == len2, "snapshot B");
    CHECK(base != NULL && other != NULL && memcmp(base, other, len) == 0,
          "identity xform == gfx_draw_image_at（逐字节）");
    free(other);

    printf("== 2. alpha 与 gfx_draw_image_alpha 等价 ==\n");
    clear_canvas(g);
    (void)gfx_draw_image_alpha(g, 1, 10, 10, 128);
    other = canvas_snapshot(g, &len2);
    CHECK(other != NULL, "snapshot C");
    clear_canvas(g);
    (void)gfx_draw_image_xform(g, 1, 10, 10, 1000, 1000, 128);
    CHECK(other != NULL && len == len2 && memcmp(other, canvas_of(g, &w, &h),
          len) == 0, "identity xform 的 alpha 路径与 gfx_draw_image_alpha 等价");
    free(other);

    printf("== 3. 缩放真的生效（包围盒 = round(w*scale)）==\n");
    clear_canvas(g);
    (void)gfx_draw_image_xform(g, 1, 10, 10, 500, 500, 255);
    CHECK(ink_box(g, &bx0, &by0, &bx1, &by1, &count) == 1, "500‰ 有墨迹");
    CHECK(bx0 == 10 && by0 == 10, "左上角不动 (10,10)，实际 (%d,%d)", bx0, by0);
    CHECK(bx1 - bx0 + 1 == 20 && by1 - by0 + 1 == 15,
          "50%% 缩放后 20x15，实际 %dx%d", bx1 - bx0 + 1, by1 - by0 + 1);
    CHECK(count == 20L * 15L, "像素数 300，实际 %ld", count);

    clear_canvas(g);
    (void)gfx_draw_image_xform(g, 1, 10, 10, 2000, 2000, 255);
    CHECK(ink_box(g, &bx0, &by0, &bx1, &by1, &count) == 1, "2000‰ 有墨迹");
    CHECK(bx1 - bx0 + 1 == 80 && by1 - by0 + 1 == 60,
          "200%% 缩放后 80x60，实际 %dx%d", bx1 - bx0 + 1, by1 - by0 + 1);

    printf("== 4. alpha=0 不画；alpha 越界钳位；非法 scale 当 1.0 ==\n");
    clear_canvas(g);
    base = canvas_snapshot(g, &len);
    CHECK(base != NULL, "snapshot D");
    CHECK(gfx_draw_image_xform(g, 1, 10, 10, 1000, 1000, 0) == 0,
          "alpha=0 返回 0（不画）");
    other = canvas_snapshot(g, &len2);
    CHECK(base != NULL && other != NULL && memcmp(base, other, len) == 0,
          "alpha=0 画布未变");
    free(other);
    clear_canvas(g);
    (void)gfx_draw_image_xform(g, 1, 10, 10, 1000, 1000, 300);
    other = canvas_snapshot(g, &len2);
    clear_canvas(g);
    (void)gfx_draw_image_at(g, 1, 10, 10);
    CHECK(other != NULL && memcmp(other, canvas_of(g, &w, &h), len) == 0,
          "alpha=300 钳成 255（与静态满不透明相同）");
    free(other);
    clear_canvas(g);
    CHECK(gfx_draw_image_xform(g, 1, 10, 10, 0, -5, 255) == 1,
          "scale<=0 不崩、按 1.0 处理");
    CHECK(ink_box(g, &bx0, &by0, &bx1, &by1, &count) == 1 &&
          bx1 - bx0 + 1 == IMG_W, "scale<=0 时按原尺寸画（%d 宽）",
          bx1 - bx0 + 1);
    free(base);

    printf("== 5. 脚本参数 + motion：t=0 / 700 / 1400 的取值 ==\n");
    fvp_motion_init(&m);
    alpha_id = fvp_motion_add(&m, FVP_TITLEANIM_LOGO_SLOT, FVP_TWEEN_ALPHA,
                              FVP_TITLEANIM_ALPHA_FROM, 0.0,
                              FVP_TITLEANIM_ALPHA_TO, 0.0,
                              FVP_TITLEANIM_DUR_MS, FVP_TITLEANIM_EASE_MODE, 0);
    scale_id = fvp_motion_add(&m, FVP_TITLEANIM_LOGO_SLOT, FVP_TWEEN_SCALE,
                              FVP_TITLEANIM_SCALE_FROM,
                              FVP_TITLEANIM_SCALE_FROM,
                              FVP_TITLEANIM_SCALE_TO, FVP_TITLEANIM_SCALE_TO,
                              FVP_TITLEANIM_DUR_MS, FVP_TITLEANIM_EASE_MODE, 0);
    CHECK(alpha_id > 0 && scale_id > 0, "两条补间都建起来了");
    CHECK(fvp_motion_active_count(&m) == 2, "两条补间都在跑");
    CHECK(fvp_motion_value(&m, alpha_id, &av, NULL) == 1 && av == 0.0,
          "t=0 alpha=0（实际 %.1f）", av);
    CHECK(fvp_motion_value(&m, scale_id, &sx, &sy) == 1 &&
          sx == 10000.0 && sy == 10000.0, "t=0 scale=10000（实际 %.1f/%.1f）",
          sx, sy);

    /* mode 3 = 缓出（二次）：p=0.5 -> e = 1-(0.5)^2 = 0.75 */
    fvp_motion_advance(&m, 700.0);
    (void)fvp_motion_value(&m, alpha_id, &av, NULL);
    (void)fvp_motion_value(&m, scale_id, &sx, &sy);
    CHECK(fabs(av - 191.25) < 1e-6, "t=700 alpha=191.25（缓出 0.75*255，实际 %.2f）",
          av);
    CHECK(fabs(sx - 3250.0) < 1e-6,
          "t=700 scale=3250（10000+(1000-10000)*0.75，实际 %.1f）", sx);
    CHECK(sx == sy, "两个轴同值");
    CHECK(fvp_motion_active_count(&m) == 2, "中途仍在跑");

    fvp_motion_advance(&m, 700.0);
    (void)fvp_motion_value(&m, alpha_id, &av, NULL);
    (void)fvp_motion_value(&m, scale_id, &sx, &sy);
    CHECK(av == 255.0 && sx == 1000.0 && sy == 1000.0,
          "t=1400 终值 alpha=255 / scale=1000（实际 %.1f / %.1f）", av, sx);
    CHECK(fvp_motion_active_count(&m) == 0, "t=1400 补间都结束了");

    printf("== 6. 动画结束时画出的画布与静态绘制逐字节相同 ==\n");
    clear_canvas(g);
    (void)gfx_draw_image_at(g, 1, 10, 10);
    base = canvas_snapshot(g, &len);
    CHECK(base != NULL, "snapshot E");
    clear_canvas(g);
    (void)gfx_draw_image_xform(g, 1, 10, 10, (int)(sx + 0.5), (int)(sy + 0.5),
                               (int)(av + 0.5));
    CHECK(base != NULL &&
          memcmp(base, canvas_of(g, &w, &h), len) == 0,
          "终值画出的画布 == 静态绘制（冻结基线安全）");
    free(base);
    fvp_motion_free(&m);

    printf("== 7. 确定性 ==\n");
    clear_canvas(g);
    (void)gfx_draw_image_xform(g, 1, 12, 14, 750, 1250, 200);
    base = canvas_snapshot(g, &len);
    clear_canvas(g);
    (void)gfx_draw_image_xform(g, 1, 12, 14, 750, 1250, 200);
    CHECK(base != NULL &&
          memcmp(base, canvas_of(g, &w, &h), len) == 0,
          "同样参数两次绘制逐字节相同");
    free(base);

    gfx_destroy(g);
    printf("\n%d passed, %d failed\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}