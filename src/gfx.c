/* FVP engine -- gfx: primitive table, layer compositing, text layout.
 *
 * Port of the proven Python prototype fvp/work/render.py; the rules are
 * documented in fvp/docs/render-model.md and the port notes in
 * fvp/docs/c-gfx.md.  This file is self contained apart from:
 *   - src/bin.c  + src/hzc1.c + src/inflate.c   (only for the optional
 *     gfx_archive_host_* convenience host)
 *   - libm (floor/ceil/pow/sqrt for resampling and rotation)
 *
 * Pipeline per frame (gfx_render):
 *   1. fill the canvas with the background colour (default opaque black)
 *   2. depth-first walk of the group tree rooted at prim 0
 *   3. append sprite prims that are not in any group, in ascending id order
 *   4. draw the text windows (dialogue box, name plate, chapter title)
 */
#include "gfx.h"

#include "motion.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "bin.h"
#include "hzc1.h"

#if defined(_WIN32)
/* src/bin.c opens packages with fopen(), which on Windows only accepts the
 * active ANSI code page.  The archive host therefore converts the UTF-8 game
 * directory (as written by the tooling) to ANSI before opening. */
#include <windows.h>
#endif

#define GFX_TEX_BUCKETS 1024
#define GFX_MAX_FONTS 16
#define GFX_TEXT_LINES 16
#define GFX_MAX_CHILDREN 256
#define GFX_COEFF_MAX 64
#define GFX_MAX_IMAGE_BYTES (512u * 1024u * 1024u)

/* 纹理缓存默认上限。1920x1080 的 RGBA 整屏图约 8.3 MB，本作跑 2 分钟就会缓存
 * 80+ 张（~110 MiB）。不设上限时内存随场景线性增长（实测 8 分钟 194→296 MiB），
 * 所以默认给 256 MiB 的上限并按 LRU 淘汰：被图元引用的不会被淘汰，只回收没有
 * 引用的缓存条目，需要时重新解码。 */
#define GFX_TEX_CACHE_DEFAULT_BYTES (256u * 1024u * 1024u)

/* ----------------------------------------------------------------------- */
/* small helpers                                                           */
/* ----------------------------------------------------------------------- */

static int clampi(int v, int lo, int hi)
{
    return v < lo ? lo : (v > hi ? hi : v);
}

static uint32_t rd32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) |
           ((uint32_t)p[3] << 24);
}

/* Pillow's MULDIV255 macro (libImaging, used by "RGBa" conversion). */
static int mouldiv255(int a, int b)
{
    int t = a * b + 128;
    return ((t >> 8) + t) >> 8;
}

/* Python's round(): banker's rounding (half to even) on the double value. */
static int py_round(double v)
{
    double fl = floor(v);
    double diff = v - fl;
    int fi = (int)fl;

    if (diff > 0.5) {
        return fi + 1;
    }
    if (diff < 0.5) {
        return fi;
    }
    return (fi & 1) ? fi + 1 : fi;
}

/* Python's // for an int and a positive divisor. */
static int py_floordiv(int a, int b)
{
    if (a >= 0) {
        return a / b;
    }
    return -(((-a) + b - 1) / b);
}

/* Python's a % b for b > 0. */
static int py_mod(int a, int b)
{
    int r = a % b;
    return r < 0 ? r + b : r;
}

static int utf8_next(const char **pp)
{
    const unsigned char *s = (const unsigned char *)*pp;
    int cp;
    int n;

    if (*s == 0) {
        return -1;
    }
    if (s[0] < 0x80) {
        cp = s[0];
        n = 1;
    } else if ((s[0] & 0xE0) == 0xC0 && s[1] != 0) {
        cp = ((s[0] & 0x1F) << 6) | (s[1] & 0x3F);
        n = 2;
    } else if ((s[0] & 0xF0) == 0xE0 && s[1] != 0 && s[2] != 0) {
        cp = ((s[0] & 0x0F) << 12) | ((s[1] & 0x3F) << 6) | (s[2] & 0x3F);
        n = 3;
    } else if ((s[0] & 0xF8) == 0xF0 && s[1] != 0 && s[2] != 0 && s[3] != 0) {
        cp = ((s[0] & 0x07) << 18) | ((s[1] & 0x3F) << 12) |
             ((s[2] & 0x3F) << 6) | (s[3] & 0x3F);
        n = 4;
    } else {
        cp = 0xFFFD;
        n = 1;
    }
    *pp += n;
    return cp;
}

/* ----------------------------------------------------------------------- */
/* built-in 8x16 bitmap font (generated from msgothic, see docs/c-gfx.md)  */
/* ----------------------------------------------------------------------- */

/* 95 printable ASCII glyphs, 8x16, 1 bit per pixel, row-major, MSB = leftmost */
static const unsigned char g_font8x16[95][16] = {
    {0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00}, {0x00,0x10,0x10,0x10,0x10,0x10,0x10,0x10,0x10,0x10,0x00,0x00,0x18,0x18,0x00,0x00}, {0x36,0x24,0x48,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00}, {0x00,0x24,0x24,0x24,0x24,0xFE,0x48,0x48,0x48,0x48,0xFC,0x48,0x48,0x48,0x48,0x00},
    {0x10,0x38,0x54,0x92,0x92,0x50,0x30,0x18,0x14,0x12,0x92,0x92,0x54,0x38,0x10,0x00}, {0x00,0x62,0x92,0x94,0x94,0x68,0x08,0x10,0x20,0x2C,0x52,0x52,0x92,0x8C,0x00,0x00}, {0x00,0x30,0x48,0x48,0x48,0x48,0x30,0x20,0x54,0x94,0x88,0x88,0x94,0x62,0x00,0x00}, {0x30,0x30,0x10,0x20,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00},
    {0x04,0x08,0x10,0x10,0x20,0x20,0x20,0x20,0x20,0x20,0x20,0x10,0x10,0x08,0x04,0x00}, {0x40,0x20,0x10,0x10,0x08,0x08,0x08,0x08,0x08,0x08,0x08,0x10,0x10,0x20,0x40,0x00}, {0x00,0x00,0x00,0x10,0x92,0x54,0x38,0x10,0x38,0x54,0x92,0x10,0x00,0x00,0x00,0x00}, {0x00,0x00,0x00,0x00,0x10,0x10,0x10,0xFE,0x10,0x10,0x10,0x00,0x00,0x00,0x00,0x00},
    {0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x30,0x30,0x10,0x20,0x00}, {0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x7C,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00}, {0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x30,0x30,0x00,0x00}, {0x00,0x02,0x02,0x04,0x04,0x08,0x08,0x10,0x20,0x20,0x40,0x40,0x80,0x80,0x00,0x00},
    {0x00,0x30,0x48,0x84,0x84,0x84,0x84,0x84,0x84,0x84,0x84,0x84,0x48,0x30,0x00,0x00}, {0x00,0x10,0x70,0x10,0x10,0x10,0x10,0x10,0x10,0x10,0x10,0x10,0x10,0x10,0x00,0x00}, {0x00,0x30,0x48,0x84,0x84,0x04,0x08,0x08,0x10,0x20,0x20,0x40,0x80,0xFC,0x00,0x00}, {0x00,0x30,0x48,0x84,0x84,0x04,0x08,0x30,0x08,0x04,0x84,0x84,0x48,0x30,0x00,0x00},
    {0x00,0x08,0x08,0x18,0x18,0x28,0x28,0x48,0x48,0x88,0xFC,0x08,0x08,0x08,0x00,0x00}, {0x00,0xFC,0x80,0x80,0x80,0xB0,0xC8,0x84,0x04,0x04,0x04,0x84,0x48,0x30,0x00,0x00}, {0x00,0x30,0x48,0x84,0x84,0x80,0xB0,0xC8,0x84,0x84,0x84,0x84,0x48,0x30,0x00,0x00}, {0x00,0xFC,0x04,0x04,0x08,0x08,0x08,0x10,0x10,0x10,0x20,0x20,0x20,0x20,0x00,0x00},
    {0x00,0x30,0x48,0x84,0x84,0x84,0x48,0x30,0x48,0x84,0x84,0x84,0x48,0x30,0x00,0x00}, {0x00,0x30,0x48,0x84,0x84,0x84,0x84,0x4C,0x34,0x04,0x84,0x84,0x48,0x30,0x00,0x00}, {0x00,0x00,0x00,0x00,0x00,0x30,0x30,0x00,0x00,0x00,0x00,0x30,0x30,0x00,0x00,0x00}, {0x00,0x00,0x00,0x00,0x00,0x30,0x30,0x00,0x00,0x00,0x00,0x30,0x30,0x10,0x20,0x00},
    {0x00,0x00,0x04,0x08,0x10,0x20,0x40,0x80,0x40,0x20,0x10,0x08,0x04,0x00,0x00,0x00}, {0x00,0x00,0x00,0x00,0x00,0x7C,0x00,0x00,0x00,0x7C,0x00,0x00,0x00,0x00,0x00,0x00}, {0x00,0x00,0x80,0x40,0x20,0x10,0x08,0x04,0x08,0x10,0x20,0x40,0x80,0x00,0x00,0x00}, {0x00,0x30,0x48,0x84,0x84,0x04,0x08,0x10,0x20,0x20,0x00,0x00,0x30,0x30,0x00,0x00},
    {0x00,0x38,0x44,0x82,0x9A,0xAA,0xAA,0xAA,0xAA,0xAA,0x9C,0x80,0x42,0x3C,0x00,0x00}, {0x00,0x10,0x10,0x28,0x28,0x28,0x28,0x44,0x44,0x44,0x7C,0x82,0x82,0x82,0x00,0x00}, {0x00,0xF8,0x84,0x82,0x82,0x82,0x84,0xF8,0x84,0x82,0x82,0x82,0x84,0xF8,0x00,0x00}, {0x00,0x38,0x44,0x82,0x82,0x80,0x80,0x80,0x80,0x80,0x82,0x82,0x44,0x38,0x00,0x00},
    {0x00,0xF8,0x84,0x82,0x82,0x82,0x82,0x82,0x82,0x82,0x82,0x82,0x84,0xF8,0x00,0x00}, {0x00,0xFE,0x80,0x80,0x80,0x80,0x80,0xFC,0x80,0x80,0x80,0x80,0x80,0xFE,0x00,0x00}, {0x00,0xFE,0x80,0x80,0x80,0x80,0x80,0xFC,0x80,0x80,0x80,0x80,0x80,0x80,0x00,0x00}, {0x00,0x38,0x44,0x82,0x82,0x80,0x80,0x80,0x8E,0x82,0x82,0x82,0x46,0x3A,0x00,0x00},
    {0x00,0x82,0x82,0x82,0x82,0x82,0x82,0xFE,0x82,0x82,0x82,0x82,0x82,0x82,0x00,0x00}, {0x00,0x38,0x10,0x10,0x10,0x10,0x10,0x10,0x10,0x10,0x10,0x10,0x10,0x38,0x00,0x00}, {0x00,0x04,0x04,0x04,0x04,0x04,0x04,0x04,0x04,0x04,0x84,0x84,0x48,0x30,0x00,0x00}, {0x00,0x82,0x84,0x84,0x88,0x90,0x90,0xA0,0xD0,0x88,0x88,0x84,0x82,0x82,0x00,0x00},
    {0x00,0x80,0x80,0x80,0x80,0x80,0x80,0x80,0x80,0x80,0x80,0x80,0x80,0xFE,0x00,0x00}, {0x00,0x82,0x82,0xC6,0xC6,0xC6,0xC6,0xAA,0xAA,0xAA,0xAA,0x92,0x92,0x92,0x00,0x00}, {0x00,0x82,0x82,0xC2,0xC2,0xA2,0xA2,0x92,0x92,0x8A,0x8A,0x86,0x86,0x82,0x00,0x00}, {0x00,0x38,0x44,0x82,0x82,0x82,0x82,0x82,0x82,0x82,0x82,0x82,0x44,0x38,0x00,0x00},
    {0x00,0xF8,0x84,0x82,0x82,0x82,0x84,0xF8,0x80,0x80,0x80,0x80,0x80,0x80,0x00,0x00}, {0x00,0x38,0x44,0x82,0x82,0x82,0x82,0x82,0x82,0x82,0x92,0x8A,0x44,0x3A,0x00,0x00}, {0x00,0xF8,0x84,0x82,0x82,0x82,0x84,0xF8,0x88,0x88,0x84,0x84,0x82,0x82,0x00,0x00}, {0x00,0x38,0x44,0x82,0x82,0x80,0x60,0x18,0x04,0x02,0x82,0x82,0x44,0x38,0x00,0x00},
    {0x00,0xFE,0x10,0x10,0x10,0x10,0x10,0x10,0x10,0x10,0x10,0x10,0x10,0x10,0x00,0x00}, {0x00,0x82,0x82,0x82,0x82,0x82,0x82,0x82,0x82,0x82,0x82,0x82,0x44,0x38,0x00,0x00}, {0x00,0x82,0x82,0x82,0x44,0x44,0x44,0x44,0x28,0x28,0x28,0x10,0x10,0x10,0x00,0x00}, {0x00,0x92,0x92,0x92,0x92,0xAA,0xAA,0xAA,0xAA,0x44,0x44,0x44,0x44,0x44,0x00,0x00},
    {0x00,0x82,0x82,0x44,0x44,0x28,0x28,0x10,0x28,0x28,0x44,0x44,0x82,0x82,0x00,0x00}, {0x00,0x82,0x82,0x44,0x44,0x28,0x28,0x10,0x10,0x10,0x10,0x10,0x10,0x10,0x00,0x00}, {0x00,0xFE,0x02,0x04,0x04,0x08,0x08,0x10,0x20,0x20,0x40,0x40,0x80,0xFE,0x00,0x00}, {0x7C,0x40,0x40,0x40,0x40,0x40,0x40,0x40,0x40,0x40,0x40,0x40,0x40,0x40,0x7C,0x00},
    {0x00,0x82,0x82,0x44,0x44,0x28,0x28,0x7C,0x10,0x10,0x7C,0x10,0x10,0x10,0x00,0x00}, {0x7C,0x04,0x04,0x04,0x04,0x04,0x04,0x04,0x04,0x04,0x04,0x04,0x04,0x04,0x7C,0x00}, {0x10,0x28,0x44,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00}, {0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0xFF},
    {0x20,0x10,0x08,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00}, {0x00,0x00,0x00,0x00,0x00,0x00,0x78,0x84,0x04,0x3C,0x44,0x84,0x8C,0x76,0x00,0x00}, {0x00,0x80,0x80,0x80,0x80,0x80,0xB8,0xC4,0x82,0x82,0x82,0x82,0xC4,0xB8,0x00,0x00}, {0x00,0x00,0x00,0x00,0x00,0x00,0x3C,0x42,0x80,0x80,0x80,0x80,0x42,0x3C,0x00,0x00},
    {0x00,0x02,0x02,0x02,0x02,0x02,0x3A,0x46,0x82,0x82,0x82,0x82,0x46,0x3A,0x00,0x00}, {0x00,0x00,0x00,0x00,0x00,0x00,0x38,0x44,0x82,0xFE,0x80,0x80,0x42,0x3C,0x00,0x00}, {0x00,0x18,0x20,0x20,0x20,0x20,0xF8,0x20,0x20,0x20,0x20,0x20,0x20,0x20,0x00,0x00}, {0x00,0x00,0x00,0x00,0x00,0x00,0x3A,0x44,0x44,0x38,0x40,0x7C,0x82,0x82,0x7C,0x00},
    {0x00,0x80,0x80,0x80,0x80,0x80,0xB8,0xC4,0x82,0x82,0x82,0x82,0x82,0x82,0x00,0x00}, {0x00,0x00,0x10,0x10,0x00,0x00,0x10,0x10,0x10,0x10,0x10,0x10,0x10,0x10,0x00,0x00}, {0x00,0x00,0x10,0x10,0x00,0x00,0x10,0x10,0x10,0x10,0x10,0x10,0x10,0x10,0x60,0x00}, {0x00,0x80,0x80,0x80,0x80,0x80,0x84,0x88,0x90,0xA0,0xD0,0x88,0x84,0x82,0x00,0x00},
    {0x00,0x10,0x10,0x10,0x10,0x10,0x10,0x10,0x10,0x10,0x10,0x10,0x10,0x10,0x00,0x00}, {0x00,0x00,0x00,0x00,0x00,0x00,0xAC,0xD2,0x92,0x92,0x92,0x92,0x92,0x92,0x00,0x00}, {0x00,0x00,0x00,0x00,0x00,0x00,0xB8,0xC4,0x82,0x82,0x82,0x82,0x82,0x82,0x00,0x00}, {0x00,0x00,0x00,0x00,0x00,0x00,0x38,0x44,0x82,0x82,0x82,0x82,0x44,0x38,0x00,0x00},
    {0x00,0x00,0x00,0x00,0x00,0x00,0xB8,0xC4,0x82,0x82,0x82,0xC4,0xB8,0x80,0x80,0x00}, {0x00,0x00,0x00,0x00,0x00,0x00,0x3A,0x46,0x82,0x82,0x82,0x46,0x3A,0x02,0x02,0x00}, {0x00,0x00,0x00,0x00,0x00,0x00,0x2E,0x30,0x20,0x20,0x20,0x20,0x20,0x20,0x00,0x00}, {0x00,0x00,0x00,0x00,0x00,0x00,0x7C,0x82,0x80,0x60,0x1C,0x02,0x82,0x7C,0x00,0x00},
    {0x00,0x00,0x20,0x20,0x20,0x20,0xF8,0x20,0x20,0x20,0x20,0x20,0x20,0x18,0x00,0x00}, {0x00,0x00,0x00,0x00,0x00,0x00,0x82,0x82,0x82,0x82,0x82,0x82,0x46,0x3A,0x00,0x00}, {0x00,0x00,0x00,0x00,0x00,0x00,0x82,0x82,0x44,0x44,0x28,0x28,0x10,0x10,0x00,0x00}, {0x00,0x00,0x00,0x00,0x00,0x00,0x92,0x92,0x92,0xAA,0xAA,0x44,0x44,0x44,0x00,0x00},
    {0x00,0x00,0x00,0x00,0x00,0x00,0x82,0x44,0x28,0x10,0x10,0x28,0x44,0x82,0x00,0x00}, {0x00,0x00,0x00,0x00,0x00,0x00,0x82,0x82,0x44,0x44,0x28,0x28,0x10,0x20,0xC0,0x00}, {0x00,0x00,0x00,0x00,0x00,0x00,0xFE,0x04,0x08,0x10,0x20,0x40,0x80,0xFE,0x00,0x00}, {0x1C,0x10,0x10,0x10,0x10,0x10,0x10,0x20,0x10,0x10,0x10,0x10,0x10,0x10,0x1C,0x00},
    {0x10,0x10,0x10,0x10,0x10,0x10,0x10,0x10,0x10,0x10,0x10,0x10,0x10,0x10,0x10,0x10}, {0x70,0x10,0x10,0x10,0x10,0x10,0x10,0x08,0x10,0x10,0x10,0x10,0x10,0x10,0x70,0x00}, {0x64,0x98,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00},
};

/* ----------------------------------------------------------------------- */
/* fonts                                                                   */
/* ----------------------------------------------------------------------- */

struct GfxFont {
    int builtin;
    int size;    /* pixel em size (atlas) or 16 (built-in)              */
    int ascent;
    int descent;
    int margin;
    int cell_h;  /* ascent + descent + 2*margin (atlas cell height)     */
    int layers;  /* 1 = glyph only, 3 = glyph + stroke width 1 and 2    */
    int count;
    uint32_t *cp;   /* codepoints, ascending                              */
    int *adv;       /* advance in pixels                                  */
    int *gw;        /* glyph cell width                                   */
    const uint8_t *px; /* atlas pixel blocks (count of them)              */
    uint8_t *data;  /* owned file image (NULL for built-in)               */
};

static GfxFont g_builtin_font;
static int g_builtin_ready;

static void font_builtin_init(void)
{
    int i;

    if (g_builtin_ready) {
        return;
    }
    g_builtin_font.builtin = 1;
    g_builtin_font.size = 16;
    g_builtin_font.ascent = 16;
    g_builtin_font.descent = 0;
    g_builtin_font.margin = 0;
    g_builtin_font.cell_h = 16;
    g_builtin_font.count = 95;
    g_builtin_font.cp = (uint32_t *)malloc(sizeof(uint32_t) * 95);
    g_builtin_font.adv = (int *)malloc(sizeof(int) * 95);
    g_builtin_font.gw = (int *)malloc(sizeof(int) * 95);
    if (g_builtin_font.cp == NULL || g_builtin_font.adv == NULL ||
        g_builtin_font.gw == NULL) {
        return;
    }
    for (i = 0; i < 95; i++) {
        g_builtin_font.cp[i] = (uint32_t)(32 + i);
        g_builtin_font.adv[i] = 8;
        g_builtin_font.gw[i] = 8;
    }
    g_builtin_font.px = &g_font8x16[0][0];
    g_builtin_ready = 1;
}

static GfxFont *font_builtin(void)
{
    font_builtin_init();
    return &g_builtin_font;
}

void gfx_font_free(GfxFont *f)
{
    if (f == NULL || f == &g_builtin_font) {
        return;
    }
    free(f->cp);
    free(f->adv);
    free(f->gw);
    free(f->data);
    free(f);
}

GfxFont *gfx_font_load(const char *path)
{
    FILE *fp;
    long fsize;
    uint8_t *buf = NULL;
    uint32_t version;
    uint32_t size;
    uint32_t margin;
    uint32_t ascent;
    uint32_t descent;
    uint32_t count;
    uint32_t layers;
    uint32_t i;
    size_t off;
    GfxFont *f;
    int cell_h;

    if (path == NULL) {
        return NULL;
    }
    fp = fopen(path, "rb");
    if (fp == NULL) {
        return NULL;
    }
    if (fseek(fp, 0, SEEK_END) != 0) {
        fclose(fp);
        return NULL;
    }
    fsize = ftell(fp);
    if (fsize < 28) {
        fclose(fp);
        return NULL;
    }
    rewind(fp);
    buf = (uint8_t *)malloc((size_t)fsize);
    if (buf == NULL) {
        fclose(fp);
        return NULL;
    }
    if (fread(buf, 1, (size_t)fsize, fp) != (size_t)fsize) {
        free(buf);
        fclose(fp);
        return NULL;
    }
    fclose(fp);

    if (memcmp(buf, "GFXF", 4) != 0) {
        free(buf);
        return NULL;
    }
    version = rd32(buf + 4);
    size = rd32(buf + 8);
    margin = rd32(buf + 12);
    ascent = rd32(buf + 16);
    descent = rd32(buf + 20);
    count = rd32(buf + 24);
    if ((version != 1 && version != 2) || size == 0 || size > 512 ||
        count > 65535) {
        free(buf);
        return NULL;
    }
    cell_h = (int)(ascent + descent + 2 * margin);
    if (cell_h <= 0 || cell_h > 1024) {
        free(buf);
        return NULL;
    }
    layers = (version >= 2) ? 3 : 1;
    if (28u + (size_t)count * 12u > (size_t)fsize) {
        free(buf);
        return NULL;
    }
    f = (GfxFont *)calloc(1, sizeof *f);
    if (f == NULL) {
        free(buf);
        return NULL;
    }
    f->cp = (uint32_t *)malloc(sizeof(uint32_t) * (count ? count : 1));
    f->adv = (int *)malloc(sizeof(int) * (count ? count : 1));
    f->gw = (int *)malloc(sizeof(int) * (count ? count : 1));
    if (f->cp == NULL || f->adv == NULL || f->gw == NULL) {
        gfx_font_free(f);
        free(buf);
        return NULL;
    }
    f->builtin = 0;
    f->size = (int)size;
    f->ascent = (int)ascent;
    f->descent = (int)descent;
    f->margin = (int)margin;
    f->cell_h = cell_h;
    f->layers = layers;
    f->count = (int)count;
    f->data = buf;
    off = 28u + (size_t)count * 12u;
    f->px = buf + off;
    {
        size_t need = off;
        for (i = 0; i < count; i++) {
            const uint8_t *rec = buf + 28u + (size_t)i * 12u;
            f->cp[i] = rd32(rec);
            f->adv[i] = (int)rd32(rec + 4);
            f->gw[i] = (int)rd32(rec + 8);
            if (f->gw[i] <= 0 || f->gw[i] > 1024) {
                gfx_font_free(f);
                return NULL;
            }
            need += (size_t)f->gw[i] * (size_t)cell_h * (size_t)layers;
        }
        if (need > (size_t)fsize) {
            gfx_font_free(f);
            return NULL;
        }
    }
    /* pixel blocks are stored back to back: compute the per-glyph offsets
     * lazily with a prefix scan on demand (gw[] keeps the widths). */
    return f;
}

int gfx_font_pixel_size(const GfxFont *f)
{
    return f != NULL ? f->size : 0;
}

int gfx_font_glyph_count(const GfxFont *f)
{
    return f != NULL ? f->count : 0;
}

/* Pixel block of glyph gi inside the atlas. */
/* Coverage block of glyph gi: layer 0 = glyph, 1 = stroke width 1,
 * 2 = stroke width 2 (v2 atlases), or the single glyph mask (v1/built-in). */
static const uint8_t *font_glyph_px(const GfxFont *f, int gi, int layer)
{
    size_t off = 0;
    int i;

    if (layer < 0 || layer >= f->layers) {
        layer = 0;
    }
    for (i = 0; i < gi; i++) {
        off += (size_t)f->gw[i] * (size_t)f->cell_h * (size_t)f->layers;
    }
    off += (size_t)layer * (size_t)f->gw[gi] * (size_t)f->cell_h;
    return f->px + off;
}

static int font_find(const GfxFont *f, uint32_t cp)
{
    int lo = 0;
    int hi = f->count - 1;

    while (lo <= hi) {
        int mid = (lo + hi) / 2;
        if (f->cp[mid] == cp) {
            return mid;
        }
        if (f->cp[mid] < cp) {
            lo = mid + 1;
        } else {
            hi = mid - 1;
        }
    }
    return -1;
}

/* ----------------------------------------------------------------------- */
/* prims, slots, parts, text windows                                       */
/* ----------------------------------------------------------------------- */

typedef struct GfxPrim {
    int id;
    unsigned char type;
    unsigned char draw;
    unsigned char blend;
    unsigned char alpha;
    unsigned char wh_set; /* 引擎 prim+0x06 的 bit0：PrimSetWH 写过 w/h */
    unsigned short flags;
    unsigned short z;
    int x, y;
    unsigned short w, h, u, v, op1, op2;
    unsigned short angle;
    unsigned short scale_x, scale_y;
    int graph;
    int tw;
    unsigned short tile;
    int parent;
    int nchildren;
    int cap_children;
    int *children;
    int clip_x, clip_y, clip_w, clip_h;
    /* transformed-image cache (resize/alpha/rotate result) */
    uint8_t *tc_img;
    int tc_ok;
    int tc_slot, tc_ver, tc_rw, tc_rh, tc_alpha, tc_angle, tc_mode;
} GfxPrim;

typedef struct GfxTex {
    struct GfxTex *next;
    char *name;
    GfxImage img;
    int refs;
    uint64_t use; /* LRU 计数：越大越新（tex_ref 命中/新建时刷新） */
} GfxTex;

typedef struct GfxOverlay {
    int used;
    int parts;
    GfxTex *tex;
    int frame;
    int x, y;
} GfxOverlay;

