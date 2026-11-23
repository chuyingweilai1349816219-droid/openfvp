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
} GfxTex;

typedef struct GfxOverlay {
    int used;
    int parts;
    GfxTex *tex;
    int frame;
    int x, y;
} GfxOverlay;

typedef struct GfxSlot {
    GfxTex *tex;
    char *name; /* base resource name for the draw list */
    int frame;
    int compo_version;
    int compo_built;
    uint8_t *compo;
    int compo_w, compo_h;
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
    int pos[2];
    int has_pos;
    int out_size[2];
    GfxTextLayout lay;
    int lay_custom;
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
    GfxStats stats;
    GfxDrawItem *draw;
    int ndraw;
    int cap_draw;
    int group_xform;
    size_t tex_bytes; /* 缓存中图像的 RGBA 字节数 */
    int tex_count;
    FvpMotion motion; /* 动画补间表；motion_on = 0 时只走时间轴、不动图元 */
    int motion_on;
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
    p->parent = -1;
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

static void tex_unref(Gfx *g, GfxTex *t)
{
    unsigned b;

    if (t == NULL) {
        return;
    }
    if (--t->refs > 0) {
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

static GfxTex *tex_ref(Gfx *g, const char *name)
{
    GfxTex *t = tex_find(g, name);
    if (t != NULL) {
        t->refs++;
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
    g->tex_bytes += (size_t)t->img.w * (size_t)t->img.h * 4u *
                    (size_t)(t->img.frames > 0 ? t->img.frames : 1);
    g->tex_count += 1;
    {
        unsigned b = tex_hash(name) % GFX_TEX_BUCKETS;
        t->next = g->tex_hash[b];
        g->tex_hash[b] = t;
    }
    return t;
}

/* ----------------------------------------------------------------------- */
/* graph slots                                                             */
/* ----------------------------------------------------------------------- */

static void slot_drop_compo(GfxSlot *s)
{
    free(s->compo);
    s->compo = NULL;
    s->compo_built = 0;
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
        return -1;
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
                rotate_rgba(img, tw, th, -(double)angle_eff / 10.0, &ow, &oh);
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
            xf.angle += (double)p->angle / 10.0;
        }
        for (i = 0; i < p->nchildren; i++) {
            walk_tree(g, p->children[i], depth + 1, &xf);
        }
    } else if (p->type == 4) {
        if (p->alpha > 0) {
            render_sprite(g, p, parent);
        }
    }
    /* type 5 (text) prims carry no texture; the text windows are drawn
     * after the sprite pass, exactly like the Python renderer. */
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
    (void)g;
    if (!f->builtin) {
        gi = font_find(f, cp);
        if (gi >= 0) {
            return f->adv[gi];
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

/* one run of text at (x, y); y is the ascender top, like PIL's default
 * "la" anchor */
static void draw_text_run(Gfx *g, GfxFont *f, int x, int y, const char *s,
                          int r, int gr, int b, int ca, int stroke_w, int sr,
                          int sg, int sb, int sca)
{
    const char *p = s;
    int pen = x;
    int cp;

    while ((cp = utf8_next(&p)) >= 0) {
        if (!f->builtin) {
            int gi = font_find(f, (uint32_t)cp);
            if (gi >= 0) {
                draw_glyph_atlas(g, f, gi, pen - f->margin, y - f->margin, r,
                                 gr, b, ca, stroke_w, sr, sg, sb, sca);
                pen += f->adv[gi];
                continue;
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
static int text_visible(const GfxTextWin *tw)
{
    return tw->used && tw->nlines > 0;
}

static void draw_text_window(Gfx *g, int idx)
{
    GfxTextWin *tw = &g->tw[idx];
    GfxTextLayout lay;
    GfxFont *f;
    GfxLine *lines = NULL;
    char **raw = NULL;
    int nraw = 0, nlines = 0;
    int size, i, x, y;
    int nkeep;

    if (!text_visible(tw)) {
        return;
    }
    lay = tw->lay_custom ? tw->lay : gfx_layout_default(idx);
    size = lay.size;
    if (idx == 0) {
        int sh = tw->size[1];
        size = clampi(sh > 8 ? sh : 34, 18, 40);
    }
    f = pick_font(g, size);

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
    if (idx == 9) {
        for (i = 0; i < nlines; i++) {
            int wpx = text_width(g, f, lines[i].s);
            int xc = (g->w - wpx) / 2;
            draw_text_run(g, f, xc, y, lines[i].s, 255, 255, 255, 255, 2, 0, 0,
                          0, 200);
            y += size + 8;
        }
    } else {
        if (idx == 8) {
            int wpx = text_width(g, f, lines[0].s);
            draw_name_plate(g, x, y, wpx, size);
        }
        for (i = 0; i < nlines; i++) {
            int cr = tw->color[0], cg = tw->color[1], cb = tw->color[2];
            if (cr + cg + cb > 600) {
                cr = 32;
                cg = 40;
                cb = 60;
            }
            draw_text_run(g, f, x, y, lines[i].s, cr, cg, cb, 255, 1, 255, 255,
                          255, 120);
            y += size + 6;
        }
    }
    for (i = 0; i < nlines; i++) {
        free(lines[i].s);
    }
    free(lines);
    g->stats.texts++;
}

static void draw_text_all(Gfx *g)
{
    int i;
    if (text_visible(&g->tw[0]) || text_visible(&g->tw[8])) {
        draw_dialog_box(g);
    }
    for (i = 0; i < GFX_TEXT_MAX; i++) {
        draw_text_window(g, i);
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
        break;
    case GFX_F_H:
        p->h = (unsigned short)(v & 0xFFFF);
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

int gfx_motion_add(Gfx *g, int prim_id, int kind, int from_x, int from_y,
                   int to_x, int to_y, int dur_ms, int mode)
{
    if (g == NULL || prim_id < 0 || prim_id >= GFX_PRIM_MAX) {
        return 0;
    }
    if (dur_ms < 0) {
        dur_ms = 0;
    }
    return fvp_motion_add(&g->motion, prim_id, kind, (double)from_x,
                          (double)from_y, (double)to_x, (double)to_y,
                          (double)dur_ms, mode, 1);
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
    fvp_motion_reap(&g->motion);
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

/* ---- text API -------------------------------------------------------- */

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

void gfx_text_set_size(Gfx *g, int layer, int w, int h)
{
    if (g == NULL || layer < 0 || layer >= GFX_TEXT_MAX) {
        return;
    }
    g->tw[layer].used = 1;
    g->tw[layer].size[0] = w;
    g->tw[layer].size[1] = h;
}

void gfx_text_set_pos(Gfx *g, int layer, int x, int y)
{
    if (g == NULL || layer < 0 || layer >= GFX_TEXT_MAX) {
        return;
    }
    g->tw[layer].used = 1;
    g->tw[layer].has_pos = 1;
    g->tw[layer].pos[0] = x;
    g->tw[layer].pos[1] = y;
}

void gfx_text_set_out_size(Gfx *g, int layer, int w, int h)
{
    if (g == NULL || layer < 0 || layer >= GFX_TEXT_MAX) {
        return;
    }
    g->tw[layer].used = 1;
    g->tw[layer].out_size[0] = w;
    g->tw[layer].out_size[1] = h;
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

GfxArchiveHost *gfx_archive_host_create(const char *game_dir)
{
    static const char *names[6] = {"graph",    "graph_bg",  "graph_bs",
                                   "graph_vis", "graph_vis1", "graph_vis2"};
    GfxArchiveHost *h;
    const char *dir;
    int i;

    if (game_dir == NULL) {
        return NULL;
    }
    dir = gfx_win32_ansi_path(game_dir);
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

int gfx_tex_count(const Gfx *g)
{
    return (g != NULL) ? g->tex_count : 0;
}
