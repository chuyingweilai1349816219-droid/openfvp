/* FVP Win32 desktop shell -- window, input capture and GDI presentation. */
#ifndef FVP_WIN32_WINDOW_H
#define FVP_WIN32_WINDOW_H

#include <windows.h>
#include <stdint.h>

/* FvpWin32Input.nav 的位 */
#define FVP_NAV_UP 1
#define FVP_NAV_DOWN 2
#define FVP_NAV_LEFT 4
#define FVP_NAV_RIGHT 8
#define FVP_NAV_ENTER 16
#define FVP_NAV_ESC 32

#ifdef __cplusplus
extern "C" {
#endif

/* Input snapshot filled by the window procedure and consumed by the frame
 * loop.  Canvas coordinates are in the engine's fixed 1920x1080 space. */
typedef struct FvpWin32Input {
    int mouse_down;      /* left button (or Click key) currently held        */
    int mouse_pressed;   /* number of press edges not yet consumed           */
    int mouse_released;  /* number of release edges not yet consumed         */
    int wheel_delta;     /* accumulated wheel delta (120 per notch)          */
    int client_x;        /* last mouse position in client pixels             */
    int client_y;
    int canvas_x;        /* same position in 1920x1080 canvas coordinates    */
    int canvas_y;
    int key_down;        /* Enter / Space held                               */
    int key_pressed;     /* number of Enter / Space press edges              */

    /* 菜单导航按键（标题菜单用）：按下沿的位掩码，消费后由调用方清零。
     * capture_nav = 1 时 ESC 只置位、不让窗口直接退出。 */
    int nav;             /* bitmask of FVP_NAV_* press edges                 */
    int capture_nav;     /* 1 = 菜单模式（ESC/方向键交给菜单）                 */
} FvpWin32Input;

typedef struct FvpWin32Window {
    HINSTANCE hinst;
    HWND hwnd;

    int client_w;  /* physical client pixels */
    int client_h;
    int canvas_w;
    int canvas_h;
    int logical_w; /* client size at 96 dpi (what the user asked for) */
    int logical_h;

    int fullscreen;
    int quit;

    int filter_mode; /* 0 = nearest (COLORONCOLOR), 1 = bilinear (HALFTONE) */

    FvpWin32Input input;

    /* Destination rectangle of the scaled canvas inside the client area. */
    RECT dest;

    /* Source DIB: the RGBA canvas converted to BGRA once per frame. */
    HDC src_dc;
    HBITMAP src_bmp;
    HGDIOBJ src_old;
    void *src_bits;
    int src_w;
    int src_h;

    /* Double buffer: 32bpp top-down DIB section selected into mem_dc. */
    HDC mem_dc;
    HBITMAP mem_bmp;
    HGDIOBJ mem_old;
    void *mem_bits;
    int mem_w;
    int mem_h;
    int has_frame;

    /* Fullscreen restore state. */
    WINDOWPLACEMENT placement;
    LONG_PTR style;
    RECT windowed_rect;

    int dpi;

    /* Last present() cost breakdown in milliseconds (diagnostics). */
    double t_fill_ms;
    double t_stretch_ms;
    double t_blit_ms;
} FvpWin32Window;

/* Create the window.  width/height are client pixels; canvas_w/canvas_h are
 * the source canvas dimensions (1920x1080).  Returns 1 on success. */
int fvp_win32_create(FvpWin32Window *w, HINSTANCE inst, const wchar_t *title,
                     int width, int height, int canvas_w, int canvas_h,
                     int filter_mode, int start_fullscreen);

void fvp_win32_destroy(FvpWin32Window *w);

/* Pump pending messages.  Returns 0 once the window asked to quit. */
int fvp_win32_pump(FvpWin32Window *w);

/* Scale the canvas into the client area (letterboxed 16:9) and blit it.
 * Returns 1 when a frame was presented. */
int fvp_win32_present(FvpWin32Window *w, const uint32_t *rgba,
                      int canvas_w, int canvas_h);

void fvp_win32_set_title(FvpWin32Window *w, const wchar_t *title);

/* Client size -> 16:9-agnostic canvas coordinates (clamped). */
void fvp_win32_client_to_canvas(const FvpWin32Window *w, int x, int y,
                                int *cx, int *cy);

#ifdef __cplusplus
}
#endif

#endif /* FVP_WIN32_WINDOW_H */