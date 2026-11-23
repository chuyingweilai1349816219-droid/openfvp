/* FVP Win32 desktop shell -- window, input and GDI presentation.
 *
 * Design notes
 * ------------
 *  - The engine canvas is a fixed 1920x1080 RGBA buffer.  It is scaled 16:9
 *    into the client area (letterboxed, never stretched out of proportion).
 *  - StretchDIBits with BI_BITFIELDS straight from the RGBA pointer is very
 *    slow in GDI (slow per-pixel colour conversion: ~43 ms for one 1920x1080
 *    frame on this machine).  The canvas is therefore converted once per frame
 *    into a BGRA DIB section with a plain 32-bit loop (~2 ms) and presented
 *    with BitBlt / StretchBlt, which use the fast blitter:
 *      * 1:1 (canvas == destination): one BitBlt to the window DC;
 *      * scaled: StretchBlt into an offscreen DIB, then BitBlt to the window
 *        (double buffered, no flicker while resizing).
 *  - The window is per-monitor DPI aware (V2 when the OS supports it).  The
 *    size the caller requests is a logical (96 dpi) size: a 1280x720 window on
 *    a 150% display is physically 1920x1080, so the canvas presents 1:1.
 *  - F11 toggles borderless fullscreen, ESC asks the frame loop to quit.
 *  - Input is captured here and translated into engine canvas coordinates; the
 *    frame loop decides how to feed it to VM syscalls.
 */
#include "win32_window.h"

#include <stdio.h>
#include <string.h>

#include "win32_log.h"

#define FVP_WIN32_CLASS L"FvpWin32Shell"

static int g_class_registered;

static LARGE_INTEGER g_perf_freq;
static int g_perf_ready;

static double win_now_ms(void)
{
    LARGE_INTEGER c;
    if (!g_perf_ready) {
        QueryPerformanceFrequency(&g_perf_freq);
        g_perf_ready = 1;
    }
    QueryPerformanceCounter(&c);
    return (double)c.QuadPart * 1000.0 / (double)g_perf_freq.QuadPart;
}

/* ------------------------------------------------------------------ */
/* Geometry helpers                                                    */
/* ------------------------------------------------------------------ */

static void update_dest(FvpWin32Window *w)
{
    double sx;
    double sy;
    double s;
    int dw;
    int dh;

    if (w->client_w <= 0 || w->client_h <= 0 || w->canvas_w <= 0 ||
        w->canvas_h <= 0) {
        SetRect(&w->dest, 0, 0, 0, 0);
        return;
    }
    sx = (double)w->client_w / (double)w->canvas_w;
    sy = (double)w->client_h / (double)w->canvas_h;
    s = sx < sy ? sx : sy;
    dw = (int)((double)w->canvas_w * s + 0.5);
    dh = (int)((double)w->canvas_h * s + 0.5);
    if (dw < 1) {
        dw = 1;
    }
    if (dh < 1) {
        dh = 1;
    }
    if (dw > w->client_w) {
        dw = w->client_w;
    }
    if (dh > w->client_h) {
        dh = w->client_h;
    }
    w->dest.left = (w->client_w - dw) / 2;
    w->dest.top = (w->client_h - dh) / 2;
    w->dest.right = w->dest.left + dw;
    w->dest.bottom = w->dest.top + dh;
}

void fvp_win32_client_to_canvas(const FvpWin32Window *w, int x, int y,
                                int *cx, int *cy)
{
    int dw = w->dest.right - w->dest.left;
    int dh = w->dest.bottom - w->dest.top;
    int vx;
    int vy;

    if (dw <= 0 || dh <= 0) {
        if (cx != NULL) {
            *cx = w->canvas_w / 2;
        }
        if (cy != NULL) {
            *cy = w->canvas_h / 2;
        }
        return;
    }
    vx = (int)(((double)(x - w->dest.left) / (double)dw) *
                   (double)w->canvas_w +
               0.5);
    vy = (int)(((double)(y - w->dest.top) / (double)dh) *
                   (double)w->canvas_h +
               0.5);
    if (vx < 0) {
        vx = 0;
    }
    if (vy < 0) {
        vy = 0;
    }
    if (vx > w->canvas_w - 1) {
        vx = w->canvas_w - 1;
    }
    if (vy > w->canvas_h - 1) {
        vy = w->canvas_h - 1;
    }
    if (cx != NULL) {
        *cx = vx;
    }
    if (cy != NULL) {
        *cy = vy;
    }
}

