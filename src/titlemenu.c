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
        m->img_ready[i] = 0;
    }
    /* 图像模式常数：graph.bin 的 title_start* 都是 340x80，贴图内文字右对齐，
     * 墨迹在贴图内 x=15..324、y=19..60（对原版帧模板匹配得到，见头文件注释）。 */
    m->use_images = 0;
    m->img_w = 340;
    m->img_h = 80;
    m->img_x = 1445;
    m->img_y0 = 492;
    m->ink_x0_off = 15;
    m->ink_x1_off = 324;
    m->ink_y0_off = 19;
}

/* ------------------------------------------------------------------ */
/* 每作品 profile（数字全部来自原版截图实测）                            */
/* ------------------------------------------------------------------ */

/* 五彩斑斓的世界 HD（iroseka）：右对齐、行距 100、贴图 340x80（文字 + 发光烘焙
 * 在同一张图里），第 1 项墨迹上沿 511。实测表见 docs/title-menu.md §1。 */
const FvpTitleProfile fvp_title_profile_iroseka = {
    "iroseka", FVP_TITLE_ALIGN_RIGHT, 1920, 1080, 100, 511, 1769, 960,
    {
        /* 2026-11-25 更正：iroseka 的 5 个菜单项**各自都有贴图**
         * （`title_start` / `title_continue` / `title_album` / `title_option` /
         * `title_end`），每项另有 5 个状态变体（`title_start1..5` 这种，选中/
         * 悬停是**换图**）。贴图矩形直接来自素材头（ref/title-rects.json）：
         *   start    (1445,495,340,80)  continue (1495,595,290,80)
         *   album    (1560,695,225,80)  option   (1560,795,225,90)
         *   end      (1645,895,140,80)
         * 右边缘 5 项全部 1785、行距恰好 100 —— 见 docs/title-menu.md §9。 */
        { "New Game", "title_start", "title_start1", "", 340, 80, 1445, 495, 1461, 1769, 511, 552 },
        { "Continue", "title_continue", "title_continue1", "", 290, 80, 1495, 595, 1514, 1769, 611, 652 },
        { "Album", "title_album", "title_album1", "", 225, 80, 1560, 695, 1580, 1768, 712, 753 },
        { "Option", "title_option", "title_option1", "", 225, 90, 1560, 795, 1575, 1768, 811, 852 },
        { "Exit", "title_end", "title_end1", "", 140, 80, 1645, 895, 1662, 1768, 912, 951 }
    },
    "title_bg1_A", /* 白天背景 2400x1350：只显示中间 1920x1080 一块 */
    240, 135,      /* 中心裁剪（(2400-1920)/2, (1350-1080)/2) */
    /* 实测（work/compose_iro_layers.py）：A 层只有云，风车/花瓣/光束在 B/C/D 里；
     * 三种顺序的平均差相差 <0.6，这里用测到最好的 C→D→B。 */
    { "title_bg1_C", "title_bg1_D", "title_bg1_B", "" }, 3,
    /* 原版菜单是深蓝衬线字：实测暗部均值 RGB(7,26,146) */
    { 10, 30, 140 },
    "title_logo1", /* 标题 logo（位置在图片头里） */
    ""             /* 版权行素材还没指认 */
};

/* 星空的记忆 HD（hoshimemo）：**全部居中于 x=960**、行距 90、贴图 275x40 级
 * （文字/发光是分开的两张图：title_start + title_start_ray），第 1 项墨迹上沿 581。
 * 2026-11-24 用 work/measure_hoshi.py 在原始标题帧上量出来的。 */
