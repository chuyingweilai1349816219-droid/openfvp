/* FVP Win32 desktop shell -- stand-in renderer for tests.
 *
 * This file is only compiled when fvp/src/gfx.c does not exist yet (the
 * Makefile defines FVP_STUB_GFX in that case, see "make win32").  It
 * implements the same small surface the shell uses (create / destroy / render
 * / canvas) and draws a moving test pattern so the window, the frame loop, the
 * canvas scaling and the mouse mapping can be verified before the real render
 * layer lands.
 *
 * The pattern is deliberately cheap (one pass over 1920x1080) and animated:
 *   - 8 colour bars on top (classic test card)
 *   - a diagonal checkerboard + horizontal colour ramps
 *   - a moving vertical "scan" line
 *   - a heartbeat bar that refills every 60 frames
 *   - an FPS bar (length = fps / 60 * 600 px)
 *   - a crosshair at the mouse position in canvas coordinates (green, red while
 *     the left button is held) and an expanding ring on every click
 */
#include <math.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "gfx_iface.h"

struct Gfx {
    int w;
    int h;
    uint32_t *px;

    int mouse_x;
    int mouse_y;
    int mouse_down;
    int fps;
    int frame;
    int click_flash;
    unsigned long long steps;
};

#ifdef FVP_GFX_HAVE_HEADER
Gfx *gfx_create(int w, int h, const GfxHost *host)
#else
Gfx *gfx_create(int w, int h, const void *host)
#endif
{
    Gfx *g;
    size_t n;

    (void)host;
    if (w <= 0 || h <= 0) {
        w = 1920;
        h = 1080;
    }
    g = (Gfx *)calloc(1, sizeof(Gfx));
    if (g == NULL) {
        return NULL;
    }
    n = (size_t)w * (size_t)h;
    g->px = (uint32_t *)calloc(n, sizeof(uint32_t));
    if (g->px == NULL) {
        free(g);
        return NULL;
    }
    g->w = w;
    g->h = h;
    g->mouse_x = w / 2;
    g->mouse_y = h / 2;
    return g;
}

void gfx_destroy(Gfx *g)
{
    if (g == NULL) {
        return;
    }
    free(g->px);
    free(g);
}

static inline uint32_t rgba(int r, int gg, int b, int a)
{
    if (r < 0) {
        r = 0;
    }
    if (r > 255) {
        r = 255;
    }
    if (gg < 0) {
        gg = 0;
    }
    if (gg > 255) {
        gg = 255;
    }
    if (b < 0) {
        b = 0;
    }
    if (b > 255) {
        b = 255;
    }
    if (a < 0) {
        a = 0;
    }
    if (a > 255) {
        a = 255;
    }
    return (uint32_t)r | ((uint32_t)gg << 8) | ((uint32_t)b << 16) |
           ((uint32_t)a << 24);
}

static void put(Gfx *g, int x, int y, uint32_t c)
{
    if (x < 0 || y < 0 || x >= g->w || y >= g->h) {
        return;
    }
    g->px[(size_t)y * (size_t)g->w + (size_t)x] = c;
}

static void fill_rect(Gfx *g, int x0, int y0, int x1, int y1, uint32_t c)
{
    int y;
    if (x0 < 0) {
        x0 = 0;
    }
    if (y0 < 0) {
        y0 = 0;
    }
    if (x1 > g->w) {
        x1 = g->w;
    }
    if (y1 > g->h) {
        y1 = g->h;
    }
    for (y = y0; y < y1; ++y) {
        uint32_t *row = g->px + (size_t)y * (size_t)g->w;
        int x;
        for (x = x0; x < x1; ++x) {
            row[x] = c;
        }
    }
}

/* The shell calls fvp_stub_overlay() then gfx_render(); the overlay values
 * have to survive between the two calls.  A file-scope staging block keeps the
 * stub self contained. */
static struct {
    int mouse_x;
    int mouse_y;
    int mouse_down;
    int fps;
    int frame;
    int click_flash;
    unsigned long long steps;
} g_stage;

void fvp_stub_overlay(int mouse_x, int mouse_y, int mouse_down,
                      unsigned long long steps, int fps, int frame,
                      int click_flash)
{
    g_stage.mouse_x = mouse_x;
    g_stage.mouse_y = mouse_y;
    g_stage.mouse_down = mouse_down;
    g_stage.steps = steps;
    g_stage.fps = fps;
    g_stage.frame = frame;
    g_stage.click_flash = click_flash;
}