static void update_mouse_position(FvpWin32Window *w, int x, int y)
{
    w->input.client_x = x;
    w->input.client_y = y;
    fvp_win32_client_to_canvas(w, x, y, &w->input.canvas_x,
                               &w->input.canvas_y);
}

/* ------------------------------------------------------------------ */
/* Presentation buffers                                                */
/* ------------------------------------------------------------------ */

static void release_present_buffers(FvpWin32Window *w)
{
    if (w->src_dc != NULL) {
        if (w->src_old != NULL) {
            SelectObject(w->src_dc, w->src_old);
            w->src_old = NULL;
        }
        if (w->src_bmp != NULL) {
            DeleteObject(w->src_bmp);
            w->src_bmp = NULL;
        }
        DeleteDC(w->src_dc);
        w->src_dc = NULL;
    }
    w->src_bits = NULL;
    w->src_w = 0;
    w->src_h = 0;

    if (w->mem_dc != NULL) {
        if (w->mem_old != NULL) {
            SelectObject(w->mem_dc, w->mem_old);
            w->mem_old = NULL;
        }
        if (w->mem_bmp != NULL) {
            DeleteObject(w->mem_bmp);
            w->mem_bmp = NULL;
        }
        DeleteDC(w->mem_dc);
        w->mem_dc = NULL;
    }
    w->mem_bits = NULL;
    w->mem_w = 0;
    w->mem_h = 0;
    w->has_frame = 0;
}

static HBITMAP make_dib(HDC dc, int w, int h, void **bits)
{
    BITMAPINFO bmi;

    memset(&bmi, 0, sizeof(bmi));
    bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bmi.bmiHeader.biWidth = w;
    bmi.bmiHeader.biHeight = -h; /* top-down */
    bmi.bmiHeader.biPlanes = 1;
    bmi.bmiHeader.biBitCount = 32;
    bmi.bmiHeader.biCompression = BI_RGB;
    return CreateDIBSection(dc, &bmi, DIB_RGB_COLORS, bits, NULL, 0);
}

static int ensure_src(FvpWin32Window *w, int cw, int ch)
{
    HDC wdc;

    if (w->src_dc != NULL && w->src_w == cw && w->src_h == ch) {
        return 1;
    }
    if (w->src_dc != NULL) {
        if (w->src_old != NULL) {
            SelectObject(w->src_dc, w->src_old);
            w->src_old = NULL;
        }
        if (w->src_bmp != NULL) {
            DeleteObject(w->src_bmp);
            w->src_bmp = NULL;
        }
        DeleteDC(w->src_dc);
        w->src_dc = NULL;
        w->src_bits = NULL;
        w->src_w = 0;
        w->src_h = 0;
    }
    wdc = GetDC(w->hwnd);
    if (wdc == NULL) {
        return 0;
    }
    w->src_dc = CreateCompatibleDC(wdc);
    if (w->src_dc != NULL) {
        w->src_bmp = make_dib(wdc, cw, ch, &w->src_bits);
        if (w->src_bmp == NULL) {
            DeleteDC(w->src_dc);
            w->src_dc = NULL;
        } else {
            w->src_old = SelectObject(w->src_dc, w->src_bmp);
            w->src_w = cw;
            w->src_h = ch;
        }
    }
    ReleaseDC(w->hwnd, wdc);
    if (w->src_dc == NULL) {
        fvp_logf("window: source DIB allocation failed (%lu)",
                 (unsigned long)GetLastError());
        return 0;
    }
    return 1;
}

