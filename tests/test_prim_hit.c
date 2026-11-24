/* 命中测试单元测试：PrimHit(76) → gfx_prim_hit()。
 *
 * 语义逐条来自静态分析（docs/render-model.md §8）：
 *   handler 0x43B9C0：primId 必须 1..0xFFF（0 非法）；flag = 第二参数非空
 *   0x43B8D0：parent == -1（原版 0xFFFF）→ 未命中；flag==0 且 draw==0 → 未命中；
 *             沿 parent 链累加 x/y，任一层 draw==0 → 未命中；链必须走到 prim 0
 *   0x43B5F0：type 4 sprite → 矩形 = 图头 x/y + prim x/y + 累加偏移；
 *             写过 WH（prim+0x06 bit0）则 w/h 取 min(prim, 图)，否则用整图；
 *             type 2 tile → 矩形 = prim x/y + 累加偏移，alpha 必须非 0；
 *             type 1 group → 递归任一子命中即命中（**不额外加自己的 x/y**）；
 *             type 0/3/5 → 未命中
 *   矩形判定左闭右开；sprite 还要 UV 后的源坐标仍在图内。
 *
 * 注意：矩形之后的"可点像素掩码"测试尚未实现（见 gfx.h 的说明），这里只测
 * 已经落地的部分。
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

/* 图片尺寸/图头锚点由资源名编码：   "A<w>x<h>_<hx>_<hy>"  */
static int fake_load(void *ud, const char *name, GfxImage *out)
{
    int w = 0;
    int h = 0;
    int hx = 0;
    int hy = 0;

    (void)ud;
    if (sscanf(name, "A%dx%d_%d_%d", &w, &h, &hx, &hy) != 4) {
        return 1;
    }
    out->w = w;
    out->h = h;
    out->x = hx;
    out->y = hy;
    out->frames = 1;
    out->rgba = (uint8_t *)malloc((size_t)w * (size_t)h * 4u);
    if (out->rgba == NULL) {
        return 1;
    }
    memset(out->rgba, 0x80, (size_t)w * (size_t)h * 4u);
    return 0;
}

static void fake_free(void *ud, GfxImage *img)
{
    (void)ud;
    free(img->rgba);
    img->rgba = NULL;
}

static void set(Gfx *g, int id, int field, int v)
{
    if (gfx_set_prim(g, id, field, v) != 0) {
        printf("FAIL set prim %d field %d = %d\n", id, field, v);
        g_fail += 1;
    }
}

/* 一个 sprite 图元：type 4 + graph 槽 + parent */
static void sprite(Gfx *g, int id, int graph, int parent, int x, int y)
{
    set(g, id, GFX_F_TYPE, 4);
    set(g, id, GFX_F_GRAPH, graph);
    set(g, id, GFX_F_X, x);
    set(g, id, GFX_F_Y, y);
    if (parent >= 0) {
        (void)gfx_prim_group_in(g, id, parent);
    }
}

