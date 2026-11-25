/* 原版 GDI 绘制协议（cp932 + GGO_BITMAP）单元测试。
 *
 * 为什么值得单独测：`docs/original-verification.md` §8 从 exe 里静态提取出原版的
 * 文字绘制协议——CreateFontA 用 SHIFTJIS_CHARSET(0x80)、字重/质量全 0，
 * GetGlyphOutlineA 只用 GGO_METRICS 与 GGO_BITMAP，三次共用一个单位 MAT2。
 * 据此实现的 `win32_gdi_text_mask_original()` 必须满足两条可测的性质：
 *   1. 掩码是**二值**的（只有 0 / 255），因为 GGO_BITMAP 就是 1bpp；
 *   2. 进位来自 GLYPHMETRICS.gmCellIncX，所以全角字是等宽整数倍。
 * 另外还要锁住一条限制：原版是 cp932 的 ANSI 程序，cp932 表示不了的字符
 * （例如「龘」）原版自己也画不出来 —— 我们的实现必须返回 0，而不是画个替身。
 *
 * 这是 Windows 专属测试（要 gdi32），不依赖游戏数据。
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../src/platform/win32/win32_gdi_text.h"

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

/* 统计掩码里 0 / 255 / 中间值 的像素数。 */
static void mask_stats(const unsigned char *mask, int w, int h, int *zero,
                       int *full, int *mid)
{
    int i;
    int n = w * h;

    *zero = 0;
    *full = 0;
    *mid = 0;
    for (i = 0; i < n; ++i) {
        if (mask[i] == 0) {
            ++*zero;
        } else if (mask[i] == 255) {
            ++*full;
        } else {
            ++*mid;
        }
    }
}