static int ensure_backbuffer(FvpWin32Window *w)
{
    HDC wdc;

    if (w->client_w <= 0 || w->client_h <= 0) {
        return 0;
    }
    if (w->mem_dc != NULL && w->mem_w == w->client_w &&
        w->mem_h == w->client_h) {
        return 1;
    }
    if (w->mem_dc != NULL) {
        if (w->mem_old != NULL) {
            SelectObject(w->mem_dc, w->mem_old);
            w->mem_old = NULL;
        }
        if (w->mem_bmp != NULL) {
            DeleteObject(w->mem_bmp);
            w->mem_bmp = NULL;
        }
        DeleteDC(w->mem_dc);
        w->mem_dc = NULL;
        w->mem_bits = NULL;
        w->mem_w = 0;
        w->mem_h = 0;
    }
    wdc = GetDC(w->hwnd);
    if (wdc == NULL) {
        return 0;
    }
    w->mem_dc = CreateCompatibleDC(wdc);
    if (w->mem_dc != NULL) {
        w->mem_bmp = make_dib(wdc, w->client_w, w->client_h, &w->mem_bits);
        if (w->mem_bmp == NULL) {
            DeleteDC(w->mem_dc);
            w->mem_dc = NULL;
        } else {
            w->mem_old = SelectObject(w->mem_dc, w->mem_bmp);
            w->mem_w = w->client_w;
            w->mem_h = w->client_h;
        }
    }
    ReleaseDC(w->hwnd, wdc);
    if (w->mem_dc == NULL) {
        fvp_logf("window: back buffer allocation failed (%lu)",
                 (unsigned long)GetLastError());
        return 0;
    }
    return 1;
}

/* RGBA8888 (R in the low byte) -> 32bpp BI_RGB BGRA in memory. */
static void convert_canvas(FvpWin32Window *w, const uint32_t *rgba, int cw,
                           int ch)
{
    uint32_t *dst = (uint32_t *)w->src_bits;
    int y;

    if (dst == NULL) {
        return;
    }
    for (y = 0; y < ch; ++y) {
        const uint32_t *src = rgba + (size_t)y * (size_t)cw;
        uint32_t *out = dst + (size_t)y * (size_t)cw;
        int x;
        for (x = 0; x < cw; ++x) {
            uint32_t d = src[x];
            out[x] = ((d & 0x000000FFu) << 16) | (d & 0x0000FF00u) |
                     ((d & 0x00FF0000u) >> 16) | 0xFF000000u;
        }
    }
}

int fvp_win32_present(FvpWin32Window *w, const uint32_t *rgba, int canvas_w,
                      int canvas_h)
{
    RECT client;
    HDC wdc;
    int dw;
    int dh;
    int ok = 1;
    double t0;

    if (w == NULL || rgba == NULL || w->hwnd == NULL) {
        return 0;
    }
    if (canvas_w > 0 && canvas_h > 0) {
        w->canvas_w = canvas_w;
        w->canvas_h = canvas_h;
    }
    update_dest(w);
    SetRect(&client, 0, 0, w->client_w, w->client_h);
    dw = w->dest.right - w->dest.left;
    dh = w->dest.bottom - w->dest.top;

    if (!ensure_src(w, w->canvas_w, w->canvas_h)) {
        return 0;
    }
    t0 = win_now_ms();
    convert_canvas(w, rgba, w->canvas_w, w->canvas_h);
    w->t_fill_ms = win_now_ms() - t0;

    wdc = GetDC(w->hwnd);
    if (wdc == NULL) {
        return 0;
    }

    if (dw == w->canvas_w && dh == w->canvas_h && w->dest.left == 0 &&
        w->dest.top == 0) {
        /* 1:1, no letterbox: single fast blit. */
        t0 = win_now_ms();
        BitBlt(wdc, 0, 0, dw, dh, w->src_dc, 0, 0, SRCCOPY);
        w->t_stretch_ms = win_now_ms() - t0;
        w->t_blit_ms = 0.0;
    } else if (ensure_backbuffer(w)) {
        FillRect(w->mem_dc, &client, (HBRUSH)GetStockObject(BLACK_BRUSH));
        if (w->filter_mode == 1) {
            SetStretchBltMode(w->mem_dc, HALFTONE);
            SetBrushOrgEx(w->mem_dc, 0, 0, NULL);
        } else {
            /* Nearest-neighbour keeps the frame loop at 60 fps: GDI HALFTONE
             * costs ~14 ms for a 2/3 downscale and 40-60 ms for a 1080p ->
             * 1600p upscale here.  --filter bilinear opts into that quality. */
            SetStretchBltMode(w->mem_dc, COLORONCOLOR);
        }
        t0 = win_now_ms();
        StretchBlt(w->mem_dc, w->dest.left, w->dest.top, dw, dh, w->src_dc,
                   0, 0, w->canvas_w, w->canvas_h, SRCCOPY);
        w->t_stretch_ms = win_now_ms() - t0;
        t0 = win_now_ms();
        BitBlt(wdc, 0, 0, w->client_w, w->client_h, w->mem_dc, 0, 0, SRCCOPY);
        w->t_blit_ms = win_now_ms() - t0;
    } else {
        FillRect(wdc, &client, (HBRUSH)GetStockObject(BLACK_BRUSH));
        t0 = win_now_ms();
        StretchBlt(wdc, w->dest.left, w->dest.top, dw, dh, w->src_dc, 0, 0,
                   w->canvas_w, w->canvas_h, SRCCOPY);
        w->t_stretch_ms = win_now_ms() - t0;
        w->t_blit_ms = 0.0;
        ok = 0;
    }
    ReleaseDC(w->hwnd, wdc);
    w->has_frame = 1;
    return ok;
}