int main(void)
{
    GfxHost host;
    Gfx *g;

    memset(&host, 0, sizeof host);
    host.load_image = fake_load;
    host.free_image = fake_free;
    g = gfx_create(1920, 1080, &host);
    if (g == NULL) {
        printf("FAIL gfx_create\n");
        return 1;
    }
    /* 槽 1：200x100，图头锚点 (10,20)；槽 2：50x60，锚点 (0,0) */
    CHECK(gfx_graph_load(g, 1, "A200x100_10_20") == 0, "load slot 1");
    CHECK(gfx_graph_load(g, 2, "A50x60_0_0") == 0, "load slot 2");

    /* ---- 1. 参数合法性 ---- */
    CHECK(gfx_prim_hit(NULL, 5, 1, 0, 0) == 0, "NULL gfx");
    CHECK(gfx_prim_hit(g, 0, 1, 0, 0) == 0, "prim 0 非法（原版 1..0xFFF）");
    CHECK(gfx_prim_hit(g, -3, 1, 0, 0) == 0, "负 id");
    CHECK(gfx_prim_hit(g, GFX_PRIM_MAX, 1, 0, 0) == 0, "id 越界");

    /* ---- 2. 没有 parent（原版 prim+0x0A == 0xFFFF）→ 未命中 ---- */
    sprite(g, 5, 1, -1, 0, 0);
    CHECK(gfx_prim_hit(g, 5, 1, 10, 20) == 0, "无 parent 的图元不可命中");

    /* ---- 3. 挂到根（prim 0）上：矩形 = 图头(10,20) + prim(0,0) + 累加(0,0) ---- */
    CHECK(gfx_prim_group_in(g, 5, 0) == 0, "prim5 -> 根");
    /* 矩形 [10,210) x [20,120)，左闭右开 */
    CHECK(gfx_prim_hit(g, 5, 1, 10, 20) == 1, "左上角算命中");
    CHECK(gfx_prim_hit(g, 5, 1, 209, 119) == 1, "右下角内一像素算命中");
    CHECK(gfx_prim_hit(g, 5, 1, 210, 119) == 0, "右边界外（左闭右开）");
    CHECK(gfx_prim_hit(g, 5, 1, 209, 120) == 0, "下边界外（左闭右开）");
    CHECK(gfx_prim_hit(g, 5, 1, 9, 20) == 0, "左边界外");
    CHECK(gfx_prim_hit(g, 5, 1, 10, 19) == 0, "上边界外");
    CHECK(gfx_prim_hit(g, 5, 1, 0, 0) == 0, "(0,0) 不在矩形里");

    /* ---- 4. 组偏移累加到子图元上 ---- */
    set(g, 0, GFX_F_X, 100);
    set(g, 0, GFX_F_Y, 50);
    /* 矩形变成 [110,310) x [70,170) */
    CHECK(gfx_prim_hit(g, 5, 1, 110, 70) == 1, "根的 x/y 被累加（命中点前移）");
    CHECK(gfx_prim_hit(g, 5, 1, 109, 70) == 0, "累加后左边界外");
    CHECK(gfx_prim_hit(g, 5, 1, 10, 20) == 0, "未累加时的那一点已不该命中");

    /* ---- 5. 父组隐藏 → 整棵子树不可命中（ignore_draw 也救不了） ---- */
    set(g, 0, GFX_F_DRAW, 0);
    CHECK(gfx_prim_hit(g, 5, 0, 110, 70) == 0, "父组隐藏：flag=0 未命中");
    CHECK(gfx_prim_hit(g, 5, 1, 110, 70) == 0, "父组隐藏：flag=1 仍未命中");
    set(g, 0, GFX_F_DRAW, 1);

    /* ---- 6. 起始图元隐藏：flag=0 未命中，flag=1 仍可命中 ---- */
    set(g, 5, GFX_F_DRAW, 0);
    CHECK(gfx_prim_hit(g, 5, 0, 110, 70) == 0, "自身隐藏 + flag=0 → 未命中");
    CHECK(gfx_prim_hit(g, 5, 1, 110, 70) == 1, "自身隐藏 + flag=1 → 仍命中");
    set(g, 5, GFX_F_DRAW, 1);

    /* ---- 7. 写过 WH：w/h 取 min(prim, 图) ---- */
    set(g, 5, GFX_F_W, 300); /* min(300, 200) = 200 */
    set(g, 5, GFX_F_H, 40);  /* min(40, 100)  = 40  */
    CHECK(gfx_prim_hit(g, 5, 1, 110, 70 + 39) == 1, "WH 覆盖后高度内仍命中");
    CHECK(gfx_prim_hit(g, 5, 1, 110, 70 + 40) == 0, "WH 覆盖后高度外不命中");
    CHECK(gfx_prim_hit(g, 5, 1, 110 + 199, 70) == 1, "WH 覆盖后宽度仍是 min=200");
    set(g, 5, GFX_F_W, 0); /* 显式写 0：原版 flags bit0 已置，w 就是 0 */
    set(g, 5, GFX_F_H, 0);
    CHECK(gfx_prim_hit(g, 5, 1, 110, 70) == 0, "显式 WH=0 → 矩形为空，不命中");
    /* 恢复：把 WH 写回整图尺寸 */
    set(g, 5, GFX_F_W, 200);
    set(g, 5, GFX_F_H, 100);

    /* ---- 8. sprite 的 UV 后源坐标必须仍在图内 ---- */
    set(g, 5, GFX_F_U, 190); /* u_src = 190 + (cx - x0) 必须 < 200 */
    CHECK(gfx_prim_hit(g, 5, 1, 110, 70) == 1, "u=190 + dx=0 → 190 < 200 命中");
    CHECK(gfx_prim_hit(g, 5, 1, 120, 70) == 0, "u=190 + dx=10 → 200 越界未命中");
    set(g, 5, GFX_F_U, 0);
    set(g, 5, GFX_F_V, 95);
    CHECK(gfx_prim_hit(g, 5, 1, 110, 70) == 1, "v=95 + dy=0 → 95 < 100 命中");
    CHECK(gfx_prim_hit(g, 5, 1, 110, 75) == 0, "v=95 + dy=5 → 100 越界未命中");
    set(g, 5, GFX_F_V, 0);

    /* ---- 9. type 2（tile）：prim 矩形 + alpha 非 0 ---- */
    set(g, 6, GFX_F_TYPE, 2);
    set(g, 6, GFX_F_W, 30);
    set(g, 6, GFX_F_H, 20);
    set(g, 6, GFX_F_X, 0);
    set(g, 6, GFX_F_Y, 0);
    CHECK(gfx_prim_group_in(g, 6, 0) == 0, "prim6 -> 根");
    /* 累加偏移 (100,50) → 矩形 [100,130) x [50,70) */
    CHECK(gfx_prim_hit(g, 6, 1, 100, 50) == 1, "tile 左上角命中");
    CHECK(gfx_prim_hit(g, 6, 1, 129, 69) == 1, "tile 右下内一像素命中");
    CHECK(gfx_prim_hit(g, 6, 1, 130, 69) == 0, "tile 右边界外");
    CHECK(gfx_prim_hit(g, 6, 1, 129, 70) == 0, "tile 下边界外");
    set(g, 6, GFX_F_ALPHA, 0);
    CHECK(gfx_prim_hit(g, 6, 1, 110, 60) == 0, "tile alpha=0 不可命中");
    set(g, 6, GFX_F_ALPHA, 255);

    /* ---- 10. type 1（组）：递归子图元；组自己的 x/y **不**额外加 ---- */
    set(g, 7, GFX_F_TYPE, 1);
    set(g, 7, GFX_F_X, 1000); /* 组自己的偏移：按原版不被计入子图元 */
    set(g, 7, GFX_F_Y, 1000);
    CHECK(gfx_prim_group_in(g, 7, 0) == 0, "prim7 组 -> 根");
    sprite(g, 8, 2, 7, 0, 0); /* 50x60 图，锚点 (0,0) */
    /* 直接测子图元：链 8→7→0，累加 = 根(100,50) + 组(1000,1000) */
    CHECK(gfx_prim_hit(g, 8, 1, 1100, 1050) == 1, "子图元自身链含组偏移");
    CHECK(gfx_prim_hit(g, 8, 1, 100, 50) == 0, "子图元不在未累加的位置");
    /* 测组本身：链 7→0 只有根偏移，递归给子图元的也是这份偏移 */
    CHECK(gfx_prim_hit(g, 7, 1, 100, 50) == 1,
          "组的命中 = 子图元 + 只算祖先（不加组自己的 x/y）");
    CHECK(gfx_prim_hit(g, 7, 1, 1100, 1050) == 0, "组自己的 x/y 不被计入");

    /* ---- 11. type 5（文本）与 type 0（空）恒不命中 ---- */
    set(g, 9, GFX_F_TYPE, 5);
    (void)gfx_prim_group_in(g, 9, 0);
    CHECK(gfx_prim_hit(g, 9, 1, 100, 50) == 0, "type 5 文本图元不可命中");

    /* ---- 12. 链没走到根（悬空的子树）→ 未命中 ---- */
    sprite(g, 20, 1, -1, 0, 0);
    sprite(g, 21, 1, 20, 0, 0); /* 21 的 parent = 20 */
    CHECK(gfx_prim_group_out(g, 20) == 0, "detach 20（21 仍指向它）");
    CHECK(gfx_prim_hit(g, 21, 1, 10, 20) == 0,
          "链终止在悬空节点（没经过 prim 0）→ 未命中");

    /* ---- 13. 环状父子关系不会挂死 ---- */
    CHECK(gfx_prim_group_in(g, 30, 31) == 0, "30 -> 31");
    CHECK(gfx_prim_group_in(g, 31, 30) == 0, "31 -> 30（成环）");
    CHECK(gfx_prim_hit(g, 30, 1, 0, 0) == 0, "环状链安全返回 0");

    gfx_destroy(g);
    printf("test_prim_hit: %d passed, %d failed\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}