/* 外字表项：码点 → 图形槽 + 原始字号（见 gfx_gaiji_add 的说明）。 */
typedef struct GfxGaiji {
    uint32_t cp;
    int size;
    int slot;
} GfxGaiji;

typedef struct GfxSlot {
    GfxTex *tex;
    char *name; /* base resource name for the draw list */
    int frame;
    int compo_version;
    int compo_built;
    uint8_t *compo;
    int compo_w, compo_h;
    /* 仿射层合成用的两个缓存（都按 compo_version 失效）：
     *   compo_opaque: 0 = 未判定 / 1 = alpha 全 255（走直写快路径）/ 2 = 有透明像素
     *   compo_mask:   8x8 覆盖掩码（稀疏层整屏扫描时先做粗剔除） */
    unsigned char compo_opaque;
    int compo_opaque_ver;
    unsigned char *compo_mask;
    int compo_mask_w, compo_mask_h;
    int compo_mask_ver;
    int nov;
    GfxOverlay ov[GFX_MAX_OVERLAYS];
} GfxSlot;

typedef struct GfxPart {
    GfxTex *tex;
    int select;
    int slot;
} GfxPart;

typedef struct GfxTextWin {
    int used;
    char *lines[GFX_TEXT_LINES];
    int nlines;
    int color[3];
    int size[2];
    /* size[1]（字模格高 = 字号）是不是**命令设过**的：gfx_create 的默认 32 只
     * 是历史默认值，不能当成"脚本指定了字号"（TextSize(8,64,true) 只写宽度，
     * 层 8 仍应按版面表用 34 号字 —— test_glyph_fallback 就靠这条）。 */
    int size_h_set;
    int pos[2];
    int has_pos;
    /* TextOutSize(120) 的两个装饰参数（历史字段名）：[0] = arg2 0..12（tw+0x595）、
     * [1] = arg3 0..8（tw+0x596）。不是宽高，见 gfx_text_set_out_size() 的注释。 */
    int out_size[2];
    int out_attr_set;
    /* TextBuff(110)：这个窗口自己的**离屏面板**尺寸（原版 handler 0x43C830 →
     * 0x430EA0：分配 w*h*4 的 RGBA 缓冲到 tw+0x520，把 w/h 写进 tw+0x5DC/+0x5DE，
     * 两分量上限 0x400；任一分量越界则整条命令放弃）。0 = 还没设过。
     * 图元驱动模式下，面板尺寸就是文本区的换行宽度与裁剪高度（原版把文字画进
     * 面板，再由 type-5 图元把面板贴到图元 x/y 上）；旧路径（--text-fixed-layout）
     * 不读它，冻结基线不变。 */
    int panel_w, panel_h;
    int panel_set;
    GfxTextLayout lay;
    int lay_custom;
    /* 逐字显示（打字机效果）。typewriter_on == 0 时这些字段不参与绘制，
     * 文本一次全部可见 —— 已验收的帧因此一个像素都不变。 */
    int speed_ms;    /* TextSpeed(129)：每字毫秒，0 = 不等待 */
    int shown_chars; /* 当前可见的字符数（按 UTF-8 码点算） */
    int total_chars; /* 最近一次 TextPrint 的字符数 */
    int accum_ms;    /* 累计的、还没凑够一个字的毫秒数 */
    int paused;      /* TextPause(121)：暂停逐字推进（原版 tw+0x5DA） */
    /* TextShadowDist(125)：阴影距离 0..12（原版写 tw+0x597）。
     * TextSpace(128)：字间距/行间距，两个分量各 -32..32（原版写 tw+0x5A8/+0x5AA）。
     * 两者都**只记录、不渲染** —— 原版的阴影形状/颜色与间距的实际用法都没有证据，
     * 参考实现 work/render.py 也是只存不画。见 docs/render-model.md §7.1。 */
    int shadow_dist;
    int space[2];
    /* TextFont(113)：两个字体号（原版写 tw+0x590 / tw+0x593）。只记录，
     * 渲染仍按字号挑图集 —— 引擎字体号 -1/-2 对应哪套字模没有证据。 */
    int font_id;
    int font_id2;
    /* TextFormat(118) / TextFunction(119)：排版参数与三个开关。只记录，
     * 不参与排版 —— 原版怎么用这些值没有证据。 */
    int fmt[6]; /* tw+0x5A8 / 0x5AA / 0x5AE / 0x5AC / 0x5B4 / 0x5B0 */
    int func[3]; /* tw+0x5A5 / 0x5A4 / 0x5A6 */
} GfxTextWin;

struct Gfx {
    int w, h;
    uint8_t *canvas;
    uint32_t bg;
    GfxHost host;
    GfxPrim *prim;
    GfxSlot *slot;
    GfxPart *part;
    GfxTextWin *tw;
    GfxTex *tex_hash[GFX_TEX_BUCKETS];
    GfxFont *fonts[GFX_MAX_FONTS];
    int nfonts;
    GfxGaiji gaiji[GFX_MAX_GAIJI];
    int ngaiji;
    int gaiji_on; /* 默认 1；只给 A/B 验证用，见 gfx_gaiji_set_enabled */
    GfxStats stats;
    const GfxGlyphFallback *glyph_fb; /* 缺字回退（可选） */
    GfxDrawItem *draw;
    int ndraw;
    int cap_draw;
    int group_xform;
    size_t tex_bytes; /* 缓存中图像的 RGBA 字节数 */
    int tex_count;
    uint64_t tex_tick;    /* LRU 单调计数 */
    size_t tex_cap;       /* 缓存上限（字节）；0 = 不限制 */
    unsigned long tex_evictions; /* 因上限被淘汰的图像数 */
    FvpMotion motion; /* 动画补间表；motion_on = 0 时只走时间轴、不动图元 */
    int motion_on;
    int typewriter_on; /* 逐字显示总开关（外壳 --typewriter，默认关） */
    /* 诊断开关：1 = PrimHit 一律答"命中"。给
     * headless 用：它没有真光标，PrimHit 永远 0 会让脚本卡在
     * 标题菜单，而"把引擎推到正文"是导出帧的前提。 */
    int prim_hit_all;
    /* 文本渲染模式。
     *
     * 默认（0）= **图元驱动**，也就是原版做法：文本窗口由 `PrimSetText(86)` 建出来的
     * type 5 文本图元承载，在 prim 树里按图元顺序、画在图元的绝对位置上；
     * **没有图元绑定的窗口不画**，引擎也**不画**合成对话框/名牌（框与名牌是脚本自己
     * GraphLoad 的图元）。
     *
     * 1 = 旧路径（外壳 `--text-fixed-layout`）：引擎画一个合成对话框，然后把 32 个
     * 文本窗口全部按 `gfx_layout_default(layer)` 的固定版式画一遍。冻结像素基线
     * （verify_gfx 等）就是用这条路径冻结的，所以它必须原样保留。 */
    int text_fixed_layout;
    unsigned char text_bound[GFX_TEXT_MAX]; /* 本帧已由图元画过的文本窗口 */
    unsigned char tw_hidden[GFX_TEXT_MAX]; /* 外壳 --hide-text-layer：不画这层文本 */
    /* 标题背景缩放缓存（gfx_draw_image_zoomed）。1920x1080 的三角形滤波缩放
     * 一次 ~80 ms，而脚本动画 40 s 内只变化 ~25‰/s；把源矩形量化到 16 px 后
     * 每 ~0.6 s 才会重算一次，其余帧直接贴缓存。 */
    uint8_t *zoom_img;
    int zoom_ok;
    int zoom_slot;
    int zoom_ver;
    int zoom_sw;
    int zoom_sh;
    int zoom_dw;
    int zoom_dh;
    char err[256];
};

/* ---- error helper ---------------------------------------------------- */

static void gfx_err(Gfx *g, const char *msg)
{
    if (g == NULL) {
        return;
    }
    if (msg == NULL) {
        g->err[0] = '\0';
        return;
    }
    snprintf(g->err, sizeof g->err, "%s", msg);
}

const char *gfx_last_error(const Gfx *g)
{
    return g != NULL ? g->err : "";
}

/* ---- text layout defaults (render.py TEXT_LAYOUT) -------------------- */

static GfxTextLayout gfx_layout_default(int layer)
{
    GfxTextLayout lay;

    switch (layer) {
    case 0:
        lay.x = 150;
        lay.y = 905;
        lay.max_w = 1620;
        lay.size = 34;
        lay.max_lines = 3;
        break;
    case 8:
        lay.x = 150;
        lay.y = 845;
        lay.max_w = 700;
        lay.size = 34;
        lay.max_lines = 1;
        break;
    case 9:
        lay.x = 0;
        lay.y = 380;
        lay.max_w = 1920;
        lay.size = 56;
        lay.max_lines = 2;
        break;
    default:
        lay.x = 120;
        lay.y = 880;
        lay.max_w = 1600;
        lay.size = 30;
        lay.max_lines = 3;
        break;
    }
    return lay;
}

/* ----------------------------------------------------------------------- */
/* create / destroy                                                        */
/* ----------------------------------------------------------------------- */

static GfxPrim *prim_at(Gfx *g, int id)
{
    if (g == NULL || id < 0 || id >= GFX_PRIM_MAX) {
        return NULL;
    }
    return &g->prim[id];
}

static void prim_init(GfxPrim *p, int id)
{
    memset(p, 0, sizeof *p);
    p->id = id;
    p->type = 0;
    p->draw = 1;
    p->alpha = 255;
    p->blend = 0;
    p->w = 0;
    p->h = 0;
    p->scale_x = 1000;
    p->scale_y = 1000;
    p->graph = -1;
    p->tw = -1;
    /* 新图元默认挂在**根组 0** 下（原版 prim+0x0A 的"不在任何组里"值是 0xFFFF，
     * 但脚本创建的图元都在根组里 —— 否则 PrimHit 的父链走不到根，永远不命中，
     * 标题菜单就点不进去）。0 号图元自己不能以 0 为父，用 -1 表示"没有父"。 */
    p->parent = (id == 0) ? -1 : 0;
    p->clip_w = 0;
}

Gfx *gfx_create(int w, int h, const GfxHost *host)
{
    Gfx *g;
    int i;

    if (w <= 0 || h <= 0 || w > GFX_MAX_W || h > GFX_MAX_H) {
        return NULL;
    }
    g = (Gfx *)calloc(1, sizeof *g);
    if (g == NULL) {
        return NULL;
    }
    g->w = w;
    g->h = h;
    g->tex_cap = GFX_TEX_CACHE_DEFAULT_BYTES;
    g->gaiji_on = 1;
    g->gaiji_on = 1;
    g->bg = 0xFF000000u; /* opaque black, R,G,B,A bytes = 0,0,0,255 */
    g->canvas = (uint8_t *)calloc((size_t)w * (size_t)h, 4);
    g->prim = (GfxPrim *)calloc(GFX_PRIM_MAX, sizeof(GfxPrim));
    g->slot = (GfxSlot *)calloc(GFX_GRAPH_MAX, sizeof(GfxSlot));
    g->part = (GfxPart *)calloc(GFX_PARTS_MAX, sizeof(GfxPart));
    g->tw = (GfxTextWin *)calloc(GFX_TEXT_MAX, sizeof(GfxTextWin));
    if (g->canvas == NULL || g->prim == NULL || g->slot == NULL ||
        g->part == NULL || g->tw == NULL) {
        gfx_destroy(g);
        return NULL;
    }
    for (i = 0; i < GFX_PRIM_MAX; i++) {
        prim_init(&g->prim[i], i);
    }
    for (i = 0; i < GFX_PARTS_MAX; i++) {
        g->part[i].slot = -1;
    }
    for (i = 0; i < GFX_TEXT_MAX; i++) {
        g->tw[i].color[0] = 255;
        g->tw[i].color[1] = 255;
        g->tw[i].color[2] = 255;
        g->tw[i].size[0] = 32;
        g->tw[i].size[1] = 32;
        g->tw[i].out_size[0] = 64;
        g->tw[i].out_size[1] = 16;
        g->tw[i].lay = gfx_layout_default(i);
    }
    g->group_xform = 1;
    fvp_motion_init(&g->motion);
    if (host != NULL) {
        g->host = *host;
    }
    return g;
}

/* ---- texture cache --------------------------------------------------- */

static unsigned tex_hash(const char *s)
{
    unsigned h = 2166136261u;
    while (*s != 0) {
        h ^= (unsigned char)*s++;
        h *= 16777619u;
    }
    return h;
}

static GfxTex *tex_find(Gfx *g, const char *name)
{
    unsigned b = tex_hash(name) % GFX_TEX_BUCKETS;
    GfxTex *t;

    for (t = g->tex_hash[b]; t != NULL; t = t->next) {
        if (strcmp(t->name, name) == 0) {
            return t;
        }
    }
    return NULL;
}

/* 真正把一张图从缓存里摘掉并释放。只有两条路径会调它：LRU 淘汰，和
 * gfx_destroy()。普通 tex_unref() 到达 0 引用时**不再释放**，图像留在缓存里
 * 供下次复用，由淘汰策略决定什么时候回收 —— 这就是"缓存"的语义。 */
static void tex_free(Gfx *g, GfxTex *t)
{
    unsigned b;

    if (t == NULL) {
        return;
    }
    b = tex_hash(t->name) % GFX_TEX_BUCKETS;
    {
        GfxTex **pp = &g->tex_hash[b];
        while (*pp != NULL) {
            if (*pp == t) {
                *pp = t->next;
                break;
            }
            pp = &(*pp)->next;
        }
    }
    {
        size_t bytes = (size_t)t->img.w * (size_t)t->img.h * 4u *
                       (size_t)(t->img.frames > 0 ? t->img.frames : 1);

        if (g->tex_bytes >= bytes) {
            g->tex_bytes -= bytes;
        } else {
            g->tex_bytes = 0;
        }
        if (g->tex_count > 0) {
            g->tex_count -= 1;
        }
    }
    if (g->host.free_image != NULL) {
        g->host.free_image(g->host.ud, &t->img);
    } else {
        free(t->img.rgba);
    }
    free(t->name);
    free(t);
}

/* 放弃一个外部引用。refs 归零只表示「没有图元/组/部件再用它了」，图像仍留在
 * 缓存里（refs == 0 正是可淘汰状态），所以这里不释放。 */
static void tex_unref(Gfx *g, GfxTex *t)
{
    (void)g;
    if (t == NULL) {
        return;
    }
    if (t->refs > 0) {
        t->refs -= 1;
    }
}

static int tex_evict(Gfx *g);

static GfxTex *tex_ref(Gfx *g, const char *name)
{
    GfxTex *t = tex_find(g, name);
    if (t != NULL) {
        t->refs++;
        t->use = ++g->tex_tick;
        g->stats.cache_hits++;
        return t;
    }
    if (g->host.load_image == NULL) {
        gfx_err(g, "no image host configured");
        return NULL;
    }
    t = (GfxTex *)calloc(1, sizeof *t);
    if (t == NULL) {
        gfx_err(g, "out of memory (texture)");
        return NULL;
    }
    g->stats.image_loads++;
    if (g->host.load_image(g->host.ud, name, &t->img) != 0 ||
        t->img.rgba == NULL || t->img.w <= 0 || t->img.h <= 0 ||
        t->img.frames <= 0) {
        if (g->host.free_image != NULL) {
            g->host.free_image(g->host.ud, &t->img);
        }
        free(t);
        snprintf(g->err, sizeof g->err, "image not found: %s", name);
        return NULL;
    }
    t->name = (char *)malloc(strlen(name) + 1);
    if (t->name == NULL) {
        if (g->host.free_image != NULL) {
            g->host.free_image(g->host.ud, &t->img);
        }
        free(t);
        return NULL;
    }
    strcpy(t->name, name);
    t->refs = 1;
    t->use = ++g->tex_tick;
    g->tex_bytes += (size_t)t->img.w * (size_t)t->img.h * 4u *
                    (size_t)(t->img.frames > 0 ? t->img.frames : 1);
    g->tex_count += 1;
    {
        unsigned b = tex_hash(name) % GFX_TEX_BUCKETS;
        t->next = g->tex_hash[b];
        g->tex_hash[b] = t;
    }
    tex_evict(g);
    return t;
}

/* 缓存超上限时按 LRU 淘汰。只淘汰 refs == 0 的条目（没有任何图元/组/部件在
 * 用）；refs > 0 的一律不动，所以淘汰永远安全：下次需要时会重新 load_image。
 * 返回本次淘汰掉的图像数。 */
static int tex_evict(Gfx *g)
{
    int evicted = 0;

    while (g->tex_cap > 0 && g->tex_bytes > g->tex_cap) {
        GfxTex *victim = NULL;
        unsigned b;

        for (b = 0; b < GFX_TEX_BUCKETS; b++) {
            GfxTex *t;

            for (t = g->tex_hash[b]; t != NULL; t = t->next) {
                if (t->refs != 0) {
                    continue;
                }
                if (victim == NULL || t->use < victim->use) {
                    victim = t;
                }
            }
        }
        if (victim == NULL) {
            break; /* 全被引用，暂时不能淘汰 */
        }
        g->tex_evictions += 1u;
        evicted += 1;
        tex_free(g, victim);
    }
    return evicted;
}

/* ----------------------------------------------------------------------- */
/* graph slots                                                             */
/* ----------------------------------------------------------------------- */

static void slot_drop_compo(GfxSlot *s)
{
    free(s->compo);
    s->compo = NULL;
    s->compo_built = 0;
    free(s->compo_mask);
    s->compo_mask = NULL;
    s->compo_mask_w = 0;
    s->compo_mask_h = 0;
    s->compo_mask_ver = -1;
    s->compo_opaque = 0;
    s->compo_opaque_ver = -1;
}

static void slot_release(Gfx *g, int slot)
{
    GfxSlot *s;

    if (slot < 0 || slot >= GFX_GRAPH_MAX) {
        return;
    }
    s = &g->slot[slot];
    tex_unref(g, s->tex);
    s->tex = NULL;
    free(s->name);
    s->name = NULL;
    slot_drop_compo(s);
    s->nov = 0;
    s->compo_version++;
}

int gfx_graph_load(Gfx *g, int slot, const char *name)
{
    GfxSlot *s;
    GfxTex *t;

    if (g == NULL || slot < 0 || slot >= GFX_GRAPH_MAX) {
        return -1;
    }
    s = &g->slot[slot];
    if (name == NULL) {
        slot_release(g, slot);
        return 0;
    }
    t = tex_ref(g, name);
    if (t == NULL) {
        /* the engine/Python prototype keep the previous image on failure */
        if (getenv("FVP_DBG_GRAPH") != NULL) {
            fprintf(stderr, "graph_load slot=%d name=\"%s\" -> MISSING\n",
                    slot, name);
        }
        return -1;
    }
    if (getenv("FVP_DBG_GRAPH") != NULL) {
        fprintf(stderr, "graph_load slot=%d name=\"%s\" -> %dx%d\n",
                slot, name, t->img.w, t->img.h);
    }
    tex_unref(g, s->tex);
    free(s->name);
    s->name = (char *)malloc(strlen(name) + 1);
    if (s->name == NULL) {
        tex_unref(g, t);
        gfx_err(g, "out of memory (slot name)");
        return -1;
    }
    strcpy(s->name, name);
    s->tex = t; /* keeps the ref from tex_ref() */
    s->frame = 0;
    s->nov = 0;
    slot_drop_compo(s);
    s->compo_version++;
    return 0;
}

void gfx_graph_release(Gfx *g, int slot)
{
    gfx_graph_load(g, slot, NULL);
}

int gfx_graph_set_frame(Gfx *g, int slot, int frame)
{
    GfxSlot *s;

    if (g == NULL || slot < 0 || slot >= GFX_GRAPH_MAX) {
        return -1;
    }
    s = &g->slot[slot];
    if (s->tex == NULL) {
        return -1;
    }
    if (frame < 0) {
        frame = 0;
    }
    if (frame >= s->tex->img.frames) {
        frame %= s->tex->img.frames;
    }
    if (s->frame != frame) {
        s->frame = frame;
        slot_drop_compo(s);
        s->compo_version++;
    }
    return 0;
}

int gfx_graph_add_overlay(Gfx *g, int slot, const char *name, int x, int y,
                          int frame)
{
    GfxSlot *s;
    GfxTex *t;
    int i, at = -1;

    if (g == NULL || slot < 0 || slot >= GFX_GRAPH_MAX || name == NULL) {
        return -1;
    }
    s = &g->slot[slot];
    if (s->tex == NULL) {
        return -1;
    }
    t = tex_ref(g, name);
    if (t == NULL) {
        return -1;
    }
    if (frame < 0) {
        frame = 0;
    }
    if (frame >= t->img.frames) {
        frame %= t->img.frames;
    }
    /* like the Python Graph.overlays list: drop an identical entry, then
     * append at the end */
    for (i = 0; i < s->nov; i++) {
        if (s->ov[i].used && s->ov[i].tex == t && s->ov[i].frame == frame) {
            tex_unref(g, s->ov[i].tex);
            for (; i + 1 < s->nov; i++) {
                s->ov[i] = s->ov[i + 1];
            }
            s->nov--;
            break;
        }
    }
    if (s->nov < GFX_MAX_OVERLAYS) {
        at = s->nov++;
    } else {
        /* replace the oldest overlay */
        tex_unref(g, s->ov[0].tex);
        for (i = 0; i + 1 < s->nov; i++) {
            s->ov[i] = s->ov[i + 1];
        }
        at = s->nov - 1;
    }
    s->ov[at].used = 1;
    s->ov[at].parts = -1;
    s->ov[at].tex = t;
    s->ov[at].frame = frame;
    s->ov[at].x = x;
    s->ov[at].y = y;
    slot_drop_compo(s);
    s->compo_version++;
    return 0;
}

/* ---- parts ----------------------------------------------------------- */

int gfx_parts_load(Gfx *g, int parts_id, const char *name)
{
    GfxPart *p;
    GfxTex *t;

    if (g == NULL || parts_id < 0 || parts_id >= GFX_PARTS_MAX) {
        return -1;
    }
    p = &g->part[parts_id];
    if (name == NULL) {
        tex_unref(g, p->tex);
        p->tex = NULL;
        return 0;
    }
    t = tex_ref(g, name);
    if (t == NULL) {
        return -1;
    }
    tex_unref(g, p->tex);
    p->tex = t;
    p->select = 0;
    return 0;
}

static int parts_apply(Gfx *g, int parts_id)
{
    GfxPart *p;
    GfxSlot *s;
    GfxOverlay *o;
    int i, at;

    if (g == NULL || parts_id < 0 || parts_id >= GFX_PARTS_MAX) {
        return -1;
    }
    p = &g->part[parts_id];
    if (p->tex == NULL || p->slot < 0 || p->slot >= GFX_GRAPH_MAX) {
        return -1;
    }
    s = &g->slot[p->slot];
    if (s->tex == NULL) {
        return -1;
    }
    at = -1;
    for (i = 0; i < s->nov; i++) {
        if (s->ov[i].parts == parts_id) {
            at = i;
            break;
        }
    }
    if (at < 0) {
        if (s->nov >= GFX_MAX_OVERLAYS) {
            return -1;
        }
        at = s->nov++;
        s->ov[at].parts = parts_id;
        s->ov[at].used = 1;
        s->ov[at].tex = NULL;
    }
    o = &s->ov[at];
    if (o->tex != p->tex) {
        tex_unref(g, o->tex);
        o->tex = p->tex;
        o->tex->refs++;
    }
    o->frame = p->select < p->tex->img.frames ? p->select
                                              : (p->select % p->tex->img.frames);
    o->x = p->tex->img.x;
    o->y = p->tex->img.y;
    slot_drop_compo(s);
    s->compo_version++;
    return 0;
}

int gfx_parts_assign(Gfx *g, int parts_id, int slot)
{
    if (g == NULL || parts_id < 0 || parts_id >= GFX_PARTS_MAX || slot < 0 ||
        slot >= GFX_GRAPH_MAX) {
        return -1;
    }
    /* drop a previous overlay on the old slot */
    if (g->part[parts_id].slot >= 0 && g->part[parts_id].slot != slot) {
        GfxSlot *old = &g->slot[g->part[parts_id].slot];
        int i;
        for (i = 0; i < old->nov; i++) {
            if (old->ov[i].parts == parts_id) {
                tex_unref(g, old->ov[i].tex);
                for (; i + 1 < old->nov; i++) {
                    old->ov[i] = old->ov[i + 1];
                }
                old->nov--;
                slot_drop_compo(old);
                old->compo_version++;
                break;
            }
        }
    }
    g->part[parts_id].slot = slot;
    return parts_apply(g, parts_id);
}

int gfx_parts_select(Gfx *g, int parts_id, int frame)
{
    if (g == NULL || parts_id < 0 || parts_id >= GFX_PARTS_MAX) {
        return -1;
    }
    g->part[parts_id].select = frame;
    return parts_apply(g, parts_id);
}

void gfx_parts_release(Gfx *g, int parts_id)
{
    if (g == NULL || parts_id < 0 || parts_id >= GFX_PARTS_MAX) {
        return;
    }
    tex_unref(g, g->part[parts_id].tex);
    g->part[parts_id].tex = NULL;
    g->part[parts_id].slot = -1;
}

/* ---- slot composite: base frame + parts overlays --------------------- */

static void over_px(uint8_t *d, int sr, int sg, int sb, int sa)
{
    int da = d[3];
    int oa;
    int den;

    if (sa <= 0) {
        return;
    }
    if (sa >= 255 && da >= 255) {
        d[0] = (uint8_t)sr;
        d[1] = (uint8_t)sg;
        d[2] = (uint8_t)sb;
        d[3] = 255;
        return;
    }
    oa = sa + (da * (255 - sa) + 127) / 255;
    if (oa <= 0) {
        d[0] = d[1] = d[2] = d[3] = 0;
        return;
    }
    if (oa > 255) {
        oa = 255;
    }
    den = oa * 255;
    d[0] = (uint8_t)clampi((sr * sa * 255 + d[0] * da * (255 - sa) + den / 2) /
                               den,
                           0, 255);
    d[1] = (uint8_t)clampi((sg * sa * 255 + d[1] * da * (255 - sa) + den / 2) /
                               den,
                           0, 255);
    d[2] = (uint8_t)clampi((sb * sa * 255 + d[2] * da * (255 - sa) + den / 2) /
                               den,
                           0, 255);
    d[3] = (uint8_t)oa;
}

