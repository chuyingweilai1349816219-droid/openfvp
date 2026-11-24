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
        attrs[i].shadow_dist = 0;
        attrs[i].space_x = 0;
        attrs[i].space_y = 0;
        attrs[i].skip_mode = 0;
        attrs[i].font_id = 0;
        attrs[i].font_id2 = 0;
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

/* TextShadowDist(twIdx, dist)：静态分析 **0x43CBB0**（不是 0x43CE10！）。
 *   arg0 必须是 int 0..0x1F（twIdx）；
 *   arg1 不是 int 时按 0 处理；是 int 时 <0 当 0、>12 当 12（钳制）；
 *   最后转 0x4248B0(tw, dist)，那里就是 `mov %al,0x597(%ecx)` —— 只存一字节。 */
int fvp_textattr_set_shadow(FvpTextAttr *attrs, int count, int idx, int dist)
{
    if (attrs == NULL || idx < 0 || idx >= count || idx >= FVP_TEXT_WINDOWS) {
        return 0;
    }
    attrs[idx].shadow_dist = clamp(dist, FVP_TEXT_SHADOW_MIN,
                                   FVP_TEXT_SHADOW_MAX);
    attrs[idx].set_mask |= FVP_TEXTATTR_SHADOW;
    return 1;
}

/* TextSpace(twIdx, x, y)：静态分析 **0x43CE10**。两个分量各自独立判定：
 * 某个分量超出 -32..32 时只跳过该分量，另一个仍然写进去
 * （两个独立的 jl/jg 分支，互不影响）；两个都被跳过时返回 0。 */
int fvp_textattr_set_space(FvpTextAttr *attrs, int count, int idx, int x, int y)
{
    int wrote = 0;

    if (attrs == NULL || idx < 0 || idx >= count || idx >= FVP_TEXT_WINDOWS) {
        return 0;
    }
    if (x >= FVP_TEXT_SPACE_MIN && x <= FVP_TEXT_SPACE_MAX) {
        attrs[idx].space_x = x;
        wrote = 1;
    }
    if (y >= FVP_TEXT_SPACE_MIN && y <= FVP_TEXT_SPACE_MAX) {
        attrs[idx].space_y = y;
        wrote = 1;
    }
    if (wrote) {
        attrs[idx].set_mask |= FVP_TEXTATTR_SPACE;
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

/* TextFont(twIdx, fontId, fontId2)：静态分析 **0x43DA70**。
 *   arg0 必须是 int 0..0x1F（twIdx）；
 *   arg1 是 int 且 >= -5 且 <= (引擎字体数-1) → 0x4247F0 写 tw+0x590（u16）；
 *   arg2 是 int 且 >= -5 且 <= (引擎字体数-1) → 0x424860 写 tw+0x593（u8）；
 *   两个分量各自独立判定，超范围的那一个跳过。VM 层用 FVP_TEXT_FONT_MAX 代替
 *   "字体数-1"（真正的上界在渲染层，见 textattr.h 的说明）。 */
int fvp_textattr_set_font(FvpTextAttr *attrs, int count, int idx, int id,
                          int id2)
{
    int wrote = 0;

    if (attrs == NULL || idx < 0 || idx >= count || idx >= FVP_TEXT_WINDOWS) {
        return 0;
    }
    if (id >= FVP_TEXT_FONT_MIN && id <= FVP_TEXT_FONT_MAX) {
        attrs[idx].font_id = id;
        wrote = 1;
    }
    if (id2 >= FVP_TEXT_FONT_MIN && id2 <= FVP_TEXT_FONT_MAX) {
        attrs[idx].font_id2 = id2;
        wrote = 1;
    }
    if (wrote) {
        attrs[idx].set_mask |= FVP_TEXTATTR_FONT;
    }
    return wrote;
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
