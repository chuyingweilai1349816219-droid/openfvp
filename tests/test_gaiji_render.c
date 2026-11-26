/* 外字渲染单元测试：GaijiLoad(21) → gfx_gaiji_add() → draw_text_run 里的接管。
 *
 *   bin/test_gaiji_render.exe
 *
 * 语义（docs/roadmap.md「下一步」+ docs/vm-commands.md 的 21 行）：
 *   原版 GaijiLoad(字符, 字号, 图名) 把某个字符在绘制时换成图。
 *   本站：syscall 层把图装进高号图形槽 + gfx_gaiji_add(cp, size, slot) 登记；
 *   文本绘制命中码点时用槽里的图按**当前字号**等比缩放画上去、按缩放宽度推进。
 *
 * 这个测试用假 host 造一张 8x8 纯红图，直接验证"画没画上去"和参数校验。
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../src/gfx.h"

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

/* 假 host：任何名字都给一张 8x8 纯红（R=255,G=0,B=0,A=255）的图。
 * 名字以 "BAD" 开头时返回失败，用来测"加载失败就不登记"。 */
static int fake_load(void *ud, const char *name, GfxImage *out)
{
    int i;

    (void)ud;
    if (strncmp(name, "BAD", 3) == 0) {
        return 1;
    }
    out->w = 8;
    out->h = 8;
    out->x = 0;
    out->y = 0;
    out->frames = 1;
    out->rgba = (uint8_t *)malloc(8u * 8u * 4u);
    if (out->rgba == NULL) {
        return 1;
    }
    for (i = 0; i < 8 * 8; i++) {
        out->rgba[i * 4 + 0] = 255;
        out->rgba[i * 4 + 1] = 0;
        out->rgba[i * 4 + 2] = 0;
        out->rgba[i * 4 + 3] = 255;
    }
    return 0;
}

static void fake_free(void *ud, GfxImage *img)
{
    (void)ud;
    free(img->rgba);
    img->rgba = NULL;
}

/* 在 (x0,y0)-(x1,y1) 里找纯红像素 */
static int has_red(Gfx *g, int x0, int y0, int x1, int y1)
{
    const uint8_t *c;
    int w = 0;
    int h = 0;

    c = gfx_canvas_rgba(g, &w, &h);
    if (c == NULL) {
        return 0;
    }
    if (x1 > w) {
        x1 = w;
    }
    if (y1 > h) {
        y1 = h;
    }
    for (; y0 < y1; y0++) {
        int x;

        for (x = x0; x < x1; x++) {
            const uint8_t *p = c + ((size_t)y0 * (size_t)w + (size_t)x) * 4u;

            if (p[0] > 200 && p[1] < 60 && p[2] < 60) {
                return 1;
            }
        }
    }
    return 0;
}

int main(void)
{
    GfxHost host;
    Gfx *g;

    memset(&host, 0, sizeof host);
    host.load_image = fake_load;
    host.free_image = fake_free;

    /* 画布够大：层 0 的默认排版是 x=150,y=905,size=34 */
    g = gfx_create(400, 1000, &host);
    CHECK(g != NULL, "gfx_create");
    if (g == NULL) {
        return 1;
    }

    /* 2026-11-26 起默认路径不再用版面表兜底（用户要求 #3），
     * 所以先给 0 号窗口建一个文本图元，把文字固定在版面位置 (150,905)。 */
    (void)gfx_set_prim(g, 201, GFX_F_TYPE, 5);
    (void)gfx_set_prim(g, 201, GFX_F_TW, 0);
    (void)gfx_set_prim(g, 201, GFX_F_X, 150);
    (void)gfx_set_prim(g, 201, GFX_F_Y, 905);
    (void)gfx_set_prim(g, 201, GFX_F_ALPHA, 255);
    (void)gfx_set_prim(g, 201, GFX_F_DRAW, 1);
    (void)gfx_set_prim(g, 201, GFX_F_PARENT, 0);

    /* --- 1. 没登记外字时，字符 'X' 走内建字形，不是纯红 --- */
    CHECK(gfx_gaiji_count(g) == 0, "fresh gaiji table");
    gfx_text_append(g, 0, "X");
    gfx_render(g);
    CHECK(!has_red(g, 140, 895, 200, 950), "no red before GaijiLoad");

    /* --- 2. 把图装进图形槽并登记 'X' --- */
    CHECK(gfx_graph_load(g, 0x0F00, "GA8") == 0, "graph load");
    CHECK(gfx_gaiji_add(g, 'X', 34, 0x0F00) == 1, "gaiji add");
    CHECK(gfx_gaiji_count(g) == 1, "count 1 (got %d)", gfx_gaiji_count(g));

    gfx_text_clear(g, 0);
    gfx_text_append(g, 0, "X");
    gfx_render(g);
    CHECK(has_red(g, 140, 895, 200, 950), "gaiji image drawn at the text pos");

    /* --- 3. 重复登记同一码点：覆盖而不加新条目 --- */
    CHECK(gfx_gaiji_add(g, 'X', 34, 0x0F00) == 1, "re-add ok");
    CHECK(gfx_gaiji_count(g) == 1, "count stays 1 (got %d)",
          gfx_gaiji_count(g));

    /* --- 4. 参数校验 --- */
    CHECK(gfx_gaiji_add(g, 0u, 34, 0x0F00) == 0, "codepoint 0 rejected");
    CHECK(gfx_gaiji_add(g, 'Y', 34, GFX_GRAPH_MAX) == 0, "slot overflow");
    CHECK(gfx_gaiji_add(g, 'Y', 34, -1) == 0, "negative slot rejected");
    CHECK(gfx_gaiji_add(NULL, 'Y', 34, 0x0F00) == 0, "NULL gfx rejected");
    CHECK(gfx_gaiji_count(NULL) == 0, "NULL count");
    CHECK(gfx_gaiji_count(g) == 1, "table unchanged by rejects (got %d)",
          gfx_gaiji_count(g));

    /* --- 5. 没加载成功的槽（空槽）不应画东西，也不应崩 --- */
    CHECK(gfx_gaiji_add(g, 'Z', 34, 0x0F01) == 1, "register unused slot");
    gfx_text_clear(g, 0);
    gfx_text_append(g, 0, "Z");
    gfx_render(g);
    CHECK(!has_red(g, 140, 895, 200, 950), "empty slot draws nothing");

    gfx_destroy(g);

    printf("\ntest_gaiji_render: %d passed, %d failed\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}