const FvpTitleProfile fvp_title_profile_hoshimemo = {
    "hoshimemo", FVP_TITLE_ALIGN_CENTER, 1920, 1080, 90, 581, 1769, 960,
    {
        /* 贴图矩形来自素材头（ref/title-rects.json）：行距恰好 90、中心 960；
         * GAME START 的素材宽 275 是奇数（右边有透明边），所以它的 tex_x 是 820
         * 而不是 822。2026-11-25 把 tex_x/tex_y 填成素材头的原值。 */
        { "GAME START", "title_start", "", "title_start_ray", 275, 40, 820, 580, 830, 1088,
          581, 614 },
        { "CONTINUE", "title_continue", "", "title_continue_ray", 230, 40, 845, 670, 856,
          1064, 671, 704 },
        { "ALBUM", "title_album", "", "title_album_ray", 160, 40, 880, 760, 891, 1028, 761,
          793 },
        { "OPTION", "title_option", "", "title_option_ray", 170, 40, 875, 850, 884, 1036, 851,
          884 },
        { "EXIT", "title_end", "", "title_end_ray", 110, 40, 905, 940, 914, 1005, 941, 973 }
    },
    "title_bg1",       /* 整屏白天背景（1920x1080，已与原版截图核对内容） */
    0, 0,
    /* 原版背景上还有一层整体提亮：title_frame2 是纯白 RGBA（alpha 均值 72），
     * 叠上后与原版标题帧的平均差从 48.0 降到 34.6；title_frame 再补一点暗角。 */
    { "title_frame2", "title_frame", "", "" }, 2,
    { 246, 246, 252 }, /* 星空走贴图模式，颜色只用于兜底文字 */
    "title_logo1",     /* 标题 logo：1082x418 @ (419,72) */
    "title_copyright"  /* 版权行：445x24 @ (737,1043) */
};

static int ascii_lower(int c)
{
    return (c >= 'A' && c <= 'Z') ? c - 'A' + 'a' : c;
}

static int str_contains_ci(const char *haystack, const char *needle)
{
    size_t hl;
    size_t nl;
    size_t i;

    if (haystack == NULL || needle == NULL) {
        return 0;
    }
    hl = strlen(haystack);
    nl = strlen(needle);
    if (nl == 0 || nl > hl) {
        return 0;
    }
    for (i = 0; i + nl <= hl; ++i) {
        size_t j;

        for (j = 0; j < nl; ++j) {
            if (ascii_lower((unsigned char)haystack[i + j]) !=
                ascii_lower((unsigned char)needle[j])) {
                break;
            }
        }
        if (j == nl) {
            return 1;
        }
    }
    return 0;
}

const FvpTitleProfile *fvp_title_profile_find(const char *hint)
{
    if (hint == NULL) {
        return NULL;
    }
    if (str_contains_ci(hint, "iroseka") ||
        str_contains_ci(hint, "irotoridori")) {
        return &fvp_title_profile_iroseka;
    }
    if (str_contains_ci(hint, "hoshimemo") ||
        str_contains_ci(hint, "hoshimemoria") ||
        str_contains_ci(hint, "hoshizora")) {
        return &fvp_title_profile_hoshimemo;
    }
    return NULL;
}

void fvp_title_apply_profile(FvpTitleMenu *m, const FvpTitleProfile *p)
{
    if (m == NULL || p == NULL) {
        return;
    }
    m->client_w = p->client_w;
    m->client_h = p->client_h;
    m->pitch = p->pitch;
    m->y_first = p->ink_top0;
    m->x_right = p->right_x;
    /* 居中模式：所有项的墨迹都居中在 center_x，文字模式用这个基准 */
    if (p->align == FVP_TITLE_ALIGN_CENTER) {
        m->x_right = p->center_x;
    }
    m->hover = -1;
    m->selected = -1;
}

int fvp_title_profile_ink_rect(const FvpTitleProfile *p, int item, int *x0,
                               int *y0, int *x1, int *y1)
{
    const FvpTitleItemDef *d;

    if (p == NULL || item < 0 || item >= FVP_TITLE_ITEMS) {
        return 0;
    }
    d = &p->items[item];
    if (x0 != NULL) {
        *x0 = d->ink_x0;
    }
    if (y0 != NULL) {
        *y0 = d->ink_y0;
    }
    if (x1 != NULL) {
        *x1 = d->ink_x1;
    }
    if (y1 != NULL) {
        *y1 = d->ink_y1;
    }
    return 1;
}

