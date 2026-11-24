/* 跨图集字形兜底（glyph fallback）单元测试。
 *
 * 背景：图集是"只收了实际用到的字"的位图集 —— 游戏里 56px 的章节标题图集只有
 * 5 个字形（`・上学屋校`），于是标题里的「空」被画成豆腐块（用户实测报"窗口中间
 * 的黑方块"）。修法是：目标图集缺字时，去其它已注册图集里找同一个字，按字号
 * 比例缩放画上去，只有全都找不到才画豆腐块。
 *
 * 这里用两个临时 GFXF 文件造图集，不依赖游戏数据：
 *   34px 图集只有 'B'、16px 图集只有 'A'；文本层 8 的排版字号正好是 34，
 *   所以 "A" 必须走兜底、"B" 直接命中、"あ" 两边都没有（豆腐块）。
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

static void put32(unsigned char *p, unsigned v)
{
    p[0] = (unsigned char)(v & 0xFFu);
    p[1] = (unsigned char)((v >> 8) & 0xFFu);
    p[2] = (unsigned char)((v >> 16) & 0xFFu);
    p[3] = (unsigned char)((v >> 24) & 0xFFu);
}

/* 写一个最小 GFXF v2 图集：字号 size，字形表里每个码点一个"实心方块"字形。 */
static int write_atlas(const char *path, int size, const unsigned *cps,
                       int count)
{
    FILE *fp = fopen(path, "wb");
    unsigned char hdr[28];
    int margin = 1;
    int ascent = size;
    int descent = 1;
    int cell_h = ascent + descent + 2 * margin;
    int i;

    if (fp == NULL) {
        return -1;
    }
    memset(hdr, 0, sizeof hdr);
    memcpy(hdr, "GFXF", 4);
    put32(hdr + 4, 2);                 /* version */
    put32(hdr + 8, (unsigned)size);    /* size  */
    put32(hdr + 12, (unsigned)margin); /* margin */
    put32(hdr + 16, (unsigned)ascent);
    put32(hdr + 20, (unsigned)descent);
    put32(hdr + 24, (unsigned)count);
    fwrite(hdr, 1, sizeof hdr, fp);
    for (i = 0; i < count; ++i) {
        unsigned char ent[12];

        memset(ent, 0, sizeof ent);
        put32(ent + 0, cps[i]);          /* codepoint */
        put32(ent + 4, (unsigned)size);  /* advance   */
        put32(ent + 8, (unsigned)size);  /* glyph width */
        fwrite(ent, 1, sizeof ent, fp);
    }
    /* 每个字形 3 层（字形 + 描边 1 + 描边 2），每层 gw*cell_h 字节 */
    for (i = 0; i < count; ++i) {
        size_t bytes = (size_t)size * (size_t)cell_h;
        unsigned char *buf = (unsigned char *)malloc(bytes);
        int layer;

        if (buf == NULL) {
            fclose(fp);
            return -1;
        }
        memset(buf, 0x80, bytes);
        for (layer = 0; layer < 3; ++layer) {
            fwrite(buf, 1, bytes, fp);
        }
        free(buf);
    }
    fclose(fp);
    return 0;
}

/* 临时图集放到 %TEMP%（回落到当前目录），这样从任何工作目录跑都写得出文件 ——
 * run_all.py 是从仓库根调这个程序的，以前写死 out/fuzz/ 就只有 fvp/ 下能过。 */
static char g_tmp34[512];
static char g_tmp16[512];

static void make_tmp_paths(void)
{
    const char *t = getenv("TEMP");

    if (t == NULL || t[0] == '\0') {
        t = getenv("TMP");
    }
    if (t == NULL || t[0] == '\0') {
        t = ".";
    }
    snprintf(g_tmp34, sizeof g_tmp34, "%s/fvp_atlas34.bin", t);
    snprintf(g_tmp16, sizeof g_tmp16, "%s/fvp_atlas16.bin", t);
}

int main(void)
{
    const char *p34;
    const char *p16;
    unsigned cps34[1];
    unsigned cps16[1];
    GfxFont *f34;
    GfxFont *f16;
    Gfx *g;
    const GfxStats *st;

    make_tmp_paths();
    p34 = g_tmp34;
    p16 = g_tmp16;
    cps34[0] = 'B';
    cps16[0] = 'A';
    CHECK(write_atlas(p34, 34, cps34, 1) == 0, "write 34px atlas");
    CHECK(write_atlas(p16, 16, cps16, 1) == 0, "write 16px atlas");
    f34 = gfx_font_load(p34);
    f16 = gfx_font_load(p16);
    CHECK(f34 != NULL && f16 != NULL, "load atlases");
    if (f34 == NULL || f16 == NULL) {
        return 1;
    }
    CHECK(gfx_font_pixel_size(f34) == 34, "34px size (%d)",
          gfx_font_pixel_size(f34));
    CHECK(gfx_font_glyph_count(f34) == 1, "34px has 1 glyph (%d)",
          gfx_font_glyph_count(f34));

    g = gfx_create(320, 180, NULL);
    CHECK(g != NULL, "gfx_create");
    if (g == NULL) {
        return 1;
    }
    CHECK(gfx_add_font(g, f34) == 0, "add 34px font");
    CHECK(gfx_add_font(g, f16) == 0, "add 16px font");

    /* --- 1. 目标图集里有的字：直接命中，不兜底、不豆腐块 --- */
    gfx_text_append(g, 8, "B"); /* 层 8 的排版字号 = 34 */
    gfx_render(g);
    st = gfx_stats(g);
    CHECK(st != NULL, "stats");
    if (st == NULL) {
        return 1;
    }
    CHECK(st->fallback_glyphs == 0, "B hit the 34px atlas (fallback=%d)",
          st->fallback_glyphs);
    CHECK(st->tofu_glyphs == 0, "B is not tofu (tofu=%d)", st->tofu_glyphs);

    /* --- 2. 目标图集没有、16px 图集有：缩放兜底，不画豆腐块 --- */
    gfx_text_clear(g, 8);
    gfx_text_append(g, 8, "A");
    gfx_render(g);
    st = gfx_stats(g);
    CHECK(st->fallback_glyphs == 1, "A came from the 16px atlas (fallback=%d)",
          st->fallback_glyphs);
    CHECK(st->tofu_glyphs == 0, "A is not tofu (tofu=%d)", st->tofu_glyphs);

    /* --- 3. 所有图集都没有：才画豆腐块 --- */
    gfx_text_clear(g, 8);
    gfx_text_append(g, 8, "\xE3\x81\x82"); /* あ */
    gfx_render(g);
    st = gfx_stats(g);
    CHECK(st->tofu_glyphs == 1, "missing glyph is tofu (tofu=%d)",
          st->tofu_glyphs);
    CHECK(st->fallback_glyphs == 0, "no fallback for a missing glyph (%d)",
          st->fallback_glyphs);

    /* --- 4. 计数每次 render 归零 --- */
    gfx_text_clear(g, 8);
    gfx_render(g);
    st = gfx_stats(g);
    CHECK(st->tofu_glyphs == 0 && st->fallback_glyphs == 0,
          "counters reset per render (%d/%d)", st->tofu_glyphs,
          st->fallback_glyphs);

    gfx_destroy(g);
    gfx_font_free(f34);
    gfx_font_free(f16);
    remove(p34);
    remove(p16);

    printf("test_glyph_fallback: %d passed, %d failed\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}