/* Build the per-slot image (base frame + parts overlays) on demand. */
static int slot_compose(Gfx *g, int slot, const uint8_t **out, int *ow, int *oh)
{
    GfxSlot *s;
    const GfxImage *img;
    size_t frame_bytes;
    int i;

    if (slot < 0 || slot >= GFX_GRAPH_MAX) {
        return -1;
    }
    s = &g->slot[slot];
    if (s->tex == NULL) {
        return -1;
    }
    img = &s->tex->img;
    if (s->compo != NULL && s->compo_built == s->compo_version) {
        *out = s->compo;
        *ow = s->compo_w;
        *oh = s->compo_h;
        return 0;
    }
    frame_bytes = (size_t)img->w * (size_t)img->h * 4u;
    if (s->compo == NULL || s->compo_w != img->w || s->compo_h != img->h) {
        free(s->compo);
        s->compo = (uint8_t *)malloc(frame_bytes);
        if (s->compo == NULL) {
            gfx_err(g, "out of memory (slot composite)");
            return -1;
        }
        s->compo_w = img->w;
        s->compo_h = img->h;
    }
    memcpy(s->compo, img->rgba + (size_t)s->frame * frame_bytes, frame_bytes);
    g->stats.compo_builds++;
    for (i = 0; i < s->nov; i++) {
        GfxOverlay *o = &s->ov[i];
        const GfxImage *oi;
        const uint8_t *src;
        int x, y, w, h, yy, xx;

        if (!o->used || o->tex == NULL) {
            continue;
        }
        oi = &o->tex->img;
        if (o->frame < 0 || o->frame >= oi->frames) {
            continue;
        }
        src = oi->rgba + (size_t)o->frame * (size_t)oi->w * (size_t)oi->h * 4u;
        w = oi->w;
        h = oi->h;
        x = o->x;
        y = o->y;
        for (yy = 0; yy < h; yy++) {
            int dy = y + yy;
            if (dy < 0 || dy >= img->h) {
                continue;
            }
            for (xx = 0; xx < w; xx++) {
                int dx = x + xx;
                const uint8_t *sp;
                uint8_t *dp;
                if (dx < 0 || dx >= img->w) {
                    continue;
                }
                sp = src + ((size_t)yy * (size_t)w + (size_t)xx) * 4u;
                dp = s->compo + ((size_t)dy * (size_t)img->w + (size_t)dx) * 4u;
                over_px(dp, sp[0], sp[1], sp[2], sp[3]);
            }
        }
    }
    s->compo_built = s->compo_version;
    *out = s->compo;
    *ow = s->compo_w;
    *oh = s->compo_h;
    return 0;
}

/* ----------------------------------------------------------------------- */
/* resampling (Pillow 12 semantics: premultiply RGBA, triangle filter,     */
/* two separable uint8 passes, then unpremultiply)                         */
/* ----------------------------------------------------------------------- */

typedef struct GfxCoeff {
    int n;
    int first;
    float w[GFX_COEFF_MAX];
} GfxCoeff;

static GfxCoeff *coeffs_build(int in_size, int out_size, int *ksize_out)
{
    GfxCoeff *c;
    double scale;
    double fs;
    double support;
    int x;

    if (in_size <= 0 || out_size <= 0) {
        return NULL;
    }
    scale = (double)in_size / (double)out_size;
    fs = scale < 1.0 ? 1.0 : scale;
    support = fs;
    c = (GfxCoeff *)calloc((size_t)out_size, sizeof *c);
    if (c == NULL) {
        return NULL;
    }
    for (x = 0; x < out_size; x++) {
        double center = ((double)x + 0.5) * scale;
        int lo = (int)floor(center - support + 0.5);
        int hi = (int)ceil(center + support + 0.5);
        int i, n = 0;
        double sum = 0.0;

        if (lo < 0) {
            lo = 0;
        }
        if (hi > in_size) {
            hi = in_size;
        }
        if (hi - lo > GFX_COEFF_MAX) {
            hi = lo + GFX_COEFF_MAX;
        }
        for (i = lo; i < hi; i++) {
            double t = fabs(((double)i + 0.5 - center) / fs);
            double w = t < 1.0 ? 1.0 - t : 0.0;
            if (w <= 0.0) {
                continue;
            }
            c[x].w[n] = (float)w;
            if (n == 0) {
                c[x].first = i;
            }
            sum += w;
            n++;
        }
        c[x].n = n;
        if (n == 0) {
            c[x].first = clampi((int)(center), 0, in_size - 1);
            c[x].w[0] = 1.0f;
            c[x].n = 1;
            sum = 1.0;
        }
        for (i = 0; i < n; i++) {
            c[x].w[i] = (float)(c[x].w[i] / sum);
        }
    }
    if (ksize_out != NULL) {
        *ksize_out = GFX_COEFF_MAX;
    }
    return c;
}

static uint8_t *resize_rgba(const uint8_t *src, int sw, int sh, int dw, int dh)
{
    GfxCoeff *hc, *vc;
    uint8_t *pm, *tmp, *dst;
    int x, y, i, k;

    if (sw <= 0 || sh <= 0 || dw <= 0 || dh <= 0) {
        return NULL;
    }
    hc = coeffs_build(sw, dw, NULL);
    vc = coeffs_build(sh, dh, NULL);
    if (hc == NULL || vc == NULL) {
        free(hc);
        free(vc);
        return NULL;
    }
    pm = (uint8_t *)malloc((size_t)sw * (size_t)sh * 4u);
    tmp = (uint8_t *)malloc((size_t)sh * (size_t)dw * 4u);
    dst = (uint8_t *)malloc((size_t)dw * (size_t)dh * 4u);
    if (pm == NULL || tmp == NULL || dst == NULL) {
        free(pm);
        free(tmp);
        free(dst);
        free(hc);
        free(vc);
        return NULL;
    }
    /* premultiply: Pillow converts RGBA -> "RGBa" with MULDIV255 */
    for (i = 0, k = 0; i < sw * sh; i++, k += 4) {
        int a = src[k + 3];
        pm[k + 0] = (uint8_t)mouldiv255(src[k + 0], a);
        pm[k + 1] = (uint8_t)mouldiv255(src[k + 1], a);
        pm[k + 2] = (uint8_t)mouldiv255(src[k + 2], a);
        pm[k + 3] = (uint8_t)a;
    }
    /* horizontal pass */
    for (y = 0; y < sh; y++) {
        const uint8_t *row = pm + (size_t)y * (size_t)sw * 4u;
        uint8_t *orow = tmp + (size_t)y * (size_t)dw * 4u;
        for (x = 0; x < dw; x++) {
            GfxCoeff *cf = &hc[x];
            float acc0 = 0.0f, acc1 = 0.0f, acc2 = 0.0f, acc3 = 0.0f;
            for (i = 0; i < cf->n; i++) {
                const uint8_t *sp =
                    row + ((size_t)cf->first + (size_t)i) * 4u;
                float w = cf->w[i];
                acc0 += w * (float)sp[0];
                acc1 += w * (float)sp[1];
                acc2 += w * (float)sp[2];
                acc3 += w * (float)sp[3];
            }
            orow[x * 4 + 0] = (uint8_t)clampi((int)floor(acc0 + 0.5f), 0, 255);
            orow[x * 4 + 1] = (uint8_t)clampi((int)floor(acc1 + 0.5f), 0, 255);
            orow[x * 4 + 2] = (uint8_t)clampi((int)floor(acc2 + 0.5f), 0, 255);
            orow[x * 4 + 3] = (uint8_t)clampi((int)floor(acc3 + 0.5f), 0, 255);
        }
    }
    /* vertical pass */
    for (y = 0; y < dh; y++) {
        GfxCoeff *cf = &vc[y];
        uint8_t *orow = dst + (size_t)y * (size_t)dw * 4u;
        for (x = 0; x < dw; x++) {
            float acc0 = 0.0f, acc1 = 0.0f, acc2 = 0.0f, acc3 = 0.0f;
            for (i = 0; i < cf->n; i++) {
                const uint8_t *sp =
                    tmp + (size_t)(cf->first + i) * (size_t)dw * 4u +
                    (size_t)x * 4u;
                float w = cf->w[i];
                acc0 += w * (float)sp[0];
                acc1 += w * (float)sp[1];
                acc2 += w * (float)sp[2];
                acc3 += w * (float)sp[3];
            }
            orow[x * 4 + 0] = (uint8_t)clampi((int)floor(acc0 + 0.5f), 0, 255);
            orow[x * 4 + 1] = (uint8_t)clampi((int)floor(acc1 + 0.5f), 0, 255);
            orow[x * 4 + 2] = (uint8_t)clampi((int)floor(acc2 + 0.5f), 0, 255);
            orow[x * 4 + 3] = (uint8_t)clampi((int)floor(acc3 + 0.5f), 0, 255);
        }
    }
    /* unpremultiply: Pillow RGBa -> RGBA = floor(c*255/a) */
    for (i = 0, k = 0; i < dw * dh; i++, k += 4) {
        int a = dst[k + 3];
        if (a <= 0) {
            dst[k + 0] = 0;
            dst[k + 1] = 0;
            dst[k + 2] = 0;
        } else {
            dst[k + 0] = (uint8_t)clampi(dst[k + 0] * 255 / a, 0, 255);
            dst[k + 1] = (uint8_t)clampi(dst[k + 1] * 255 / a, 0, 255);
            dst[k + 2] = (uint8_t)clampi(dst[k + 2] * 255 / a, 0, 255);
        }
    }
    free(pm);
    free(tmp);
    free(hc);
    free(vc);
    return dst;
}

/* Rotate about the centre, expanding the canvas (Pillow rotate(expand=True)
 * semantics: the rotated image is then pasted with its top-left at the
 * destination origin).  Bilinear sampling. */
static uint8_t *rotate_rgba(const uint8_t *src, int w, int h, double deg,
                            int *ow, int *oh)
{
    double rad = deg * 3.14159265358979323846 / 180.0;
    double ca = cos(rad);
    double sa = sin(rad);
    double xmin = 0.0, xmax = 0.0, ymin = 0.0, ymax = 0.0;
    int nw, nh, x, y, i;
    uint8_t *dst;
    const double cx = (double)w / 2.0;
    const double cy = (double)h / 2.0;

    /* forward-transform the four corners to get the expanded bbox */
    for (i = 0; i < 4; i++) {
        double px = (i & 1) ? (double)w : 0.0;
        double py = (i & 2) ? (double)h : 0.0;
        double dx = (px - cx) * ca - (py - cy) * sa + cx;
        double dy = (px - cx) * sa + (py - cy) * ca + cy;
        if (i == 0) {
            xmin = xmax = dx;
            ymin = ymax = dy;
        } else {
            if (dx < xmin) xmin = dx;
            if (dx > xmax) xmax = dx;
            if (dy < ymin) ymin = dy;
            if (dy > ymax) ymax = dy;
        }
    }
    nw = (int)ceil(xmax) - (int)floor(xmin);
    nh = (int)ceil(ymax) - (int)floor(ymin);
    if (nw <= 0) nw = 1;
    if (nh <= 0) nh = 1;
    dst = (uint8_t *)calloc((size_t)nw * (size_t)nh, 4);
    if (dst == NULL) {
        return NULL;
    }
    for (y = 0; y < nh; y++) {
        for (x = 0; x < nw; x++) {
            /* inverse map: destination (x,y) -> source pixel centre */
            double dx = (double)x + 0.5 + floor(xmin);
            double dy = (double)y + 0.5 + floor(ymin);
            double sx = (dx - cx) * ca + (dy - cy) * sa + cx - 0.5;
            double sy = -(dx - cx) * sa + (dy - cy) * ca + cy - 0.5;
            int x0 = (int)floor(sx);
            int y0 = (int)floor(sy);
            double fx = sx - x0;
            double fy = sy - y0;
            int c;
            uint8_t *o = dst + ((size_t)y * (size_t)nw + (size_t)x) * 4u;
            for (c = 0; c < 4; c++) {
                double v00, v10, v01, v11, v;
                const uint8_t *p00, *p10, *p01, *p11;
                int xa = clampi(x0, 0, w - 1);
                int xb = clampi(x0 + 1, 0, w - 1);
                int ya = clampi(y0, 0, h - 1);
                int yb = clampi(y0 + 1, 0, h - 1);
                if (sx < -0.5 || sy < -0.5 || sx > w - 0.5 || sy > h - 0.5) {
                    o[c] = 0;
                    continue;
                }
                p00 = src + ((size_t)ya * (size_t)w + (size_t)xa) * 4u;
                p10 = src + ((size_t)ya * (size_t)w + (size_t)xb) * 4u;
                p01 = src + ((size_t)yb * (size_t)w + (size_t)xa) * 4u;
                p11 = src + ((size_t)yb * (size_t)w + (size_t)xb) * 4u;
                v00 = p00[c];
                v10 = p10[c];
                v01 = p01[c];
                v11 = p11[c];
                v = (v00 * (1 - fx) + v10 * fx) * (1 - fy) +
                    (v01 * (1 - fx) + v11 * fx) * fy;
                o[c] = (uint8_t)clampi((int)floor(v + 0.5), 0, 255);
            }
        }
    }
    *ow = nw;
    *oh = nh;
    return dst;
}

/* ----------------------------------------------------------------------- */
/* canvas composite primitives                                             */
/* ----------------------------------------------------------------------- */

static void put_px(Gfx *g, int x, int y, int r, int gr, int b, int a)
{
    uint8_t *d;
    if (x < 0 || y < 0 || x >= g->w || y >= g->h) {
        return;
    }
    d = g->canvas + ((size_t)y * (size_t)g->w + (size_t)x) * 4u;
    over_px(d, r, gr, b, a);
}

static void clear_canvas(Gfx *g)
{
    uint8_t *p = g->canvas;
    size_t n = (size_t)g->w * (size_t)g->h;
    size_t i;
    int r = (int)(g->bg & 0xFFu);
    int gr = (int)((g->bg >> 8) & 0xFFu);
    int b = (int)((g->bg >> 16) & 0xFFu);
    int a = (int)((g->bg >> 24) & 0xFFu);

    for (i = 0; i < n; i++) {
        p[0] = (uint8_t)r;
        p[1] = (uint8_t)gr;
        p[2] = (uint8_t)b;
        p[3] = (uint8_t)a;
        p += 4;
    }
}

/* Fast path for an opaque destination (the canvas during the sprite pass).
 * Mathematically identical to over_px() with dst alpha 255, but the constant
 * divisor lets the compiler turn the divide into a multiply-shift. */
static void over_px_opaque(uint8_t *d, int sr, int sg, int sb, int sa)
{
    int inv;

    if (sa >= 255) {
        d[0] = (uint8_t)sr;
        d[1] = (uint8_t)sg;
        d[2] = (uint8_t)sb;
        d[3] = 255;
        return;
    }
    if (sa <= 0) {
        return;
    }
    inv = 255 - sa;
    d[0] = (uint8_t)((sr * sa + d[0] * inv + 127) / 255);
    d[1] = (uint8_t)((sg * sa + d[1] * inv + 127) / 255);
    d[2] = (uint8_t)((sb * sa + d[2] * inv + 127) / 255);
    d[3] = 255;
}

/* 把 sw*sh 的 RGBA 源最近邻缩放到 dw*dh 画到 (x,y)。
 * 合成规则与 blit_sprite 的非 additive 分支一致（源 alpha 覆盖目标）。 */
static void blit_rgba_scaled(Gfx *g, int x, int y, int dw, int dh,
                             const uint8_t *src, int sw, int sh)
{
    int yy;

    if (src == NULL || sw <= 0 || sh <= 0 || dw <= 0 || dh <= 0) {
        return;
    }
    for (yy = 0; yy < dh; yy++) {
        int sy = (int)((long)yy * (long)sh / (long)dh);
        int dy = y + yy;
        int xx;

        if (dy < 0 || dy >= g->h) {
            continue;
        }
        for (xx = 0; xx < dw; xx++) {
            int sx = (int)((long)xx * (long)sw / (long)dw);
            int dx = x + xx;
            const uint8_t *s;
            uint8_t *d;
            int a;

            if (dx < 0 || dx >= g->w) {
                continue;
            }
            s = src + ((size_t)sy * (size_t)sw + (size_t)sx) * 4u;
            a = s[3];
            if (a == 0) {
                continue;
            }
            d = g->canvas + ((size_t)dy * (size_t)g->w + (size_t)dx) * 4u;
            if (a == 255) {
                d[0] = s[0];
                d[1] = s[1];
                d[2] = s[2];
                d[3] = 255;
            } else {
                int c;
                double af = (double)a / 255.0;

                for (c = 0; c < 3; c++) {
                    d[c] = (uint8_t)((double)s[c] * af +
                                     (double)d[c] * (1.0 - af));
                }
                d[3] = 255;
            }
        }
    }
}

static const GfxGaiji *gfx_gaiji_find(const Gfx *g, uint32_t cp)
{
    int i;

    for (i = 0; i < g->ngaiji; i++) {
        if (g->gaiji[i].cp == cp) {
            return &g->gaiji[i];
        }
    }
    return NULL;
}

int gfx_gaiji_add(Gfx *g, uint32_t codepoint, int size, int slot)
{
    int i;

    if (g == NULL || codepoint == 0u) {
        return 0;
    }
    if (slot < 0 || slot >= GFX_GRAPH_MAX) {
        return 0;
    }
    /* 同一个码点重复登记：覆盖原条目（脚本重进场景时会重新 GaijiLoad） */
    for (i = 0; i < g->ngaiji; i++) {
        if (g->gaiji[i].cp == codepoint) {
            g->gaiji[i].size = size;
            g->gaiji[i].slot = slot;
            return 1;
        }
    }
    if (g->ngaiji >= GFX_MAX_GAIJI) {
        return 0;
    }
    g->gaiji[g->ngaiji].cp = codepoint;
    g->gaiji[g->ngaiji].size = size;
    g->gaiji[g->ngaiji].slot = slot;
    g->ngaiji += 1;
    return 1;
}

int gfx_gaiji_count(const Gfx *g)
{
    return g == NULL ? 0 : g->ngaiji;
}

void gfx_gaiji_set_enabled(Gfx *g, int on)
{
    if (g != NULL) {
        g->gaiji_on = on ? 1 : 0;
    }
}

int gfx_gaiji_enabled(const Gfx *g)
{
    return g == NULL ? 0 : g->gaiji_on;
}

int gfx_gaiji_entry(const Gfx *g, int i, uint32_t *codepoint, int *size,
                    int *slot, int *image_ok)
{
    const GfxTex *t;

    if (g == NULL || i < 0 || i >= g->ngaiji) {
        return 0;
    }
    if (codepoint != NULL) {
        *codepoint = g->gaiji[i].cp;
    }
    if (size != NULL) {
        *size = g->gaiji[i].size;
    }
    if (slot != NULL) {
        *slot = g->gaiji[i].slot;
    }
    if (image_ok != NULL) {
        t = g->slot[g->gaiji[i].slot].tex;
        *image_ok = (t != NULL && t->img.rgba != NULL && t->img.w > 0 &&
                     t->img.h > 0)
                        ? 1
                        : 0;
    }
    return 1;
}

static void blit_sprite(Gfx *g, int x, int y, int rw, int rh,
                        const uint8_t *src, int additive, const GfxPrim *p)
{
    int x0 = x < 0 ? 0 : x;
    int y0 = y < 0 ? 0 : y;
    int x1 = x + rw;
    int y1 = y + rh;
    int yy;
    int dst_opaque;

    if (x1 > g->w) {
        x1 = g->w;
    }
    if (y1 > g->h) {
        y1 = g->h;
    }
    if (p->clip_w > 0) {
        if (p->clip_x > x0) {
            x0 = p->clip_x;
        }
        if (p->clip_y > y0) {
            y0 = p->clip_y;
        }
        if (p->clip_x + p->clip_w < x1) {
            x1 = p->clip_x + p->clip_w;
        }
        if (p->clip_y + p->clip_h < y1) {
            y1 = p->clip_y + p->clip_h;
        }
    }
    if (x1 <= x0 || y1 <= y0) {
        return;
    }
    dst_opaque = ((g->bg >> 24) & 0xFFu) == 255u;
    for (yy = y0; yy < y1; yy++) {
        const uint8_t *s = src + ((size_t)(yy - y) * (size_t)rw +
                                 (size_t)(x0 - x)) * 4u;
        uint8_t *d = g->canvas + ((size_t)yy * (size_t)g->w + (size_t)x0) * 4u;
        int xx;
        for (xx = x0; xx < x1; xx++, s += 4, d += 4) {
            if (additive) {
                int sa = s[3];
                int c;
                for (c = 0; c < 3; c++) {
                    int add = (int)((double)s[c] * ((double)sa / 255.0));
                    int v = d[c] + add;
                    d[c] = (uint8_t)(v > 255 ? 255 : v);
                }
                d[3] = 255;
            } else if (dst_opaque) {
                over_px_opaque(d, s[0], s[1], s[2], s[3]);
            } else {
                over_px(d, s[0], s[1], s[2], s[3]);
            }
        }
        g->stats.pixels += (long long)(x1 - x0);
    }
}

/* 与 blit_sprite 相同，但把**额外的整体 alpha** 乘进覆盖度。
 *
 * 为什么单独一个函数：`blit_sprite()` 历史上**忽略 p->alpha**（只用源像素的 s[3]），
 * 已验收的帧（verify_gfx 的 528000 步画面）就是按这个行为冻结的 —— 直接改它会
 * 让 PSNR 从 49.94 掉到 35 左右。所以保持 blit_sprite 原样，让
 * gfx_draw_image_alpha() / gfx_draw_image_xform() 走这条 alpha 感知的路径。 */
static void blit_sprite_alpha(Gfx *g, int x, int y, int rw, int rh,
                              const uint8_t *src, int additive, int alpha)
{
    int x0 = x < 0 ? 0 : x;
    int y0 = y < 0 ? 0 : y;
    int x1 = x + rw;
    int y1 = y + rh;
    int yy;
    int dst_opaque;
    GfxPrim tmp;

    if (alpha >= 255) {
        memset(&tmp, 0, sizeof tmp);
        tmp.alpha = 255;
        blit_sprite(g, x, y, rw, rh, src, additive, &tmp);
        return;
    }
    if (alpha <= 0) {
        return;
    }
    if (x1 > g->w) {
        x1 = g->w;
    }
    if (y1 > g->h) {
        y1 = g->h;
    }
    if (x1 <= x0 || y1 <= y0) {
        return;
    }
    dst_opaque = ((g->bg >> 24) & 0xFFu) == 255u;
    for (yy = y0; yy < y1; yy++) {
        const uint8_t *s = src + ((size_t)(yy - y) * (size_t)rw +
                                 (size_t)(x0 - x)) * 4u;
        uint8_t *d = g->canvas + ((size_t)yy * (size_t)g->w + (size_t)x0) * 4u;
        int xx;

        for (xx = x0; xx < x1; xx++, s += 4, d += 4) {
            int sa = (int)((s[3] * alpha + 127) / 255);

            if (additive) {
                int c;

                for (c = 0; c < 3; c++) {
                    int add = (int)((double)s[c] * ((double)sa / 255.0));
                    int v = d[c] + add;

                    d[c] = (uint8_t)(v > 255 ? 255 : v);
                }
                d[3] = 255;
            } else if (dst_opaque) {
                over_px_opaque(d, s[0], s[1], s[2], sa);
            } else {
                over_px(d, s[0], s[1], s[2], sa);
            }
        }
        g->stats.pixels += (long long)(x1 - x0);
    }
}

/* ----------------------------------------------------------------------- */
/* draw list                                                               */
/* ----------------------------------------------------------------------- */

static void draw_add(Gfx *g, int prim, const char *name, int x, int y, int w,
                     int h, int mode, int z, int alpha)
{
    GfxDrawItem *it;

    if (g->ndraw >= g->cap_draw) {
        int cap = g->cap_draw ? g->cap_draw * 2 : 64;
        GfxDrawItem *ni =
            (GfxDrawItem *)realloc(g->draw, (size_t)cap * sizeof *ni);
        if (ni == NULL) {
            return;
        }
        g->draw = ni;
        g->cap_draw = cap;
    }
    it = &g->draw[g->ndraw++];
    it->prim = prim;
    if (name != NULL) {
        snprintf(it->name, sizeof it->name, "%s", name);
    } else {
        it->name[0] = '\0';
    }
    it->x = x;
    it->y = y;
    it->w = w;
    it->h = h;
    it->mode = mode;
    it->z = z;
    it->alpha = alpha;
}

/* ----------------------------------------------------------------------- */
/* sprite rendering                                                        */
/* ----------------------------------------------------------------------- */

typedef struct GfxXf {
    double tx, ty;
    double sx, sy;
    int alpha;
    double angle; /* degrees */
} GfxXf;

static void xf_identity(GfxXf *xf)
{
    xf->tx = 0.0;
    xf->ty = 0.0;
    xf->sx = 1.0;
    xf->sy = 1.0;
    xf->alpha = 255;
    xf->angle = 0.0;
}

static void render_sprite(Gfx *g, GfxPrim *p, const GfxXf *xf)
{
    GfxSlot *s;
    const uint8_t *compo = NULL;
    int cw = 0, ch = 0;
    int rw, rh, mode, x, y;
    int rec_w, rec_h;
    int alpha_eff;
    double gax, gay;
    double scx, scy;
    int need_xform = 0;
    int angle_eff;

    if (p->graph < 0 || p->graph >= GFX_GRAPH_MAX) {
        fprintf(stderr, "[dbg] prim %d bad graph %d\n", p->id, p->graph);
        return;
    }
    s = &g->slot[p->graph];
    if (s->tex == NULL) {
        return;
    }
    if (slot_compose(g, p->graph, &compo, &cw, &ch) != 0) {
        return;
    }
    alpha_eff = clampi((p->alpha * xf->alpha + 127) / 255, 0, 255);
    if (alpha_eff <= 0) {
        return;
    }
    scx = (double)p->scale_x / 1000.0;
    scy = (double)p->scale_y / 1000.0;
    if (scx == 0.0) {
        scx = 1.0;
    }
    if (scy == 0.0) {
        scy = 1.0;
    }
    scx *= xf->sx;
    scy *= xf->sy;
    gax = xf->tx + (double)(s->tex->img.x + p->x);
    gay = xf->ty + (double)(s->tex->img.y + p->y);
    angle_eff = (int)(p->angle + xf->angle * 10.0);

    if (ch > cw) {
        /* standing portrait: bottom-centre anchor (render-model.md 2.2) */
        int rw2 = py_round((double)cw * scx);
        rh = py_round((double)ch * scx);
        rw = rw2;
        x = (int)floor(gax) - py_floordiv(rw, 2);
        y = (int)floor(gay) - rh;
        mode = GFX_MODE_CHAR;
    } else if (cw > (int)((double)g->w * 1.15) ||
               ch > (int)((double)g->h * 1.15)) {
        /* oversized scenery: centred, 1:1 */
        x = py_floordiv(g->w - cw, 2) + (int)floor(gax);
        y = py_floordiv(g->h - ch, 2) + (int)floor(gay);
        rw = cw;
        rh = ch;
        mode = GFX_MODE_BG;
    } else {
        int base_w = p->w != 0 ? (int)p->w : cw;
        int base_h = p->h != 0 ? (int)p->h : ch;
        rw = py_round((double)base_w * scx);
        rh = py_round((double)base_h * scy);
        x = (int)floor(gax);
        y = (int)floor(gay);
        mode = GFX_MODE_UI;
    }
    if (rw <= 0 || rh <= 0) {
        return;
    }
    rec_w = rw;
    rec_h = rh;
    if (x >= g->w || y >= g->h || x + rw <= 0 || y + rh <= 0) {
        return;
    }

    if (mode == GFX_MODE_UI && angle_eff != 0) {
        need_xform = 1;
    }
    if (rw != cw || rh != ch || alpha_eff < 255) {
        need_xform = 1;
    }

    if (!need_xform) {
        /* fast path: the slot composite is used as-is */
        blit_sprite(g, x, y, rw, rh, compo, p->blend != 0, p);
        draw_add(g, p->id, s->name, x, y, rec_w, rec_h, mode,
                 (int)p->z, (int)p->alpha);
        g->stats.sprites++;
        return;
    }

    /* cached transformed image? */
    if (p->tc_ok && p->tc_img != NULL && p->tc_slot == p->graph &&
        p->tc_ver == s->compo_version && p->tc_rw == rw && p->tc_rh == rh &&
        p->tc_alpha == alpha_eff && p->tc_angle == angle_eff &&
        p->tc_mode == mode) {
        g->stats.resize_cache_hits++;
        blit_sprite(g, x, y, rw, rh, p->tc_img, p->blend != 0, p);
        draw_add(g, p->id, s->name, x, y, rec_w, rec_h, mode,
                 (int)p->z, (int)p->alpha);
        g->stats.sprites++;
        return;
    }

    {
        uint8_t *img;
        int tw = cw, th = ch;
        if (rw != cw || rh != ch) {
            img = resize_rgba(compo, cw, ch, rw, rh);
            if (img == NULL) {
                gfx_err(g, "out of memory (resize)");
                return;
            }
            tw = rw;
            th = rh;
        } else {
            img = (uint8_t *)malloc((size_t)cw * (size_t)ch * 4u);
            if (img == NULL) {
                gfx_err(g, "out of memory (sprite copy)");
                return;
            }
            memcpy(img, compo, (size_t)cw * (size_t)ch * 4u);
        }
        if (alpha_eff < 255) {
            size_t i, n = (size_t)tw * (size_t)th;
            for (i = 0; i < n; i++) {
                uint8_t *q = img + i * 4u + 3u;
                *q = (uint8_t)((*q * alpha_eff) / 255);
            }
        }
        if (mode == GFX_MODE_UI && angle_eff != 0) {
            int ow = 0, oh = 0;
            uint8_t *rot =
                rotate_rgba(img, tw, th, -(double)angle_eff / 20.0, &ow, &oh);
            free(img);
            if (rot == NULL) {
                return;
            }
            img = rot;
            tw = ow;
            th = oh;
            rw = ow;
            rh = oh;
        }
        free(p->tc_img);
        p->tc_img = img;
        p->tc_ok = 1;
        p->tc_slot = p->graph;
        p->tc_ver = s->compo_version;
        p->tc_rw = rw;
        p->tc_rh = rh;
        p->tc_alpha = alpha_eff;
        p->tc_angle = angle_eff;
        p->tc_mode = mode;
        blit_sprite(g, x, y, rw, rh, img, p->blend != 0, p);
        draw_add(g, p->id, s->name, x, y, rec_w, rec_h, mode, (int)p->z,
                 (int)p->alpha);
        g->stats.sprites++;
    }
}

