/* 标题菜单布局/命中的单元测试。
 *
 *   bin/test_titlemenu.exe
 *
 * 验收标准来自原版截图实测（docs/title-menu.md 与 titlemenu.h 文件头）：
 * 5 项右对齐到 x=1769、行距 100、墨迹高 40..42（由字号 56 反推）、
 * 墨迹中心 y = 531/631/732/831/931。这里用一个"假测宽"复现原版实测宽度，
 * 检查布局算出来的矩形与实测值逐项吻合（允许 ±1 px，原版本身行距就有 99..101）。
 */
#include <stdio.h>
#include <string.h>

#include "../src/titlemenu.h"

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

/* 原版实测墨迹：左边界、宽度、中心 y、墨迹高 */
static const int kOrigX0[FVP_TITLE_ITEMS] = { 1461, 1514, 1580, 1575, 1662 };
static const int kOrigW[FVP_TITLE_ITEMS] = { 309, 256, 189, 194, 107 };
static const int kOrigCy[FVP_TITLE_ITEMS] = { 531, 631, 732, 831, 931 };
static const int kOrigInkH[FVP_TITLE_ITEMS] = { 42, 42, 42, 42, 40 };

/* 假测宽：按标签返回上表里的实测宽度（字符串比较，不用查表顺序） */
static int fake_measure(void *ctx, const char *utf8, int font_px, int *width)
{
    int i;

    (void)ctx;
    (void)font_px;
    for (i = 0; i < FVP_TITLE_ITEMS; ++i) {
        if (strcmp(utf8, fvp_title_label(i)) == 0) {
            *width = kOrigW[i];
            return 1;
        }
    }
    return 0;
}

static int abs_diff(int a, int b)
{
    int d = a - b;

    return d < 0 ? -d : d;
}