/* ------------------------------------------------------------------ */
/* Fullscreen                                                          */
/* ------------------------------------------------------------------ */

static void toggle_fullscreen(FvpWin32Window *w)
{
    if (!w->fullscreen) {
        MONITORINFO mi;
        w->placement.length = sizeof(w->placement);
        GetWindowPlacement(w->hwnd, &w->placement);
        GetWindowRect(w->hwnd, &w->windowed_rect);
        w->style = GetWindowLongPtrW(w->hwnd, GWL_STYLE);

        SetWindowLongPtrW(w->hwnd, GWL_STYLE,
                          (w->style & ~(WS_CAPTION | WS_THICKFRAME |
                                        WS_MINIMIZEBOX | WS_MAXIMIZEBOX |
                                        WS_SYSMENU)) |
                              WS_POPUP);
        mi.cbSize = sizeof(mi);
        if (GetMonitorInfoW(MonitorFromWindow(w->hwnd,
                                              MONITOR_DEFAULTTONEAREST),
                            &mi)) {
            SetWindowPos(w->hwnd, HWND_TOP, mi.rcMonitor.left,
                         mi.rcMonitor.top,
                         mi.rcMonitor.right - mi.rcMonitor.left,
                         mi.rcMonitor.bottom - mi.rcMonitor.top,
                         SWP_FRAMECHANGED | SWP_SHOWWINDOW);
        }
        w->fullscreen = 1;
        fvp_logf("window: fullscreen ON");
    } else {
        SetWindowLongPtrW(w->hwnd, GWL_STYLE, w->style);
        SetWindowPos(w->hwnd, HWND_TOP, w->windowed_rect.left,
                     w->windowed_rect.top,
                     w->windowed_rect.right - w->windowed_rect.left,
                     w->windowed_rect.bottom - w->windowed_rect.top,
                     SWP_FRAMECHANGED | SWP_SHOWWINDOW);
        SetWindowPlacement(w->hwnd, &w->placement);
        w->fullscreen = 0;
        fvp_logf("window: fullscreen OFF");
    }
    {
        RECT rc;
        GetClientRect(w->hwnd, &rc);
        w->client_w = rc.right;
        w->client_h = rc.bottom;
    }
    update_dest(w);
    InvalidateRect(w->hwnd, NULL, FALSE);
}

/* ------------------------------------------------------------------ */
/* Window procedure                                                    */
/* ------------------------------------------------------------------ */

static void on_size(FvpWin32Window *w, int cw, int ch)
{
    if (cw < 1) {
        cw = 1;
    }
    if (ch < 1) {
        ch = 1;
    }
    if (cw == w->client_w && ch == w->client_h) {
        return;
    }
    w->client_w = cw;
    w->client_h = ch;
    update_dest(w);
}