/* ----------------------------------------------------------------------- */
/* group tree walk (engine 0x438ab0: depth first, insertion order)         */
/* ----------------------------------------------------------------------- */

/* 文本相关：walk_tree 里要按图元顺序画文本窗口（默认模式），定义在后面。 */
static int text_visible(const GfxTextWin *tw);
static void draw_text_window(Gfx *g, int idx, const int *origin, int alpha);
static void prim_abs_xy(const Gfx *g, int prim_id, int *ox, int *oy);

static void walk_tree(Gfx *g, int pid, int depth, const GfxXf *parent)
{
    GfxPrim *p;
    GfxXf xf;
    int i;

    if (depth > 32 || pid < 0 || pid >= GFX_PRIM_MAX) {
        return;
    }
    p = &g->prim[pid];
    if (!p->draw) {
        return;
    }
    if (p->type == 1) {
        xf = *parent;
        if (g->group_xform) {
            xf.tx += xf.sx * (double)p->x;
            xf.ty += xf.sy * (double)p->y;
            xf.sx *= (double)p->scale_x / 1000.0;
            xf.sy *= (double)p->scale_y / 1000.0;
            xf.alpha = (p->alpha * xf.alpha + 127) / 255;
            xf.angle += (double)p->angle / 20.0;
        }
        for (i = 0; i < p->nchildren; i++) {
            walk_tree(g, p->children[i], depth + 1, &xf);
        }
    } else if (p->type == 4) {
        if (p->alpha > 0) {
            render_sprite(g, p, parent);
        }
    } else if (p->type == 5 && !g->text_fixed_layout) {
        /* 原版路径：文本图元在图元树里按顺序画，位置取图元（含父链）的绝对坐标，
         * alpha 跟着图元走；这样文本可以和 CG/立绘正确前后遮挡，也能浮在 CG 上。
         * 旧路径（text_fixed_layout）仍把全部窗口留到最后按版面表画。 */
        int origin[2];

        if (p->draw && p->alpha > 0 && p->tw >= 0 && p->tw < GFX_TEXT_MAX &&
            !g->tw_hidden[p->tw] && text_visible(&g->tw[p->tw])) {
            prim_abs_xy(g, pid, &origin[0], &origin[1]);
            draw_text_window(g, p->tw, origin, p->alpha);
            g->text_bound[p->tw] = 1u;
            g->stats.text_prims += 1;
        }
    }
}

static void mark_tree(Gfx *g, int pid, unsigned char *seen)
{
    GfxPrim *p;
    int i;

    if (pid < 0 || pid >= GFX_PRIM_MAX || seen[pid]) {
        return;
    }
    seen[pid] = 1;
    p = &g->prim[pid];
    for (i = 0; i < p->nchildren; i++) {
        mark_tree(g, p->children[i], seen);
    }
}

/* ----------------------------------------------------------------------- */
/* text                                                                    */
/* ----------------------------------------------------------------------- */

static GfxFont *font_fallback(Gfx *g, const GfxFont *want, uint32_t cp);

static GfxFont *pick_font(Gfx *g, int size)
{
    int i;
    for (i = 0; i < g->nfonts; i++) {
        if (g->fonts[i]->size == size) {
            return g->fonts[i];
        }
    }
    return font_builtin();
}

static int char_advance(Gfx *g, GfxFont *f, uint32_t cp)
{
    int gi;

    if (!f->builtin) {
        gi = font_find(f, cp);
        if (gi >= 0) {
            return f->adv[gi];
        }
        {
            GfxFont *fb = font_fallback(g, f, cp);

            if (fb != NULL) {
                int gi2 = font_find(fb, cp);

                if (gi2 >= 0 && fb->size > 0) {
                    /* 跨字号取字：推进量按字号比例缩放，排版才对齐 */
                    double sc = (double)f->size / (double)fb->size;

                    return (int)((double)fb->adv[gi2] * sc + 0.5);
                }
            }
        }
    }
    if (f->builtin && cp >= 32 && cp <= 126) {
        return 8;
    }
    if (cp >= 32 && cp <= 126) {
        return 8; /* built-in ASCII fallback */
    }
    return f->size ? f->size : 16; /* tofu advance */
}

/* 目标图集 f 里没有 cp 时，去其它已注册图集里找（优先字号最接近的）。
 * 这是为了让"某套图集只收了少数几个字"不至于直接退化成豆腐块 —— 例如游戏里
 * 56px 的章节标题图集只有 5 个字形，而标题正文来自 34px 图集。
 * 返回 NULL 表示所有图集都没有这个字。 */
static GfxFont *font_fallback(Gfx *g, const GfxFont *want, uint32_t cp)
{
    GfxFont *best = NULL;
    int best_d = 0;
    int i;

    if (g == NULL || want == NULL) {
        return NULL;
    }
    for (i = 0; i < g->nfonts; ++i) {
        GfxFont *f = g->fonts[i];
        int d;

        if (f == NULL || f == want || f->builtin || f->count <= 0) {
            continue;
        }
        if (font_find(f, cp) < 0) {
            continue;
        }
        d = (f->size > want->size) ? (f->size - want->size)
                                   : (want->size - f->size);
        if (best == NULL || d < best_d) {
            best = f;
            best_d = d;
        }
    }
    return best;
}

static int text_width(Gfx *g, GfxFont *f, const char *s)
{
    const char *p = s;
    int w = 0;
    int cp;
    while ((cp = utf8_next(&p)) >= 0) {
        w += char_advance(g, f, (uint32_t)cp);
    }
    return w;
}

typedef struct GfxLine {
    char *s;
} GfxLine;

/* wrap_text() of render.py: per-character wrap, no word breaking */
static int text_wrap(Gfx *g, GfxFont *f, const char *text, int maxw,
                     GfxLine **out)
{
    GfxLine *lines = NULL;
    int n = 0, cap = 0;
    size_t buflen = strlen(text) + 8;
    char *cur = (char *)malloc(buflen);
    size_t curlen = 0;
    int curw = 0;
    const char *p = text;

    if (cur == NULL) {
        *out = NULL;
        return 0;
    }
    cur[0] = '\0';
    for (;;) {
        const char *start = p;
        int cp = utf8_next(&p);
        int adv;
        size_t add;

        if (cp < 0) {
            break;
        }
        if (cp == '\n') {
            goto flush;
        }
        adv = char_advance(g, f, (uint32_t)cp);
        if (curlen > 0 && curw + adv > maxw) {
            if (n + 1 >= cap) {
                int nc = cap ? cap * 2 : 8;
                GfxLine *nl = (GfxLine *)realloc(
                    lines, (size_t)nc * sizeof(GfxLine));
                if (nl == NULL) {
                    break;
                }
                lines = nl;
                cap = nc;
            }
            lines[n++].s = cur;
            cur = (char *)malloc(buflen);
            if (cur == NULL) {
                break;
            }
            cur[0] = '\0';
            curlen = 0;
            curw = 0;
        }
        add = (size_t)(p - start);
        if (curlen + add + 1 > buflen) {
            buflen = curlen + add + 8;
            {
                char *nc = (char *)realloc(cur, buflen);
                if (nc == NULL) {
                    break;
                }
                cur = nc;
            }
        }
        memcpy(cur + curlen, start, add);
        curlen += add;
        cur[curlen] = '\0';
        curw += adv;
        continue;
    flush:
        if (n + 1 >= cap) {
            int nc = cap ? cap * 2 : 8;
            GfxLine *nl =
                (GfxLine *)realloc(lines, (size_t)nc * sizeof(GfxLine));
            if (nl == NULL) {
                break;
            }
            lines = nl;
            cap = nc;
        }
        lines[n++].s = cur;
        cur = (char *)malloc(buflen);
        if (cur == NULL) {
            break;
        }
        cur[0] = '\0';
        curlen = 0;
        curw = 0;
    }
    /* final (possibly empty) line, like Python's wrap_text */
    {
        int nc = n + 1;
        GfxLine *nl = (GfxLine *)realloc(lines, (size_t)nc * sizeof(GfxLine));
        if (nl != NULL) {
            lines = nl;
            lines[n++].s = cur;
        } else {
            free(cur);
        }
    }
    *out = lines;
    return n;
}

/* PIL ImageDraw text ink: every channel (alpha included) is blended with the
 * glyph coverage, like Pillow's draw_bitmap BLEND macro.  This also
 * reproduces the reference PNG's semi-transparent stroke pixels: a fully
 * covered stroke pixel ends up as the ink RGBA (alpha included), because
 * ImageDraw writes the ink instead of alpha-compositing it. */
static void ink_px(Gfx *g, int x, int y, int r, int gr, int b, int a, int cov)
{
    uint8_t *d;
    int inv;

    if (cov <= 0 || x < 0 || y < 0 || x >= g->w || y >= g->h) {
        return;
    }
    d = g->canvas + ((size_t)y * (size_t)g->w + (size_t)x) * 4u;
    if (cov >= 255) {
        d[0] = (uint8_t)r;
        d[1] = (uint8_t)gr;
        d[2] = (uint8_t)b;
        d[3] = (uint8_t)a;
        return;
    }
    inv = 255 - cov;
    d[0] = (uint8_t)clampi(mouldiv255(r, cov) + mouldiv255(d[0], inv), 0, 255);
    d[1] = (uint8_t)clampi(mouldiv255(gr, cov) + mouldiv255(d[1], inv), 0, 255);
    d[2] = (uint8_t)clampi(mouldiv255(b, cov) + mouldiv255(d[2], inv), 0, 255);
    d[3] = (uint8_t)clampi(mouldiv255(a, cov) + mouldiv255(d[3], inv), 0, 255);
}

/* Paint one glyph cell with optional outline.  x/y is the cell's top-left in
 * canvas space (the caller subtracts f->margin from the pen position). */
static void draw_glyph_atlas(Gfx *g, GfxFont *f, int gi, int x, int y,
                             int r, int gr, int b, int ca, int stroke_w,
                             int sr, int sg, int sb, int sca)
{
    int gw = f->gw[gi];
    int ch = f->cell_h;
    const uint8_t *gm = font_glyph_px(f, gi, 0);
    int yy, xx;

    if (x + gw <= 0 || y + ch <= 0 || x >= g->w || y >= g->h) {
        return;
    }
    if (stroke_w > 0) {
        if (f->layers >= 3 && stroke_w <= 2) {
            /* exact FreeType stroke coverage baked into the atlas */
            const uint8_t *sm = font_glyph_px(f, gi, stroke_w);
            for (yy = 0; yy < ch; yy++) {
                for (xx = 0; xx < gw; xx++) {
                    int cov = sm[(size_t)yy * (size_t)gw + (size_t)xx];
                    if (cov != 0) {
                        ink_px(g, x + xx, y + yy, sr, sg, sb, sca, cov);
                    }
                }
            }
        } else {
            /* fallback for widths > 2 / v1 atlases: dilate the glyph */
            for (yy = 0; yy < ch; yy++) {
                for (xx = 0; xx < gw; xx++) {
                    int dy, dx, scov = 0;
                    for (dy = -stroke_w; dy <= stroke_w; dy++) {
                        int yy2 = yy + dy;
                        if (yy2 < 0 || yy2 >= ch) {
                            continue;
                        }
                        for (dx = -stroke_w; dx <= stroke_w; dx++) {
                            int xx2 = xx + dx;
                            int v;
                            if (xx2 < 0 || xx2 >= gw) {
                                continue;
                            }
                            v = gm[(size_t)yy2 * (size_t)gw + (size_t)xx2];
                            if (v > scov) {
                                scov = v;
                            }
                        }
                    }
                    if (scov != 0) {
                        ink_px(g, x + xx, y + yy, sr, sg, sb, sca, scov);
                    }
                }
            }
        }
    }
    for (yy = 0; yy < ch; yy++) {
        for (xx = 0; xx < gw; xx++) {
            int cov = gm[(size_t)yy * (size_t)gw + (size_t)xx];
            if (cov != 0) {
                ink_px(g, x + xx, y + yy, r, gr, b, ca, cov);
            }
        }
    }
}

static void draw_tofu(Gfx *g, int x, int y, int size, int r, int gr, int b,
                      int ca, int stroke_w, int sr, int sg, int sb, int sca)
{
    int yy, xx;
    int t = size / 16 + 1;

    for (yy = 0; yy < size; yy++) {
        for (xx = 0; xx < size; xx++) {
            int edge = (xx < t || yy < t || xx >= size - t || yy >= size - t);
            if (edge) {
                if (stroke_w > 0) {
                    ink_px(g, x + xx, y + yy, sr, sg, sb, sca, 255);
                }
                ink_px(g, x + xx, y + yy, r, gr, b, ca, 255);
            } else if (stroke_w > 0) {
                ink_px(g, x + xx, y + yy, sr, sg, sb, sca, 255);
            }
        }
    }
}

/* 把 src 图集里的字形按目标字号缩放画上去（最近邻采样）。
 * 用于 font_fallback()：目标图集缺这个字，但别的图集有。 */
static void draw_glyph_scaled(Gfx *g, GfxFont *f, const GfxFont *src, int gi,
                              int x, int y, int r, int gr, int b, int ca,
                              int stroke_w, int sr, int sg, int sb, int sca)
{
    int sw = src->gw[gi];
    int sh = src->cell_h;
    int dw;
    int dh;
    int dx;
    int dy;
    const uint8_t *body;
    const uint8_t *stroke = NULL;

    if (src->size <= 0 || sw <= 0 || sh <= 0 || f->size <= 0) {
        return;
    }
    dw = (int)((double)sw * (double)f->size / (double)src->size + 0.5);
    dh = (int)((double)sh * (double)f->size / (double)src->size + 0.5);
    if (dw <= 0 || dh <= 0) {
        return;
    }
    body = font_glyph_px(src, gi, 0);
    if (body == NULL) {
        return;
    }
    if (stroke_w > 0 && src->layers >= 3) {
        stroke = font_glyph_px(src, gi, stroke_w <= 2 ? stroke_w : 2);
    }
    for (dy = 0; dy < dh; ++dy) {
        int sy = (int)((long)dy * sh / dh);

        if (sy >= sh) {
            sy = sh - 1;
        }
        for (dx = 0; dx < dw; ++dx) {
            int sx = (int)((long)dx * sw / dw);
            size_t off = (size_t)sy * (size_t)sw + (size_t)sx;
            int cov;

            if (sx >= sw) {
                sx = sw - 1;
            }
            if (stroke != NULL) {
                cov = stroke[(size_t)sy * (size_t)sw + (size_t)sx];
                if (cov != 0) {
                    ink_px(g, x + dx, y + dy, sr, sg, sb, sca, cov);
                }
            }
            cov = body[off];
            if (cov != 0) {
                ink_px(g, x + dx, y + dy, r, gr, b, ca, cov);
            }
        }
    }
    g->stats.fallback_glyphs += 1;
}

/* one run of text at (x, y); y is the ascender top, like PIL's default
 * "la" anchor */
/* 把一份覆盖率掩码按颜色混进画布（缺字回退用）。 */
static void blit_mask(Gfx *g, const unsigned char *mask, int mw, int mh, int x,
                      int y, int r, int gr, int b, int alpha)
{
    int yy;

    for (yy = 0; yy < mh; ++yy) {
        int cy = y + yy;
        int xx;

        if (cy < 0 || cy >= g->h) {
            continue;
        }
        for (xx = 0; xx < mw; ++xx) {
            int cx = x + xx;
            int cov;
            uint32_t *dst;
            int a;

            if (cx < 0 || cx >= g->w) {
                continue;
            }
            cov = mask[yy * mw + xx];
            if (cov == 0) {
                continue;
            }
            a = (cov * alpha) / 255;
            if (a <= 0) {
                continue;
            }
            dst = (uint32_t *)g->canvas + (size_t)cy * (size_t)g->w + (size_t)cx;
            *dst = ((uint32_t)0xFFu << 24) | ((uint32_t)((r * a) / 255) << 16) |
                   ((uint32_t)((gr * a) / 255) << 8) |
                   (uint32_t)((b * a) / 255);
        }
    }
}

/* 单码点回退：返回推进宽度（0 = 回退不可用/失败）。 */
static int fallback_one_codepoint(Gfx *g, uint32_t cp, int x, int y, int px,
                                  int r, int gr, int b, int alpha)
{
    char buf[5];
    unsigned char *mask = NULL;
    int mw = 0;
    int mh = 0;
    int n = 0;
    int adv;

    if (g == NULL || g->glyph_fb == NULL || g->glyph_fb->mask_of == NULL) {
        return 0;
    }
    /* UTF-8 编码一个码点 */
    if (cp < 0x80u) {
        buf[n++] = (char)cp;
    } else if (cp < 0x800u) {
        buf[n++] = (char)(0xC0u | (cp >> 6));
        buf[n++] = (char)(0x80u | (cp & 0x3Fu));
    } else if (cp < 0x10000u) {
        buf[n++] = (char)(0xE0u | (cp >> 12));
        buf[n++] = (char)(0x80u | ((cp >> 6) & 0x3Fu));
        buf[n++] = (char)(0x80u | (cp & 0x3Fu));
    } else {
        buf[n++] = (char)(0xF0u | (cp >> 18));
        buf[n++] = (char)(0x80u | ((cp >> 12) & 0x3Fu));
        buf[n++] = (char)(0x80u | ((cp >> 6) & 0x3Fu));
        buf[n++] = (char)(0x80u | (cp & 0x3Fu));
    }
    buf[n] = '\0';
    if (!g->glyph_fb->mask_of(buf, px, &mask, &mw, &mh) || mask == NULL) {
        free(mask);
        return 0;
    }
    if (mw > 0 && mh > 0) {
        blit_mask(g, mask, mw, mh, x, y, r, gr, b, alpha);
    }
    free(mask);
    g->stats.glyph_fb_runs += 1;
    adv = mw;
    if (g->glyph_fb->advance_of != NULL) {
        int a2 = g->glyph_fb->advance_of(buf, px);

        if (a2 > 0) {
            adv = a2;
        }
    }
    return adv > 0 ? adv : 8;
}

static void draw_text_run(Gfx *g, GfxFont *f, int x, int y, const char *s,
                          int r, int gr, int b, int ca, int stroke_w, int sr,
                          int sg, int sb, int sca)
{
    const char *p = s;
    int pen = x;
    int cp;

    while ((cp = utf8_next(&p)) >= 0) {
        /* 外字优先：这个码点登记过 GaijiLoad 就直接画图（按当前字号缩放），
         * 并且按缩放后的宽度推进笔位，排版仍然对。 */
        if (g->ngaiji > 0 && g->gaiji_on) {
            const GfxGaiji *gj = gfx_gaiji_find(g, (uint32_t)cp);

            if (gj != NULL) {
                const GfxTex *t = g->slot[gj->slot].tex;

                if (t != NULL && t->img.rgba != NULL && t->img.w > 0 &&
                    t->img.h > 0) {
                    int dh = f->size;
                    int dw = (int)((long)t->img.w * (long)dh /
                                   (long)t->img.h);

                    if (dw < 1) {
                        dw = 1;
                    }
                    blit_rgba_scaled(g, pen, y, dw, dh, t->img.rgba,
                                     t->img.w, t->img.h);
                    pen += dw;
                    continue;
                }
            }
        }
        if (!f->builtin) {
            int gi = font_find(f, (uint32_t)cp);

            if (gi >= 0) {
                draw_glyph_atlas(g, f, gi, pen - f->margin, y - f->margin, r,
                                 gr, b, ca, stroke_w, sr, sg, sb, sca);
                pen += f->adv[gi];
                continue;
            }
            /* 本图集没有这个字：用别的图集缩放补上（比豆腐块好） */
            {
                GfxFont *fb = font_fallback(g, f, (uint32_t)cp);

                if (fb != NULL) {
                    int gi2 = font_find(fb, (uint32_t)cp);

                    if (gi2 >= 0) {
                        int m = (int)((double)fb->margin * (double)f->size /
                                          (double)fb->size +
                                      0.5);

                        draw_glyph_scaled(g, f, fb, gi2, pen - m, y - m, r, gr,
                                          b, ca, stroke_w, sr, sg, sb, sca);
                        pen += char_advance(g, f, (uint32_t)cp);
                        continue;
                    }
                }
            }
        }
        if (cp >= 32 && cp <= 126) {
            int gi = cp - 32;
            int yy, xx;
            const uint8_t *bits = g_font8x16[gi];
            for (yy = 0; yy < 16; yy++) {
                int row = bits[yy];
                for (xx = 0; xx < 8; xx++) {
                    if (row & (1 << (7 - xx))) {
                        if (stroke_w > 0) {
                            int dy, dx;
                            for (dy = -1; dy <= 1; dy++) {
                                for (dx = -1; dx <= 1; dx++) {
                                    ink_px(g, pen + xx + dx, y + yy + dy, sr,
                                           sg, sb, sca, 255);
                                }
                            }
                        }
                        ink_px(g, pen + xx, y + yy, r, gr, b, ca, 255);
                    }
                }
            }
            pen += 8;
        } else {
            int k;
            int seen = 0;

            if (getenv("FVP_DEBUG_TOFU") != NULL) {
                fprintf(stderr,
                        "tofu: cp=U+%04X font=%p builtin=%d size=%d count=%d "
                        "nfonts=%d\n",
                        (unsigned)cp, (void *)f, f->builtin, f->size, f->count,
                        g->nfonts);
            }
            /* 先试平台回退（汉化文本的汉字等），成功就不算豆腐块 */
            if (g->glyph_fb != NULL) {
                int adv = fallback_one_codepoint(g, (uint32_t)cp, pen, y,
                                                 f->size, r, gr, b, ca);

                if (adv > 0) {
                    pen += adv;
                    continue;
                }
            }
            g->stats.tofu_glyphs += 1;
            /* 记下前 8 个不同的缺字码点（诊断用：兼容性排查要能指名道姓）。 */
            for (k = 0; k < g->stats.ntofu_cp; ++k) {
                if (g->stats.tofu_cp[k] == (uint32_t)cp) {
                    seen = 1;
                    break;
                }
            }
            if (!seen && g->stats.ntofu_cp < 8) {
                g->stats.tofu_cp[g->stats.ntofu_cp++] = (uint32_t)cp;
            }
            draw_tofu(g, pen, y, f->size ? f->size : 16, r, gr, b, ca,
                      stroke_w, sr, sg, sb, sca);
            pen += f->size ? f->size : 16;
        }
    }
}

/* rounded-rectangle hit test in PIL ImageDraw coordinates: the box is
 * inclusive (x0..x1 / y0..y1) and the corner circles are centred at
 * (x0+r, y0+r) etc., radius r.  Pixel centres are used for the distance. */
static int rr_hit(int x, int y, int x0, int y0, int x1, int y1, int r)
{
    double px = (double)x + 0.5;
    double py = (double)y + 0.5;
    double rr = (double)(r > 0 ? r : 0);

    if (x < x0 || x > x1 || y < y0 || y > y1) {
        return 0;
    }
    if (x < x0 + r && y < y0 + r) {
        double dx = px - (double)(x0 + r);
        double dy = py - (double)(y0 + r);
        return dx * dx + dy * dy <= rr * rr;
    }
    if (x > x1 - r && y < y0 + r) {
        double dx = px - (double)(x1 - r);
        double dy = py - (double)(y0 + r);
        return dx * dx + dy * dy <= rr * rr;
    }
    if (x < x0 + r && y > y1 - r) {
        double dx = px - (double)(x0 + r);
        double dy = py - (double)(y1 - r);
        return dx * dx + dy * dy <= rr * rr;
    }
    if (x > x1 - r && y > y1 - r) {
        double dx = px - (double)(x1 - r);
        double dy = py - (double)(y1 - r);
        return dx * dx + dy * dy <= rr * rr;
    }
    return 1;
}

/* fill + outline of one rounded rectangle, written into a transparent
 * overlay first and alpha-composited once -- matching ImageDraw semantics
 * (the outline replaces the fill on the boundary, it is not composited on
 * top of it) */
static void paint_round_box(Gfx *g, int x0, int y0, int x1, int y1, int r,
                            int fr, int fg, int fb, int fa, int has_ol,
                            int orr, int og, int ob, int oa, int width)
{
    int x, y;

    for (y = y0; y <= y1; y++) {
        for (x = x0; x <= x1; x++) {
            if (!rr_hit(x, y, x0, y0, x1, y1, r)) {
                continue;
            }
            if (has_ol && width > 0 &&
                !rr_hit(x, y, x0 + width, y0 + width, x1 - width,
                        y1 - width, r - width)) {
                put_px(g, x, y, orr, og, ob, oa);
            } else {
                put_px(g, x, y, fr, fg, fb, fa);
            }
        }
    }
}

static void draw_dialog_box(Gfx *g)
{
    /* render.py _draw_dialog_box: box (60,860)-(1860,1060) radius 18,
     * shadow at (+4,+6) black alpha 90, fill (238,242,250,232), outline
     * (120,160,215,220) width 3.  Both shapes live in ONE transparent
     * overlay that is composited once, so the box is not darkened by the
     * shadow underneath it. */
    const int x0 = 60, y0 = 860, x1 = 1860, y1 = 1060, r = 18;
    const int sx0 = x0 + 4, sy0 = y0 + 6, sx1 = x1 + 4, sy1 = y1 + 6;
    int x, y;

    for (y = y0; y <= sy1; y++) {
        for (x = x0; x <= sx1; x++) {
            int cr = 0, cg = 0, cb = 0, ca = 0;
            if (rr_hit(x, y, sx0, sy0, sx1, sy1, r)) {
                cr = 0;
                cg = 0;
                cb = 0;
                ca = 90;
            }
            if (rr_hit(x, y, x0, y0, x1, y1, r)) {
                if (!rr_hit(x, y, x0 + 3, y0 + 3, x1 - 3, y1 - 3, r - 3)) {
                    cr = 120;
                    cg = 160;
                    cb = 215;
                    ca = 220;
                } else {
                    cr = 238;
                    cg = 242;
                    cb = 250;
                    ca = 232;
                }
            }
            if (ca > 0) {
                put_px(g, x, y, cr, cg, cb, ca);
            }
        }
    }
}

