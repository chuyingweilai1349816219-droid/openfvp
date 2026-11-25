/*
 * 引擎侧游戏内菜单（src/gamemenu.c）单元测试。
 *
 * 原版游戏内菜单由 exe 自己画（脚本只有 MenuMessSkip / TitleMenu 两条命令），
 * 所以这里和标题菜单一样归外壳实现。纯逻辑（布局/命中/导航/激活/可用性）在
 * gamemenu.c 里，这个测试把它钉死：坐标边界、跳过不可用项、循环导航、
 * 打开时 hover 落在第一项、关闭后不再响应。
 */
#include <stdio.h>
#include <string.h>

#include "../src/gamemenu.h"

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

static const char *const kLabels[] = {"存档", "读档", "设置", "返回标题",
                                      "退出"};

int main(void)
{
    FvpGameMenu m;
    int i;
    int w;

    /* --- 1. 初始化 --- */
    fvp_gamemenu_init(&m, kLabels, 5, 300, 64);
    CHECK(m.count == 5, "count (%d)", m.count);
    CHECK(m.active == 0, "starts closed");
    CHECK(m.hover == -1 && m.selected == -1, "no hover/selection");
    CHECK(strcmp(m.items[0].label, "存档") == 0, "label copied (%s)",
          m.items[0].label);
    CHECK(m.items[4].enabled == 1, "items enabled by default");
    CHECK(fvp_gamemenu_item_y(&m, 0) == 300, "item0 y (%d)",
          fvp_gamemenu_item_y(&m, 0));
    CHECK(fvp_gamemenu_item_y(&m, 4) == 300 + 4 * 64, "item4 y (%d)",
          fvp_gamemenu_item_y(&m, 4));

    /* --- 2. 布局：居中 + 最小宽度 --- */
    fvp_gamemenu_layout(&m, 1920, 240, 40);
    w = m.width;
    CHECK(w == 320, "width = text + 2*pad (%d)", w);
    CHECK(m.x == (1920 - w) / 2, "centered x (%d)", m.x);
    fvp_gamemenu_layout(&m, 400, 10, 0);
    CHECK(m.width == 120, "minimum width 120 (%d)", m.width);
    CHECK(m.x == 140, "x clamped by layout (%d)", m.x);
    fvp_gamemenu_layout(&m, 1920, 240, 40);

    /* --- 3. 关闭状态下不响应 --- */
    CHECK(fvp_gamemenu_hit(&m, m.x + 10, 310) == -1, "closed: no hit");
    CHECK(fvp_gamemenu_activate(&m) == -1, "closed: no activate");
    CHECK(fvp_gamemenu_click(&m, m.x + 10, 310) == -1, "closed: no click");
    CHECK(fvp_gamemenu_nav(&m, 1) == -1, "closed: no nav");

    /* --- 4. 打开：hover 落在第一项 --- */
    fvp_gamemenu_open(&m);
    CHECK(m.active == 1, "opened");
    CHECK(m.hover == 0, "hover = 0 on open (%d)", m.hover);

    /* --- 5. 命中测试：行内命中、行间空隙不命中、越界不命中 --- */
    CHECK(fvp_gamemenu_hit(&m, m.x + 5, 300) == 0, "hit item 0 top edge");
    CHECK(fvp_gamemenu_hit(&m, m.x + 5, 300 + 63) == 0, "hit item 0 bottom");
    CHECK(fvp_gamemenu_hit(&m, m.x + 5, 300 + 64) == 1, "hit item 1");
    CHECK(fvp_gamemenu_hit(&m, m.x - 1, 300) == -1, "left of menu: miss");
    CHECK(fvp_gamemenu_hit(&m, m.x + m.width, 300) == -1,
          "right of menu: miss");
    CHECK(fvp_gamemenu_hit(&m, m.x + 5, 300 + 5 * 64) == -1,
          "below the last item: miss");

    /* --- 6. 鼠标移动只更新 hover --- */
    fvp_gamemenu_move(&m, m.x + 5, 300 + 2 * 64 + 3);
    CHECK(m.hover == 2, "move sets hover (%d)", m.hover);
    fvp_gamemenu_move(&m, m.x - 50, 300);
    CHECK(m.hover == 2, "move outside keeps hover (%d)", m.hover);

    /* --- 7. 键盘导航：循环 + 跳过不可用项 --- */
    CHECK(fvp_gamemenu_nav(&m, 1) == 3, "nav down wraps (%d)",
          m.hover);
    CHECK(fvp_gamemenu_nav(&m, 1) == 4, "nav down -> 4 (%d)", m.hover);
    CHECK(fvp_gamemenu_nav(&m, 1) == 0, "nav down wraps to 0 (%d)", m.hover);
    CHECK(fvp_gamemenu_nav(&m, -1) == 4, "nav up wraps to 4 (%d)", m.hover);
    m.items[3].enabled = 0;
    m.hover = 2;
    CHECK(fvp_gamemenu_nav(&m, 1) == 4, "nav skips disabled item (%d)",
          m.hover);
    CHECK(fvp_gamemenu_hit(&m, m.x + 5, 300 + 3 * 64 + 1) == -1,
          "disabled item is not hittable");
    m.items[3].enabled = 1;

    /* --- 8. 激活/点击记录 selected --- */
    m.hover = 2;
    CHECK(fvp_gamemenu_activate(&m) == 2, "activate returns hover");
    CHECK(m.selected == 2, "selected recorded (%d)", m.selected);
    CHECK(fvp_gamemenu_click(&m, m.x + 5, 300 + 4 * 64 + 1) == 4,
          "click returns hit index");
    CHECK(m.selected == 4 && m.hover == 4, "click updates hover/selected");
    CHECK(fvp_gamemenu_click(&m, m.x - 10, 300) == -1, "click outside: -1");

    /* --- 9. 关闭后复位 --- */
    fvp_gamemenu_close(&m);
    CHECK(m.active == 0 && m.hover == -1, "close resets");
    CHECK(m.selected == 4, "selected survives close (%d)", m.selected);

    /* --- 10. 边界：count 上限与 NULL 安全 --- */
    fvp_gamemenu_init(&m, kLabels, 99, 0, 10);
    CHECK(m.count == FVP_GAMEMENU_MAX, "count clamped to %d (%d)",
          FVP_GAMEMENU_MAX, m.count);
    fvp_gamemenu_init(NULL, kLabels, 5, 0, 10);
    fvp_gamemenu_open(NULL);
    fvp_gamemenu_close(NULL);
    CHECK(fvp_gamemenu_hit(NULL, 0, 0) == -1, "NULL hit");
    CHECK(fvp_gamemenu_nav(NULL, 1) == -1, "NULL nav");
    CHECK(fvp_gamemenu_activate(NULL) == -1, "NULL activate");
    for (i = 0; i < m.count; ++i) {
        CHECK(fvp_gamemenu_item_y(&m, i) == i * 10, "item y i=%d", i);
    }

    printf("test_gamemenu: %d passed, %d failed\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}