static LRESULT CALLBACK fvp_win32_proc(HWND hwnd, UINT msg, WPARAM wp,
                                       LPARAM lp)
{
    FvpWin32Window *w =
        (FvpWin32Window *)GetWindowLongPtrW(hwnd, GWLP_USERDATA);

    if (msg == WM_NCCREATE) {
        CREATESTRUCTW *cs = (CREATESTRUCTW *)lp;
        w = (FvpWin32Window *)cs->lpCreateParams;
        if (w != NULL) {
            w->hwnd = hwnd;
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, (LONG_PTR)w);
        }
        return DefWindowProcW(hwnd, msg, wp, lp);
    }
    if (w == NULL) {
        return DefWindowProcW(hwnd, msg, wp, lp);
    }

    switch (msg) {
    case WM_CLOSE:
        fvp_logf("window: WM_CLOSE");
        w->quit = 1;
        DestroyWindow(hwnd);
        return 0;
    case WM_DESTROY:
        w->quit = 1;
        PostQuitMessage(0);
        return 0;
    case WM_ERASEBKGND:
        return 1; /* everything is painted by the frame loop */
    case WM_PRINTCLIENT: {
        /* Lets PrintWindow() capture the frame even when the game window is
         * behind other windows (used by the shell's verification scripts). */
        HDC dc = (HDC)wp;
        RECT client;
        GetClientRect(hwnd, &client);
        if (w->has_frame && w->mem_dc != NULL) {
            BitBlt(dc, 0, 0, client.right, client.bottom, w->mem_dc, 0, 0,
                   SRCCOPY);
        } else if (w->has_frame && w->src_dc != NULL &&
                   w->client_w == w->canvas_w &&
                   w->client_h == w->canvas_h) {
            BitBlt(dc, 0, 0, client.right, client.bottom, w->src_dc, 0, 0,
                   SRCCOPY);
        } else {
            FillRect(dc, &client, (HBRUSH)GetStockObject(BLACK_BRUSH));
        }
        return 0;
    }
    case WM_PRINT:
        if ((lp & PRF_CLIENT) != 0 && wp != 0) {
            SendMessageW(hwnd, WM_PRINTCLIENT, wp, lp);
            return 0;
        }
        break;
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(hwnd, &ps);
        RECT client;
        GetClientRect(hwnd, &client);
        if (w->has_frame && w->mem_dc != NULL) {
            BitBlt(dc, 0, 0, client.right, client.bottom, w->mem_dc, 0, 0,
                   SRCCOPY);
        } else if (w->has_frame && w->src_dc != NULL &&
                   w->client_w == w->canvas_w &&
                   w->client_h == w->canvas_h) {
            BitBlt(dc, 0, 0, client.right, client.bottom, w->src_dc, 0, 0,
                   SRCCOPY);
        } else {
            FillRect(dc, &client, (HBRUSH)GetStockObject(BLACK_BRUSH));
        }
        EndPaint(hwnd, &ps);
        return 0;
    }
    case WM_SETFOCUS:
        return 0;
    case WM_KILLFOCUS:
        /* Do not leave a stuck button behind when focus is lost. */
        if (w->input.mouse_down) {
            w->input.mouse_down = 0;
            w->input.mouse_released += 1;
        }
        if (w->input.key_down) {
            w->input.key_down = 0;
        }
        return 0;
    case WM_SIZE:
        on_size(w, (int)LOWORD(lp), (int)HIWORD(lp));
        return 0;
    case WM_DPICHANGED: {
        RECT *r = (RECT *)lp;
        w->dpi = (int)LOWORD(wp);
        SetWindowPos(hwnd, NULL, r->left, r->top, r->right - r->left,
                     r->bottom - r->top, SWP_NOZORDER | SWP_NOACTIVATE);
        fvp_logf("window: WM_DPICHANGED dpi=%d", w->dpi);
        return 0;
    }
    case WM_GETMINMAXINFO: {
        MINMAXINFO *mmi = (MINMAXINFO *)lp;
        mmi->ptMinTrackSize.x = 320;
        mmi->ptMinTrackSize.y = 180;
        return 0;
    }
    case WM_SYSCOMMAND:
        if ((wp & 0xFFF0) == SC_KEYMENU) {
            return 0; /* swallow Alt menu activation while playing */
        }
        break;
    case WM_SETCURSOR:
        if (LOWORD(lp) == HTCLIENT) {
            /* IDC_ARROW as an ordinal resource (MAKEINTRESOURCEW(32512)). */
            SetCursor(LoadCursorW(NULL, MAKEINTRESOURCEW(32512)));
            return TRUE;
        }
        break;
    case WM_MOUSEMOVE:
        update_mouse_position(w, (int)(short)LOWORD(lp),
                              (int)(short)HIWORD(lp));
        return 0;
    case WM_LBUTTONDOWN:
        SetCapture(hwnd);
        update_mouse_position(w, (int)(short)LOWORD(lp),
                              (int)(short)HIWORD(lp));
        if (!w->input.mouse_down) {
            w->input.mouse_pressed += 1;
        }
        w->input.mouse_down = 1;
        return 0;
    case WM_LBUTTONUP:
        update_mouse_position(w, (int)(short)LOWORD(lp),
                              (int)(short)HIWORD(lp));
        if (w->input.mouse_down) {
            w->input.mouse_released += 1;
        }
        w->input.mouse_down = 0;
        if (GetCapture() == hwnd) {
            ReleaseCapture();
        }
        return 0;
    case WM_MOUSEWHEEL:
        w->input.wheel_delta += GET_WHEEL_DELTA_WPARAM(wp);
        return 0;
    case WM_KEYDOWN: {
        int repeat = (int)((lp >> 30) & 1);
        if (wp == VK_ESCAPE) {
            if (!repeat) {
                fvp_logf("window: ESC pressed -> quit");
                w->quit = 1;
            }
            return 0;
        }
        if (wp == VK_F11) {
            if (!repeat) {
                toggle_fullscreen(w);
            }
            return 0;
        }
        if (wp == VK_RETURN || wp == VK_SPACE) {
            if (!repeat) {
                w->input.key_down = 1;
                w->input.key_pressed += 1;
            }
            return 0;
        }
        break;
    }
    case WM_KEYUP:
        if (wp == VK_RETURN || wp == VK_SPACE) {
            w->input.key_down = 0;
            return 0;
        }
        break;
    default:
        break;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