static void draw_name_plate(Gfx *g, int x, int y, int tw, int size)
{
    /* render.py: (x-26, y-10, x+tw+26, y+size+14) radius 12 */
    paint_round_box(g, x - 26, y - 10, x + tw + 26, y + size + 14, 12, 238,
                    242, 250, 232, 1, 120, 160, 215, 220, 2);
}
static int utf8_count_chars(const char *s);
static char *utf8_truncate(const char *s, int n);

static int text_visible(const GfxTextWin *tw)
{
    return tw->used && tw->nlines > 0;
}

static void draw_text_window(Gfx *g, int idx, const int *origin,
                             int alpha)
{
    GfxTextWin *tw = &g->tw[idx];
    GfxTextLayout lay;
    GfxFont *f;
    GfxLine *lines = NULL;
    char **raw = NULL;
    char *clip = NULL;   /* 逐字显示时截断后的最后一行 */
    int nraw = 0, nlines = 0;
    int size, i, x, y;
    int nkeep;
    int budget = 0;      /* 还剩几个可见字符（只在新行里扣） */
    int clipped = 0;

    if (!text_visible(tw) || g->tw_hidden[idx]) {
        return;
    }
    /* 逐字显示：只把前 shown_chars 个字符交给排版。关闭时 budget 无限。 */
    if (g->typewriter_on && tw->shown_chars < tw->total_chars) {
        budget = tw->shown_chars;
        clipped = 1;
        if (budget <= 0) {
            return; /* 还没开始显示 */
        }
    }
    lay = tw->lay_custom ? tw->lay : gfx_layout_default(idx);
    size = lay.size;
    if (g->text_fixed_layout) {
        /* 旧路径（冻结像素基线）：保持历史行为，只对 0 号窗口按 TextSize 的
         * 字模格高选字号（18..40，没设过就用 34）。 */
        if (idx == 0) {
            int sh = tw->size[1];
            size = clampi(sh > 8 ? sh : 34, 18, 40);
        }
    } else {
        /* 图元驱动模式（原版）：
         *   - TextSize(126) 写的是字模格高（tw+0x594，合法 8..32），它就是字号；
         *   - TextBuff(110) 给出离屏面板尺寸，面板宽度就是换行宽度、面板高度
         *     能放几行就是行数上限（原版把文字画进面板再贴图元；我们直接画，
         *     所以用这两个数当排版约束）。没设过的窗口仍用版面表。 */
        if (tw->size_h_set && tw->size[1] >= 8 && tw->size[1] <= 64) {
            size = clampi(tw->size[1], 12, 64);
        }
        if (tw->panel_set) {
            int lh;
            int nl;

            if (tw->panel_w > 0) {
                lay.max_w = tw->panel_w;
            }
            lh = size + 6;
            nl = (lh > 0) ? (tw->panel_h / lh) : 0;
            if (nl < 1) {
                nl = 1;
            }
            if (nl > GFX_TEXT_LINES) {
                nl = GFX_TEXT_LINES;
            }
            lay.max_lines = nl;
        }
    }
    f = pick_font(g, size);
    if (getenv("FVP_DEBUG_TOFU") != NULL && f != NULL && f->builtin) {
        fprintf(stderr,
                "builtin-font: layer=%d size=%d nlines=%d text=\"%s\"\n", idx,
                size, tw->nlines, tw->nlines > 0 ? tw->lines[0] : "");
    }

    raw = tw->lines;
    nraw = tw->nlines;
    for (i = 0; i < nraw; i++) {
        GfxLine *wl = NULL;
        int wn = text_wrap(g, f, raw[i], lay.max_w, &wl);
        int j;
        GfxLine *nl;
        if (wn <= 0 || wl == NULL) {
            free(wl);
            continue;
        }
        nl = (GfxLine *)realloc(lines, (size_t)(nlines + wn) * sizeof(GfxLine));
        if (nl == NULL) {
            free(wl);
            continue;
        }
        lines = nl;
        for (j = 0; j < wn; j++) {
            lines[nlines++] = wl[j];
        }
        free(wl);
    }
    if (clipped) {
        /* 按字符预算裁掉尾部：只保留前 budget 个码点，最后一行可能被切开。 */
        int used = 0;

        for (i = 0; i < nlines; i++) {
            int n = utf8_count_chars(lines[i].s);

            if (used + n <= budget) {
                used += n;
                continue;
            }
            if (used >= budget) {
                free(lines[i].s);
                lines[i].s = NULL;
                continue;
            }
            clip = utf8_truncate(lines[i].s, budget - used);
            free(lines[i].s);
            lines[i].s = clip;
            clip = NULL;
            used = budget;
        }
        /* 裁空的行从中间去掉 */
        {
            int w = 0;

            for (i = 0; i < nlines; i++) {
                if (lines[i].s != NULL) {
                    lines[w++] = lines[i];
                }
            }
            nlines = w;
        }
    }
    if (nlines == 0) {
        return;
    }
    nkeep = nlines < lay.max_lines ? nlines : lay.max_lines;
    for (i = nkeep; i < nlines; i++) {
        free(lines[i].s);
    }
    nlines = nkeep;
    x = lay.x;
    y = lay.y;
    if (tw->has_pos) {
        x = tw->pos[0];
        y = tw->pos[1];
    }
    /* 文本图元（PrimSetText 86）给出的绝对原点优先：原版把文本窗口挂在图元上，
     * 位置由图元的 x/y（含父链）决定，所以 `--text-prims` 打开时用这个原点。 */
    if (origin != NULL) {
        x = origin[0];
        y = origin[1];
    }
    /* TextFormat(118) 的 arg4（写 tw+0x5AC，范围 0..64）是**左缩进**：
     * 原版排版起点 `pen_x = tw+0x5AC`（0x4337A9 读 0x5AC → 0x4337BE 写 0x5E8），
     * 换行后恢复行首保存值（0x4330A8），所以每一行都从这条缩进起排。
     * 放在原点判定之后：缩进是"从面板/图元原点再往里推"，两者叠加。
     * 见 ref/exe-text-layout.json 与 tests/verify_exe_text_layout.py。 */
    if (tw->fmt[3] > 0) {
        x += tw->fmt[3];
    }
    if (idx == 9 && origin == NULL) {
        /* 旧路径的“章节标题居中 + 白色描边”是版面表的一部分（合成版式）。
         * 图元驱动时位置由图元决定，不再套这层版式；标题样式待后续从 exe 逆。 */
        for (i = 0; i < nlines; i++) {
            int wpx = text_width(g, f, lines[i].s);
            int xc = (g->w - wpx) / 2;
            int sca = (200 * alpha) / 255;

            draw_text_run(g, f, xc, y, lines[i].s, 255, 255, 255, alpha, 2, 0,
                          0, 0, sca);
            y += size + 8;
        }
    } else {
        if (idx == 8 && origin == NULL) {
            /* 名牌同样是旧路径的合成版式；图元驱动时名牌应来自脚本图元。 */
            int wpx = text_width(g, f, lines[0].s);
            draw_name_plate(g, x, y, wpx, size);
        }
        for (i = 0; i < nlines; i++) {
            int cr = tw->color[0], cg = tw->color[1], cb = tw->color[2];
            int sca = (120 * alpha) / 255;

            if (cr + cg + cb > 600) {
                cr = 32;
                cg = 40;
                cb = 60;
            }
            /* 图元 alpha（`--text-prims` 的反向结果）：本引擎的字形覆盖是
             * "一次性写屏"，`ink_px()` 在 cov == 255 时直接写 RGB、不参与 DST 混合，
             * 所以这里把 alpha 折进颜色通道（按比例压暗）—— 视觉上就是淡出，
             * 但**不是**物理正确的 src-over 混合。差别只在 alpha < 255 时出现，
             * 冻结基线（alpha 恒 255）一个像素都不变。见 docs/render-model.md §1.3。 */
            if (alpha < 255) {
                cr = (cr * alpha) / 255;
                cg = (cg * alpha) / 255;
                cb = (cb * alpha) / 255;
            }
            /* TextShadowDist(125) 只记录偏移（gfx_text_set_shadow），**这里不画阴影层**。
             * 理由见 docs/render-model.md §7.1：脚本给的是 (12,0)，画出来是一份向右 12 px
             * 的实心深色重影（用户实测报「残影」）；而原版阴影的颜色/透明度/合成方式
             * 都没有静态分析证据，参考实现（work/render.py）也是只记录不渲染。因此宁可
             * 不画 —— 这是唯一能与像素基线保持一致的选择。 */
            draw_text_run(g, f, x, y, lines[i].s, cr, cg, cb, alpha, 1, 255,
                          255, 255, sca);
            y += size + 6;
        }
    }
    for (i = 0; i < nlines; i++) {
        free(lines[i].s);
    }
    free(lines);
    g->stats.texts++;
}

/* 文本图元（type 5）的绝对坐标：沿父链累加 x/y。父链断了就用已累加的值。 */
static void prim_abs_xy(const Gfx *g, int prim_id, int *ox, int *oy)
{
    int x = 0;
    int y = 0;
    int p = prim_id;
    int guard = 0;

    while (p > 0 && p < GFX_PRIM_MAX && guard++ < GFX_PRIM_MAX) {
        x += g->prim[p].x;
        y += g->prim[p].y;
        p = g->prim[p].parent;
    }
    if (ox != NULL) {
        *ox = x;
    }
    if (oy != NULL) {
        *oy = y;
    }
}

static void draw_text_all(Gfx *g)
{
    int i;

    if (!g->text_fixed_layout) {
        /* 默认（原版）。
         *
         * 已绑定的窗口在图元树里画过了；这里只补画**未绑定**的窗口：
         *   - 8（人名）/9（章节标题）是覆盖层窗口：原版只在脚本给它们建了
         *     PrimSetText(86) 文本图元时才上屏。未绑定就不画 —— 否则就会凭空
         *     出现一个实机看不到的「标题」/名牌（2026-11-26 用户实机反馈）。
         *   - **所有**未绑定窗口都不补画：2026-11-26 用户要求（正文渲染要求 #3）
         *     "文本按图元绝对坐标绘制；固定版面表只保留给冻结像素基线的旧路径，
         *     默认路径不得使用"。所以默认路径下没有 PrimSetText(86) 图元的窗口
         *     不上屏，版面表只在 --text-fixed-layout 里用。
         * 合成对话框/名牌不画：框与名牌是脚本自己的图元。
         */
        (void)i;
        if (getenv("FVP_DEBUG_TEXT") != NULL) {
            for (i = 1; i < GFX_PRIM_MAX; i++) {
                const GfxPrim *p = &g->prim[i];
                int ax = 0;
                int ay = 0;

                if (p->type != 5 || p->tw < 0 || p->tw >= GFX_TEXT_MAX) {
                    continue;
                }
                prim_abs_xy(g, i, &ax, &ay);
                fprintf(stderr,
                        "text prim %d: tw=%d draw=%d alpha=%d visible=%d "
                        "lines=%d parent=%d xy=%d,%d abs=%d,%d wh=%u,%u "
                        "cell=%dx%d panel=%dx%d\n",
                        i, p->tw, p->draw, p->alpha,
                        text_visible(&g->tw[p->tw]) ? 1 : 0,
                        g->tw[p->tw].nlines, p->parent, p->x, p->y, ax, ay,
                        (unsigned)p->w, (unsigned)p->h,
                        g->tw[p->tw].size[0], g->tw[p->tw].size[1],
                        g->tw[p->tw].panel_w, g->tw[p->tw].panel_h);
                {
                    int q = p->parent;
                    int guard = 0;

                    while (q > 0 && q < GFX_PRIM_MAX && guard++ < 64) {
                        const GfxPrim *pp = &g->prim[q];

                        fprintf(stderr,
                                "   chain %d: type=%d xy=%d,%d parent=%d "
                                "alpha=%d scale=%u,%u angle=%u\n",
                                q, pp->type, pp->x, pp->y, pp->parent,
                                pp->alpha, (unsigned)pp->scale_x,
                                (unsigned)pp->scale_y, (unsigned)pp->angle);
                        q = pp->parent;
                    }
                }
            }
            for (i = 0; i < GFX_TEXT_MAX; i++) {
                if (text_visible(&g->tw[i])) {
                    fprintf(stderr, "text window %d: lines=%d (unbound?)\n", i,
                            g->tw[i].nlines);
                }
            }
        }
        return;
    }
    /* 旧路径（--text-fixed-layout）：合成对话框 + 全部窗口按版面表各画一遍。
     * 冻结像素基线（verify_gfx 的 293 px 等）依赖这条路径，故原样保留。 */
    if (text_visible(&g->tw[0]) || text_visible(&g->tw[8])) {
        draw_dialog_box(g);
    }
    for (i = 0; i < GFX_TEXT_MAX; i++) {
        draw_text_window(g, i, NULL, 255);
    }
}

/* ----------------------------------------------------------------------- */
/* public rendering API                                                    */
/* ----------------------------------------------------------------------- */

void gfx_set_background(Gfx *g, int r, int gr, int b, int a)
{
    if (g == NULL) {
        return;
    }
    g->bg = (uint32_t)(r & 255) | ((uint32_t)(gr & 255) << 8) |
            ((uint32_t)(b & 255) << 16) | ((uint32_t)(a & 255) << 24);
}

void gfx_render(Gfx *g)
{
    unsigned char *seen;
    int i;
    GfxXf xf;

    if (g == NULL) {
        return;
    }
    clear_canvas(g);
    g->ndraw = 0;
    g->stats.sprites = 0;
    g->stats.texts = 0;
    g->stats.compo_builds = 0;
    g->stats.pixels = 0;
    g->stats.resize_cache_hits = 0;
    g->stats.fallback_glyphs = 0;
    g->stats.tofu_glyphs = 0;
    g->stats.text_prims = 0;
    memset(g->text_bound, 0, sizeof g->text_bound);
    xf_identity(&xf);
    walk_tree(g, 0, 0, &xf);
    seen = (unsigned char *)calloc(GFX_PRIM_MAX, 1);
    if (seen != NULL) {
        mark_tree(g, 0, seen);
        for (i = 1; i < GFX_PRIM_MAX; i++) {
            GfxPrim *p = &g->prim[i];
            if (seen[i] || p->type != 4 || !p->draw || p->alpha == 0) {
                continue;
            }
            render_sprite(g, p, &xf);
        }
        free(seen);
    }
    draw_text_all(g);
}

int gfx_draw_count(const Gfx *g)
{
    return g != NULL ? g->ndraw : 0;
}

const GfxDrawItem *gfx_draw_item(const Gfx *g, int index)
{
    if (g == NULL || index < 0 || index >= g->ndraw) {
        return NULL;
    }
    return &g->draw[index];
}

const GfxStats *gfx_stats(const Gfx *g)
{
    static const GfxStats zero;
    return g != NULL ? &g->stats : &zero;
}

int gfx_width(const Gfx *g)
{
    return g != NULL ? g->w : 0;
}

int gfx_height(const Gfx *g)
{
    return g != NULL ? g->h : 0;
}

const uint32_t *gfx_canvas(Gfx *g, int *w, int *h)
{
    if (g == NULL) {
        return NULL;
    }
    if (w != NULL) {
        *w = g->w;
    }
    if (h != NULL) {
        *h = g->h;
    }
    return (const uint32_t *)g->canvas;
}

const uint8_t *gfx_canvas_rgba(Gfx *g, int *w, int *h)
{
    if (g == NULL) {
        return NULL;
    }
    if (w != NULL) {
        *w = g->w;
    }
    if (h != NULL) {
        *h = g->h;
    }
    return g->canvas;
}

void gfx_destroy(Gfx *g)
{
    int i, b;

    if (g == NULL) {
        return;
    }
    free(g->zoom_img);
    g->zoom_img = NULL;
    for (i = 0; i < GFX_PARTS_MAX; i++) {
        tex_unref(g, g->part[i].tex);
    }
    fvp_motion_free(&g->motion);
    for (i = 0; i < GFX_GRAPH_MAX; i++) {
        slot_release(g, i);
    }
    for (b = 0; b < GFX_TEX_BUCKETS; b++) {
        while (g->tex_hash[b] != NULL) {
            GfxTex *t = g->tex_hash[b];
            g->tex_hash[b] = t->next;
            if (g->host.free_image != NULL) {
                g->host.free_image(g->host.ud, &t->img);
            } else {
                free(t->img.rgba);
            }
            free(t->name);
            free(t);
        }
    }
    if (g->prim != NULL) {
        for (i = 0; i < GFX_PRIM_MAX; i++) {
            free(g->prim[i].children);
            free(g->prim[i].tc_img);
        }
    }
    for (i = 0; i < GFX_TEXT_MAX; i++) {
        int j;
        for (j = 0; j < g->tw[i].nlines; j++) {
            free(g->tw[i].lines[j]);
        }
    }
    free(g->draw);
    free(g->canvas);
    free(g->prim);
    free(g->slot);
    free(g->part);
    free(g->tw);
    free(g);
}

/* ---- primitive setters ----------------------------------------------- */

int gfx_prim_reset(Gfx *g, int id)
{
    GfxPrim *p = prim_at(g, id);
    int parent;
    int *children;
    int nchildren;
    int cap_children;

    if (p == NULL) {
        return -1;
    }
    parent = p->parent;
    children = p->children;
    nchildren = p->nchildren;
    cap_children = p->cap_children;
    free(p->tc_img);
    p->tc_img = NULL;
    p->tc_ok = 0;
    prim_init(p, id);
    p->parent = parent;
    p->children = children;
    p->nchildren = nchildren;
    p->cap_children = cap_children;
    return 0;
}

int gfx_set_prim(Gfx *g, int id, int field, int v)
{
    GfxPrim *p = prim_at(g, id);

    if (p == NULL || field < 0 || field >= GFX_F_COUNT) {
        return -1;
    }
    switch (field) {
    case GFX_F_TYPE:
        p->type = (unsigned char)(v & 0xFF);
        break;
    case GFX_F_DRAW:
        p->draw = v ? 1 : 0;
        break;
    case GFX_F_ALPHA:
        p->alpha = (unsigned char)clampi(v, 0, 255);
        break;
    case GFX_F_BLEND:
        p->blend = v ? 1 : 0;
        break;
    case GFX_F_Z:
        p->z = (unsigned short)clampi(v, 100, 10000);
        break;
    case GFX_F_X:
        p->x = (int)(int16_t)(v & 0xFFFF);
        break;
    case GFX_F_Y:
        p->y = (int)(int16_t)(v & 0xFFFF);
        break;
    case GFX_F_W:
        p->w = (unsigned short)(v & 0xFFFF);
        p->wh_set = 1; /* 原版 PrimSetWH 会 orb 上 prim+0x06 的 bit0 */
        break;
    case GFX_F_H:
        p->h = (unsigned short)(v & 0xFFFF);
        p->wh_set = 1;
        break;
    case GFX_F_U:
        p->u = (unsigned short)(v & 0xFFFF);
        break;
    case GFX_F_V:
        p->v = (unsigned short)(v & 0xFFFF);
        break;
    case GFX_F_OP1:
        p->op1 = (unsigned short)(v & 0xFFFF);
        break;
    case GFX_F_OP2:
        p->op2 = (unsigned short)(v & 0xFFFF);
        break;
    case GFX_F_ANGLE:
        p->angle = (unsigned short)py_mod(v, 3600);
        break;
    case GFX_F_SCALE_X:
        p->scale_x = (unsigned short)clampi(v, 100, 10000);
        break;
    case GFX_F_SCALE_Y:
        p->scale_y = (unsigned short)clampi(v, 100, 10000);
        break;
    case GFX_F_GRAPH:
        p->graph = clampi(v, -1, GFX_GRAPH_MAX - 1);
        break;
    case GFX_F_TW:
        p->tw = clampi(v, -1, GFX_TEXT_MAX - 1);
        break;
    case GFX_F_TILE:
        p->tile = (unsigned short)(v & 0xFFFF);
        break;
    case GFX_F_PARENT:
        if (v < 0) {
            return gfx_prim_group_out(g, id);
        }
        return gfx_prim_group_in(g, id, v);
    case GFX_F_FLAGS:
        p->flags = (unsigned short)(v & 0xFFFF);
        break;
    default:
        return -1;
    }
    return 0;
}

int gfx_get_prim(const Gfx *g, int id, int field, int *out)
{
    const GfxPrim *p;

    if (g == NULL || out == NULL || id < 0 || id >= GFX_PRIM_MAX ||
        field < 0 || field >= GFX_F_COUNT) {
        return -1;
    }
    p = &g->prim[id];
    switch (field) {
    case GFX_F_TYPE: *out = p->type; break;
    case GFX_F_DRAW: *out = p->draw; break;
    case GFX_F_ALPHA: *out = p->alpha; break;
    case GFX_F_BLEND: *out = p->blend; break;
    case GFX_F_Z: *out = p->z; break;
    case GFX_F_X: *out = p->x; break;
    case GFX_F_Y: *out = p->y; break;
    case GFX_F_W: *out = p->w; break;
    case GFX_F_H: *out = p->h; break;
    case GFX_F_U: *out = p->u; break;
    case GFX_F_V: *out = p->v; break;
    case GFX_F_OP1: *out = p->op1; break;
    case GFX_F_OP2: *out = p->op2; break;
    case GFX_F_ANGLE: *out = p->angle; break;
    case GFX_F_SCALE_X: *out = p->scale_x; break;
    case GFX_F_SCALE_Y: *out = p->scale_y; break;
    case GFX_F_GRAPH: *out = p->graph; break;
    case GFX_F_TW: *out = p->tw; break;
    case GFX_F_TILE: *out = p->tile; break;
    case GFX_F_FLAGS: *out = p->flags; break;
    default: return -1;
    }
    return 0;
}

/* ---- 动画补间（gfx 侧）------------------------------------------------ */

int gfx_motion_cancel(Gfx *g, int prim_id, int kind)
{
    if (g == NULL) {
        return 0;
    }
    return fvp_motion_cancel(&g->motion, prim_id, kind);
}

int gfx_motion_add(Gfx *g, int prim_id, int kind, int from_x, int from_y,
                   int to_x, int to_y, int dur_ms, int mode)
{
    if (g == NULL || prim_id < 0 || prim_id >= GFX_PRIM_MAX) {
        return 0;
    }
    if (dur_ms < 0) {
        dur_ms = 0;
    }
    {
        int r = fvp_motion_add(&g->motion, prim_id, kind, (double)from_x,
                               (double)from_y, (double)to_x, (double)to_y,
                               (double)dur_ms, mode, 1);

        if (getenv("FVP_DEBUG_MOTION") != NULL) {
            fprintf(stderr,
                    "motion_add prim=%d kind=%d from=%d,%d to=%d,%d "
                    "dur=%d mode=%d -> %d (now_ms=%.0f)\n",
                    prim_id, kind, from_x, from_y, to_x, to_y, dur_ms, mode, r,
                    g->motion.now_ms);
        }
        return r;
    }
}

void gfx_motion_set_enabled(Gfx *g, int on)
{
    if (g != NULL) {
        g->motion_on = on ? 1 : 0;
    }
}

int gfx_motion_enabled(const Gfx *g)
{
    return (g != NULL) ? g->motion_on : 0;
}

void gfx_motion_advance(Gfx *g, int dt_ms)
{
    size_t i;

    if (g == NULL) {
        return;
    }
    fvp_motion_advance(&g->motion, (double)(dt_ms > 0 ? dt_ms : 0));
    if (!g->motion_on) {
        return;
    }
    /* 把每条活跃补间的当前值写回图元字段（未启用时只走时间轴，不动画面）。 */
    for (i = 0; i < g->motion.n; ++i) {
        const FvpTween *t = &g->motion.e[i];
        double xv = 0.0;
        double yv = 0.0;

        if (!fvp_motion_value(&g->motion, t->id, &xv, &yv)) {
            continue;
        }
        switch (t->kind) {
        case FVP_TWEEN_MOVE_XY:
            gfx_set_prim(g, t->prim_id, GFX_F_X, (int)(xv + 0.5));
            gfx_set_prim(g, t->prim_id, GFX_F_Y, (int)(yv + 0.5));
            break;
        case FVP_TWEEN_ALPHA:
            gfx_set_prim(g, t->prim_id, GFX_F_ALPHA, (int)(xv + 0.5));
            break;
        case FVP_TWEEN_SCALE:
            gfx_set_prim(g, t->prim_id, GFX_F_SCALE_X, (int)(xv + 0.5));
            gfx_set_prim(g, t->prim_id, GFX_F_SCALE_Y, (int)(yv + 0.5));
            break;
        default:
            break;
        }
    }
    /* 诊断：FVP_MOTION_NO_REAP=1 时不回收已完成的补间。
     * 原版的 `*Test` 只查“容器里有没有这条记录”（0x422990，
     * 不看进度）；我们每帧 reap 会让“已跑完但还没 Stop”的记录消失，
     * 这一差异是待验证项。默认开❤旧行为不变❤。 */
    if (getenv("FVP_MOTION_NO_REAP") == NULL) {
        fvp_motion_reap(&g->motion);
    }
}

int gfx_motion_test(Gfx *g, int prim_id, int kind)
{
    int r;

    if (g == NULL) {
        return 0;
    }
    r = fvp_motion_test(&g->motion, prim_id, kind);
    if (getenv("FVP_DEBUG_MOTION") != NULL) {
        fprintf(stderr, "motion_test prim=%d kind=%d -> %d (table=%u now_ms=%.0f)\n",
                prim_id, kind, r, (unsigned)g->motion.n, g->motion.now_ms);
    }
    return r;
}

int gfx_motion_active(const Gfx *g)
{
    return (g != NULL) ? (int)fvp_motion_active_count(&g->motion) : 0;
}

void gfx_motion_clear(Gfx *g)
{
    if (g != NULL) {
        fvp_motion_clear(&g->motion);
    }
}

int gfx_prim_set_clip(Gfx *g, int id, int x, int y, int w, int h)
{
    GfxPrim *p = prim_at(g, id);
    if (p == NULL) {
        return -1;
    }
    p->clip_x = x;
    p->clip_y = y;
    p->clip_w = w;
    p->clip_h = h;
    return 0;
}

static void prim_detach(Gfx *g, GfxPrim *p)
{
    GfxPrim *par;
    int i;

    if (p->parent < 0 || p->parent >= GFX_PRIM_MAX) {
        p->parent = -1;
        return;
    }
    par = &g->prim[p->parent];
    for (i = 0; i < par->nchildren; i++) {
        if (par->children[i] == p->id) {
            int j;
            for (j = i; j + 1 < par->nchildren; j++) {
                par->children[j] = par->children[j + 1];
            }
            par->nchildren--;
            break;
        }
    }
    p->parent = -1;
}