int main(void)
{
    unsigned char *mask = NULL;
    unsigned char *mask2 = NULL;
    int w = 0;
    int h = 0;
    int w2 = 0;
    int h2 = 0;
    int zero;
    int full;
    int mid;

    /* 1. 进位：原版取自 GLYPHMETRICS.gmCellIncX */
    {
        int a1 = win32_gdi_text_advance_original("\xE3\x81\x82", NULL, 32); /* あ */
        int a2 = win32_gdi_text_advance_original(
            "\xE3\x81\x82\xE3\x81\x82", NULL, 32); /* ああ */

        CHECK(a1 > 0, "advance(あ, 32) 应为正数（实际 %d）", a1);
        CHECK(a2 == a1 * 2, "全角等宽：advance(ああ)=%d 应为 2×%d", a2, a1);
        /* 「日」在 cp932 里是 0x93FA，也是全角。这里**不断言与「あ」完全相等**：
         * 本机没装 MS Gothic 时 GDI 会做字体替换，不同码点可能落到不同替身字体，
         * 进位会有几像素差；只锁"同一个字符重复 N 次 = N 倍"这条硬性质。 */
        {
            int ad = win32_gdi_text_advance_original("\xE6\x97\xA5", NULL, 32);

            CHECK(ad > 0, "advance(日, 32) 应为正数（实际 %d）", ad);
            CHECK(ad >= 16 && ad <= 64,
                  "advance(日) 应在 32px 全角字的合理范围（实际 %d）", ad);
        }
    }

    /* 2. 掩码是二值的（1bpp 协议），且有墨迹 */
    {
        int ok = win32_gdi_text_mask_original("\xE3\x81\x82", NULL, 32, &mask, &w,
                                              &h);

        CHECK(ok == 1, "mask_original(あ) 应成功（实际 %d）", ok);
        CHECK(w > 0 && h > 0, "掩码尺寸应为正（实际 %dx%d）", w, h);
        CHECK(h >= 32, "掩码高度应不小于字号（实际 h=%d px=32）", h);
        if (ok == 1) {
            mask_stats(mask, w, h, &zero, &full, &mid);
            CHECK(mid == 0, "原版协议掩码必须是二值：中间值像素应为 0（实际 %d）",
                  mid);
            CHECK(full > 0, "掩码里应有墨迹（实际 %d 个 255）", full);
            CHECK(zero > 0, "掩码里应有背景像素（实际 %d 个 0）", zero);
        }
        free(mask);
        mask = NULL;
    }

    /* 3. 与抗锯齿路径并存：那条路径允许中间值（这里只报告，不断言） */
    {
        int ok = win32_gdi_text_mask("\xE6\x97\xA5", 32, &mask2, &w2, &h2);

        CHECK(ok == 1, "抗锯齿 mask(日) 也应成功（实际 %d）", ok);
        if (ok == 1) {
            mask_stats(mask2, w2, h2, &zero, &full, &mid);
            printf("info: 抗锯齿路径 日@32 中间值像素 %d / 总面积 %d\n", mid,
                   w2 * h2);
            CHECK(full + mid > 0, "抗锯齿掩码应有墨迹");
        }
        free(mask2);
        mask2 = NULL;
    }

    /* 4. cp932 表示不了的字符：原版画不出来，这里必须返回 0 */
    {
        int ok = win32_gdi_text_mask_original("\xE9\xBE\x98", NULL, 32, &mask, &w,
                                              &h); /* 龘 U+9F98 */
        int adv = win32_gdi_text_advance_original("\xE9\xBE\x98", NULL, 32);

        CHECK(ok == 0, "cp932 表示不了的「龘」应返回 0（实际 %d）", ok);
        CHECK(mask == NULL, "失败时不应交回掩码");
        CHECK(adv == 0, "cp932 表示不了的字符进位应为 0（实际 %d）", adv);
    }

    /* 5. 适配器与带族名的版本一致 */
    {
        int ok1 = win32_gdi_text_mask_cp932("\xE6\x97\xA5", 24, &mask, &w, &h);

        CHECK(ok1 == 1, "适配器 mask_cp932(日, 24) 应成功（实际 %d）", ok1);
        CHECK(win32_gdi_text_advance_cp932("\xE6\x97\xA5", 24)
                  == win32_gdi_text_advance_original("\xE6\x97\xA5", NULL, 24),
              "适配器进位应与显式默认族名一致");
        free(mask);
        mask = NULL;
    }

    /* 6. 默认族名就是 exe 内置注册表的第 0 项，可改可复位 */
    {
        const char *def = win32_gdi_text_default_face();

        CHECK(def != NULL && strcmp(def,
                                    "\xEF\xBC\xAD\xEF\xBC\xB3 \xE3\x82\xB4"
                                    "\xE3\x82\xB7\xE3\x83\x83\xE3\x82\xAF")
                                 == 0,
              "默认族名应为「ＭＳ ゴシック」（实际 %s）", def);
        win32_gdi_text_set_default_face("MS Gothic");
        CHECK(strcmp(win32_gdi_text_default_face(), "MS Gothic") == 0,
              "设置默认族名后应读回（实际 %s）",
              win32_gdi_text_default_face());
        win32_gdi_text_set_default_face(NULL);
        CHECK(strcmp(win32_gdi_text_default_face(), "MS Gothic") == 0,
              "NULL 不应清空族名");
        win32_gdi_text_set_default_face(def);
        CHECK(strcmp(win32_gdi_text_default_face(), def) == 0,
              "应能把默认族名改回去");
    }

    /* 7. NULL / 越界安全 */
    {
        int ww = 7;
        int hh = 9;

        CHECK(win32_gdi_text_mask_original(NULL, NULL, 32, &mask, &ww, &hh) == 0,
              "utf8 为 NULL 应返回 0");
        CHECK(ww == 0 && hh == 0, "失败时 w/h 应清零（实际 %d/%d）", ww, hh);
        CHECK(win32_gdi_text_mask_original("A", NULL, 0, &mask, &ww, &hh) == 0,
              "px=0 应返回 0");
        CHECK(win32_gdi_text_mask_original("A", NULL, 32, NULL, &ww, &hh) == 0,
              "mask 出参为 NULL 应返回 0");
        CHECK(win32_gdi_text_advance_original(NULL, NULL, 32) == 0,
              "advance(NULL) 应返回 0");
        CHECK(win32_gdi_text_advance_original("A", NULL, -5) == 0,
              "advance(px<0) 应返回 0");
        CHECK(win32_gdi_text_mask_original("", NULL, 32, &mask, &ww, &hh) == 0,
              "空串应返回 0");
    }

    printf("test_gdi_ggo: %d passed, %d failed\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}