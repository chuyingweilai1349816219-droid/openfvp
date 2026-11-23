/* FVP Win32 desktop shell -- renderer interface.
 *
 * The desktop shell only needs four entry points from the render layer:
 *
 *      Gfx *gfx_create(int w, int h, const GfxHost *host);
 *      void gfx_destroy(Gfx *g);
 *      void gfx_render(Gfx *g);
 *      const uint32_t *gfx_canvas(Gfx *g, int *w, int *h);
 *
 * When fvp/src/gfx.h exists (the render layer has landed) it is included and
 * its declarations are used verbatim.  Until then a local mirror of the agreed
 * contract is used so the shell can be built against tests/stub_gfx.c.
 *
 * Canvas format agreed with the render layer: 1920x1080, 32bpp, top-down,
 * stride = w*4, memory byte order R,G,B,A.  StretchDIBits consumes it directly
 * through BI_BITFIELDS masks (R=0x000000FF, G=0x0000FF00, B=0x00FF0000).
 */
#ifndef FVP_WIN32_GFX_IFACE_H
#define FVP_WIN32_GFX_IFACE_H

#include <stdint.h>

#if defined(__has_include)
#  if __has_include("gfx.h")
#    include "gfx.h"
#    define FVP_GFX_HAVE_HEADER 1
#  endif
#endif

#ifndef FVP_GFX_HAVE_HEADER
/* Fallback mirror of the contract; keep in sync with src/gfx.h when it lands. */
typedef struct Gfx Gfx;

typedef struct GfxImage {
    int w;
    int h;
    int x;
    int y;
    int frames;
    uint8_t *rgba; /* w*h*4*frames, top-down RGBA */
} GfxImage;

typedef struct GfxHost {
    void *ud;
    int (*load_image)(void *ud, const char *name, GfxImage *out);
    void (*free_image)(void *ud, GfxImage *img);
} GfxHost;

Gfx *gfx_create(int w, int h, const GfxHost *host);
void gfx_destroy(Gfx *g);
void gfx_render(Gfx *g);
const uint32_t *gfx_canvas(Gfx *g, int *w, int *h);
const uint8_t *gfx_canvas_rgba(Gfx *g, int *w, int *h);
#endif /* !FVP_GFX_HAVE_HEADER */

/* Hook implemented by tests/stub_gfx.c only (FVP_STUB_GFX is defined by the
 * Makefile when src/gfx.c does not exist).  Lets the test pattern visualise
 * the frame counter, FPS and the mouse cursor in canvas coordinates so the
 * window shell can be verified without the render layer. */
#ifdef FVP_STUB_GFX
void fvp_stub_overlay(int mouse_x, int mouse_y, int mouse_down,
                      unsigned long long steps, int fps, int frame,
                      int click_flash);
#endif

#endif /* FVP_WIN32_GFX_IFACE_H */