static int prim_append_child(Gfx *g, GfxPrim *par, int child)
{
    if (par->nchildren >= GFX_MAX_CHILDREN) {
        gfx_err(g, "prim child list overflow");
        return -1;
    }
    if (par->nchildren >= par->cap_children) {
        int cap = par->cap_children ? par->cap_children * 2 : 8;
        int *nc;
        if (cap > GFX_MAX_CHILDREN) {
            cap = GFX_MAX_CHILDREN;
        }
        nc = (int *)realloc(par->children, (size_t)cap * sizeof(int));
        if (nc == NULL) {
            return -1;
        }
        par->children = nc;
        par->cap_children = cap;
    }
    par->children[par->nchildren++] = child;
    return 0;
}

int gfx_prim_group_in(Gfx *g, int child, int parent)
{
    GfxPrim *c;
    GfxPrim *par;

    if (g == NULL || child < 0 || child >= GFX_PRIM_MAX || parent < 0 ||
        parent >= GFX_PRIM_MAX || child == parent) {
        return -1;
    }
    c = &g->prim[child];
    prim_detach(g, c);
    c->parent = parent;
    par = &g->prim[parent];
    par->type = 1;
    return prim_append_child(g, par, child);
}

int gfx_prim_group_out(Gfx *g, int child)
{
    if (g == NULL || child < 0 || child >= GFX_PRIM_MAX) {
        return -1;
    }
    prim_detach(g, &g->prim[child]);
    return 0;
}

/* ----------------------------------------------------------------------- */
/* 命中测试 PrimHit(76) —— 语义见 docs/render-model.md §8                 */
/*                                                                         */
/* 三层结构照抄原版：                                                       */
/*   0x43B9C0 handler → 0x43B8D0（沿父链累加 x/y + 可见性）→ 0x43B5F0（矩形）*/
/* 唯一没落地的是最后一层：矩形内还要查"可点像素掩码"，那一段（0x43B750 起的  */
/* 循环 + 0x4259B0）还没解出来，所以现在**矩形内即算命中**。对外文档必须写明。 */
/* ----------------------------------------------------------------------- */

static int prim_hit_rect(Gfx *g, int pid, int accx, int accy, int cx, int cy,
                         int depth)
{
    const GfxPrim *p;

    if (g == NULL || pid < 0 || pid >= GFX_PRIM_MAX || depth > 32) {
        return 0;
    }
    p = &g->prim[pid];
    if (p->type == 1) {
        /* 0x43B891：组的分支 = 递归任一子图元命中（偏移不额外加自己的 x/y，
         * 照抄原版；祖先偏移由外层父链累加提供）。 */
        int i;

        for (i = 0; i < p->nchildren; ++i) {
            if (prim_hit_rect(g, p->children[i], accx, accy, cx, cy,
                              depth + 1)) {
                return 1;
            }
        }
        return 0;
    }
    if (p->type == 2) {
        /* 0x43B842：tile 分支 = alpha 非 0 + (prim.x/y + 累加) 的矩形 */
        int x0;
        int y0;

        if (p->alpha == 0) {
            return 0;
        }
        x0 = p->x + accx;
        y0 = p->y + accy;
        return (cx >= x0 && cx < x0 + (int)p->w && cy >= y0 &&
                cy < y0 + (int)p->h)
                   ? 1
                   : 0;
    }
    if (p->type == 4) {
        /* 0x43B654：sprite 分支 = 图头 x/y + prim 偏移；WH 写过则取 min(prim, 图) */
        const GfxSlot *s;
        int x0;
        int y0;
        int w;
        int h;
        int u;
        int v;

        if (p->graph < 0 || p->graph >= GFX_GRAPH_MAX) {
            return 0;
        }
        s = &g->slot[p->graph];
        if (s->tex == NULL) {
            return 0;
        }
        x0 = s->tex->img.x + p->x + accx;
        y0 = s->tex->img.y + p->y + accy;
        if (p->wh_set) {
            w = (int)p->w;
            h = (int)p->h;
            if (w > s->tex->img.w) {
                w = s->tex->img.w;
            }
            if (h > s->tex->img.h) {
                h = s->tex->img.h;
            }
        } else {
            w = s->tex->img.w;
            h = s->tex->img.h;
        }
        if (cx < x0 || cx >= x0 + w || cy < y0 || cy >= y0 + h) {
            return 0;
        }
        /* 0x43B720：UV 偏移后的源坐标必须仍在图内（原版这里的检查是
         * `u >= graph.w || v >= graph.h → 未命中`）。 */
        u = (int)p->u + (cx - x0);
        v = (int)p->v + (cy - y0);
        if (u >= s->tex->img.w || v >= s->tex->img.h) {
            return 0;
        }
        return 1;
    }
    return 0; /* 0/3/5：原版同样返回 0（文本图元不可点） */
}

/* 诊断：把图元表里"活着"的项打成一行一条（--dump-prims / FVP_DBG_PRIMS）。
 * 用来回答"标题菜单那 5 个图元到底有没有被创建、在不在组里、是不是可见"。 */
void gfx_debug_dump_prims(Gfx *g, FILE *out)
{
    int i;

    if (g == NULL || out == NULL) {
        return;
    }
    for (i = 1; i < GFX_PRIM_MAX; ++i) {
        const GfxPrim *p = &g->prim[i];

        if (p->type == 0 && p->graph < 0 && p->tw < 0) {
            continue;
        }
        fprintf(out,
                "prim id=%d type=%u draw=%u alpha=%u parent=%d xy=%d,%d "
                "wh=%u,%u graph=%d tw=%d z=%u tex=",
                i, (unsigned)p->type, (unsigned)p->draw, (unsigned)p->alpha,
                p->parent, p->x, p->y, (unsigned)p->w, (unsigned)p->h,
                p->graph, p->tw, (unsigned)p->z);
        if (p->graph >= 0 && p->graph < GFX_PRIM_MAX &&
            g->slot[p->graph].tex != NULL &&
            g->slot[p->graph].tex->img.rgba != NULL) {
            fprintf(out, "%dx%d name=%s",
                    g->slot[p->graph].tex->img.w,
                    g->slot[p->graph].tex->img.h,
                    g->slot[p->graph].name != NULL ? g->slot[p->graph].name
                                                   : "?");
        } else {
            fputs("-", out);
        }
        /* type-5 文本图元：把绑定的文本窗口最后一行也打出来，这样"图元在、
         * 内容是不是正文"一眼能对（诊断用；不影响渲染）。 */
        if (p->type == 5 && p->tw >= 0 && p->tw < GFX_TEXT_MAX) {
            const GfxTextWin *tw = &g->tw[p->tw];

            if (tw->nlines > 0 && tw->lines[tw->nlines - 1] != NULL) {
                fprintf(out, " text=\"%s\"", tw->lines[tw->nlines - 1]);
            } else {
                fputs(" text=\"\"", out);
            }
        }
        fputc('\n', out);
    }
}

int gfx_prim_hit(Gfx *g, int prim_id, int ignore_draw, int cx, int cy)
{
    int parent;
    int accx = 0;
    int accy = 0;
    int reached_root = 0;
    int guard = 0;

    /* 0x43B9C0：primId 必须是 1..0xFFF（0 非法） */
    if (g == NULL || prim_id < 1 || prim_id >= GFX_PRIM_MAX) {
        return 0;
    }
    /* 诊断（--primhit-all）：不看光标/图元矩形，直接报命中。
     * 仅用于 headless 推进脚本到正文，默认关。 */
    if (g->prim_hit_all) {
        return 1;
    }
    /* 0x43B904：prim+0x0A == 0xFFFF（不在任何组里）直接未命中 */
    if (g->prim[prim_id].parent < 0) {
        return 0;
    }
    /* 0x43B8ED：flag == 0 且 prim+0x05(draw) == 0 → 未命中 */
    if (!ignore_draw && !g->prim[prim_id].draw) {
        return 0;
    }
    /* 0x43B8FB 起的父链：累加每层 x/y，任一层 draw == 0 就未命中，
     * 并且必须走到 prim 0（根）才算有效链。 */
    parent = g->prim[prim_id].parent;
    for (;;) {
        if (guard++ > GFX_PRIM_MAX) {
            return 0; /* 环状父子关系（原版会挂死，这里保守返回 0） */
        }
        if (parent == 0) {
            reached_root = 1;
        }
        if (!g->prim[parent].draw) {
            return 0;
        }
        accx += g->prim[parent].x;
        accy += g->prim[parent].y;
        parent = g->prim[parent].parent;
        if (parent < 0) {
            break;
        }
    }
    if (!reached_root) {
        return 0; /* 0x43B943：链没经过根 → 未命中 */
    }
    return prim_hit_rect(g, prim_id, accx, accy, cx, cy, 0);
}

/* ---- text API -------------------------------------------------------- */

static int utf8_step(unsigned char c);

/* UTF-8 码点计数（逐字显示用）。孤立的续字节按 1 个字符算，且不允许跨过
 * 字符串结尾（否则会读到缓冲区外）。 */
static int utf8_step(unsigned char c)
{
    if (c < 0x80u) {
        return 1;
    }
    if ((c & 0xE0u) == 0xC0u) {
        return 2;
    }
    if ((c & 0xF0u) == 0xE0u) {
        return 3;
    }
    if ((c & 0xF8u) == 0xF0u) {
        return 4;
    }
    return 1; /* 孤立续字节 */
}

static int utf8_count_chars(const char *s)
{
    int n = 0;

    if (s == NULL) {
        return 0;
    }
    while (*s != '\0') {
        int step = utf8_step((unsigned char)*s);

        while (step > 1 && s[step - 1] == '\0') {
            step -= 1; /* 截断的多字节序列：只吃掉真实存在的字节 */
        }
        s += step;
        n += 1;
    }
    return n;
}

/* 返回 s 的前 n 个码点的 malloc 副本（按 UTF-8 边界截断）。 */
static char *utf8_truncate(const char *s, int n)
{
    size_t len;
    size_t i = 0;
    char *out;

    if (s == NULL) {
        return NULL;
    }
    len = strlen(s);
    while (i < len && n > 0) {
        int step = utf8_step((unsigned char)s[i]);

        if (i + (size_t)step > len) {
            step = (int)(len - i);
        }
        i += (size_t)step;
        n -= 1;
    }
    out = (char *)malloc(i + 1);
    if (out == NULL) {
        return NULL;
    }
    memcpy(out, s, i);
    out[i] = '\0';
    return out;
}

static char *ruby_strip(const char *s)
{
    size_t n = strlen(s);
    char *out = (char *)malloc(n + 1);
    size_t o = 0;
    size_t i = 0;

    if (out == NULL) {
        return NULL;
    }
    while (i < n) {
        if (s[i] == '[') {
            const char *close = strchr(s + i + 1, ']');
            if (close != NULL) {
                const char *bar = NULL;
                const char *q;
                for (q = s + i + 1; q < close; q++) {
                    if (*q == '|') {
                        bar = q;
                        break;
                    }
                }
                if (bar != NULL) {
                    memcpy(out + o, bar + 1, (size_t)(close - bar - 1));
                    o += (size_t)(close - bar - 1);
                    i = (size_t)(close - s) + 1;
                    continue;
                }
            }
        }
        out[o++] = s[i++];
    }
    out[o] = '\0';
    return out;
}

/* ------------------------------------------------------------------ */
/* 任意位置的单行文本（引擎侧 UI 用，例如标题菜单）                      */
/* ------------------------------------------------------------------ */

/* 用与文本窗口同一套字形/推进量量一行宽（像素）。没有字体时返回 0。
 *
 * 字号不匹配时按 px / 字体自带字号 等比缩放推进量：内建 8x16 位图只有 16 px
 * 一档，但引擎侧 UI 要按实测的 56 px 画，所以这里必须缩放（否则一行只有 8 px
 * 一个字、和原版对不上）。 */
int gfx_text_width(Gfx *g, const char *utf8, int px)
{
    GfxFont *f;
    int w = 0;
    const char *p = utf8;
    int cp;
    double sc;

    if (g == NULL || utf8 == NULL || px <= 0) {
        return 0;
    }
    f = pick_font(g, px);
    if (f == NULL || f->size <= 0) {
        return 0;
    }
    if (f->size == px) {
        return text_width(g, f, utf8);
    }
    sc = (double)px / (double)f->size;
    while ((cp = utf8_next(&p)) >= 0) {
        int gi = font_find(f, (uint32_t)cp);
        int adv = gi >= 0 ? f->adv[gi] : 8;

        w += (int)((double)adv * sc + 0.5);
    }
    return w;
}

/* 把某个图形槽里已经装好的图直接贴到画布上（引擎侧 UI 用，例如标题菜单
 * 的预渲染按钮贴图）。x/y 是左上角；返回 1 = 画了、0 = 槽里没图。
 * 走的是和精灵渲染同一条合成/贴图路径（slot_compose + blit_sprite），
 * 所以缩放/多帧/叠加层行为与图元一致。 */
int gfx_draw_image_scaled(Gfx *g, int slot, int x, int y, int w, int h)
{
    const uint8_t *compo = NULL;
    int cw = 0;
    int ch = 0;
    uint8_t *img;
    GfxPrim tmp;

    if (g == NULL || w <= 0 || h <= 0) {
        return 0;
    }
    if (slot_compose(g, slot, &compo, &cw, &ch) != 0 || compo == NULL) {
        return 0;
    }
    if (cw == w && ch == h) {
        return gfx_draw_image_at(g, slot, x, y);
    }
    img = resize_rgba(compo, cw, ch, w, h);
    if (img == NULL) {
        return 0;
    }
    memset(&tmp, 0, sizeof tmp);
    tmp.alpha = 255;
    blit_sprite(g, x, y, w, h, img, 0, &tmp);
    free(img);
    g->stats.sprites += 1;
    return 1;
}

int gfx_draw_image_cropped(Gfx *g, int slot, int dx, int dy, int sx, int sy,
                           int w, int h)
{
    const uint8_t *compo = NULL;
    int cw = 0;
    int ch = 0;
    uint8_t *sub;
    GfxPrim tmp;
    int y;

    if (g == NULL || w <= 0 || h <= 0) {
        return 0;
    }
    if (slot_compose(g, slot, &compo, &cw, &ch) != 0 || compo == NULL) {
        return 0;
    }
    /* 源矩形裁到图内 */
    if (sx < 0) {
        dx -= sx;
        w += sx;
        sx = 0;
    }
    if (sy < 0) {
        dy -= sy;
        h += sy;
        sy = 0;
    }
    if (sx + w > cw) {
        w = cw - sx;
    }
    if (sy + h > ch) {
        h = ch - sy;
    }
    if (w <= 0 || h <= 0) {
        return 0;
    }
    sub = (uint8_t *)malloc((size_t)w * (size_t)h * 4u);
    if (sub == NULL) {
        return 0;
    }
    for (y = 0; y < h; ++y) {
        memcpy(sub + (size_t)y * (size_t)w * 4u,
               compo + ((size_t)(sy + y) * (size_t)cw + (size_t)sx) * 4u,
               (size_t)w * 4u);
    }
    memset(&tmp, 0, sizeof tmp);
    tmp.alpha = 255;
    blit_sprite(g, dx, dy, w, h, sub, 0, &tmp);
    free(sub);
    g->stats.sprites += 1;
    return 1;
}

/* 从图形槽里取 (sx,sy,sw,sh) 一块，缩放到 (dx,dy,dw,dh) 贴上去。
 *
 * 标题背景缩放用（星空 1920x1080 背景按 MotionMoveS2 2000→1000 缩放、轴心 (0,0)）：
 * 直接裁剪"可见的源区域"再缩放到屏幕，避免把 1920x1080 放大到 3840x2160 再裁。
 * dw==sw && dh==sh 时走 1:1 直贴（与 gfx_draw_image_cropped 相同的快速路径）。 */
/* 标题背景缩放（带缓存）：按 per-mille 缩放把整屏背景缩放后铺满 (0,0,dw,dh)。
 *
 * 轴心默认 (0,0)（左上锚）——对应星空背景（无 PrimSetOP，见
 * ref/title-bg-motion.json）。只处理"屏幕上可见的源区域"；源矩形**不量化**
 * （逐像素跟着缩放走），用快速双线性重采样并缓存最近一帧的结果：
 * 每帧重算一次约 10 ms（三角形滤波要 ~80 ms；而且**不量化源矩形**——直接按
 * "屏幕像素 → 源坐标"的分数映射做双线性，避免整数源像素台阶造成的顿挫）。
 * 静态/终态仍走 1:1 直贴，像素基线不受影响。
 */
/* 分数缩放的屏幕→源坐标映射：屏幕像素 (x,y) 采样源坐标
 * ((x+0.5)/sx - 0.5, (y+0.5)/sy - 0.5)，双线性取 4 邻域。
 * 背景图不透明（alpha 全 255），所以不做 pre-multiply。 */
/* 分数缩放的屏幕→源坐标映射：屏幕像素 (x,y) 采样源坐标
 * ((x+0.5)/sx - 0.5, (y+0.5)/sy - 0.5)，双线性取 4 邻域。
 * 背景图不透明（alpha 全 255），所以不做 pre-multiply。
 * x 方向的源坐标/权重每帧预计算成表（dw 次除法），内层循环纯整数。 */
static uint8_t *zoom_warp_rgba(const uint8_t *src, int sw, int sh, int dw,
                               int dh, int sx1000, int sy1000)
{
    uint8_t *dst;
    int *xtab;
    int x, y;

    dst = (uint8_t *)malloc((size_t)dw * (size_t)dh * 4u);
    xtab = (int *)malloc((size_t)dw * 2u * sizeof(int));
    if (dst == NULL || xtab == NULL) {
        free(dst);
        free(xtab);
        return NULL;
    }
    for (x = 0; x < dw; ++x) {
        double fx = ((double)x + 0.5) * 1000.0 / (double)sx1000 - 0.5;
        int x0;
        int wx;

        if (fx < 0.0) {
            fx = 0.0;
        }
        if (fx > (double)(sw - 1)) {
            fx = (double)(sw - 1);
        }
        x0 = (int)fx;
        wx = (int)((fx - (double)x0) * 256.0 + 0.5);
        if (x0 > sw - 2) {
            x0 = sw - 2;
        }
        xtab[x * 2 + 0] = x0;
        xtab[x * 2 + 1] = wx;
    }
    for (y = 0; y < dh; ++y) {
        double fy = ((double)y + 0.5) * 1000.0 / (double)sy1000 - 0.5;
        int y0;
        int wy;
        const uint8_t *r0;
        const uint8_t *r1;
        uint8_t *orow = dst + (size_t)y * (size_t)dw * 4u;

        if (fy < 0.0) {
            fy = 0.0;
        }
        if (fy > (double)(sh - 1)) {
            fy = (double)(sh - 1);
        }
        y0 = (int)fy;
        wy = (int)((fy - (double)y0) * 256.0 + 0.5);
        if (y0 > sh - 2) {
            y0 = sh - 2;
        }
        r0 = src + (size_t)y0 * (size_t)sw * 4u;
        r1 = src + (size_t)(y0 + 1) * (size_t)sw * 4u;
        {
            int iwy = 256 - wy;

            for (x = 0; x < dw; ++x) {
                int x0 = xtab[x * 2 + 0];
                int wx = xtab[x * 2 + 1];
                int iw = 256 - wx;
                const uint8_t *a = r0 + (size_t)x0 * 4u;
                const uint8_t *b = r0 + (size_t)(x0 + 1) * 4u;
                const uint8_t *c = r1 + (size_t)x0 * 4u;
                const uint8_t *d = r1 + (size_t)(x0 + 1) * 4u;
                uint8_t *o = orow + (size_t)x * 4u;

                o[0] = (uint8_t)(((((int)a[0] * iw + (int)b[0] * wx) >> 8) *
                                  iwy +
                                  (((int)c[0] * iw + (int)d[0] * wx) >> 8) *
                                      wy) >> 8);
                o[1] = (uint8_t)(((((int)a[1] * iw + (int)b[1] * wx) >> 8) *
                                  iwy +
                                  (((int)c[1] * iw + (int)d[1] * wx) >> 8) *
                                      wy) >> 8);
                o[2] = (uint8_t)(((((int)a[2] * iw + (int)b[2] * wx) >> 8) *
                                  iwy +
                                  (((int)c[2] * iw + (int)d[2] * wx) >> 8) *
                                      wy) >> 8);
                o[3] = 255;
            }
        }
    }
    free(xtab);
    return dst;
}

/* 逐层仿射合成（见 gfx.h 的 GfxAffineLayer）：
 *   屏幕 = 锚点 + S·R·(图头 + 源像素 − OP)
 * 内部分辨率由 div 决定（div=1 = 全分辨率，直接画到画布；div>1 先画到
 * (w/div)x(h/div) 再双线性放大）。
 *
 * 性能与清晰度的三个手段（2026-11-26，用户反馈「羽毛变糊」+「风车后多一道
 * 黑色柱状剪影」）：
 *   1) 逆映射用 16.16 定点**增量**步进：每行算一次 u/v，行内只做整数加法，
 *      不再每像素 4 次浮点乘加；
 *   2) 每层缓存一张 8x8 的覆盖掩码（slot_compo_mask）：羽毛/风车这类稀疏叠加层
 *      只占几个百分点的像素，命中空掩码格时直接跳过 4 次双线性取样；
 *   3) 合成图 alpha 全 255 的层（不透明背景）走"直写 RGB"快路径，跳过 alpha
 *      双线性与混合除法。
 * 结果：div=2（960x540 内部）从 30 fps 提到 50+ fps，div=1 全分辨率也能到
 * 35 fps 以上，所以默认改成 div=1（最清晰）。
 */

/* 合成图是否不透明（alpha 全 255）：按 compo_version 缓存。返回 1 = 不透明。 */
static int slot_compo_opaque(Gfx *g, int slot)
{
    GfxSlot *s = &g->slot[slot];
    const uint8_t *compo;
    int cw = 0;
    int ch = 0;
    size_t i;
    size_t n;

    if (slot_compose(g, slot, &compo, &cw, &ch) != 0 || compo == NULL) {
        return 0;
    }
    if (s->compo_opaque_ver == s->compo_version && s->compo_opaque != 0) {
        return s->compo_opaque == 1;
    }
    n = (size_t)cw * (size_t)ch;
    for (i = 0; i < n; ++i) {
        if (compo[i * 4u + 3u] != 255u) {
            s->compo_opaque = 2;
            s->compo_opaque_ver = s->compo_version;
            return 0;
        }
    }
    s->compo_opaque = 1;
    s->compo_opaque_ver = s->compo_version;
    return 1;
}

/* 8x8 覆盖掩码（1 字节一格）：该格里有没有 alpha > 0 的源像素。
 * 用来在整屏扫描时对稀疏层做一次粗剔除。按 compo_version 缓存。 */
static int slot_compo_mask(Gfx *g, int slot, const unsigned char **out,
                           int *mw, int *mh)
{
    GfxSlot *s = &g->slot[slot];
    const uint8_t *compo;
    int cw = 0;
    int ch = 0;
    int mx;
    int my;

    if (slot_compose(g, slot, &compo, &cw, &ch) != 0 || compo == NULL) {
        return -1;
    }
    mx = (cw + 7) / 8;
    my = (ch + 7) / 8;
    if (s->compo_mask == NULL || s->compo_mask_w != mx ||
        s->compo_mask_h != my || s->compo_mask_ver != s->compo_version) {
        int x;
        int y;

        free(s->compo_mask);
        s->compo_mask = (unsigned char *)calloc((size_t)mx * (size_t)my, 1u);
        if (s->compo_mask == NULL) {
            return -1;
        }
        s->compo_mask_w = mx;
        s->compo_mask_h = my;
        s->compo_mask_ver = s->compo_version;
        for (y = 0; y < ch; ++y) {
            const uint8_t *row = compo + (size_t)y * (size_t)cw * 4u;
            unsigned char *mrow = s->compo_mask + (size_t)(y >> 3) * (size_t)mx;

            for (x = 0; x < cw; ++x) {
                if (row[x * 4u + 3u] != 0u) {
                    mrow[x >> 3] = 1u;
                }
            }
        }
    }
    *out = s->compo_mask;
    *mw = mx;
    *mh = my;
    return 0;
}

