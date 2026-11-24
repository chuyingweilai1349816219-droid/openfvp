/* 文本窗口属性存储。见 textattr.h。 */
#include "textattr.h"

#include <stddef.h>

static int clamp(int v, int lo, int hi)
{
    if (v < lo) {
        return lo;
    }
    if (v > hi) {
        return hi;
    }
    return v;
}

void fvp_textattr_reset(FvpTextAttr *attrs, int count)
{
    int i;

    if (attrs == NULL) {
        return;
    }
    for (i = 0; i < count; ++i) {
        attrs[i].r = 255;
        attrs[i].g = 255;
        attrs[i].b = 255;
        attrs[i].w = 12;
        attrs[i].h = 12;
        attrs[i].speed_ms = 0;
        attrs[i].shadow_x = 0;
        attrs[i].shadow_y = 0;
        attrs[i].skip_mode = 0;
        attrs[i].set_mask = 0u;
    }
}

int fvp_textattr_set_color(FvpTextAttr *attrs, int count, int idx, int r, int g,
                           int b)
{
    if (attrs == NULL || idx < 0 || idx >= count || idx >= FVP_TEXT_WINDOWS) {
        return 0;
    }
    attrs[idx].r = clamp(r, 0, FVP_TEXT_COLOR_MAX);
    attrs[idx].g = clamp(g, 0, FVP_TEXT_COLOR_MAX);
    attrs[idx].b = clamp(b, 0, FVP_TEXT_COLOR_MAX);
    attrs[idx].set_mask |= FVP_TEXTATTR_COLOR;
    return 1;
}

int fvp_textattr_set_size(FvpTextAttr *attrs, int count, int idx, int w, int h)
{
    if (attrs == NULL || idx < 0 || idx >= count || idx >= FVP_TEXT_WINDOWS) {
        return 0;
    }
    attrs[idx].w = clamp(w, FVP_TEXT_W_MIN, FVP_TEXT_W_MAX);
    attrs[idx].h = clamp(h, FVP_TEXT_H_MIN, FVP_TEXT_H_MAX);
    attrs[idx].set_mask |= FVP_TEXTATTR_SIZE;
    return 1;
}

int fvp_textattr_set_speed(FvpTextAttr *attrs, int count, int idx, int ms)
{
    if (attrs == NULL || idx < 0 || idx >= count || idx >= FVP_TEXT_WINDOWS) {
        return 0;
    }
    /* 静态分析 0x43CE70：`cmp $-1,%edx; jl ret` 与 `cmp $300000,%edx; jg ret`
     * —— 超范围是**整条命令被忽略**（不改字段），不是钳制。
     * -1 是合法值，在引擎里表示"用设置里的文字速度"；0 = 立即显示。 */
    if (ms < -1 || ms > FVP_TEXT_SPEED_MAX) {
        return 0;
    }
    attrs[idx].speed_ms = ms;
    attrs[idx].set_mask |= FVP_TEXTATTR_SPEED;
    return 1;
}

/* TextShadowDist(twIdx, dx, dy)：静态分析 0x43CE10，两个分量各钳到 -32..32，
 * 分别写 tw+0x5a8（x）与 tw+0x5aa（y），都是 16 位。超范围的分量被忽略
 * （另一个分量仍然生效），与原版程序里两个独立的 jl/jg 判定一致。 */
int fvp_textattr_set_shadow(FvpTextAttr *attrs, int count, int idx, int dx,
                            int dy)
{
    int wrote = 0;

    if (attrs == NULL || idx < 0 || idx >= count || idx >= FVP_TEXT_WINDOWS) {
        return 0;
    }
    if (dx >= FVP_TEXT_SHADOW_MIN && dx <= FVP_TEXT_SHADOW_MAX) {
        attrs[idx].shadow_x = dx;
        wrote = 1;
    }
    if (dy >= FVP_TEXT_SHADOW_MIN && dy <= FVP_TEXT_SHADOW_MAX) {
        attrs[idx].shadow_y = dy;
        wrote = 1;
    }
    if (wrote) {
        attrs[idx].set_mask |= FVP_TEXTATTR_SHADOW;
    }
    return wrote;
}

/* TextSkip(twIdx, mode 0..3)：静态分析 0x43CDD0，字节写 tw+0x5d7；
 * mode < 0 或 > 3 时整条命令被忽略。 */
int fvp_textattr_set_skip(FvpTextAttr *attrs, int count, int idx, int mode)
{
    if (attrs == NULL || idx < 0 || idx >= count || idx >= FVP_TEXT_WINDOWS) {
        return 0;
    }
    if (mode < 0 || mode > FVP_TEXT_SKIP_MAX) {
        return 0;
    }
    attrs[idx].skip_mode = mode;
    attrs[idx].set_mask |= FVP_TEXTATTR_SKIP;
    return 1;
}

int fvp_textattr_configured_count(const FvpTextAttr *attrs, int count)
{
    int i;
    int n = 0;

    if (attrs == NULL) {
        return 0;
    }
    for (i = 0; i < count; ++i) {
        if (attrs[i].set_mask != 0u) {
            n += 1;
        }
    }
    return n;
}