/* ------------------------------------------------------------------ */
/* DPI helpers                                                         */
/* ------------------------------------------------------------------ */

static int get_dpi_for_window(FvpWin32Window *w)
{
    typedef UINT(WINAPI * GetDpiForWindowFn)(HWND);
    HMODULE user32 = GetModuleHandleW(L"user32.dll");
    FARPROC proc = NULL;
    GetDpiForWindowFn fn = NULL;

    if (user32 != NULL) {
        proc = GetProcAddress(user32, "GetDpiForWindow");
    }
    if (proc != NULL) {
        memcpy(&fn, &proc, sizeof(fn));
        if (fn != NULL) {
            return (int)fn(w->hwnd);
        }
    }
    return 96;
}

static int get_system_dpi(void)
{
    typedef UINT(WINAPI * GetDpiForSystemFn)(void);
    HMODULE user32 = GetModuleHandleW(L"user32.dll");
    FARPROC proc = NULL;
    GetDpiForSystemFn fn = NULL;

    if (user32 != NULL) {
        proc = GetProcAddress(user32, "GetDpiForSystem");
    }
    if (proc != NULL) {
        memcpy(&fn, &proc, sizeof(fn));
        if (fn != NULL) {
            return (int)fn();
        }
    }
    return 96;
}

/* AdjustWindowRectEx with per-monitor DPI support (Win10 1607+). */
static void adjust_window_rect(RECT *rc, DWORD style, int dpi)
{
    typedef BOOL(WINAPI * AdjustWindowRectExForDpiFn)(LPRECT, DWORD, BOOL,
                                                      DWORD, UINT);
    HMODULE user32 = GetModuleHandleW(L"user32.dll");
    FARPROC proc = NULL;
    AdjustWindowRectExForDpiFn fn = NULL;

    if (user32 != NULL) {
        proc = GetProcAddress(user32, "AdjustWindowRectExForDpi");
    }
    if (proc != NULL) {
        memcpy(&fn, &proc, sizeof(fn));
        if (fn != NULL && fn(rc, style, FALSE, 0, (UINT)dpi)) {
            return;
        }
    }
    AdjustWindowRectEx(rc, style, FALSE, 0);
}

/* ------------------------------------------------------------------ */
/* Create / destroy / pump                                             */
/* ------------------------------------------------------------------ */

