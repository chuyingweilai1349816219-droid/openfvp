/* 纹理缓存 LRU 上限测试。
 *
 * 背景：长跑采样发现 RSS 是阶梯式增长（194 -> 296 MiB / 8 min），每一跳都对应
 * 纹理缓存的阶跃 —— 缓存只进不出。这里用一个假 host 造固定大小的图，验证：
 *
 *   1. cap = 0 时不淘汰（保持旧行为，可当开关）；
 *   2. 设了 cap 后 tex_bytes 不再超过 cap，且确实发生淘汰；
 *   3. **被引用的图（refs > 0）永远不会被淘汰**（否则画面会炸）；
 *   4. 引用释放后图像仍留在缓存里，需要时直接命中（不再重新解码）；
 *   5. 淘汰之后重新取同一张图仍然可用（真的重新 load_image）；
 *   6. gfx_tex_set_cap_bytes() 立即触发一次淘汰；
 *   7. NULL 安全。
 *
 * 语义要点：refs 只数**外部持有者**（图元 / 组槽 / 部件）。refs == 0 表示
 * "没人用了，可以淘汰"，但图像仍留在哈希表里供复用。
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../src/gfx.h"

static int g_fail = 0;
static int g_pass = 0;
static int g_loads = 0;

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

/* 每张图固定 100x100 RGBA = 40,000 字节。 */
#define IMG_W 100
#define IMG_H 100
#define IMG_BYTES ((size_t)IMG_W * IMG_H * 4)
#define SLOTS 80

static int fake_load(void *ud, const char *name, GfxImage *out)
{
    (void)ud;
    (void)name;
    g_loads += 1;
    out->w = IMG_W;
    out->h = IMG_H;
    out->x = 0;
    out->y = 0;
    out->frames = 1;
    out->rgba = (uint8_t *)malloc(IMG_BYTES);
    if (out->rgba == NULL) {
        return 1;
    }
    memset(out->rgba, 0x7F, IMG_BYTES);
    return 0;
}

static void fake_free(void *ud, GfxImage *img)
{
    (void)ud;
    free(img->rgba);
    img->rgba = NULL;
}

static char namebuf[32];

static const char *img_name(int i)
{
    sprintf(namebuf, "graph/T%05d", i);
    return namebuf;
}

static void load_slot(Gfx *g, int i, int slot)
{
    if (gfx_graph_load(g, slot, img_name(i)) != 0) {
        printf("load failed: %s\n", img_name(i));
    }
}

static void release_all(Gfx *g)
{
    int s;

    for (s = 1; s <= SLOTS; ++s) {
        gfx_graph_release(g, s);
    }
}

