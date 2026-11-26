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
        /* 默认字号：exe 文本窗口构造函数 0x430EA0 写 +0x592=0x10(16)、
         * +0x594=0x0C(12)（ref/exe-text-defaults.json）。之前这里写的是 12x12，
         * 与原版不一致，已按静态分析改正。 */
        attrs[i].w = 16;
        attrs[i].h = 12;
        attrs[i].speed_ms = 0;
        attrs[i].shadow_dist = 0;
        attrs[i].space_x = 0;
        attrs[i].space_y = 0;
        attrs[i].skip_mode = 0;
        /* 默认字体号：构造函数写 +0x590=0xFFFE（word，-2）、+0x593=0xFE
         * （byte，-2）——**不是 0**。0 是"第 0 个字体"，-2 才是"未指定"。 */
        attrs[i].font_id = -2;
        attrs[i].font_id2 = -2;
        attrs[i].fmt_pad_a = 0;
        attrs[i].fmt_pad_b = 0;
        attrs[i].fmt_off_a = 0;
        attrs[i].fmt_off_b = 0;
        attrs[i].fmt_width_floor = 0;
        attrs[i].func_a = 0;
        attrs[i].func_b = 0;
        attrs[i].func_c = 0;
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
int fvp_textattr_set_font_ex(FvpTextAttr *attrs, int count, int idx,
                             int id, int id2, int id_max,
                             int has_id, int has_id2)
{
    int wrote = 0;

    if (attrs == NULL || idx < 0 || idx >= count || idx >= FVP_TEXT_WINDOWS) {
        return 0;
    }
    if (id_max > FVP_TEXT_FONT_MAX) {
        id_max = FVP_TEXT_FONT_MAX;
    }
    if (has_id && id >= FVP_TEXT_FONT_MIN && id <= id_max) {
        attrs[idx].font_id = id;
        wrote = 1;
    }
    if (has_id2 && id2 >= FVP_TEXT_FONT_MIN && id2 <= id_max) {
        attrs[idx].font_id2 = id2;
        wrote = 1;
    }
    if (wrote) {
        attrs[idx].set_mask |= FVP_TEXTATTR_FONT;
    }
    return wrote;
}


/* 单字段赋值：值在 [lo,hi] 内才写，返回 1 表示写了。 */
static int set_in_range(int *dst, int v, int lo, int hi)
{
    if (v < lo || v > hi) {
        return 0;
    }
    *dst = v;
    return 1;
}

/* TextFormat(twIdx, a, b, c, d, e, f)：静态分析 **0x43CC10**，六个字段各自独立判定
 * （每个字段都是 `jl` / `jg` 两个跳过分支，互不影响）：
 *   a → tw+0x5A8（-32..32，与 TextSpace 同一字段）→ space_x
 *   b → tw+0x5AA（-32..32）                        → space_y
 *   c → tw+0x5AE（0..64）                          → fmt_pad_a
 *   d → tw+0x5AC（0..64，且把 tw+0x5E8 抬到 >= d）  → fmt_pad_b
 *   e → tw+0x5B4（-16..16）                        → fmt_off_a
 *   f → tw+0x5B0（-16..16）                        → fmt_off_b
 * 六个全都超范围时返回 0。 */
int fvp_textattr_set_format(FvpTextAttr *attrs, int count, int idx, int a,
                            int b, int c, int d, int e, int f)
{
    int wrote = 0;
    FvpTextAttr *ta;

    if (attrs == NULL || idx < 0 || idx >= count || idx >= FVP_TEXT_WINDOWS) {
        return 0;
    }
    ta = &attrs[idx];
    wrote |= set_in_range(&ta->space_x, a, FVP_TEXT_SPACE_MIN,
                          FVP_TEXT_SPACE_MAX);
    wrote |= set_in_range(&ta->space_y, b, FVP_TEXT_SPACE_MIN,
                          FVP_TEXT_SPACE_MAX);
    wrote |= set_in_range(&ta->fmt_pad_a, c, 0, FVP_TEXT_FMT_PAD_MAX);
    if (set_in_range(&ta->fmt_pad_b, d, 0, FVP_TEXT_FMT_PAD_MAX) != 0) {
        /* tw+0x5E8：只有比当前值更大才抬（静态分析 0x43CC9B 的 `jge` 跳过写入） */
        if (d > ta->fmt_width_floor) {
            ta->fmt_width_floor = d;
        }
        wrote |= 1;
    }
    wrote |= set_in_range(&ta->fmt_off_a, e, FVP_TEXT_FMT_OFF_MIN,
                          FVP_TEXT_FMT_OFF_MAX);
    wrote |= set_in_range(&ta->fmt_off_b, f, FVP_TEXT_FMT_OFF_MIN,
                          FVP_TEXT_FMT_OFF_MAX);
    if (wrote) {
        ta->set_mask |= FVP_TEXTATTR_FORMAT;
    }
    return wrote;
}

/* TextFunction(twIdx, a, b, c)：静态分析 **0x43CCF0**，三个开关各自独立判定：
 *   a → tw+0x5A5（0..1）、b → tw+0x5A4（0..2）、c → tw+0x5A6（0..2）。
 * 注意写盘顺序是 +0x5A5 / +0x5A4 / +0x5A6 —— 字段编号与参数顺序不一致。 */
int fvp_textattr_set_function(FvpTextAttr *attrs, int count, int idx, int a,
                              int b, int c)
{
    int wrote = 0;
    FvpTextAttr *ta;

    if (attrs == NULL || idx < 0 || idx >= count || idx >= FVP_TEXT_WINDOWS) {
        return 0;
    }
    ta = &attrs[idx];
    wrote |= set_in_range(&ta->func_a, a, 0, FVP_TEXT_FUNC1_MAX);
    wrote |= set_in_range(&ta->func_b, b, 0, FVP_TEXT_FUNC2_MAX);
    wrote |= set_in_range(&ta->func_c, c, 0, FVP_TEXT_FUNC3_MAX);
    if (wrote) {
        ta->set_mask |= FVP_TEXTATTR_FUNCTION;
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
