/* 存档缩略图缩放（box filter）。见 thumb.h。 */
#include "thumb.h"

#include <stddef.h>

int fvp_thumb_scale_bgra(const unsigned char *src, int sw, int sh,
                         unsigned char *dst, int dw, int dh)
{
    int dy;
    int dx;

    if (src == NULL || dst == NULL || sw <= 0 || sh <= 0 || dw <= 0 || dh <= 0) {
        return 0;
    }

    for (dy = 0; dy < dh; ++dy) {
        /* 目标像素覆盖的源矩形：用 64 位整数算边界，避免 1920×1080 溢出。 */
        long long y0 = (long long)dy * sh / dh;
        long long y1 = (long long)(dy + 1) * sh / dh;

        if (y1 <= y0) {
            y1 = y0 + 1;
        }
        if (y0 >= sh) {
            y0 = sh - 1;
        }
        if (y1 > sh) {
            y1 = sh;
        }

        for (dx = 0; dx < dw; ++dx) {
            long long x0 = (long long)dx * sw / dw;
            long long x1 = (long long)(dx + 1) * sw / dw;
            long long acc_b = 0;
            long long acc_g = 0;
            long long acc_r = 0;
            long long n = 0;
            long long y;
            long long x;
            unsigned char *out = dst + ((size_t)dy * (size_t)dw + (size_t)dx) * 4u;

            if (x1 <= x0) {
                x1 = x0 + 1;
            }
            if (x0 >= sw) {
                x0 = sw - 1;
            }
            if (x1 > sw) {
                x1 = sw;
            }
            for (y = y0; y < y1; ++y) {
                const unsigned char *row = src + (size_t)y * (size_t)sw * 4u;

                for (x = x0; x < x1; ++x) {
                    const unsigned char *p = row + (size_t)x * 4u;

                    acc_b += p[0];
                    acc_g += p[1];
                    acc_r += p[2];
                    n += 1;
                }
            }
            if (n <= 0) {
                out[0] = 0;
                out[1] = 0;
                out[2] = 0;
            } else {
                out[0] = (unsigned char)(acc_b / n);
                out[1] = (unsigned char)(acc_g / n);
                out[2] = (unsigned char)(acc_r / n);
            }
            out[3] = 0xFF; /* 引擎的缩略图 alpha 恒为 0xFF */
        }
    }
    return 1;
}