int gfx_draw_affine_layers(Gfx *g, const GfxAffineLayer *layers, int n,
                           int div)
{
    int w;
    int h;
    uint8_t *comp;
    int i;
    int ok = 0;

    if (g == NULL || layers == NULL || n <= 0) {
        return 0;
    }
    if (div < 1) {
        div = 1;
    }
    w = (g->w + div - 1) / div;
    h = (g->h + div - 1) / div;
    comp = (uint8_t *)malloc((size_t)w * (size_t)h * 4u);
    if (comp == NULL) {
        return 0;
    }
    memset(comp, 0, (size_t)w * (size_t)h * 4u);
    for (i = 0; i < n; ++i) {
        const GfxAffineLayer *L = &layers[i];
        const uint8_t *compo = NULL;
        const unsigned char *mask = NULL;
        int mw = 0;
        int mh = 0;
        int cw = 0;
        int ch = 0;
        int opaque;
        double s;
        double rad;
        double co;
        double si;
        double af;
        double bf;
        double c0;
        double d0;
        int64_t du;
        int64_t dv;
        int xhi;
        int yhi;
        int x;
        int y;

        if (L->scale_x1000 <= 0 || L->scale_y1000 <= 0) {
            continue;
        }
        if (slot_compose(g, L->slot, &compo, &cw, &ch) != 0 || compo == NULL ||
            cw < 2 || ch < 2) {
            continue;
        }
        opaque = slot_compo_opaque(g, L->slot);
        if (!opaque && slot_compo_mask(g, L->slot, &mask, &mw, &mh) != 0) {
            mask = NULL;
        }
        s = (double)L->scale_x1000 / 1000.0;
        rad = (double)L->angle_units * 3.141592653589793 / 3600.0;
        co = cos(rad);
        si = sin(rad);
        /* 屏幕点 q → 源像素 p：p = R(−θ)·(q − 锚点)/s + OP − 图头 */
        af = co / s;
        bf = si / s;
        c0 = (double)L->op_x - (double)L->img_x;
        d0 = (double)L->op_y - (double)L->img_y;
        du = (int64_t)(af * (double)div * 65536.0 + 0.5);
        dv = (int64_t)(-bf * (double)div * 65536.0 + 0.5);
        xhi = cw << 16; /* 允许 u 落在最后一列（含小数），索引侧再钳 */
        yhi = ch << 16;
        for (y = 0; y < h; ++y) {
            double dy = ((double)y + 0.5) * (double)div -
                        (double)L->anchor_y;
            double dx0 = 0.5 * (double)div - (double)L->anchor_x;
            double u0 = af * dx0 + bf * dy + c0;
            double v0 = -bf * dx0 + af * dy + d0;
            /* 像素中心约定：屏幕像素中心映射到源坐标 p 后要减 0.5，才是"源像素序号 +
             * 小数权重"（与 zoom_warp_rgba 的 fx 表达式一致）。少了这个 0.5，恒等映射
             * 会落在半像素上：整幅图被双线性糊一层、包围盒少 1 px（单测抓到）。 */
            int64_t u = (int64_t)((u0 - 0.5) * 65536.0 + 0.5);
            int64_t v = (int64_t)((v0 - 0.5) * 65536.0 + 0.5);
            uint8_t *orow = comp + (size_t)y * (size_t)w * 4u;

            for (x = 0; x < w; ++x, u += du, v += dv) {
                int ix;
                int iy;
                int wx;
                int wy;
                int iwx;
                int iwy;
                const uint8_t *p00;
                const uint8_t *p10;
                const uint8_t *p01;
                const uint8_t *p11;
                int sr;
                int sg;
                int sb;
                int sa;
                uint8_t *o;
                int cc;

                if (u < 0 || v < 0 || u >= xhi || v >= yhi) {
                    continue;
                }
                ix = (int)(u >> 16);
                iy = (int)(v >> 16);
                wx = (int)((u >> 8) & 0xff);
                wy = (int)((v >> 8) & 0xff);
                /* 最后一行/列也要能采到：双线性需要 ix+1，越界时把
                 * 基址退到 cw-2/ch-2 并给满权重（否则恒等映射会少
                 * 最后一列像素，包围盒小 1 px）。 */
                if (ix >= cw - 1) {
                    ix = cw - 2;
                    wx = 255;
                }
                if (iy >= ch - 1) {
                    iy = ch - 2;
                    wy = 255;
                }
                if (mask != NULL && mask[(size_t)(iy >> 3) * (size_t)mw +
                                        (size_t)(ix >> 3)] == 0u) {
                    continue;
                }
                iwx = 256 - wx;
                iwy = 256 - wy;
                p00 = compo + ((size_t)iy * (size_t)cw + (size_t)ix) * 4u;
                p10 = p00 + 4u;
                p01 = p00 + (size_t)cw * 4u;
                p11 = p01 + 4u;
                sr = (((int)p00[0] * iwx + (int)p10[0] * wx) >> 8) * iwy +
                     (((int)p01[0] * iwx + (int)p11[0] * wx) >> 8) * wy;
                sg = (((int)p00[1] * iwx + (int)p10[1] * wx) >> 8) * iwy +
                     (((int)p01[1] * iwx + (int)p11[1] * wx) >> 8) * wy;
                sb = (((int)p00[2] * iwx + (int)p10[2] * wx) >> 8) * iwy +
                     (((int)p01[2] * iwx + (int)p11[2] * wx) >> 8) * wy;
                sr >>= 8;
                sg >>= 8;
                sb >>= 8;
                if (opaque) {
                    sa = 255;
                } else {
                    sa = (((int)p00[3] * iwx + (int)p10[3] * wx) >> 8) * iwy +
                         (((int)p01[3] * iwx + (int)p11[3] * wx) >> 8) * wy;
                    sa = (sa >> 8) * L->alpha / 255;
                }
                if (sa <= 0) {
                    continue;
                }
                o = orow + (size_t)x * 4u;
                if (sa >= 255) {
                    o[0] = (uint8_t)sr;
                    o[1] = (uint8_t)sg;
                    o[2] = (uint8_t)sb;
                    o[3] = 255;
                } else {
                    int da = o[3];
                    int ia = 255 - sa;
                    int oa = sa + (da * ia + 127) / 255;

                    if (oa <= 0) {
                        continue;
                    }
                    cc = (da * ia) / 255;
                    o[0] = (uint8_t)((sr * sa + o[0] * cc + oa / 2) / oa);
                    o[1] = (uint8_t)((sg * sa + o[1] * cc + oa / 2) / oa);
                    o[2] = (uint8_t)((sb * sa + o[2] * cc + oa / 2) / oa);
                    o[3] = (uint8_t)oa;
                }
            }
        }
        ok = 1;
    }
    if (ok) {
        if (div == 1) {
            GfxPrim tmp;

            memset(&tmp, 0, sizeof tmp);
            tmp.alpha = 255;
            blit_sprite(g, 0, 0, g->w, g->h, comp, 0, &tmp);
        } else {
            uint8_t *img = zoom_warp_rgba(comp, w, h, g->w, g->h,
                                          1000 * div, 1000 * div);
            if (img != NULL) {
                GfxPrim tmp;

                memset(&tmp, 0, sizeof tmp);
                tmp.alpha = 255;
                blit_sprite(g, 0, 0, g->w, g->h, img, 0, &tmp);
                free(img);
            } else {
                ok = 0;
            }
        }
    }
    free(comp);
    g->stats.sprites += 1;
    return ok;
}

int gfx_draw_image_zoomed(Gfx *g, int slot, int scale_x1000, int scale_y1000,
                          int dw, int dh)
{
    const uint8_t *compo = NULL;
    int cw = 0;
    int ch = 0;
    int s;

    if (g == NULL || dw <= 0 || dh <= 0) {
        return 0;
    }
    if (scale_x1000 <= 0) {
        scale_x1000 = 1000;
    }
    if (scale_y1000 <= 0) {
        scale_y1000 = 1000;
    }
    if (slot_compose(g, slot, &compo, &cw, &ch) != 0 || compo == NULL ||
        cw <= 0 || ch <= 0) {
        return 0;
    }
    /* 恒等：直接贴原图（与静态画面逐字节相同） */
    if (scale_x1000 == 1000 && scale_y1000 == 1000 && dw == cw && dh == ch) {
        return gfx_draw_image_at(g, slot, 0, 0);
    }
    s = (int)g->slot[slot].compo_version;
    if (g->zoom_ok && g->zoom_img != NULL && g->zoom_slot == slot &&
        g->zoom_ver == s && g->zoom_sw == scale_x1000 &&
        g->zoom_sh == scale_y1000 && g->zoom_dw == dw &&
        g->zoom_dh == dh) {
        blit_sprite_alpha(g, 0, 0, dw, dh, g->zoom_img, 0, 255);
        g->stats.sprites += 1;
        g->stats.resize_cache_hits += 1;
        return 1;
    }
    {
        uint8_t *img = zoom_warp_rgba(compo, cw, ch, dw, dh, scale_x1000,
                                      scale_y1000);

        if (img == NULL) {
            return 0;
        }
        free(g->zoom_img);
        g->zoom_img = img;
        g->zoom_ok = 1;
        g->zoom_slot = slot;
        g->zoom_ver = s;
        g->zoom_sw = scale_x1000;
        g->zoom_sh = scale_y1000;
        g->zoom_dw = dw;
        g->zoom_dh = dh;
        blit_sprite_alpha(g, 0, 0, dw, dh, img, 0, 255);
        g->stats.sprites += 1;
        return 1;
    }
}

int gfx_draw_image_cropped_scaled(Gfx *g, int slot, int dx, int dy, int dw,
                                  int dh, int sx, int sy, int sw, int sh)
{
    const uint8_t *compo = NULL;
    int cw = 0;
    int ch = 0;
    uint8_t *sub;
    uint8_t *img;
    GfxPrim tmp;
    int y;

    if (g == NULL || dw <= 0 || dh <= 0 || sw <= 0 || sh <= 0) {
        return 0;
    }
    if (slot_compose(g, slot, &compo, &cw, &ch) != 0 || compo == NULL) {
        return 0;
    }
    /* 源矩形裁到图内 */
    if (sx < 0) {
        dw = (int)((long long)dw * (sw + sx) / sw);
        sw += sx;
        sx = 0;
    }
    if (sy < 0) {
        dh = (int)((long long)dh * (sh + sy) / sh);
        sh += sy;
        sy = 0;
    }
    if (sx + sw > cw) {
        sw = cw - sx;
    }
    if (sy + sh > ch) {
        sh = ch - sy;
    }
    if (sw <= 0 || sh <= 0) {
        return 0;
    }
    sub = (uint8_t *)malloc((size_t)sw * (size_t)sh * 4u);
    if (sub == NULL) {
        return 0;
    }
    for (y = 0; y < sh; ++y) {
        memcpy(sub + (size_t)y * (size_t)sw * 4u,
               compo + ((size_t)(sy + y) * (size_t)cw + (size_t)sx) * 4u,
               (size_t)sw * 4u);
    }
    if (dw == sw && dh == sh) {
        memset(&tmp, 0, sizeof tmp);
        tmp.alpha = 255;
        blit_sprite(g, dx, dy, dw, dh, sub, 0, &tmp);
        free(sub);
        g->stats.sprites += 1;
        return 1;
    }
    img = resize_rgba(sub, sw, sh, dw, dh);
    free(sub);
    if (img == NULL) {
        return 0;
    }
    blit_sprite_alpha(g, dx, dy, dw, dh, img, 0, 255);
    free(img);
    g->stats.sprites += 1;
    return 1;
}

int gfx_image_offset(const Gfx *g, int slot, int *x, int *y)
{
    const GfxSlot *s;

    if (x != NULL) {
        *x = 0;
    }
    if (y != NULL) {
        *y = 0;
    }
    if (g == NULL || slot < 0 || slot >= GFX_GRAPH_MAX) {
        return 0;
    }
    s = &g->slot[slot];
    if (s->tex == NULL) {
        return 0;
    }
    if (x != NULL) {
        *x = s->tex->img.x;
    }
    if (y != NULL) {
        *y = s->tex->img.y;
    }
    return 1;
}

int gfx_draw_image_alpha(Gfx *g, int slot, int x, int y, int alpha)
{
    const uint8_t *compo = NULL;
    int cw = 0;
    int ch = 0;

    if (g == NULL || alpha <= 0) {
        return 0;
    }
    if (slot_compose(g, slot, &compo, &cw, &ch) != 0) {
        return 0;
    }
    if (compo == NULL || cw <= 0 || ch <= 0) {
        return 0;
    }
    blit_sprite_alpha(g, x, y, cw, ch, compo, 0, clampi(alpha, 0, 255));
    g->stats.sprites += 1;
    return 1;
}

int gfx_draw_image_xform(Gfx *g, int slot, int x, int y, int scale_x1000,
                         int scale_y1000, int alpha)
{
    const uint8_t *compo = NULL;
    int cw = 0;
    int ch = 0;
    int w;
    int h;
    uint8_t *img;
    double sx;
    double sy;

    if (g == NULL || alpha <= 0) {
        return 0;
    }
    if (scale_x1000 == 1000 && scale_y1000 == 1000) {
        return gfx_draw_image_alpha(g, slot, x, y, alpha);
    }
    if (slot_compose(g, slot, &compo, &cw, &ch) != 0) {
        return 0;
    }
    if (compo == NULL || cw <= 0 || ch <= 0) {
        return 0;
    }
    sx = (double)scale_x1000 / 1000.0;
    sy = (double)scale_y1000 / 1000.0;
    if (sx <= 0.0) {
        sx = 1.0;
    }
    if (sy <= 0.0) {
        sy = 1.0;
    }
    w = py_round((double)cw * sx);
    h = py_round((double)ch * sy);
    if (w <= 0 || h <= 0) {
        return 0;
    }
    img = resize_rgba(compo, cw, ch, w, h);
    if (img == NULL) {
        return 0;
    }
    blit_sprite_alpha(g, x, y, w, h, img, 0, clampi(alpha, 0, 255));
    free(img);
    g->stats.sprites += 1;
    return 1;
}

/* 以脚本的 OP（+0x20/+0x22）为轴心做缩放 + 旋转 + alpha。
 *
 * 公式来自 exe（不是拟合）：
 *   * 0x42AF08 `test BYTE PTR [edi+0x6],0x2`（OP 已设）→ 局部坐标取
 *     `(图头 x − OP.x, 图头 y − OP.y)`（0x42AF1D/0x42AF2A）；
 *   * 0x42B41F 同一分支的平移是 `prim.x + OP.x + 累加偏移`（0x42B429..0x42B441）；
 *   * 0x446744 / 0x44674A / 0x446732 = D3DXMatrixScaling / Translation / Multiply，
 *     0x446750 = D3DXMatrixRotationZ（角度 = +0x24 / 10 度）。
 * 所以：**最终位置 = prim.xy + OP + R(angle)·S(scale)·(图头 xy − OP)**，
 * 即在 OP 处缩放/旋转（identity 时 OP 抵消，回到"图头 xy + prim 偏移"）。
 *
 * 本站的矩形是轴对齐 + 位图旋转（rotate_rgba，Pillow 语义、expand），等价做法是：
 * 把素材按 scale 放大后绕自身中心旋转，再把**旋转后的中心**放到变换后的中心位置。
 */
int gfx_draw_image_op_xform(Gfx *g, int slot, int x, int y, int op_x, int op_y,
                            int scale_x1000, int scale_y1000, int angle_units,
                            int alpha)
{
    const uint8_t *compo = NULL;
    const GfxImage *im;
    int cw = 0;
    int ch = 0;
    double sx;
    double sy;
    int w;
    int h;
    int ow;
    int oh;
    double du;
    double dv;
    uint8_t *img;

    if (g == NULL || alpha <= 0 || slot < 0 || slot >= GFX_GRAPH_MAX) {
        return 0;
    }
    if (g->slot[slot].tex == NULL) {
        return 0;
    }
    im = &g->slot[slot].tex->img;
    if (slot_compose(g, slot, &compo, &cw, &ch) != 0) {
        return 0;
    }
    if (compo == NULL || cw <= 0 || ch <= 0) {
        return 0;
    }
    if (scale_x1000 == 1000 && scale_y1000 == 1000 && angle_units == 0) {
        return gfx_draw_image_alpha(g, slot, x + im->x, y + im->y, alpha);
    }
    sx = (double)scale_x1000 / 1000.0;
    sy = (double)scale_y1000 / 1000.0;
    if (sx <= 0.0) {
        sx = 1.0;
    }
    if (sy <= 0.0) {
        sy = 1.0;
    }
    w = py_round((double)cw * sx);
    h = py_round((double)ch * sy);
    if (w <= 0 || h <= 0) {
        return 0;
    }
    /* 素材中心相对 OP 的偏移，先缩放再旋转（y 轴向下的屏幕坐标）。 */
    du = ((double)im->x + (double)cw / 2.0 - (double)op_x) * sx;
    dv = ((double)im->y + (double)ch / 2.0 - (double)op_y) * sy;
    if (angle_units != 0) {
        double a = (double)angle_units / 20.0;
        double ca = cos(a * 3.14159265358979323846 / 180.0);
        double sa = sin(a * 3.14159265358979323846 / 180.0);
        double ru = du * ca - dv * sa;
        double rv = du * sa + dv * ca;

        du = ru;
        dv = rv;
    }
    img = resize_rgba(compo, cw, ch, w, h);
    if (img == NULL) {
        return 0;
    }
    ow = w;
    oh = h;
    if (angle_units != 0) {
        uint8_t *rot =
            rotate_rgba(img, w, h, -(double)angle_units / 20.0, &ow, &oh);

        free(img);
        if (rot == NULL) {
            return 0;
        }
        img = rot;
    }
    {
        double fcx = (double)x + (double)op_x + du;
        double fcy = (double)y + (double)op_y + dv;

        blit_sprite_alpha(g, py_round(fcx - (double)ow / 2.0),
                          py_round(fcy - (double)oh / 2.0), ow, oh, img, 0,
                          clampi(alpha, 0, 255));
    }
    free(img);
    g->stats.sprites += 1;
    return 1;
}

void gfx_fill_canvas(Gfx *g, int r, int gr, int b, int alpha)
{
    int n;
    int i;
    unsigned char a;
    unsigned char cr;
    unsigned char cg;
    unsigned char cb;

    if (g == NULL || g->canvas == NULL || alpha <= 0) {
        return;
    }
    a = (unsigned char)clampi(alpha, 0, 255);
    cr = (unsigned char)clampi(r, 0, 255);
    cg = (unsigned char)clampi(gr, 0, 255);
    cb = (unsigned char)clampi(b, 0, 255);
    n = g->w * g->h;
    for (i = 0; i < n; ++i) {
        uint8_t *q = g->canvas + (size_t)i * 4u;
        unsigned int inv = 255u - (unsigned int)a;

        q[0] = (uint8_t)(((unsigned int)cr * a + (unsigned int)q[0] * inv) /
                         255u);
        q[1] = (uint8_t)(((unsigned int)cg * a + (unsigned int)q[1] * inv) /
                         255u);
        q[2] = (uint8_t)(((unsigned int)cb * a + (unsigned int)q[2] * inv) /
                         255u);
        q[3] = 255;
    }
}

int gfx_image_size(const Gfx *g, int slot, int *w, int *h)
{
    const GfxSlot *s;

    if (w != NULL) {
        *w = 0;
    }
    if (h != NULL) {
        *h = 0;
    }
    if (g == NULL || slot < 0 || slot >= GFX_GRAPH_MAX) {
        return 0;
    }
    s = &g->slot[slot];
    if (s->tex == NULL || s->tex->img.w <= 0 || s->tex->img.h <= 0) {
        return 0;
    }
    if (w != NULL) {
        *w = s->tex->img.w;
    }
    if (h != NULL) {
        *h = s->tex->img.h;
    }
    return 1;
}

int gfx_draw_image_at(Gfx *g, int slot, int x, int y)
{
    const uint8_t *compo = NULL;
    int cw = 0;
    int ch = 0;
    GfxPrim tmp;

    if (g == NULL) {
        return 0;
    }
    if (slot_compose(g, slot, &compo, &cw, &ch) != 0) {
        return 0;
    }
    if (compo == NULL || cw <= 0 || ch <= 0) {
        return 0;
    }
    memset(&tmp, 0, sizeof tmp);
    tmp.alpha = 255;
    blit_sprite(g, x, y, cw, ch, compo, 0, &tmp);
    g->stats.sprites += 1;
    return 1;
}

/* 在 (x, y) 画一行文本：x = 左边界，y = 墨迹上沿。返回画出来的宽度。
 * 不带文本窗口的排版/换行/对话框，也没有层概念——按字号挑字体后逐字画，
 * 字号不同就用 draw_glyph_scaled() 等比放大（内建 8x16 → 56 px 时一个字 28 px
 * 宽、56 px 高，墨迹高约 42，正好对上原版实测的 40..42）。
 * 描边固定 1 px 白色，与正文层一致；供标题菜单之类的引擎侧 UI 使用。 */
void gfx_set_glyph_fallback(Gfx *g, const GfxGlyphFallback *fb)
{
    if (g != NULL) {
        g->glyph_fb = fb;
    }
}

/* 实际绘制：force != NULL 时用指定字体，否则按 px 挑图集。 */
static int draw_line_font(Gfx *g, const GfxFont *force, const char *utf8,
                          int x, int y, int px, int r, int gr, int b,
                          int alpha)
{
    GfxFont *base;
    GfxFont scaled;
    const char *p = utf8;
    int cp;
    int pen = x;
    double sc = 1.0;

    if (g == NULL || utf8 == NULL || px <= 0) {
        return 0;
    }
    base = (force != NULL) ? (GfxFont *)force : pick_font(g, px);
    /* 缺字由 draw_text_run() 里的逐码点回退处理（字距能按 GDI 的推进走），
     * 这里不再整串替换。base == NULL 时退回下面的 8px 推进逻辑。 */
    if (base == NULL || base->size <= 0) {
        return 0;
    }
    if (base->size == px) {
        draw_text_run(g, base, x, y, utf8, r, gr, b, alpha, 1, 255, 255, 255,
                      160);
        return text_width(g, base, utf8);
    }
    /* 目标字号和字体自带字号不同：拷一份只改 size，位图/推进表沿用同一份。 */
    scaled = *base;
    scaled.size = px;
    sc = (double)px / (double)base->size;
    while ((cp = utf8_next(&p)) >= 0) {
        int gi = font_find(base, (uint32_t)cp);

        if (gi >= 0) {
            draw_glyph_scaled(g, &scaled, base, gi, pen, y, r, gr, b, alpha, 1,
                              255, 255, 255, 160);
            pen += (int)((double)base->adv[gi] * sc + 0.5);
        } else {
            pen += (int)(8.0 * sc + 0.5); /* 缺字：按 8 px 推进留白 */
        }
    }
    return pen - x;
}

int gfx_draw_text_line(Gfx *g, const char *utf8, int x, int y, int px, int r,
                       int gr, int b, int alpha)
{
    return draw_line_font(g, NULL, utf8, x, y, px, r, gr, b, alpha);
}

int gfx_draw_text_line_font(Gfx *g, const GfxFont *font, const char *utf8,
                            int x, int y, int px, int r, int gr, int b,
                            int alpha)
{
    return draw_line_font(g, font, utf8, x, y, px, r, gr, b, alpha);
}

int gfx_text_width_font(const Gfx *g, const GfxFont *font, const char *utf8,
                        int px)
{
    const GfxFont *base;
    double sc;

    if (g == NULL || font == NULL || utf8 == NULL || px <= 0) {
        return 0;
    }
    base = font;
    if (base->size <= 0) {
        return 0;
    }
    sc = (double)px / (double)base->size;
    if (sc == 1.0) {
        return text_width((Gfx *)g, (GfxFont *)base, utf8);
    }
    {
        const char *p = utf8;
        int cp;
        int w = 0;

        while ((cp = utf8_next(&p)) >= 0) {
            int gi = font_find(base, (uint32_t)cp);

            w += (gi >= 0) ? (int)((double)base->adv[gi] * sc + 0.5)
                           : (int)(8.0 * sc + 0.5);
        }
        return w;
    }
}

void gfx_text_append(Gfx *g, int layer, const char *utf8)
{
    GfxTextWin *tw;
    char *body;

    if (g == NULL || layer < 0 || layer >= GFX_TEXT_MAX || utf8 == NULL) {
        return;
    }
    tw = &g->tw[layer];
    tw->used = 1;
    if (tw->nlines >= GFX_TEXT_LINES) {
        return;
    }
    body = ruby_strip(utf8);
    if (body == NULL) {
        return;
    }
    tw->lines[tw->nlines] = body;
    tw->nlines++;
    /* 逐字显示：新的一行从"尚未显示"的状态开始，等 gfx_text_advance() 推进。
     * total_chars 统计的是**整个窗口**的字符数（历史行也算），这样裁剪预算
     * 能按行顺序正确扣减。关闭时（默认）shown 直接跟上 total，绘制逻辑不看
     * 这两个字段，所以已验收的帧不受影响。 */
    tw->total_chars += utf8_count_chars(body);
    tw->accum_ms = 0;
    if (!g->typewriter_on) {
        tw->shown_chars = tw->total_chars;
    }
}

void gfx_draw_text(Gfx *g, int layer, const char *utf8)
{
    gfx_text_append(g, layer, utf8);
}

void gfx_text_clear(Gfx *g, int layer)
{
    GfxTextWin *tw;
    int i;

    if (g == NULL || layer < 0 || layer >= GFX_TEXT_MAX) {
        return;
    }
    tw = &g->tw[layer];
    tw->used = 1;
    for (i = 0; i < tw->nlines; i++) {
        free(tw->lines[i]);
        tw->lines[i] = NULL;
    }
    tw->nlines = 0;
    tw->shown_chars = 0;
    tw->total_chars = 0;
    tw->accum_ms = 0;
}

void gfx_text_set_color(Gfx *g, int layer, int r, int gr, int b)
{
    if (g == NULL || layer < 0 || layer >= GFX_TEXT_MAX) {
        return;
    }
    g->tw[layer].used = 1;
    g->tw[layer].color[0] = r & 255;
    g->tw[layer].color[1] = gr & 255;
    g->tw[layer].color[2] = b & 255;
}

int gfx_text_get_color(const Gfx *g, int layer, int *r, int *gr, int *b)
{
    if (r != NULL) {
        *r = 0;
    }
    if (gr != NULL) {
        *gr = 0;
    }
    if (b != NULL) {
        *b = 0;
    }
    if (g == NULL || layer < 0 || layer >= GFX_TEXT_MAX) {
        return 0;
    }
    if (r != NULL) {
        *r = g->tw[layer].color[0];
    }
    if (gr != NULL) {
        *gr = g->tw[layer].color[1];
    }
    if (b != NULL) {
        *b = g->tw[layer].color[2];
    }
    return 1;
}

/* TextColor(112) 的原版语义（handler 0x43C8D0）：三个分量**各自独立判定**——
 * 每个参数先必须是 int（type==2），再各自过 0..0xFF 的范围检查；不满足的那一个
 * **保持原值**（handler 直接跳过该分量），不是钳位、也不是取低 8 位。
 * has_r/has_g/has_b = 0 表示该参数不是 int（原版跳过）。 */
void gfx_text_set_color_ex(Gfx *g, int layer, int r, int gr, int b,
                           int has_r, int has_g, int has_b)
{
    if (g == NULL || layer < 0 || layer >= GFX_TEXT_MAX) {
        return;
    }
    g->tw[layer].used = 1;
    if (has_r && r >= 0 && r <= 0xFF) {
        g->tw[layer].color[0] = r;
    }
    if (has_g && gr >= 0 && gr <= 0xFF) {
        g->tw[layer].color[1] = gr;
    }
    if (has_b && b >= 0 && b <= 0xFF) {
        g->tw[layer].color[2] = b;
    }
}

void gfx_text_set_size(Gfx *g, int layer, int w, int h)
{
    if (g == NULL || layer < 0 || layer >= GFX_TEXT_MAX) {
        return;
    }
    g->tw[layer].used = 1;
    g->tw[layer].size[0] = w;
    g->tw[layer].size[1] = h;
    g->tw[layer].size_h_set = 1;
}

/* TextSize(126) 的原版语义（handler 0x43CD60）：**两个分量各自独立判定**——
 * 每个参数先必须是 int（type==2），再各自过 12..64 / 8..32 的范围检查；
 * 不满足的那一个**保持原值**，另一个照写。我们的 gfx_text_set_size() 是
 * "两个都写"的旧接口（测试与内部调用用），这里补一个按原版逐分量语义的入口，
 * 给 syscall 层用。has_w/has_h = 0 表示该参数不是 int（原版跳过）。 */
void gfx_text_set_size_ex(Gfx *g, int layer, int w, int h, int has_w, int has_h)
{
    if (g == NULL || layer < 0 || layer >= GFX_TEXT_MAX) {
        return;
    }
    if (has_w && w >= 12 && w <= 64) {
        g->tw[layer].used = 1;
        g->tw[layer].size[0] = w;
    }
    if (has_h && h >= 8 && h <= 32) {
        g->tw[layer].used = 1;
        g->tw[layer].size[1] = h;
        g->tw[layer].size_h_set = 1;
    }
}

void gfx_text_set_pos(Gfx *g, int layer, int x, int y)
{
    gfx_text_set_pos_ex(g, layer, x, y, 1, 1);
}

/* TextPos(122) 的原版语义（handler 0x43CA40）：逐轴写 tw+0x5E8 / tw+0x5EA，
 * 每个轴先要求 int；不是 int 的那一轴保持原值。has_x/has_y = 0 表示跳过该轴。 */
void gfx_text_set_pos_ex(Gfx *g, int layer, int x, int y, int has_x, int has_y)
{
    if (g == NULL || layer < 0 || layer >= GFX_TEXT_MAX) {
        return;
    }
    if (has_x) {
        g->tw[layer].used = 1;
        g->tw[layer].has_pos = 1;
        g->tw[layer].pos[0] = x;
    }
    if (has_y) {
        g->tw[layer].used = 1;
        g->tw[layer].has_pos = 1;
        g->tw[layer].pos[1] = y;
    }
}

/* TextOutSize(120)：**名字是误导**。静态分析 handler 0x43C960 显示它既不写宽也不写高：
 *   arg1 int 0..0x1F（twIdx）
 *   arg2 int **0..0xC**  → 0x424850 → `mov %al,0x595(%ecx)`
 *   arg3 int **0..0x8**  → 0x424880 → `mov %al,0x596(%ecx)`
 * 两个分量各自独立判定，越界只跳过那一个（不是整条命令作废）。0x595/0x596 与
 * TextSize 的 0x592/0x593/0x594、TextShadowDist 的 0x597 同属一段
 * **0x28 字节的 text-style 块（tw+0x590..0x5B7）**：绘制时整块被
 * `0x43396F: memcpy(stack, tw+0x590, 0x28)` 拷到栈上的绘制结构里，所以它们是
 * 真正会被消费的"文字装饰参数"，不是死存储。
 * 本站按原版语义存两个分量并逐分量钳制；`out_size[0]/[1]` 是新语义下的
 * 装饰参数 a/b（历史字段名保留，gfx_dump 的场景 JSON 还引用它）。 */
