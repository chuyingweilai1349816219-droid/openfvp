/* test_gfx_xform.c -- 动画渲染的两块底层：缩放/旋转直贴 + 脚本补间。
 *
 *   bin/test_gfx_xform.exe
 *
 * 为什么有这个测试：开场 FAVORITE logo 的缩放/旋转/位移参数是从**脚本**里读出来的
 * （`src/openinganim.h` 的 FVP_OPEN_VIEW_*，出处 `ref/opening-scene.json`）。
 * 标题画面本身的进场是**逐元素 alpha 淡入**（`FVP_TITLEENTER_*`，见 tests/test_titleenter.c），不用缩放。
 * 这里验证：
 *
 *   1. `gfx_draw_image_xform()` 在 scale=1000 / alpha=255 时与静态绘制
 *      **字节等价** —— 所以动画结束时画面与不做动画完全一样（冻结基线不受影响）；
 *   2. scale / alpha 真的生效（画出包围盒 = round(w*scale)，alpha=0 不画）；
 *   3. 用脚本里的参数建两条补间（alpha 0→255、scale 10000→1000，1400 ms，mode 3），
 *      验证 t=0 / t=700 / t=1400 的取值符合 mode 3 的缓出曲线，并且
 *      **t>=1400 后画出的画布与静态绘制逐字节相同**（这条补间属于**开场**
 *      FAVORITE logo 的 p1..p3 层；标题画面的进场是逐元素时间轴（见下）；
 *   4. `gfx_draw_image_op_xform()` 的 OP（脚本 PrimSetOP）轴心语义：
 *      identity 时 OP 抵消、OP=左上角时与 xform 逐字节相同、OP=中心时围绕中心
 *      缩放/旋转（0.5 倍 20x15、90° 后宽高互换、180° 位置尺寸不变）。
 */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../src/gfx.h"
#include "../src/motion.h"
#include "../src/openinganim.h"
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

/* 合成的 HZC1 头坐标（0x18/0x1A）。真实素材（系统菜单、命令条、确认框）全靠
 * 这两个字段定位，脚本里没有 PrimSetXY；第 10 组测试钉住"头坐标是偏置"这件事。 */