static void draw_pattern(Gfx *g)
{
    static const uint32_t bars[8] = {
        0xFFFFFFFFu, /* white   r=255 g=255 b=255 */
        0xFF00FFFFu, /* yellow  r=255 g=255 b=0   */
        0xFFFFFF00u, /* cyan    r=0   g=255 b=255 */
        0xFF00FF00u, /* green   r=0   g=255 b=0   */
        0xFFFF00FFu, /* magenta r=255 g=0   b=255 */
        0xFF0000FFu, /* red     r=255 g=0   b=0   */
        0xFFFF0000u, /* blue    r=0   g=0   b=255 */
        0xFF000000u  /* black */
    };
    int x;
    int y;
    int bar_w;
    int scan;
    int bar;

    bar_w = g->w / 8;
    if (bar_w < 1) {
        bar_w = 1;
    }

    /* Background: vertical ramp + checkerboard. */
    for (y = 0; y < g->h; ++y) {
        uint32_t *row = g->px + (size_t)y * (size_t)g->w;
        int shade = 18 + (y * 24) / (g->h > 0 ? g->h : 1);
        for (x = 0; x < g->w; ++x) {
            int cell = ((x / 160) + (y / 160)) & 1;
            int r = cell ? shade + 10 : 6;
            int gg = cell ? shade + 16 : 10;
            int b = cell ? shade + 34 : 18;
            row[x] = rgba(r, gg, b, 255);
        }
    }

    /* Colour bars. */
    for (bar = 0; bar < 8; ++bar) {
        fill_rect(g, bar * bar_w, 0,
                  bar == 7 ? g->w : (bar + 1) * bar_w, 150, bars[bar]);
    }

    /* Grid every 120 px. */
    for (x = 0; x < g->w; x += 120) {
        fill_rect(g, x, 150, x + 1, g->h, rgba(30, 70, 95, 255));
    }
    for (y = 150; y < g->h; y += 120) {
        fill_rect(g, 0, y, g->w, y + 1, rgba(30, 70, 95, 255));
    }

    /* Canvas border (proves the full 1920x1080 is on screen). */
    fill_rect(g, 0, 0, g->w, 2, rgba(220, 220, 220, 255));
    fill_rect(g, 0, g->h - 2, g->w, g->h, rgba(220, 220, 220, 255));
    fill_rect(g, 0, 0, 2, g->h, rgba(220, 220, 220, 255));
    fill_rect(g, g->w - 2, 0, g->w, g->h, rgba(220, 220, 220, 255));

    /* Moving scan line (motion detector). */
    scan = (g->frame * 9) % (g->w > 1 ? g->w - 1 : 1);
    fill_rect(g, scan, 150, scan + 3, g->h, rgba(255, 180, 40, 255));

    /* Heartbeat: refills every 60 frames. */
    {
        int phase = g->frame % 60;
        int w = (int)((long long)g->w * phase / 59);
        fill_rect(g, 0, g->h - 26, w, g->h - 8, rgba(40, 200, 120, 255));
    }

    /* FPS bar: 60 fps == full width of a 600 px gauge. */
    {
        int w = g->fps;
        if (w < 0) {
            w = 0;
        }
        if (w > 120) {
            w = 120;
        }
        w = w * 5;
        fill_rect(g, 24, g->h - 72, 24 + 600, g->h - 52,
                  rgba(12, 12, 12, 255));
        fill_rect(g, 24, g->h - 72, 24 + w, g->h - 52,
                  rgba(80, 160, 255, 255));
    }

    /* Mouse crosshair + click ring. */
    {
        int mx = g->mouse_x;
        int my = g->mouse_y;
        uint32_t c = g->mouse_down ? rgba(255, 90, 90, 255)
                                   : rgba(120, 255, 120, 255);
        fill_rect(g, mx - 40, my, mx + 41, my + 2, c);
        fill_rect(g, mx, my - 40, mx + 2, my + 41, c);
        fill_rect(g, mx - 6, my - 6, mx + 8, my + 8,
                  rgba(255, 255, 255, 200));
        if (g->click_flash > 0) {
            int rad = 10 + (10 - g->click_flash) * 9;
            int a;
            for (a = 0; a < 360; a += 3) {
                double ang = (double)a * 3.14159265358979323846 / 180.0;
                int px = mx + (int)(cos(ang) * (double)rad);
                int py = my + (int)(sin(ang) * (double)rad);
                put(g, px, py, rgba(255, 220, 60, 255));
                put(g, px + 1, py, rgba(255, 220, 60, 255));
            }
        }
    }
}

void gfx_render(Gfx *g)
{
    if (g == NULL) {
        return;
    }
    g->mouse_x = g_stage.mouse_x;
    g->mouse_y = g_stage.mouse_y;
    g->mouse_down = g_stage.mouse_down;
    g->fps = g_stage.fps;
    g->frame = g_stage.frame;
    g->steps = g_stage.steps;
    g->click_flash = g_stage.click_flash;
    draw_pattern(g);
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
    return g->px;
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
    return (const uint8_t *)g->px;
}