void gfx_text_set_out_size(Gfx *g, int layer, int a, int b)
{
    if (g == NULL || layer < 0 || layer >= GFX_TEXT_MAX) {
        return;
    }
    g->tw[layer].used = 1;
    g->tw[layer].out_attr_set = 1;
    if (a >= 0 && a <= 12) {
        g->tw[layer].out_size[0] = a;
    }
    if (b >= 0 && b <= 8) {
        g->tw[layer].out_size[1] = b;
    }
}

int gfx_text_get_out_size(const Gfx *g, int layer, int *a, int *b)
{
    if (a != NULL) {
        *a = 0;
    }
    if (b != NULL) {
        *b = 0;
    }
    if (g == NULL || layer < 0 || layer >= GFX_TEXT_MAX) {
        return 0;
    }
    if (!g->tw[layer].out_attr_set) {
        return 0;
    }
    if (a != NULL) {
        *a = g->tw[layer].out_size[0];
    }
    if (b != NULL) {
        *b = g->tw[layer].out_size[1];
    }
    return 1;
}

/* TextBuff(110)：窗口的离屏面板尺寸（见 GfxTextWin.panel_w 的注释）。
 *
 * 语义照抄 handler 0x43C830 → 构造/改尺寸函数 0x430EA0：
 *   - 两个分量各自回落到 8（缺省），并各自钳到 0..0x400；
 *   - **任一分量 > 0x400 时原版抛异常**（错误码 0x80：`0x430FF8 push $0x80` →
 *     构造 → 格式化 → `0x44837C throw`，见 ref/exe-textbuff.json）；
 *     本引擎没有 VM 异常语义，只能"放弃这条命令"并返回 0——这是能做到的最近似行为，
 *     所以脚本里越界的 `TextBuff(0,1380,1024)` 在我们这里不会中断后续设置；
 *   - 面板缓冲 = w*h*4 字节（RGBA）。
 * 我们不需要真的分配 w*h*4（文本是直接画到画布上的），只把尺寸记下来：
 * 图元驱动模式下它就是换行宽度与裁剪高度。 */
int gfx_text_set_buff(Gfx *g, int layer, int w, int h)
{
    if (g == NULL || layer < 0 || layer >= GFX_TEXT_MAX) {
        return 0;
    }
    if (w < 0) {
        w = 8;
    }
    if (h < 0) {
        h = 8;
    }
    if (w > 0x400 || h > 0x400) {
        if (getenv("FVP_DEBUG_TEXT") != NULL) {
            fprintf(stderr,
                    "TextBuff layer=%d w=%d h=%d over the 1024 limit: original "
                    "throws (code 0x80), we drop the command\n", layer, w, h);
        }
        return 0;
    }
    g->tw[layer].used = 1;
    g->tw[layer].panel_w = w;
    g->tw[layer].panel_h = h;
    g->tw[layer].panel_set = 1;
    return 1;
}

int gfx_text_get_buff(const Gfx *g, int layer, int *w, int *h)
{
    if (w != NULL) {
        *w = 0;
    }
    if (h != NULL) {
        *h = 0;
    }
    if (g == NULL || layer < 0 || layer >= GFX_TEXT_MAX) {
        return 0;
    }
    if (!g->tw[layer].panel_set) {
        return 0;
    }
    if (w != NULL) {
        *w = g->tw[layer].panel_w;
    }
    if (h != NULL) {
        *h = g->tw[layer].panel_h;
    }
    return 1;
}

int gfx_text_set_layout(Gfx *g, int layer, const GfxTextLayout *lay)
{
    if (g == NULL || layer < 0 || layer >= GFX_TEXT_MAX || lay == NULL) {
        return -1;
    }
    g->tw[layer].lay = *lay;
    g->tw[layer].lay_custom = 1;
    return 0;
}

const GfxTextLayout *gfx_text_get_layout(Gfx *g, int layer)
{
    static GfxTextLayout tmp;
    if (g == NULL || layer < 0 || layer >= GFX_TEXT_MAX) {
        return NULL;
    }
    if (g->tw[layer].lay_custom) {
        return &g->tw[layer].lay;
    }
    tmp = gfx_layout_default(layer);
    return &tmp;
}

int gfx_add_font(Gfx *g, GfxFont *f)
{
    if (g == NULL || f == NULL) {
        return -1;
    }
    if (g->nfonts >= GFX_MAX_FONTS) {
        return -1;
    }
    g->fonts[g->nfonts++] = f;
    return 0;
}

void gfx_clear_fonts(Gfx *g)
{
    if (g != NULL) {
        g->nfonts = 0;
    }
}

void gfx_set_group_transform(Gfx *g, int enabled)
{
    if (g != NULL) {
        g->group_xform = enabled ? 1 : 0;
    }
}

/* ----------------------------------------------------------------------- */
/* archive host: graph*.bin through src/bin.c + src/hzc1.c                 */
/* ----------------------------------------------------------------------- */

typedef struct ArcEnt {
    struct ArcEnt *next;
    char *key;
    int bin;
    int idx;
} ArcEnt;

struct GfxArchiveHost {
    BinArchive *bins[8];
    int nbins;
    ArcEnt *bucket[GFX_TEX_BUCKETS];
    int nentries;
    GfxHost iface;
    int lasterr;
};

static unsigned arc_hash(const char *s)
{
    return tex_hash(s) % GFX_TEX_BUCKETS;
}

static ArcEnt *arc_find(GfxArchiveHost *h, const char *key)
{
    ArcEnt *e;
    for (e = h->bucket[arc_hash(key)]; e != NULL; e = e->next) {
        if (strcmp(e->key, key) == 0) {
            return e;
        }
    }
    return NULL;
}

static void arc_add(GfxArchiveHost *h, const char *prefix, const char *name,
                    int bin, int idx)
{
    size_t pl = strlen(prefix);
    size_t nl = strlen(name);
    char *key = (char *)malloc(pl + nl + 2);
    ArcEnt *e;

    if (key == NULL) {
        return;
    }
    memcpy(key, prefix, pl);
    key[pl] = '/';
    memcpy(key + pl + 1, name, nl + 1);
    e = (ArcEnt *)calloc(1, sizeof *e);
    if (e == NULL) {
        free(key);
        return;
    }
    e->key = key;
    e->bin = bin;
    e->idx = idx;
    {
        unsigned b = arc_hash(key);
        e->next = h->bucket[b];
        h->bucket[b] = e;
    }
    h->nentries++;
}

static const char *gfx_win32_ansi_path(const char *utf8)
{
#if defined(_WIN32)
    static char buf[4096];
    wchar_t w[4096];
    if (utf8 != NULL &&
        MultiByteToWideChar(CP_UTF8, 0, utf8, -1, w, 4096) > 0 &&
        WideCharToMultiByte(CP_ACP, 0, w, -1, buf, (int)sizeof buf, NULL,
                            NULL) > 0) {
        return buf;
    }
#endif
    return utf8;
}

GfxArchiveHost *gfx_archive_host_create_ex(const char *game_dir, int ansi)
{
    static const char *names[6] = {"graph",    "graph_bg",  "graph_bs",
                                   "graph_vis", "graph_vis1", "graph_vis2"};
    GfxArchiveHost *h;
    const char *dir;
    int i;

    if (game_dir == NULL) {
        return NULL;
    }
    dir = ansi ? game_dir : gfx_win32_ansi_path(game_dir);
    h = (GfxArchiveHost *)calloc(1, sizeof *h);
    if (h == NULL) {
        return NULL;
    }
    for (i = 0; i < 6; i++) {
        char path[4608];
        BinArchive *b;
        uint32_t n, k;
        snprintf(path, sizeof path, "%s/%s.bin", dir, names[i]);
        b = bin_open(path);
        if (b == NULL) {
            snprintf(path, sizeof path, "%s\\%s.bin", dir, names[i]);
            b = bin_open(path);
        }
        if (b == NULL) {
            continue;
        }
        h->bins[h->nbins] = b;
        n = bin_count(b);
        for (k = 0; k < n; k++) {
            const char *nm = bin_name(b, k);
            if (nm != NULL) {
                arc_add(h, names[i], nm, h->nbins, (int)k);
            }
        }
        h->nbins++;
    }
    if (h->nbins == 0) {
        free(h);
        return NULL;
    }
    h->iface.ud = h;
    h->iface.load_image = NULL; /* filled below */
    h->iface.free_image = NULL;
    return h;
}

GfxArchiveHost *gfx_archive_host_create(const char *game_dir)
{
    return gfx_archive_host_create_ex(game_dir, 0);
}

int gfx_archive_host_image_count(const GfxArchiveHost *h)
{
    return h != NULL ? h->nentries : 0;
}

/* name resolution of render_assets.Archives.find() */
static ArcEnt *arc_resolve(GfxArchiveHost *h, const char *res)
{
    static const char *prefixes[7] = {"",        "graph_bg/",  "graph_bs/",
                                      "graph_vis/", "graph_vis1/", "graph_vis2/",
                                      "graph/"};
    static const char *plain[3] = {"graph/", "graph_bg/", "graph_bs/"};
    ArcEnt *e;
    const char *slash;
    int i;

    if (res == NULL || res[0] == '\0') {
        return NULL;
    }
    e = arc_find(h, res);
    if (e != NULL) {
        return e;
    }
    slash = strchr(res, '/');
    if (slash != NULL) {
        const char *nm = slash + 1;
        for (i = 0; i < 7; i++) {
            char key[512];
            if (prefixes[i][0] == '\0') {
                snprintf(key, sizeof key, "%s", nm);
            } else {
                snprintf(key, sizeof key, "%s%s", prefixes[i], nm);
            }
            e = arc_find(h, key);
            if (e != NULL) {
                return e;
            }
        }
    } else {
        for (i = 0; i < 3; i++) {
            char key[512];
            snprintf(key, sizeof key, "%s%s", plain[i], res);
            e = arc_find(h, key);
            if (e != NULL) {
                return e;
            }
        }
    }
    return NULL;
}

static int arc_load_image(void *ud, const char *name, GfxImage *out)
{
    GfxArchiveHost *h = (GfxArchiveHost *)ud;
    ArcEnt *e;
    void *buf;
    size_t size = 0;
    Hzc1Info info;
    uint64_t total;
    uint8_t *rgba;

    if (h == NULL || name == NULL || out == NULL) {
        return -1;
    }
    e = arc_resolve(h, name);
    if (e == NULL) {
        return -1;
    }
    buf = bin_read_alloc(h->bins[e->bin], (uint32_t)e->idx, &size);
    if (buf == NULL) {
        return -1;
    }
    if (hzc1_parse((const uint8_t *)buf, size, &info) != HZC1_OK) {
        free(buf);
        return -1;
    }
    total = (uint64_t)info.width * (uint64_t)info.height * 4ull *
            (uint64_t)info.frames;
    if (total == 0 || total > GFX_MAX_IMAGE_BYTES) {
        free(buf);
        return -1;
    }
    rgba = (uint8_t *)malloc((size_t)total);
    if (rgba == NULL) {
        free(buf);
        return -1;
    }
    if (hzc1_decode_all(&info, rgba) != HZC1_OK) {
        free(rgba);
        free(buf);
        return -1;
    }
    free(buf);
    out->w = (int)info.width;
    out->h = (int)info.height;
    out->x = (int)info.x;
    out->y = (int)info.y;
    out->frames = (int)info.frames;
    out->rgba = rgba;
    return 0;
}

static void arc_free_image(void *ud, GfxImage *img)
{
    (void)ud;
    if (img != NULL) {
        free(img->rgba);
        img->rgba = NULL;
    }
}

GfxHost *gfx_archive_host_iface(GfxArchiveHost *h)
{
    if (h == NULL) {
        return NULL;
    }
    h->iface.ud = h;
    h->iface.load_image = arc_load_image;
    h->iface.free_image = arc_free_image;
    return &h->iface;
}

void gfx_archive_host_destroy(GfxArchiveHost *h)
{
    int i, b;

    if (h == NULL) {
        return;
    }
    for (i = 0; i < h->nbins; i++) {
        bin_close(h->bins[i]);
    }
    for (b = 0; b < GFX_TEX_BUCKETS; b++) {
        ArcEnt *e = h->bucket[b];
        while (e != NULL) {
            ArcEnt *n = e->next;
            free(e->key);
            free(e);
            e = n;
        }
    }
    free(h);
}

size_t gfx_tex_bytes(const Gfx *g)
{
    return (g != NULL) ? g->tex_bytes : 0;
}

size_t gfx_tex_cap_bytes(const Gfx *g)
{
    return (g != NULL) ? g->tex_cap : 0;
}

/* ---- 逐字显示（打字机效果）------------------------------------------ */

/* 文本图元定位（原版做法）：PrimSetText(86) 把文本窗口挂到图元上，位置由图元
 * 决定。默认是**图元驱动**（原版做法）；`fixed` 打开后退回旧路径
     * “合成对话框 + 版面表”，冻结像素基线用它。 */
void gfx_set_prim_hit_all(Gfx *g, int on)
{
    if (g == NULL) {
        return;
    }
    g->prim_hit_all = on ? 1 : 0;
}

void gfx_text_set_hidden(Gfx *g, int layer, int hidden)
{
    if (g == NULL || layer < 0 || layer >= GFX_TEXT_MAX) {
        return;
    }
    g->tw_hidden[layer] = hidden ? 1u : 0u;
}

/* ---- 场景状态导出（供 bin/fvp.exe --scene 回放）----------------------
 *
 * 把当前渲染状态写成 `FVPS` v2 二进制：
 *   header: "FVPS" u32 version=2, i32 canvas_w, canvas_h, i32 bg r,g,b,a
 *   graphs: u32 n;  每个 { i32 slot; u32 namelen + bytes; i32 w,h,x,y,frames,frame }
 *   prims : u32 n;  每个 { i32 id,type,draw,alpha,blend,z,x,y,w,h,u,v,op1,op2,
 *                           angle,sx,sy,graph,tw,parent }（20 个 i32）
 *   texts : u32 n;  每个 { i32 idx,r,g,b,size0,size1,out0,out1,panel_w,panel_h,
 *                           has_pos,pos0,pos1,nlines; 每行 u32 len + bytes }
 *   layout: u32 n;  每个 { i32 idx,x,y,max_w,size,max_lines }
 *
 * 为什么要 C 侧导出：Python 参考模型不跑 motion，prim 的 alpha/位置停在脚本
 * 初始化值，导出出来的图元驱动帧没有对话框；C 引擎有 motion 实现，导出的才是
 * “引擎真跑到这一步”的状态。
 */
static void se_i32(FILE *f, int v)
{
    unsigned char b[4];

    b[0] = (unsigned char)(v & 0xff);
    b[1] = (unsigned char)((v >> 8) & 0xff);
    b[2] = (unsigned char)((v >> 16) & 0xff);
    b[3] = (unsigned char)((v >> 24) & 0xff);
    (void)fwrite(b, 1, 4, f);
}

static void se_str(FILE *f, const char *s)
{
    unsigned n = (s != NULL) ? (unsigned)strlen(s) : 0u;

    se_i32(f, (int)n);
    if (n > 0) {
        (void)fwrite(s, 1, n, f);
    }
}

int gfx_scene_export(Gfx *g, const char *path)
{
    FILE *f;
    int i;
    int n;

    if (g == NULL || path == NULL) {
        return 0;
    }
    f = fopen(path, "wb");
    if (f == NULL) {
        return 0;
    }
    (void)fwrite("FVPS", 1, 4, f);
    se_i32(f, 2);
    se_i32(f, g->w);
    se_i32(f, g->h);
    se_i32(f, (int)(g->bg & 0xff));
    se_i32(f, (int)((g->bg >> 8) & 0xff));
    se_i32(f, (int)((g->bg >> 16) & 0xff));
    se_i32(f, (int)((g->bg >> 24) & 0xff));
    /* graphs */
    n = 0;
    for (i = 0; i < GFX_GRAPH_MAX; ++i) {
        if (g->slot != NULL && g->slot[i].name != NULL) {
            n += 1;
        }
    }
    se_i32(f, n);
    for (i = 0; i < GFX_GRAPH_MAX; ++i) {
        GfxSlot *sl = &g->slot[i];

        if (sl->name == NULL) {
            continue;
        }
        se_i32(f, i);
        se_str(f, sl->name);
        se_i32(f, sl->tex != NULL ? sl->tex->img.w : 0);
        se_i32(f, sl->tex != NULL ? sl->tex->img.h : 0);
        se_i32(f, 0);
        se_i32(f, 0);
        se_i32(f, 1);
        se_i32(f, sl->frame);
    }
    /* prims */
    n = 0;
    for (i = 1; i < GFX_PRIM_MAX; ++i) {
        if (g->prim[i].type != 0) {
            n += 1;
        }
    }
    se_i32(f, n);
    for (i = 1; i < GFX_PRIM_MAX; ++i) {
        const GfxPrim *p = &g->prim[i];

        if (p->type == 0) {
            continue;
        }
        se_i32(f, p->id);
        se_i32(f, p->type);
        se_i32(f, p->draw);
        se_i32(f, p->alpha);
        se_i32(f, p->blend);
        se_i32(f, p->z);
        se_i32(f, p->x);
        se_i32(f, p->y);
        se_i32(f, p->w);
        se_i32(f, p->h);
        se_i32(f, p->u);
        se_i32(f, p->v);
        se_i32(f, p->op1);
        se_i32(f, p->op2);
        se_i32(f, p->angle);
        se_i32(f, p->scale_x);
        se_i32(f, p->scale_y);
        se_i32(f, p->graph);
        se_i32(f, p->tw);
        se_i32(f, p->parent);
    }
    /* texts */
    n = 0;
    for (i = 0; i < GFX_TEXT_MAX; ++i) {
        if (g->tw[i].used || g->tw[i].nlines > 0) {
            n += 1;
        }
    }
    se_i32(f, n);
    for (i = 0; i < GFX_TEXT_MAX; ++i) {
        const GfxTextWin *tw = &g->tw[i];
        int k;

        if (!tw->used && tw->nlines <= 0) {
            continue;
        }
        se_i32(f, i);
        se_i32(f, tw->color[0]);
        se_i32(f, tw->color[1]);
        se_i32(f, tw->color[2]);
        se_i32(f, tw->size[0]);
        se_i32(f, tw->size[1]);
        se_i32(f, tw->out_size[0]);
        se_i32(f, tw->out_size[1]);
        se_i32(f, tw->panel_w);
        se_i32(f, tw->panel_h);
        se_i32(f, tw->has_pos);
        se_i32(f, tw->pos[0]);
        se_i32(f, tw->pos[1]);
        se_i32(f, tw->nlines);
        for (k = 0; k < tw->nlines; ++k) {
            se_str(f, tw->lines[k]);
        }
    }
    /* text_layout */
    n = 0;
    for (i = 0; i < GFX_TEXT_MAX; ++i) {
        if (g->tw[i].lay_custom) {
            n += 1;
        }
    }
    se_i32(f, n);
    for (i = 0; i < GFX_TEXT_MAX; ++i) {
        if (!g->tw[i].lay_custom) {
            continue;
        }
        se_i32(f, i);
        se_i32(f, g->tw[i].lay.x);
        se_i32(f, g->tw[i].lay.y);
        se_i32(f, g->tw[i].lay.max_w);
        se_i32(f, g->tw[i].lay.size);
        se_i32(f, g->tw[i].lay.max_lines);
    }
    (void)fclose(f);
    return 1;
}

void gfx_set_text_fixed_layout(Gfx *g, int fixed)
{
    if (g == NULL) {
        return;
    }
    g->text_fixed_layout = fixed ? 1 : 0;
}

/* 兼容旧名（2026-11-26 之前）：`on` 表示“按图元定位”，也就是现在的默认行为，
 * 因此 on=1 等价于 fixed=0、on=0 等价于 fixed=1。新代码请用
 * gfx_set_text_fixed_layout()。 */
void gfx_set_text_prims(Gfx *g, int on)
{
    gfx_set_text_fixed_layout(g, on ? 0 : 1);
}

int gfx_text_prims_enabled(const Gfx *g)
{
    return (g != NULL && !g->text_fixed_layout) ? 1 : 0;
}

int gfx_text_fixed_layout(const Gfx *g)
{
    return (g != NULL) ? g->text_fixed_layout : 0;
}

/* 当前绑定到文本窗口的图元个数（type==5 且 tw>=0），诊断用。 */
int gfx_text_prim_bindings(const Gfx *g)
{
    int i;
    int n = 0;

    if (g == NULL) {
        return 0;
    }
    for (i = 1; i < GFX_PRIM_MAX; ++i) {
        if (g->prim[i].type == 5 && g->prim[i].tw >= 0) {
            n += 1;
        }
    }
    return n;
}

void gfx_typewriter_set_enabled(Gfx *g, int on)
{
    int i;

    if (g == NULL) {
        return;
    }
    g->typewriter_on = on ? 1 : 0;
    if (!on) {
        /* 关掉时把所有窗口的可见字符数拉到满，避免留下"半句"画面 */
        for (i = 0; i < GFX_TEXT_MAX; ++i) {
            g->tw[i].shown_chars = g->tw[i].total_chars;
            g->tw[i].accum_ms = 0;
        }
    }
}

int gfx_typewriter_enabled(const Gfx *g)
{
    return (g != NULL) ? g->typewriter_on : 0;
}

void gfx_text_set_speed(Gfx *g, int layer, int ms)
{
    if (g == NULL || layer < 0 || layer >= GFX_TEXT_MAX) {
        return;
    }
    if (ms < 0) {
        ms = 0;
    }
    g->tw[layer].speed_ms = ms;
}

int gfx_text_shown_chars(const Gfx *g, int layer)
{
    if (g == NULL || layer < 0 || layer >= GFX_TEXT_MAX) {
        return 0;
    }
    return g->tw[layer].shown_chars;
}

int gfx_text_total_chars(const Gfx *g, int layer)
{
    if (g == NULL || layer < 0 || layer >= GFX_TEXT_MAX) {
        return 0;
    }
    return g->tw[layer].total_chars;
}

/* 该窗口是否还在逐字显示途中（没有文本 / 已显示完 -> 0）。 */
int gfx_text_busy(const Gfx *g, int layer)
{
    if (g == NULL || layer < 0 || layer >= GFX_TEXT_MAX) {
        return 0;
    }
    if (!g->typewriter_on) {
        return 0;
    }
    return g->tw[layer].shown_chars < g->tw[layer].total_chars;
}

int gfx_text_set_pause(Gfx *g, int layer, int on)
{
    if (g == NULL || layer < 0 || layer >= GFX_TEXT_MAX) {
        return 0;
    }
    g->tw[layer].paused = on ? 1 : 0;
    return 1;
}

void gfx_text_advance(Gfx *g, int dt_ms)
{
    int i;

    if (g == NULL || !g->typewriter_on || dt_ms <= 0) {
        return;
    }
    for (i = 0; i < GFX_TEXT_MAX; ++i) {
        GfxTextWin *tw = &g->tw[i];
        int per;

        if (tw->shown_chars >= tw->total_chars || tw->paused) {
            continue;
        }
        per = tw->speed_ms;
        if (per <= 0) {
            tw->shown_chars = tw->total_chars; /* 速度 0 = 不等，一次显示完 */
            tw->accum_ms = 0;
            continue;
        }
        tw->accum_ms += dt_ms;
        while (tw->accum_ms >= per && tw->shown_chars < tw->total_chars) {
            tw->accum_ms -= per;
            tw->shown_chars += 1;
        }
    }
}

/* TextShadowDist(125)：阴影距离 0..12（静态分析 0x43CBB0 → 0x4248B0 写 tw+0x597）。
 * 只**记录**数值，渲染层不画阴影：原版阴影的形状/颜色/透明度都没有证据，
 * 参考实现 work/render.py 也只存不画。见 docs/render-model.md §7.1。 */
void gfx_text_set_shadow(Gfx *g, int layer, int dist)
{
    if (g == NULL || layer < 0 || layer >= GFX_TEXT_MAX) {
        return;
    }
    if (dist < 0) {
        dist = 0;
    }
    if (dist > 12) {
        dist = 12;
    }
    g->tw[layer].shadow_dist = dist;
}

/* TextFont(113)：两个字体号（静态分析 0x43DA70 → 0x4247F0 写 tw+0x590，
 * 0x424860 写 tw+0x593）。只记录、不参与选字体（字体号 -1/-2 的含义无证据）。 */
void gfx_text_set_font(Gfx *g, int layer, int id, int id2)
{
    if (g == NULL || layer < 0 || layer >= GFX_TEXT_MAX) {
        return;
    }
    if (id < -5 || id > 31) {
        id = -1;
    }
    if (id2 < -5 || id2 > 31) {
        id2 = 0;
    }
    g->tw[layer].font_id = id;
    g->tw[layer].font_id2 = id2;
}

/* TextFormat(118)：六个排版参数（静态分析 0x43CC10），只记录、不参与排版。 */
void gfx_text_set_format(Gfx *g, int layer, int a, int b, int c, int d, int e,
                         int f)
{
    if (g == NULL || layer < 0 || layer >= GFX_TEXT_MAX) {
        return;
    }
    g->tw[layer].fmt[0] = a;
    g->tw[layer].fmt[1] = b;
    g->tw[layer].fmt[2] = c;
    g->tw[layer].fmt[3] = d;
    g->tw[layer].fmt[4] = e;
    g->tw[layer].fmt[5] = f;
}

/* TextFunction(119)：三个开关（静态分析 0x43CCF0），只记录。 */
void gfx_text_set_function(Gfx *g, int layer, int a, int b, int c)
{
    if (g == NULL || layer < 0 || layer >= GFX_TEXT_MAX) {
        return;
    }
    g->tw[layer].func[0] = a;
    g->tw[layer].func[1] = b;
    g->tw[layer].func[2] = c;
}

/* TextSpace(128)：字间距/行间距，两个分量各 -32..32（静态分析 0x43CE10 写
 * tw+0x5A8 / tw+0x5AA）。同样只记录、不渲染。 */
void gfx_text_set_space(Gfx *g, int layer, int x, int y)
{
    if (g == NULL || layer < 0 || layer >= GFX_TEXT_MAX) {
        return;
    }
    if (x < -32) {
        x = -32;
    }
    if (x > 32) {
        x = 32;
    }
    if (y < -32) {
        y = -32;
    }
    if (y > 32) {
        y = 32;
    }
    g->tw[layer].space[0] = x;
    g->tw[layer].space[1] = y;
}

void gfx_text_get_shadow(const Gfx *g, int layer, int *dist)
{
    if (g == NULL || layer < 0 || layer >= GFX_TEXT_MAX) {
        if (dist != NULL) {
            *dist = 0;
        }
        return;
    }
    if (dist != NULL) {
        *dist = g->tw[layer].shadow_dist;
    }
}

void gfx_tex_set_cap_bytes(Gfx *g, size_t cap)
{
    if (g == NULL) {
        return;
    }
    g->tex_cap = cap;
    tex_evict(g);
}

unsigned long gfx_tex_evictions(const Gfx *g)
{
    return (g != NULL) ? g->tex_evictions : 0;
}

int gfx_tex_count(const Gfx *g)
{
    return (g != NULL) ? g->tex_count : 0;
}
