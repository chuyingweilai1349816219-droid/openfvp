/* 存档缩略图缩放测试。 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../src/thumb.h"

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

static void put(unsigned char *p, int b, int g, int r)
{
    p[0] = (unsigned char)b;
    p[1] = (unsigned char)g;
    p[2] = (unsigned char)r;
    p[3] = 0x00; /* 源 alpha 故意写 0，缩略图必须把它覆盖成 0xFF */
}

int main(void)
{
    /* 1. 纯色缩小：颜色不变、alpha 被强制 0xFF。 */
    {
        unsigned char src[8 * 6 * 4];
        unsigned char dst[2 * 2 * 4];
        int i;

        for (i = 0; i < 8 * 6; ++i) {
            put(src + i * 4, 10, 20, 30);
        }
        CHECK(fvp_thumb_scale_bgra(src, 8, 6, dst, 2, 2) == 1, "scale 8x6->2x2");
        CHECK(dst[0] == 10 && dst[1] == 20 && dst[2] == 30, "colour kept");
        CHECK(dst[3] == 0xFF, "alpha forced to 0xFF, got 0x%02x", dst[3]);
    }

    /* 2. 2x2 -> 1x1：四个像素的算术平均。 */
    {
        unsigned char src[2 * 2 * 4];
        unsigned char dst[4];

        put(src + 0, 0, 0, 0);
        put(src + 4, 100, 0, 0);
        put(src + 8, 0, 200, 0);
        put(src + 12, 0, 0, 40);
        CHECK(fvp_thumb_scale_bgra(src, 2, 2, dst, 1, 1) == 1, "scale 2x2->1x1");
        CHECK(dst[0] == 25, "B avg=%d (want 25)", dst[0]);
        CHECK(dst[1] == 50, "G avg=%d (want 50)", dst[1]);
        CHECK(dst[2] == 10, "R avg=%d (want 10)", dst[2]);
    }

    /* 3. 真实比例 1920x1080 -> 200x113：只检查尺寸/alpha，不跑全图比对。 */
    {
        const int sw = 1920;
        const int sh = 1080;
        const int dw = 200;
        const int dh = 113;
        unsigned char *src = (unsigned char *)malloc((size_t)sw * sh * 4u);
        unsigned char *dst = (unsigned char *)malloc((size_t)dw * dh * 4u);
        int i;
        int bad_alpha = 0;

        CHECK(src != NULL && dst != NULL, "oom");
        if (src != NULL && dst != NULL) {
            for (i = 0; i < sw * sh; ++i) {
                put(src + i * 4, 128, 64, 32);
            }
            CHECK(fvp_thumb_scale_bgra(src, sw, sh, dst, dw, dh) == 1,
                  "1920x1080 -> 200x113");
            for (i = 0; i < dw * dh; ++i) {
                if (dst[i * 4 + 3] != 0xFF) {
                    bad_alpha += 1;
                }
                if (dst[i * 4] != 128 || dst[i * 4 + 1] != 64 ||
                    dst[i * 4 + 2] != 32) {
                    bad_alpha += 1;
                }
            }
            CHECK(bad_alpha == 0, "%d bad pixel(s) in the 200x113 result",
                  bad_alpha);
            free(src);
            free(dst);
        }
    }

    /* 4. 参数非法一律返回 0。 */
    {
        unsigned char px[4] = {0, 0, 0, 0};

        CHECK(fvp_thumb_scale_bgra(NULL, 1, 1, px, 1, 1) == 0, "NULL src");
        CHECK(fvp_thumb_scale_bgra(px, 1, 1, NULL, 1, 1) == 0, "NULL dst");
        CHECK(fvp_thumb_scale_bgra(px, 0, 1, px, 1, 1) == 0, "zero width");
        CHECK(fvp_thumb_scale_bgra(px, 1, 1, px, 0, 1) == 0, "zero dst width");
    }

    printf("\ntest_thumb: %d passed, %d failed\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}