int fvp_win32_create(FvpWin32Window *w, HINSTANCE inst, const wchar_t *title,
                     int width, int height, int canvas_w, int canvas_h,
                     int filter_mode, int start_fullscreen)
{
    WNDCLASSEXW wc;
    RECT rc;
    DWORD style = WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN;
    int dpi;
    int dw;
    int dh;

    memset(w, 0, sizeof(*w));
    w->hinst = inst;
    w->canvas_w = canvas_w > 0 ? canvas_w : 1920;
    w->canvas_h = canvas_h > 0 ? canvas_h : 1080;
    w->filter_mode = filter_mode;
    w->logical_w = width > 0 ? width : 1280;
    w->logical_h = height > 0 ? height : 720;

    /* The caller asks for a logical (96 dpi) client size; the engine is
     * rendered at physical resolution, so a 1280x720 window on a 150% display
     * is physically 1920x1080 and presents the canvas 1:1. */
    dpi = get_system_dpi();
    dw = (w->logical_w * dpi + 48) / 96;
    dh = (w->logical_h * dpi + 48) / 96;
    {
        /* Never open larger than the desktop work area. */
        RECT wa;
        if (SystemParametersInfoW(SPI_GETWORKAREA, 0, &wa, 0)) {
            int wa_w = wa.right - wa.left;
            int wa_h = wa.bottom - wa.top;
            if (wa_w > 0 && wa_h > 0 && (dw > wa_w || dh > wa_h)) {
                double s = (double)wa_w / (double)dw;
                if ((double)wa_h / (double)dh < s) {
                    s = (double)wa_h / (double)dh;
                }
                dw = (int)((double)dw * s);
                dh = (int)((double)dh * s);
                if (dw < 320) {
                    dw = 320;
                }
                if (dh < 180) {
                    dh = 180;
                }
            }
        }
    }

    if (!g_class_registered) {
        memset(&wc, 0, sizeof(wc));
        wc.cbSize = sizeof(wc);
        wc.style = CS_HREDRAW | CS_VREDRAW;
        wc.lpfnWndProc = fvp_win32_proc;
        wc.hInstance = inst;
        wc.hCursor = LoadCursorW(NULL, MAKEINTRESOURCEW(32512));
        wc.hbrBackground = NULL;
        wc.lpszClassName = FVP_WIN32_CLASS;
        if (RegisterClassExW(&wc) == 0) {
            fvp_logf("window: RegisterClassExW failed (%lu)",
                     (unsigned long)GetLastError());
            return 0;
        }
        g_class_registered = 1;
    }

    SetRect(&rc, 0, 0, dw, dh);
    adjust_window_rect(&rc, style, dpi);

    w->hwnd = CreateWindowExW(0, FVP_WIN32_CLASS, title, style,
                              CW_USEDEFAULT, CW_USEDEFAULT,
                              rc.right - rc.left, rc.bottom - rc.top, NULL,
                              NULL, inst, w);
    if (w->hwnd == NULL) {
        fvp_logf("window: CreateWindowExW failed (%lu)",
                 (unsigned long)GetLastError());
        return 0;
    }

    w->dpi = get_dpi_for_window(w);
    w->input.canvas_x = w->canvas_w / 2;
    w->input.canvas_y = w->canvas_h / 2;

    GetClientRect(w->hwnd, &rc);
    w->client_w = rc.right;
    w->client_h = rc.bottom;
    update_dest(w);

    ShowWindow(w->hwnd, SW_SHOW);
    UpdateWindow(w->hwnd);
    SetForegroundWindow(w->hwnd);
    fvp_logf("window: created %dx%d physical client (%d x %d logical, "
             "canvas %dx%d, dpi %d, filter=%d)",
             w->client_w, w->client_h, w->logical_w, w->logical_h,
             w->canvas_w, w->canvas_h, w->dpi, w->filter_mode);

    if (start_fullscreen) {
        toggle_fullscreen(w);
    }
    return 1;
}

void fvp_win32_destroy(FvpWin32Window *w)
{
    if (w == NULL) {
        return;
    }
    release_present_buffers(w);
    if (w->hwnd != NULL) {
        DestroyWindow(w->hwnd);
        w->hwnd = NULL;
    }
}

int fvp_win32_pump(FvpWin32Window *w)
{
    MSG msg;
    while (PeekMessageW(&msg, NULL, 0, 0, PM_REMOVE)) {
        if (msg.message == WM_QUIT) {
            w->quit = 1;
            break;
        }
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    return !w->quit;
}

void fvp_win32_set_title(FvpWin32Window *w, const wchar_t *title)
{
    if (w != NULL && w->hwnd != NULL && title != NULL) {
        SetWindowTextW(w->hwnd, title);
    }
}