int main(void)
{
    GfxHost host;
    Gfx *g;
    int i;

    memset(&host, 0, sizeof host);
    host.load_image = fake_load;
    host.free_image = fake_free;

    g = gfx_create(64, 36, &host);
    CHECK(g != NULL, "gfx_create");
    if (g == NULL) {
        return 1;
    }

    /* --- 1. 默认上限是有限值；cap = 0 关闭淘汰 --- */
    CHECK(gfx_tex_cap_bytes(g) > 0, "default cap is finite (%lu bytes)",
          (unsigned long)gfx_tex_cap_bytes(g));
    gfx_tex_set_cap_bytes(g, 0);
    CHECK(gfx_tex_cap_bytes(g) == 0, "cap 0 = unlimited");

    /* --- 2. 无上限时全部缓存住：40 张 = 1.6 MB --- */
    for (i = 0; i < 40; ++i) {
        load_slot(g, i, i + 1);
    }
    CHECK(gfx_tex_count(g) == 40, "cached all 40 images (got %d)",
          gfx_tex_count(g));
    CHECK(gfx_tex_bytes(g) == 40u * IMG_BYTES,
          "cached bytes = 40 * %lu (got %lu)", (unsigned long)IMG_BYTES,
          (unsigned long)gfx_tex_bytes(g));
    CHECK(gfx_tex_evictions(g) == 0, "no eviction without a cap");

    /* 引用全释放后图像仍留在缓存里（这是缓存，不是立即释放） */
    release_all(g);
    CHECK(gfx_tex_count(g) == 40,
          "released refs keep the images cached (got %d)", gfx_tex_count(g));

    /* --- 3. 设上限 -> 立即淘汰到上限之内 --- */
    gfx_tex_set_cap_bytes(g, 10u * IMG_BYTES);
    CHECK(gfx_tex_bytes(g) <= 10u * IMG_BYTES,
          "bytes dropped under the new cap (now %lu)",
          (unsigned long)gfx_tex_bytes(g));
    CHECK(gfx_tex_count(g) == 10, "cache trimmed to the cap (got %d)",
          gfx_tex_count(g));
    CHECK(gfx_tex_evictions(g) == 30, "evicted exactly the excess (%lu)",
          gfx_tex_evictions(g));

    /* --- 4. 持续加载：占用不超过 cap，且淘汰持续发生 --- */
    {
        unsigned long before = gfx_tex_evictions(g);
        size_t peak = 0;

        for (i = 0; i < 200; ++i) {
            load_slot(g, i, (i % 8) + 1); /* 8 个槽轮换，逼出淘汰路径 */
            if (gfx_tex_bytes(g) > peak) {
                peak = gfx_tex_bytes(g);
            }
        }
        CHECK(peak <= 10u * IMG_BYTES,
              "peak stayed under the cap (%lu <= %lu)", (unsigned long)peak,
              (unsigned long)(10u * IMG_BYTES));
        CHECK(gfx_tex_evictions(g) > before, "eviction kept happening (%lu)",
              gfx_tex_evictions(g));
        CHECK(gfx_tex_count(g) > 0, "cache still holds images (%d)",
              gfx_tex_count(g));
    }
    release_all(g);

    /* --- 5. 被引用的图不能被淘汰，释放后才可以 --- */
    {
        release_all(g);
        gfx_tex_set_cap_bytes(g, 1); /* 先把上一节残留的缓存清空 */
        CHECK(gfx_tex_count(g) == 0, "cache flushed before the sharing test "
              "(got %d)", gfx_tex_count(g));
        gfx_tex_set_cap_bytes(g, 0);
        for (i = 0; i < 8; ++i) {
            load_slot(g, 7, i + 1); /* 8 个槽共用同一张图 */
        }
        CHECK(gfx_tex_count(g) == 1, "one texture shared by 8 slots (got %d)",
              gfx_tex_count(g));
        gfx_tex_set_cap_bytes(g, 1); /* 远小于一张图 */
        CHECK(gfx_tex_count(g) == 1,
              "referenced texture survives a 1-byte cap (got %d)",
              gfx_tex_count(g));
        CHECK(gfx_tex_bytes(g) == IMG_BYTES,
              "its bytes are still counted (%lu)",
              (unsigned long)gfx_tex_bytes(g));
        release_all(g);
        /* 引用没了，但缓存仍保留；再来一次 set_cap 就会淘汰 */
        gfx_tex_set_cap_bytes(g, 1);
        CHECK(gfx_tex_count(g) == 0,
              "unreferenced texture evicted after refs dropped (got %d)",
              gfx_tex_count(g));
    }

    /* --- 6. 淘汰后重新获取同一张图仍然可用（真的重新解码） --- */
    {
        int loads_before;
        unsigned long ev_before;

        gfx_tex_set_cap_bytes(g, 2u * IMG_BYTES);
        for (i = 0; i < 6; ++i) {
            load_slot(g, 100 + i, i + 1);
        }
        loads_before = g_loads;
        ev_before = gfx_tex_evictions(g);
        CHECK(ev_before >= 4, "earlier loads were evicted (%lu)", ev_before);

        /* 第 100 号图在缓存里；重新加载应命中缓存（不增加 host 调用） */
        load_slot(g, 100, 1);
        CHECK(g_loads == loads_before, "cached image reused without decoding");
        release_all(g);

        /* 被淘汰的 0 号图需要重新解码 */
        load_slot(g, 0, 1);
        CHECK(g_loads == loads_before + 1,
              "evicted image is decoded again (%d -> %d)", loads_before,
              g_loads);
        release_all(g);
    }

    /* --- 7. NULL 安全 --- */
    gfx_tex_set_cap_bytes(NULL, 1);
    CHECK(gfx_tex_cap_bytes(NULL) == 0, "cap(NULL) = 0");
    CHECK(gfx_tex_bytes(NULL) == 0, "bytes(NULL) = 0");
    CHECK(gfx_tex_count(NULL) == 0, "count(NULL) = 0");
    CHECK(gfx_tex_evictions(NULL) == 0, "evictions(NULL) = 0");

    CHECK(g_loads > 40, "images were really loaded through the host (%d)",
          g_loads);

    gfx_destroy(g);

    printf("\n%d passed, %d failed\n", g_pass, g_fail);
    if (g_fail == 0) {
        printf("test_tex_cache: PASS\n");
        return 0;
    }
    printf("test_tex_cache: FAIL\n");
    return 1;
}