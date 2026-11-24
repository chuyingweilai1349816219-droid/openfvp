/* FVP engine -- 标题菜单布局/命中（见 titlemenu.h 的文件头注释与实测数字）。 */
#include <string.h>

#include "titlemenu.h"

static const char *const kLabels[FVP_TITLE_ITEMS] = {
    "New Game", "Continue", "Album", "Option", "Exit"
};

const char *fvp_title_label(int item)
{
    if (item < 0 || item >= FVP_TITLE_ITEMS) {
        return "";
    }
    return kLabels[item];
}

void fvp_title_init(FvpTitleMenu *m)
{
    int i;

    if (m == NULL) {
        return;
    }
    memset(m, 0, sizeof(*m));
    m->client_w = 1920;
    m->client_h = 1080;
    m->x_right = 1769;  /* 实测：5 项墨迹右边界都在 1768/1769 */
    m->y_first = 511;   /* 实测：New Game 墨迹上沿 */
    m->pitch = 100;     /* 实测：中心 y 531/631/732/831/931 */
    m->font_px = 56;    /* 由墨迹高 42 px 反推（42 / 0.75） */
    m->hover = -1;
    m->selected = -1;
    for (i = 0; i < FVP_TITLE_ITEMS; ++i) {
        m->enabled[i] = 1;
    }
}

/* 没有字体时的等宽估计：拉丁平均字宽约为字号的 0.5 倍。 */
static int estimate_width(const char *label, int font_px)
{
    size_t n = (label != NULL) ? strlen(label) : 0u;

    return (int)(n * (size_t)(font_px / 2));
}

static int item_width(const FvpTitleMenu *m, int item,
                      FvpTitleMeasure measure, void *ctx)
{
    const char *label = fvp_title_label(item);
    int w = 0;

    if (measure != NULL && measure(ctx, label, m->font_px, &w) && w > 0) {
        return w;
    }
    return estimate_width(label, m->font_px);
}

int fvp_title_item_rect(const FvpTitleMenu *m, int item, FvpTitleMeasure measure,
                        void *ctx, int *x0, int *y0, int *x1, int *y1)
{
    int w;
    int top;
    int ink_h;

    if (m == NULL || item < 0 || item >= FVP_TITLE_ITEMS) {
        return 0;
    }
    w = item_width(m, item, measure, ctx);
    top = m->y_first + item * m->pitch;
    /* 返回的是**墨迹框**：原版实测墨迹高 40..42，这里按字号的 3/4 = 42 算。
     * 点击用的行高带（pitch，100）在 fvp_title_hit 里另外加纵向余量。 */
    ink_h = m->font_px * 3 / 4;
    if (x0 != NULL) {
        *x0 = m->x_right - w;
    }
    if (y0 != NULL) {
        *y0 = top;
    }
    if (x1 != NULL) {
        *x1 = m->x_right;
    }
    if (y1 != NULL) {
        *y1 = top + ink_h;
    }
    return 1;
}

/* 横向命中给墨迹左右各留 24 px、纵向给上下各留 (pitch - ink_h)/2 的余量，
 * 这样整行（100 px）都能点到——原版的悬停行高就是 100。 */
#define TITLE_HIT_PAD 24

int fvp_title_hit(const FvpTitleMenu *m, int mx, int my,
                  FvpTitleMeasure measure, void *ctx)
{
    int i;

    if (m == NULL) {
        return -1;
    }
    for (i = 0; i < FVP_TITLE_ITEMS; ++i) {
        int x0 = 0;
        int y0 = 0;
        int x1 = 0;
        int y1 = 0;
        int pad_y;

        if (!fvp_title_item_rect(m, i, measure, ctx, &x0, &y0, &x1, &y1)) {
            continue;
        }
        pad_y = (m->pitch - (y1 - y0)) / 2;
        if (mx >= x0 - TITLE_HIT_PAD && mx <= x1 + TITLE_HIT_PAD &&
            my >= y0 - pad_y && my < y1 + pad_y) {
            return i;
        }
    }
    return -1;
}

int fvp_title_move(FvpTitleMenu *m, int mx, int my, FvpTitleMeasure measure,
                   void *ctx)
{
    if (m == NULL) {
        return -1;
    }
    m->hover = fvp_title_hit(m, mx, my, measure, ctx);
    return m->hover;
}

int fvp_title_click(FvpTitleMenu *m, int mx, int my, FvpTitleMeasure measure,
                    void *ctx)
{
    int item;

    if (m == NULL) {
        return -1;
    }
    item = fvp_title_hit(m, mx, my, measure, ctx);
    if (item < 0 || !m->enabled[item]) {
        return -1;
    }
    m->hover = item;
    m->selected = item;
    return item;
}