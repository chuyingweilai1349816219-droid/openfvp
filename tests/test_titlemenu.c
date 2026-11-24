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

    /* --- 8. 图像模式：原版菜单项是预渲染贴图（340x80 @ (1445,492+i*100)） ---
     * 这些常数来自 graph.bin 的 title_start* + 对原始标题帧的模板匹配
     * （work/match_title_buttons.py：92.2% 吻合，墨迹右边界正好 1769）。 */
    {
        FvpTitleMenu im;
        int i2;

        fvp_title_init(&im);
        CHECK(im.use_images == 0, "images off by default");
        CHECK(im.img_w == 340 && im.img_h == 80, "image cell %dx%d", im.img_w,
              im.img_h);
        CHECK(im.img_x == 1445 && im.img_y0 == 492, "image origin (%d,%d)",
              im.img_x, im.img_y0);
        CHECK(im.ink_x0_off == 15 && im.ink_x1_off == 324 &&
                  im.ink_y0_off == 19,
              "ink offsets %d/%d/%d", im.ink_x0_off, im.ink_x1_off,
              im.ink_y0_off);

        fvp_title_set_images(&im, 1);
        CHECK(im.use_images == 1, "images on");
        for (i2 = 0; i2 < FVP_TITLE_ITEMS; ++i2) {
            int x0 = 0, y0 = 0, x1 = 0, y1 = 0;
            int ix = 0, iy = 0;

            CHECK(fvp_title_item_rect(&im, i2, NULL, NULL, &x0, &y0, &x1,
                                      &y1) == 1,
                  "image rect %d", i2);
            CHECK(x0 == 1445 && x1 == 1785, "image rect %d x=%d..%d", i2, x0,
                  x1);
            CHECK(y0 == 492 + i2 * 100 && y1 == 572 + i2 * 100,
                  "image rect %d y=%d..%d", i2, y0, y1);
            CHECK(fvp_title_image_pos(&im, i2, &ix, &iy) == 1,
                  "image pos %d", i2);
            CHECK(ix == x0 && iy == y0, "image pos == rect %d", i2);
            /* 由贴图位置推出的墨迹必须与实测**右边界**一致（每项贴图内文字右对齐，
             * 所以右偏移对所有项都是 324；左偏移随字宽不同，只有第 1 项是 15）。 */
            if (i2 == 0) {
                CHECK(abs_diff(x0 + im.ink_x0_off, kOrigX0[i2]) <= 1,
                      "derived ink left %d (want %d)", x0 + im.ink_x0_off,
                      kOrigX0[i2]);
            }
            CHECK(x0 + im.ink_x1_off == 1769, "derived ink right %d",
                  x0 + im.ink_x1_off);
            CHECK(y0 + im.ink_y0_off == kOrigCy[i2] - 20 ||
                      y0 + im.ink_y0_off == kOrigCy[i2] - 21,
                  "derived ink top %d (want ~%d)", y0 + im.ink_y0_off,
                  kOrigCy[i2] - 20);
        }
        /* 命中：图像模式的整行 100 px 带仍然可点 */
        CHECK(fvp_title_hit(&im, 1600, 531, NULL, NULL) == 0,
              "image hit row 0");
        CHECK(fvp_title_hit(&im, 1600, 631, NULL, NULL) == 1,
              "image hit row 1");
        CHECK(fvp_title_hit(&im, 1600, 481, NULL, NULL) == -1,
              "image hit above row 0");
        CHECK(fvp_title_hit(&im, 1400, 531, NULL, NULL) == -1,
              "image hit left of the picture");
        fvp_title_set_images(&im, 0);
        CHECK(im.use_images == 0, "images off again");
    }

    /* --- 9. 每作品 profile：两部作品的标题菜单**布局不一样**（实测） --- */
    {
        const FvpTitleProfile *ir = fvp_title_profile_find("iroseka.hcb");
        const FvpTitleProfile *ho =
            fvp_title_profile_find("C:\\games\\Hoshimemo_HD.hcb");
        FvpTitleMenu pm;
        int i3;

        CHECK(ir == &fvp_title_profile_iroseka, "find iroseka");
        CHECK(ho == &fvp_title_profile_hoshimemo, "find hoshimemo (case/mixed)");
        CHECK(fvp_title_profile_find("sakura.hcb") == NULL, "unknown game NULL");
        CHECK(fvp_title_profile_find(NULL) == NULL, "NULL hint");

        /* --- iroseka：右对齐、行距 100、第 1 项墨迹上沿 511 --- */
        CHECK(ir->align == FVP_TITLE_ALIGN_RIGHT, "iroseka right aligned");
        CHECK(ir->pitch == 100 && ir->ink_top0 == 511, "iroseka pitch/top %d/%d",
              ir->pitch, ir->ink_top0);
        CHECK(ir->right_x == 1769, "iroseka right_x=%d", ir->right_x);
        CHECK(strcmp(ir->items[0].label, "New Game") == 0, "iroseka item 0");
        CHECK(strcmp(ir->items[4].label, "Exit") == 0, "iroseka item 4");
        for (i3 = 0; i3 < FVP_TITLE_ITEMS; ++i3) {
            int x0 = 0, y0 = 0, x1 = 0, y1 = 0;

            CHECK(fvp_title_profile_ink_rect(ir, i3, &x0, &y0, &x1, &y1) == 1,
                  "iroseka ink rect %d", i3);
            CHECK(x1 == kOrigX0[i3] + kOrigW[i3] - 1,
                  "iroseka item %d ink right %d", i3, x1);
            CHECK(abs_diff(y0, kOrigCy[i3] - kOrigInkH[i3] / 2) <= 1,
                  "iroseka item %d ink y0 %d (want %d)", i3, y0,
                  kOrigCy[i3] - kOrigInkH[i3] / 2);
            CHECK(ir->items[i3].asset[0] != '\0', "iroseka item %d has asset",
                  i3);
        }

        /* --- hoshimemo：**居中**、行距 90、第 1 项墨迹上沿 581 --- */
        CHECK(ho->align == FVP_TITLE_ALIGN_CENTER, "hoshimemo centred");
        CHECK(ho->pitch == 90 && ho->ink_top0 == 581, "hoshimemo pitch/top %d/%d",
              ho->pitch, ho->ink_top0);
        CHECK(ho->center_x == 960, "hoshimemo center_x=%d", ho->center_x);
        CHECK(strcmp(ho->items[0].label, "GAME START") == 0,
              "hoshimemo item 0 label '%s'", ho->items[0].label);
        CHECK(strcmp(ho->items[4].label, "EXIT") == 0, "hoshimemo item 4 label");
        for (i3 = 0; i3 < FVP_TITLE_ITEMS; ++i3) {
            int x0 = 0, y0 = 0, x1 = 0, y1 = 0;
            int cx;

            CHECK(fvp_title_profile_ink_rect(ho, i3, &x0, &y0, &x1, &y1) == 1,
                  "hoshimemo ink rect %d", i3);
            cx = (x0 + x1) / 2;
            CHECK(cx >= 958 && cx <= 961, "hoshimemo item %d centred at %d", i3,
                  cx);
            CHECK(y0 == 581 + i3 * 90, "hoshimemo item %d ink top %d", i3, y0);
            CHECK(ho->items[i3].asset_glow[0] != '\0',
                  "hoshimemo item %d has a separate glow asset", i3);
        }

        /* --- 贴图位置：由"墨迹 + 贴图尺寸对称留白"反推，和实测对得上 --- */
        {
            int ix = 0, iy = 0;

            CHECK(fvp_title_profile_image_pos(ir, 0, &ix, &iy) == 1,
                  "iroseka image pos");
            CHECK(abs_diff(ix, 1445) <= 1 && abs_diff(iy, 492) <= 1,
                  "iroseka item0 texture at (%d,%d) want (1445,492)", ix, iy);
            CHECK(fvp_title_profile_image_pos(ho, 0, &ix, &iy) == 1,
                  "hoshimemo image pos");
            CHECK(abs_diff(ix + ho->items[0].tex_w / 2, 960) <= 2,
                  "hoshimemo item0 texture centre %d want 960",
                  ix + ho->items[0].tex_w / 2);
            CHECK(abs_diff(iy, 578) <= 1, "hoshimemo item0 texture y=%d want 578",
                  iy);
        }

        /* profile 命中：用实测墨迹矩形，不需要字体 */
        CHECK(fvp_title_profile_hit(ir, 1615, 531) == 0, "profile hit ir 0");
        CHECK(fvp_title_profile_hit(ir, 1715, 931) == 4, "profile hit ir 4");
        CHECK(fvp_title_profile_hit(ir, 100, 531) == -1, "profile miss ir");
        CHECK(fvp_title_profile_hit(ho, 959, 597) == 0, "profile hit ho 0");
        CHECK(fvp_title_profile_hit(ho, 959, 957) == 4, "profile hit ho 4");
        CHECK(fvp_title_profile_hit(ho, 959, 1050) == -1, "profile miss ho");
        CHECK(fvp_title_profile_hit(NULL, 0, 0) == -1, "profile hit NULL");

        /* --- apply_profile 会改掉菜单状态的几何 --- */
        fvp_title_init(&pm);
        fvp_title_apply_profile(&pm, ho);
        CHECK(pm.pitch == 90 && pm.y_first == 581, "apply hoshimemo pitch/top");
        CHECK(pm.hover == -1 && pm.selected == -1, "apply resets selection");
        fvp_title_apply_profile(&pm, ir);
        CHECK(pm.pitch == 100 && pm.x_right == 1769, "apply iroseka geometry");

        /* 两部作品的关键差别必须真的不一样（防止"只对一款调"） */
        CHECK(ir->pitch != ho->pitch, "pitches differ (100 vs 90)");
        CHECK(ir->align != ho->align, "alignments differ (right vs centre)");
        CHECK(strcmp(ir->items[0].label, ho->items[0].label) != 0,
              "first labels differ (New Game vs GAME START)");
    }

    printf("test_titlemenu: %d passed, %d failed\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}