static int g_bias_x = 0;
static int g_bias_y = 0;

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
    out->x = g_bias_x;
    out->y = g_bias_y;
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

    printf("== 2b. alpha 真的变淡（回归：blit_sprite 曾经忽略 p->alpha）==\n");
    {
        long lum_full = 0;
        long lum_half = 0;
        int yy;
        int xx;

        clear_canvas(g);
        (void)gfx_draw_image_alpha(g, 1, 10, 10, 255);
        {
            const uint8_t *c = canvas_of(g, &w, &h);

            for (yy = 10; yy < 10 + IMG_H; ++yy) {
                for (xx = 10; xx < 10 + IMG_W; ++xx) {
                    const uint8_t *q =
                        c + ((size_t)yy * (size_t)w + (size_t)xx) * 4u;

                    lum_full += (long)q[0] + (long)q[1] + (long)q[2];
                }
            }
        }
        clear_canvas(g);
        (void)gfx_draw_image_alpha(g, 1, 10, 10, 128);
        {
            const uint8_t *c = canvas_of(g, &w, &h);

            for (yy = 10; yy < 10 + IMG_H; ++yy) {
                for (xx = 10; xx < 10 + IMG_W; ++xx) {
                    const uint8_t *q =
                        c + ((size_t)yy * (size_t)w + (size_t)xx) * 4u;

                    lum_half += (long)q[0] + (long)q[1] + (long)q[2];
                }
            }
        }
        CHECK(lum_half > 0, "alpha=128 仍然画出来（亮度 %ld）", lum_half);
        CHECK(lum_half < lum_full,
              "alpha=128 比 alpha=255 暗（%ld < %ld）", lum_half, lum_full);
        /* 128/255 的一半左右：允许 ±25% 的容差（底色是纯黑，线性混合） */
        CHECK(lum_half > lum_full * 35 / 100 && lum_half < lum_full * 65 / 100,
              "alpha=128 的亮度约为满值的一半（%ld vs %ld）", lum_half,
              lum_full);
    }

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

    printf("== 5. 脚本参数 + motion：t=0 / 700 / 1400 的取值（开场 FAVORITE logo）==\n");
    CHECK(FVP_OPEN_VIEW_SCALE_FROM == 10000 &&
              FVP_OPEN_VIEW_SCALE_TO == 1000 &&
              FVP_OPEN_VIEW_MS == 1400,
          "开场 p1..p3 的缩放常量 = 10000→1000 / 1400 ms（脚本 MotionMoveS2）");
    CHECK(FVP_TITLEENTER_IROSEKA_LOGO_START_MS == 2000 &&
              FVP_TITLEENTER_LOGO_MS == 2000 &&
              FVP_TITLEENTER_IROSEKA_TOTAL_MS == 4000,
          "标题进场 = 逐元素脚本时间轴（logo 2000 ms 起、2000 ms、总计 4000）");
    fvp_motion_init(&m);
    alpha_id = fvp_motion_add(&m, 255, FVP_TWEEN_ALPHA, 0.0, 0.0, 255.0, 0.0,
                              FVP_OPEN_VIEW_MS, 3, 0);
    scale_id = fvp_motion_add(&m, 255, FVP_TWEEN_SCALE,
                              FVP_OPEN_VIEW_SCALE_FROM,
                              FVP_OPEN_VIEW_SCALE_FROM,
                              FVP_OPEN_VIEW_SCALE_TO,
                              FVP_OPEN_VIEW_SCALE_TO,
                              FVP_OPEN_VIEW_MS, 3, 0);
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

    printf("== 8. OP 轴心变换（exe 0x42AF08/0x42B41F：最终位置 = prim + OP + R*S*(img-OP)）==\n");
    /* identity：OP 抵消，等价于静态绘制 */
    clear_canvas(g);
    (void)gfx_draw_image_at(g, 1, 10, 10);
    base = canvas_snapshot(g, &len);
    CHECK(base != NULL, "snapshot F");
    clear_canvas(g);
    CHECK(gfx_draw_image_op_xform(g, 1, 10, 10, 123, 45, 1000, 1000, 0, 255) == 1,
          "op-xform identity 返回 1");
    other = canvas_snapshot(g, &len2);
    CHECK(base != NULL && other != NULL && len == len2 &&
              memcmp(base, other, len) == 0,
          "identity 时 OP 任意值都抵消（与静态绘制逐字节相同）");
    free(other);

    /* OP = 素材左上角（img 头是 0,0 → OP=(0,0)）时与左上锚 xform 逐字节相同 */
    clear_canvas(g);
    (void)gfx_draw_image_xform(g, 1, 10, 10, 500, 500, 255);
    base = canvas_snapshot(g, &len);
    CHECK(base != NULL, "snapshot G");
    clear_canvas(g);
    (void)gfx_draw_image_op_xform(g, 1, 10, 10, 0, 0, 500, 500, 0, 255);
    other = canvas_snapshot(g, &len2);
    CHECK(base != NULL && other != NULL && len == len2 &&
              memcmp(base, other, len) == 0,
          "OP=左上角 时与 gfx_draw_image_xform 逐字节相同");
    free(other);

    /* OP = 素材中心（20,15）：0.5 倍缩放围绕中心，包围盒 20x15、中心不动 */
    clear_canvas(g);
    (void)gfx_draw_image_op_xform(g, 1, 10, 10, 20, 15, 500, 500, 0, 255);
    CHECK(ink_box(g, &bx0, &by0, &bx1, &by1, &count) == 1, "OP 中心 500‰ 有墨迹");
    CHECK(bx1 - bx0 + 1 == 20 && by1 - by0 + 1 == 15,
          "围绕中心缩到 20x15，实际 %dx%d", bx1 - bx0 + 1, by1 - by0 + 1);
    CHECK(labs((long)(bx0 + bx1) - 59) <= 2 && labs((long)(by0 + by1) - 49) <= 2,
          "缩放后中心仍在 (30,25) 附近（bbox 中心 %d,%d）",
          (bx0 + bx1) / 2, (by0 + by1) / 2);

    /* 180°：尺寸不变、位置不变（重采样会在边缘留下一圈过渡像素，
     * 所以断言包围盒而不是逐字节）。 */
    clear_canvas(g);
    (void)gfx_draw_image_op_xform(g, 1, 10, 10, 20, 15, 1000, 1000, 3600, 255);
    CHECK(ink_box(g, &bx0, &by0, &bx1, &by1, &count) == 1, "180° 有墨迹");
    CHECK(labs((long)bx0 - 10) <= 1 && labs((long)by0 - 10) <= 1 &&
              bx1 - bx0 + 1 == IMG_W && by1 - by0 + 1 == IMG_H,
          "绕中心旋转 180° 后位置/尺寸不变（bbox %d,%d %dx%d，允许 1 px "
          "的重采样边界误差）",
          bx0, by0, bx1 - bx0 + 1, by1 - by0 + 1);

    /* 90°：包围盒宽高互换（40x30 → 30x40），中心仍不动 */
    clear_canvas(g);
    (void)gfx_draw_image_op_xform(g, 1, 10, 10, 20, 15, 1000, 1000, 1800, 255);
    CHECK(ink_box(g, &bx0, &by0, &bx1, &by1, &count) == 1, "90° 有墨迹");
    CHECK(bx1 - bx0 + 1 == 30 && by1 - by0 + 1 == 40,
          "旋转 90° 后包围盒 30x40，实际 %dx%d", bx1 - bx0 + 1,
          by1 - by0 + 1);
    CHECK(labs((long)(bx0 + bx1) - 59) <= 2 && labs((long)(by0 + by1) - 49) <= 2,
          "90° 旋转中心仍在 (30,25) 附近（bbox 中心 %d,%d）",
          (bx0 + bx1) / 2, (by0 + by1) / 2);

    /* alpha=0 不画；确定性 */
    clear_canvas(g);
    base = canvas_snapshot(g, &len);
    CHECK(gfx_draw_image_op_xform(g, 1, 10, 10, 20, 15, 500, 500, 900, 0) == 0,
          "op-xform alpha=0 返回 0");
    other = canvas_snapshot(g, &len2);
    CHECK(base != NULL && other != NULL && memcmp(base, other, len) == 0,
          "op-xform alpha=0 画布未变");
    free(other);
    clear_canvas(g);
    (void)gfx_draw_image_op_xform(g, 1, 12, 14, 20, 15, 750, 1250, 450, 200);
    base = canvas_snapshot(g, &len);
    clear_canvas(g);
    (void)gfx_draw_image_op_xform(g, 1, 12, 14, 20, 15, 750, 1250, 450, 200);
    CHECK(base != NULL && memcmp(base, canvas_of(g, &w, &h), len) == 0,
          "op-xform 同样参数两次绘制逐字节相同");
    free(base);

    printf("== 9. cropped_scaled（标题背景缩放：裁源区域 + 缩放到目标）==\n");
    /* 全图 1:1：与静态绘制逐字节相同 */
    clear_canvas(g);
    (void)gfx_draw_image_at(g, 1, 10, 10);
    base = canvas_snapshot(g, &len);
    CHECK(base != NULL, "snapshot H");
    clear_canvas(g);
    CHECK(gfx_draw_image_cropped_scaled(g, 1, 10, 10, IMG_W, IMG_H, 0, 0,
                                        IMG_W, IMG_H) == 1,
          "全图 1:1 返回 1");
    other = canvas_snapshot(g, &len2);
    CHECK(base != NULL && other != NULL && len == len2 &&
              memcmp(base, other, len) == 0,
          "全图 1:1 与 gfx_draw_image_at 逐字节相同");
    free(other);

    /* 左上 1/4 放大 2 倍铺满 40x30：包围盒 40x30、位置不变 */
    clear_canvas(g);
    CHECK(gfx_draw_image_cropped_scaled(g, 1, 10, 10, IMG_W, IMG_H, 0, 0,
                                        IMG_W / 2, IMG_H / 2) == 1,
          "1/4 放大 2x 返回 1");
    CHECK(ink_box(g, &bx0, &by0, &bx1, &by1, &count) == 1, "有墨迹");
    CHECK(bx0 == 10 && by0 == 10 && bx1 - bx0 + 1 == IMG_W &&
              by1 - by0 + 1 == IMG_H,
          "放大后包围盒 40x30 @(10,10)（实际 %d,%d %dx%d）", bx0, by0,
          bx1 - bx0 + 1, by1 - by0 + 1);
    CHECK(count == (long)IMG_W * IMG_H, "不透明铺满 %ld 像素（实际 %ld）",
          (long)IMG_W * IMG_H, count);

    /* 源矩形越界：自动裁到图内（不崩、仍然画） */
    clear_canvas(g);
    CHECK(gfx_draw_image_cropped_scaled(g, 1, 0, 0, IMG_W, IMG_H,
                                        IMG_W - 10, IMG_H - 10, 40, 30) == 1,
          "源矩形越界仍返回 1");

    /* alpha/参数边界 */
    CHECK(gfx_draw_image_cropped_scaled(g, 1, 0, 0, 0, 10, 0, 0, 10, 10) == 0,
          "dw=0 返回 0");
    CHECK(gfx_draw_image_cropped_scaled(g, 1, 0, 0, 10, 10, 0, 0, 0, 0) == 0,
          "sw=0 返回 0");
    /* 确定性 */
    clear_canvas(g);
    (void)gfx_draw_image_cropped_scaled(g, 1, 12, 14, 60, 30, 0, 0, 20, 10);
    base = canvas_snapshot(g, &len);
    clear_canvas(g);
    (void)gfx_draw_image_cropped_scaled(g, 1, 12, 14, 60, 30, 0, 0, 20, 10);
    CHECK(base != NULL && memcmp(base, canvas_of(g, &w, &h), len) == 0,
          "cropped_scaled 同样参数两次绘制逐字节相同");
    free(base);

    /* ================= 5. gfx_draw_affine_layers()：锚点语义 =================
     * 标题背景动画用逐层仿射合成。它必须满足两条不变式：
     *   (a) 缩放 1000‰、角度 0 时，屏幕上等价于静态的 `img + src − 裁剪原点`；
     *   (b) 锚点填的是**画面坐标**：设计画布坐标的 OP 要先减去裁剪原点，
     *       否则整层被恒定平移 —— 2026-11-26 五彩标题的"风车入场太靠中"
     *       就是锚点少减了裁剪原点（(240,135)）。 */
    printf("== 5. gfx_draw_affine_layers 锚点/恒等语义 ==\n");
    {
        static const int crop_x = 5;
        static const int crop_y = 3;
        GfxAffineLayer L;
        uint8_t *aff;
        size_t alen = 0;

        /* (a) 恒等：等于把图按 (img − 裁剪原点) 直贴 */
        clear_canvas(g);
        (void)gfx_draw_image_at(g, 1, 0 - crop_x, 0 - crop_y);
        base = canvas_snapshot(g, &len);

        memset(&L, 0, sizeof L);
        L.slot = 1;
        L.img_x = 0;
        L.img_y = 0;
        L.op_x = 20;
        L.op_y = 15;
        L.anchor_x = L.op_x - crop_x; /* 关键：OP 换算到画面坐标 */
        L.anchor_y = L.op_y - crop_y;
        L.scale_x1000 = 1000;
        L.scale_y1000 = 1000;
        L.angle_units = 0;
        L.alpha = 255;
        clear_canvas(g);
        CHECK(gfx_draw_affine_layers(g, &L, 1, 1) == 1, "affine 恒等返回 1");
        aff = canvas_snapshot(g, &alen);
        CHECK(base != NULL && aff != NULL && len == alen &&
                  memcmp(base, aff, len) == 0,
              "恒等仿射 == 静态直贴（逐字节）");
        free(aff);

        /* (b) 锚点没减裁剪原点 → 整层正好平移 (crop_x, crop_y) */
        L.anchor_x = L.op_x;
        L.anchor_y = L.op_y;
        clear_canvas(g);
        CHECK(gfx_draw_affine_layers(g, &L, 1, 1) == 1, "affine 平移返回 1");
        CHECK(ink_box(g, &bx0, &by0, &bx1, &by1, &count) == 1,
              "平移帧有内容");
        CHECK(bx0 == 0 && by0 == 0 && bx1 == IMG_W - 1 && by1 == IMG_H - 1,
              "锚点不减裁剪原点 → 画在 (0,0) 起（%d,%d..%d,%d）", bx0, by0,
              bx1, by1);

        /* (c) 500‰ 绕 OP 缩放：包围盒 20x15，中心不变 */
        L.anchor_x = L.op_x - crop_x;
        L.anchor_y = L.op_y - crop_y;
        L.scale_x1000 = 500;
        L.scale_y1000 = 500;
        clear_canvas(g);
        CHECK(gfx_draw_affine_layers(g, &L, 1, 1) == 1, "affine 500‰ 返回 1");
        CHECK(ink_box(g, &bx0, &by0, &bx1, &by1, &count) == 1, "500‰ 有内容");
        CHECK(bx1 - bx0 + 1 == IMG_W / 2 && by1 - by0 + 1 == IMG_H / 2,
              "500‰ 缩放后包围盒 %dx%d", bx1 - bx0 + 1, by1 - by0 + 1);
        CHECK(bx0 == (0 - crop_x) + IMG_W / 4 &&
                  (by0 == (0 - crop_y) + IMG_H / 4 ||
                   by0 == (0 - crop_y) + IMG_H / 4 + 1),
              "500‰ 绕 OP 缩放后左上 (%d,%d)", bx0, by0);

        /* (d) 90° 旋转：宽高互换。图放在画布中心附近，避免被画布边缘裁掉
         * （图头 (80,60)、OP (100,75)、裁剪 0 ⇒ 屏幕 = OP + R·S·(图头+源−OP)）。 */
        L.img_x = 80;
        L.img_y = 60;
        L.op_x = 100;
        L.op_y = 75;
        L.anchor_x = L.op_x;
        L.anchor_y = L.op_y;
        L.scale_x1000 = 1000;
        L.scale_y1000 = 1000;
        L.angle_units = 1800; /* 1800 * 0.05° = 90° */
        clear_canvas(g);
        CHECK(gfx_draw_affine_layers(g, &L, 1, 1) == 1, "affine 90° 返回 1");
        CHECK(ink_box(g, &bx0, &by0, &bx1, &by1, &count) == 1, "90° 有内容");
        CHECK(bx1 - bx0 + 1 == IMG_H && by1 - by0 + 1 == IMG_W,
              "90° 后包围盒 %dx%d（应为 %dx%d）", bx1 - bx0 + 1,
              by1 - by0 + 1, IMG_H, IMG_W);
        /* 再转回 0°：位置回到 (80,60) */
        L.angle_units = 0;
        clear_canvas(g);
        CHECK(gfx_draw_affine_layers(g, &L, 1, 1) == 1, "affine 0° 返回 1");
        CHECK(ink_box(g, &bx0, &by0, &bx1, &by1, &count) == 1, "0° 有内容");
        CHECK(bx0 == 80 && by0 == 60, "0° 时回到图头位置 (%d,%d)", bx0,
              by0);

        /* (e) div=2 的放大路径：回到 crop 场景，墨迹仍从 (0,0) 开始 */
        L.img_x = 0;
        L.img_y = 0;
        L.op_x = 20;
        L.op_y = 15;
        L.anchor_x = L.op_x - crop_x;
        L.anchor_y = L.op_y - crop_y;
        L.angle_units = 0;
        clear_canvas(g);
        CHECK(gfx_draw_affine_layers(g, &L, 1, 2) == 1, "affine div=2 返回 1");
        CHECK(ink_box(g, &bx0, &by0, &bx1, &by1, &count) == 1,
              "div=2 有内容");
        /* 图在 (-5,-3)，左边/上边被画布裁掉 → 墨迹从 (0,0) 开始 */
        CHECK(bx0 == 0 && by0 == 0,
              "div=2 左上 (%d,%d) = (0,0)（被画布裁剪）", bx0, by0);

        /* (f) NULL / n<=0 / 非法缩放不崩 */
        CHECK(gfx_draw_affine_layers(NULL, &L, 1, 1) == 0, "NULL gfx → 0");
        CHECK(gfx_draw_affine_layers(g, NULL, 1, 1) == 0, "NULL layers → 0");
        CHECK(gfx_draw_affine_layers(g, &L, 0, 1) == 0, "n=0 → 0");
        {
            GfxAffineLayer bad = L;

            bad.scale_x1000 = 0;
            clear_canvas(g);
            CHECK(gfx_draw_affine_layers(g, &bad, 1, 1) == 0,
                  "scale=0 层被跳过 → 0");
        }
        free(base);
    }

    gfx_destroy(g);

    printf("== 10. HZC1 头的屏幕坐标是偏置：绘制位置 = 图元坐标 + 头坐标 ==\n");
    /* 现实依据：游戏内 UI（系统菜单 8 项、命令条 26 键、确认框 7 提示）在脚本里
     * 一条 PrimSetXY 都没有，全靠 HZC1 头 0x18/0x1A 定位。这里用合成图钉住
     * "头坐标当偏置加在图元坐标上"这条渲染语义。 */
    g_bias_x = 40;
    g_bias_y = 25;
    {
        Gfx *gb = make_canvas();
        int hx0 = 0, hy0 = 0, hx1 = 0, hy1 = 0;
        long hn = 0;

        CHECK(gb != NULL, "带头坐标的画布（40,25）");
        if (gb != NULL) {
            gfx_prim_reset(gb, 9);
            (void)gfx_set_prim(gb, 9, GFX_F_TYPE, 4);
            (void)gfx_set_prim(gb, 9, GFX_F_GRAPH, 1);
            (void)gfx_set_prim(gb, 9, GFX_F_X, 10);
            (void)gfx_set_prim(gb, 9, GFX_F_Y, 10);
            clear_canvas(gb);
            gfx_render(gb);
            CHECK(ink_box(gb, &hx0, &hy0, &hx1, &hy1, &hn) == 1, "有墨迹");
            CHECK(hx0 == 50 && hy0 == 35 && hx1 == 89 && hy1 == 64,
                  "prim(10,10) + 头(40,25) = (50,35)..(89,64)（实际 %d,%d..%d,%d）",
                  hx0, hy0, hx1, hy1);
            CHECK(hn == (long)IMG_W * IMG_H, "铺满 %ld 像素（实际 %ld）",
                  (long)IMG_W * IMG_H, hn);

            /* 图元再被脚本移动：偏置不变，整体跟着平移 */
            (void)gfx_set_prim(gb, 9, GFX_F_X, 100);
            clear_canvas(gb);
            gfx_render(gb);
            CHECK(ink_box(gb, &hx0, &hy0, &hx1, &hy1, &hn) == 1, "移动后仍有墨迹");
            CHECK(hx0 == 140 && hy0 == 35,
                  "prim(100,10) + 头(40,25) = (140,35)（实际 %d,%d）", hx0, hy0);

            /* 直接 blit 不带偏置（外壳自绘 UI 用），与图元路径区分开 */
            clear_canvas(gb);
            CHECK(gfx_draw_image_at(gb, 1, 10, 10) == 1, "direct blit 返回 1");
            CHECK(ink_box(gb, &hx0, &hy0, &hx1, &hy1, &hn) == 1, "direct 有墨迹");
            CHECK(hx0 == 10 && hy0 == 10,
                  "gfx_draw_image_at 不叠加头坐标（实际 %d,%d）", hx0, hy0);
            gfx_destroy(gb);
        }
    }
    g_bias_x = 0;
    g_bias_y = 0;

    printf("\n%d passed, %d failed\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}