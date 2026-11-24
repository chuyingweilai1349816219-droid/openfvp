/* 文本窗口属性（TextColor / TextSize / TextSpeed）存储层测试。 */
#include <stdio.h>
#include <string.h>

#include "../src/textattr.h"

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
    FvpTextAttr a[FVP_TEXT_WINDOWS];

    /* 1. 默认值。 */
    fvp_textattr_reset(a, FVP_TEXT_WINDOWS);
    CHECK(a[0].r == 255 && a[0].g == 255 && a[0].b == 255, "default colour");
    CHECK(a[0].w == 12 && a[0].h == 12, "default size %dx%d", a[0].w, a[0].h);
    CHECK(a[0].speed_ms == 0, "default speed %d", a[0].speed_ms);
    CHECK(fvp_textattr_configured_count(a, FVP_TEXT_WINDOWS) == 0,
          "fresh count=%d", fvp_textattr_configured_count(a, FVP_TEXT_WINDOWS));

    /* 2. 颜色：正常 + 钳制。 */
    CHECK(fvp_textattr_set_color(a, FVP_TEXT_WINDOWS, 3, 12, 34, 56) == 1,
          "set_color");
    CHECK(a[3].r == 12 && a[3].g == 34 && a[3].b == 56, "colour %d,%d,%d",
          a[3].r, a[3].g, a[3].b);
    CHECK((a[3].set_mask & FVP_TEXTATTR_COLOR) != 0u, "colour mask");
    CHECK(fvp_textattr_set_color(a, FVP_TEXT_WINDOWS, 4, -5, 300, 1) == 1,
          "set_color clamps");
    CHECK(a[4].r == 0 && a[4].g == 255 && a[4].b == 1, "clamped %d,%d,%d",
          a[4].r, a[4].g, a[4].b);

    /* 3. 字号：区间 12..64 / 8..32。 */
    CHECK(fvp_textattr_set_size(a, FVP_TEXT_WINDOWS, 1, 24, 16) == 1, "set_size");
    CHECK(a[1].w == 24 && a[1].h == 16, "size %dx%d", a[1].w, a[1].h);
    CHECK(fvp_textattr_set_size(a, FVP_TEXT_WINDOWS, 1, 1, 1) == 1, "size low");
    CHECK(a[1].w == FVP_TEXT_W_MIN && a[1].h == FVP_TEXT_H_MIN, "size low=%dx%d",
          a[1].w, a[1].h);
    CHECK(fvp_textattr_set_size(a, FVP_TEXT_WINDOWS, 1, 999, 999) == 1,
          "size high");
    CHECK(a[1].w == FVP_TEXT_W_MAX && a[1].h == FVP_TEXT_H_MAX, "size high=%dx%d",
          a[1].w, a[1].h);

    /* 4. 速度：-1..300000（静态分析 0x43CE70：超范围是整条命令被忽略，不是钳制）。 */
    CHECK(fvp_textattr_set_speed(a, FVP_TEXT_WINDOWS, 2, 40) == 1, "set_speed");
    CHECK(a[2].speed_ms == 40, "speed %d", a[2].speed_ms);
    CHECK(fvp_textattr_set_speed(a, FVP_TEXT_WINDOWS, 2, -1) == 1,
          "-1 = use engine setting (accepted)");
    CHECK(a[2].speed_ms == -1, "speed -1 kept (%d)", a[2].speed_ms);
    CHECK(fvp_textattr_set_speed(a, FVP_TEXT_WINDOWS, 2, -7) == 0,
          "-7 rejected (engine ignores the whole command)");
    CHECK(a[2].speed_ms == -1, "rejected value did not overwrite (%d)",
          a[2].speed_ms);
    CHECK(fvp_textattr_set_speed(a, FVP_TEXT_WINDOWS, 2, 500000) == 0,
          "500000 rejected");
    CHECK(a[2].speed_ms == -1, "still -1 after the rejection (%d)",
          a[2].speed_ms);
    CHECK(fvp_textattr_set_speed(a, FVP_TEXT_WINDOWS, 2, FVP_TEXT_SPEED_MAX) == 1,
          "300000 accepted");
    CHECK(a[2].speed_ms == FVP_TEXT_SPEED_MAX, "speed max=%d", a[2].speed_ms);

    /* 4b. 阴影距离（TextShadowDist 125，handler 0x43CBB0）：**单个 0..12，钳制**。
     *     >12 当 12、<0 当 0（原版程序里是 jl/jg 之后取 0 / 12 再转 0x4248B0）。 */
    CHECK(fvp_textattr_set_shadow(a, FVP_TEXT_WINDOWS, 3, 12) == 1,
          "set_shadow 12");
    CHECK(a[3].shadow_dist == 12, "shadow dist %d", a[3].shadow_dist);
    CHECK(fvp_textattr_set_shadow(a, FVP_TEXT_WINDOWS, 3, 99) == 1,
          "99 clamped (not rejected)");
    CHECK(a[3].shadow_dist == FVP_TEXT_SHADOW_MAX, "clamped to %d (%d)",
          FVP_TEXT_SHADOW_MAX, a[3].shadow_dist);
    CHECK(fvp_textattr_set_shadow(a, FVP_TEXT_WINDOWS, 3, -7) == 1,
          "-7 clamped (not rejected)");
    CHECK(a[3].shadow_dist == 0, "clamped to 0 (%d)", a[3].shadow_dist);
    CHECK(fvp_textattr_set_shadow(a, FVP_TEXT_WINDOWS, 3, 0) == 1,
          "distance 0 accepted");
    CHECK((a[3].set_mask & FVP_TEXTATTR_SHADOW) != 0u, "shadow mask set");

    /* 4c. 字间距 / 行间距（TextSpace 128，handler 0x43CE10）：两个分量各
     *     -32..32，**独立判定**：超范围的那一个跳过，另一个照样生效。 */
    CHECK(fvp_textattr_set_space(a, FVP_TEXT_WINDOWS, 5, 12, 0) == 1,
          "set_space 12,0");
    CHECK(a[5].space_x == 12 && a[5].space_y == 0, "space %d,%d", a[5].space_x,
          a[5].space_y);
    CHECK(fvp_textattr_set_space(a, FVP_TEXT_WINDOWS, 5, 99, -5) == 1,
          "x out of range still applies y");
    CHECK(a[5].space_x == 12 && a[5].space_y == -5,
          "x kept, y updated (%d,%d)", a[5].space_x, a[5].space_y);
    CHECK(fvp_textattr_set_space(a, FVP_TEXT_WINDOWS, 5, -32, 32) == 1,
          "space boundaries accepted");
    CHECK(a[5].space_x == -32 && a[5].space_y == 32, "space boundaries %d,%d",
          a[5].space_x, a[5].space_y);
    CHECK(fvp_textattr_set_space(a, FVP_TEXT_WINDOWS, 5, -33, 33) == 0,
          "both out of range = rejected");
    CHECK(a[5].space_x == -32 && a[5].space_y == 32,
          "space unchanged after the rejection (%d,%d)", a[5].space_x,
          a[5].space_y);
    CHECK((a[5].set_mask & FVP_TEXTATTR_SPACE) != 0u, "space mask set");

    /* 4d. TextSkip(127)：0..3，超范围拒绝。 */
    CHECK(fvp_textattr_set_skip(a, FVP_TEXT_WINDOWS, 4, 2) == 1, "set_skip");
    CHECK(a[4].skip_mode == 2, "skip mode %d", a[4].skip_mode);
    CHECK(fvp_textattr_set_skip(a, FVP_TEXT_WINDOWS, 4, 4) == 0,
          "mode 4 rejected");
    CHECK(a[4].skip_mode == 2, "skip unchanged (%d)", a[4].skip_mode);
    CHECK(fvp_textattr_set_skip(a, FVP_TEXT_WINDOWS, 4, -1) == 0,
          "mode -1 rejected");
    CHECK(fvp_textattr_set_skip(a, FVP_TEXT_WINDOWS, 4, FVP_TEXT_SKIP_MAX) == 1,
          "mode 3 accepted");

    /* 5. 窗口号越界：拒绝，且不改任何窗口。 */
    CHECK(fvp_textattr_set_color(a, FVP_TEXT_WINDOWS, 32, 1, 2, 3) == 0,
          "idx 32 rejected");
    CHECK(fvp_textattr_set_size(a, FVP_TEXT_WINDOWS, -1, 20, 20) == 0,
          "idx -1 rejected");
    CHECK(fvp_textattr_set_speed(a, FVP_TEXT_WINDOWS, FVP_TEXT_WINDOWS, 5) == 0,
          "idx == count rejected");
    CHECK(fvp_textattr_set_color(NULL, FVP_TEXT_WINDOWS, 0, 1, 2, 3) == 0,
          "NULL rejected");

    /* 6. configured_count 只数设过的窗口（本例 1,2,3,4,5：4b 用了 3、4c 用了 5）。 */
    CHECK(fvp_textattr_configured_count(a, FVP_TEXT_WINDOWS) == 5,
          "configured=%d", fvp_textattr_configured_count(a, FVP_TEXT_WINDOWS));
    CHECK(fvp_textattr_configured_count(NULL, FVP_TEXT_WINDOWS) == 0,
          "configured(NULL)=0");

    /* 7. reset 清掉一切。 */
    fvp_textattr_reset(a, FVP_TEXT_WINDOWS);
    CHECK(fvp_textattr_configured_count(a, FVP_TEXT_WINDOWS) == 0,
          "reset count=%d", fvp_textattr_configured_count(a, FVP_TEXT_WINDOWS));
    CHECK(a[3].r == 255 && a[3].set_mask == 0u, "reset colour/mask");

    printf("\ntest_textattr: %d passed, %d failed\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}