int fvp_title_profile_image_pos(const FvpTitleProfile *p, int item, int *x,
                                int *y)
{
    const FvpTitleItemDef *d;
    int ink_w;
    int ink_h;
    int pad_x;
    int pad_y;

    if (p == NULL || item < 0 || item >= FVP_TITLE_ITEMS) {
        return 0;
    }
    d = &p->items[item];
    /* 素材头给了 x/y 就直接用（2026-11-25：ref/title-rects.json）。
     * 只有没给的时候，才退回"墨迹 + 四周均分留白"的反推：
     *   iroseka  item0: ink 309x42，tex 340x80 → pad (15,19) → (1446, 492)
     *   hoshimemo item0: ink 259x34，tex 275x40 → pad (8,3) → (822,578)
     * 素材头的原值分别是 (1445,495) 与 (820,580)——反推差 1–3 px，所以优先用原值。 */
    if (d->tex_x != 0 || d->tex_y != 0) {
        if (x != NULL) {
            *x = d->tex_x;
        }
        if (y != NULL) {
            *y = d->tex_y;
        }
        return 1;
    }
    ink_w = d->ink_x1 - d->ink_x0 + 1;
    ink_h = d->ink_y1 - d->ink_y0 + 1;
    pad_x = (d->tex_w - ink_w) / 2;
    pad_y = (d->tex_h - ink_h) / 2;
    if (x != NULL) {
        *x = d->ink_x0 - pad_x;
    }
    if (y != NULL) {
        *y = d->ink_y0 - pad_y;
    }
    return 1;
}

int fvp_title_profile_hit(const FvpTitleProfile *p, int mx, int my)
{
    int i;

    if (p == NULL) {
        return -1;
    }
    for (i = 0; i < FVP_TITLE_ITEMS; ++i) {
        const FvpTitleItemDef *d = &p->items[i];
        int pad_y = (p->pitch - (d->ink_y1 - d->ink_y0 + 1)) / 2;

        if (pad_y < 0) {
            pad_y = 0;
        }
        if (mx >= d->ink_x0 - 24 && mx <= d->ink_x1 + 24 &&
            my >= d->ink_y0 - pad_y && my <= d->ink_y1 + pad_y) {
            return i;
        }
    }
    return -1;
}

void fvp_title_set_images(FvpTitleMenu *m, int on)
{
    if (m != NULL) {
        m->use_images = on ? 1 : 0;
    }
}

int fvp_title_image_pos(const FvpTitleMenu *m, int item, int *x, int *y)
{
    if (m == NULL || item < 0 || item >= FVP_TITLE_ITEMS) {
        return 0;
    }
    if (x != NULL) {
        *x = m->img_x;
    }
    if (y != NULL) {
        *y = m->img_y0 + item * m->pitch;
    }
    return 1;
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
    if (m->use_images) {
        /* 图像模式：返回贴图矩形（原版就是按这个位置贴图的）。 */
        if (x0 != NULL) {
            *x0 = m->img_x;
        }
        if (y0 != NULL) {
            *y0 = m->img_y0 + item * m->pitch;
        }
        if (x1 != NULL) {
            *x1 = m->img_x + m->img_w;
        }
        if (y1 != NULL) {
            *y1 = m->img_y0 + item * m->pitch + m->img_h;
        }
        return 1;
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

int fvp_title_nav(FvpTitleMenu *m, int delta)
{
    int i;
    int start;
    int all_off = 1;

    if (m == NULL || delta == 0) {
        return m != NULL ? m->hover : -1;
    }
    for (i = 0; i < FVP_TITLE_ITEMS; ++i) {
        if (m->enabled[i]) {
            all_off = 0;
            break;
        }
    }
    if (all_off) {
        m->hover = -1;
        return -1;
    }
    if (m->hover < 0) {
        start = delta > 0 ? -1 : 0;
        m->hover = delta > 0 ? 0 : FVP_TITLE_ITEMS - 1;
        if (!m->enabled[m->hover]) {
            (void)start;
            return fvp_title_nav(m, delta);
        }
        return m->hover;
    }
    i = m->hover;
    do {
        i = (i + (delta > 0 ? 1 : FVP_TITLE_ITEMS - 1)) % FVP_TITLE_ITEMS;
    } while (!m->enabled[i] && i != m->hover);
    m->hover = i;
    return m->hover;
}

int fvp_title_activate(FvpTitleMenu *m)
{
    int i;

    if (m == NULL) {
        return -1;
    }
    if (m->hover < 0 || m->hover >= FVP_TITLE_ITEMS || !m->enabled[m->hover]) {
        for (i = 0; i < FVP_TITLE_ITEMS; ++i) {
            if (m->enabled[i]) {
                m->hover = i;
                break;
            }
        }
    }
    if (m->hover < 0 || !m->enabled[m->hover]) {
        return -1;
    }
    m->selected = m->hover;
    return m->selected;
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