int main(void)
{
    FvpTitleMenu m;
    int i;

    /* --- 1. 初始化：几何默认值就是实测值 --- */
    fvp_title_init(&m);
    CHECK(m.client_w == 1920 && m.client_h == 1080, "client %dx%d",
          m.client_w, m.client_h);
    CHECK(m.x_right == 1769, "x_right=%d", m.x_right);
    CHECK(m.y_first == 511, "y_first=%d", m.y_first);
    CHECK(m.pitch == 100, "pitch=%d", m.pitch);
    CHECK(m.font_px == 56, "font_px=%d", m.font_px);
    CHECK(m.hover == -1 && m.selected == -1, "hover/selected init");
    for (i = 0; i < FVP_TITLE_ITEMS; ++i) {
        CHECK(m.enabled[i] == 1, "item %d enabled", i);
    }

    /* --- 2. 标签 --- */
    CHECK(strcmp(fvp_title_label(0), "New Game") == 0, "label 0");
    CHECK(strcmp(fvp_title_label(1), "Continue") == 0, "label 1");
    CHECK(strcmp(fvp_title_label(2), "Album") == 0, "label 2");
    CHECK(strcmp(fvp_title_label(3), "Option") == 0, "label 3");
    CHECK(strcmp(fvp_title_label(4), "Exit") == 0, "label 4");
    CHECK(strcmp(fvp_title_label(-1), "") == 0, "label -1 empty");
    CHECK(strcmp(fvp_title_label(99), "") == 0, "label 99 empty");

    /* --- 3. 布局与原版实测逐项吻合 --- */
    for (i = 0; i < FVP_TITLE_ITEMS; ++i) {
        int x0 = 0;
        int y0 = 0;
        int x1 = 0;
        int y1 = 0;
        int cy;

        CHECK(fvp_title_item_rect(&m, i, fake_measure, NULL, &x0, &y0, &x1,
                                  &y1) == 1,
              "rect %d ok", i);
        CHECK(abs_diff(x1, 1769) == 0, "item %d right edge %d (want 1769)", i,
              x1);
        CHECK(abs_diff(x0, kOrigX0[i]) <= 1, "item %d left %d (want %d)", i, x0,
              kOrigX0[i]);
        cy = (y0 + y1) / 2;
        CHECK(abs_diff(cy, kOrigCy[i]) <= 2, "item %d centre y %d (want %d)", i,
              cy, kOrigCy[i]);
        /* 墨迹框高（字号 3/4 = 42）必须和实测墨迹高（40..42）对得上 */
        CHECK(abs_diff(y1 - y0, kOrigInkH[i]) <= 2, "item %d ink h %d (want %d)",
              i, y1 - y0, kOrigInkH[i]);
    }

    /* --- 4. 命中判定 --- */
    CHECK(fvp_title_hit(&m, 1615, 531, fake_measure, NULL) == 0, "hit New Game");
    CHECK(fvp_title_hit(&m, 1641, 631, fake_measure, NULL) == 1, "hit Continue");
    CHECK(fvp_title_hit(&m, 1674, 732, fake_measure, NULL) == 2, "hit Album");
    CHECK(fvp_title_hit(&m, 1671, 831, fake_measure, NULL) == 3, "hit Option");
    CHECK(fvp_title_hit(&m, 1715, 931, fake_measure, NULL) == 4, "hit Exit");
    /* 左侧空白处不命中 */
    CHECK(fvp_title_hit(&m, 100, 531, fake_measure, NULL) == -1, "miss left");
    /* 行高带（100 px）把整行都做成可点区，所以行与行之间没有空隙；
     * 但第一行上方 / 最后一行下方仍是空白：New Game 带 482..581、Exit 带 882..980 */
    CHECK(fvp_title_hit(&m, 1615, 590, fake_measure, NULL) == 1,
          "band tiling: 590 hits Continue");
    CHECK(fvp_title_hit(&m, 1615, 470, fake_measure, NULL) == -1, "miss above");
    CHECK(fvp_title_hit(&m, 1615, 1000, fake_measure, NULL) == -1,
          "miss below");
    /* 横向 ±24 px 余量：x0-20 命中、x0-40 不命中（New Game 左边界 1460） */
    CHECK(fvp_title_hit(&m, 1460 - 20, 531, fake_measure, NULL) == 0,
          "hit pad -20");
    CHECK(fvp_title_hit(&m, 1460 - 40, 531, fake_measure, NULL) == -1,
          "miss pad -40");

    /* --- 5. 移动 / 点击 / 禁用项 --- */
    CHECK(fvp_title_move(&m, 1615, 531, fake_measure, NULL) == 0, "move hover 0");
    CHECK(m.hover == 0, "hover=%d", m.hover);
    CHECK(fvp_title_move(&m, 10, 10, fake_measure, NULL) == -1, "move off");
    CHECK(m.hover == -1, "hover cleared");
    CHECK(fvp_title_click(&m, 1615, 531, fake_measure, NULL) == 0, "click 0");
    CHECK(m.selected == 0, "selected=%d", m.selected);
    CHECK(fvp_title_click(&m, 1671, 831, fake_measure, NULL) == 3, "click 3");
    CHECK(m.selected == 3, "selected=%d", m.selected);
    m.enabled[1] = 0; /* Continue 没存档时禁用 */
    CHECK(fvp_title_click(&m, 1641, 631, fake_measure, NULL) == -1,
          "click disabled Continue");
    CHECK(m.selected == 3, "disabled click keeps selection");
    CHECK(fvp_title_click(&m, 10, 10, fake_measure, NULL) == -1, "click miss");
    m.enabled[1] = 1;

    /* --- 6. 没有字体时回落到等宽估计，但右对齐不变 --- */
    for (i = 0; i < FVP_TITLE_ITEMS; ++i) {
        int x0 = 0;
        int y0 = 0;
        int x1 = 0;
        int y1 = 0;

        CHECK(fvp_title_item_rect(&m, i, NULL, NULL, &x0, &y0, &x1, &y1) == 1,
              "estimate rect %d", i);
        CHECK(x1 == 1769, "estimate right edge %d", x1);
        CHECK(x0 == 1769 - (int)(strlen(fvp_title_label(i)) * 28),
              "estimate left %d for '%s'", x0, fvp_title_label(i));
        CHECK(y1 > y0, "estimate band non-empty %d", i);
    }

    /* --- 7. NULL 安全 --- */
    CHECK(fvp_title_item_rect(NULL, 0, NULL, NULL, NULL, NULL, NULL, NULL) == 0,
          "rect NULL menu");
    CHECK(fvp_title_item_rect(&m, 99, NULL, NULL, NULL, NULL, NULL, NULL) == 0,
          "rect bad item");
    CHECK(fvp_title_hit(NULL, 0, 0, NULL, NULL) == -1, "hit NULL menu");
    CHECK(fvp_title_move(NULL, 0, 0, NULL, NULL) == -1, "move NULL menu");
    CHECK(fvp_title_click(NULL, 0, 0, NULL, NULL) == -1, "click NULL menu");

    printf("test_titlemenu: %d passed, %d failed\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}