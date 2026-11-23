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
    attrs[idx].speed_ms = clamp(ms, 0, FVP_TEXT_SPEED_MAX);
    attrs[idx].set_mask |= FVP_TEXTATTR_SPEED;
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