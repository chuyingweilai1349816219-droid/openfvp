/* 平台层的缺字回退：用 GDI 现场把一串文本栅格化成覆盖率掩码。
 *
 * 为什么需要它：桌面外壳用预生成的位图图集（gfx-font-*.bin）画字，图集是按
 * 目标作品的日文脚本生成的；汉化补丁的存档/文本里会出现图集没有的汉字。
 * 原版 FVP 本身就用 GDI（CreateFontA/GetGlyphOutline）画字，这里用同样的思路
 * 只对"图集缺字"的字符串做现场栅格化，已覆盖的字仍走图集，保证冻结基线不变。
 */
#include "win32_gdi_text.h"

#include <stdlib.h>
#include <string.h>

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

/* 把 UTF-8 转成 UTF-16；返回写出的 wchar 数（不含 NUL），失败返回 -1。 */
static int to_wide(const char *utf8, wchar_t *out, int cap)
{
    int n;

    if (utf8 == NULL || out == NULL || cap <= 0) {
        return -1;
    }
    n = MultiByteToWideChar(CP_UTF8, 0, utf8, -1, out, cap);
    if (n <= 0) {
        return -1;
    }
    return n - 1;
}

int win32_gdi_text_mask(const char *utf8, int px, unsigned char **mask, int *w,
                        int *h)
{
    HDC screen;
    HDC mem;
    HFONT font;
    HGDIOBJ old_font;
    BITMAPINFO bi;
    unsigned int *bits = NULL;
    SIZE size;
    wchar_t *wide;
    int n;
    int i;
    int mw;
    int mh;
    unsigned char *out;

    if (utf8 == NULL || mask == NULL || w == NULL || h == NULL || px <= 0) {
        return 0;
    }
    *mask = NULL;
    *w = 0;
    *h = 0;
    wide = (wchar_t *)malloc((strlen(utf8) + 1) * sizeof(wchar_t));
    if (wide == NULL) {
        return 0;
    }
    n = to_wide(utf8, wide, (int)strlen(utf8) + 1);
    if (n < 0) {
        free(wide);
        return 0;
    }
    screen = CreateCompatibleDC(NULL);
    if (screen == NULL) {
        free(wide);
        return 0;
    }
    mem = CreateCompatibleDC(screen);
    if (mem == NULL) {
        DeleteDC(screen);
        free(wide);
        return 0;
    }
    /* 用系统中文字体族；不存在时 GDI 会按 DEFAULT_CHARSET 回退到能显示汉字的字体 */
    font = CreateFontW(-px, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                       DEFAULT_CHARSET, OUT_TT_PRECIS, CLIP_DEFAULT_PRECIS,
                       ANTIALIASED_QUALITY, DEFAULT_PITCH | FF_DONTCARE,
                       L"Microsoft YaHei");
    if (font == NULL) {
        DeleteDC(mem);
        DeleteDC(screen);
        free(wide);
        return 0;
    }
    old_font = SelectObject(mem, font);
    if (!GetTextExtentPoint32W(mem, wide, n, &size)) {
        SelectObject(mem, old_font);
        DeleteObject(font);
        DeleteDC(mem);
        DeleteDC(screen);
        free(wide);
        return 0;
    }
    mw = size.cx + 2;
    mh = px + 4;
    if (mw <= 0 || mh <= 0) {
        SelectObject(mem, old_font);
        DeleteObject(font);
        DeleteDC(mem);
        DeleteDC(screen);
        free(wide);
        return 0;
    }
    memset(&bi, 0, sizeof(bi));
    bi.bmiHeader.biSize = sizeof(bi.bmiHeader);
    bi.bmiHeader.biWidth = mw;
    bi.bmiHeader.biHeight = -mh; /* 自上而下 */
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;
    {
        HGDIOBJ old_bmp;
        HBITMAP bmp = CreateDIBSection(mem, &bi, DIB_RGB_COLORS,
                                       (void **)&bits, NULL, 0);

        if (bmp == NULL || bits == NULL) {
            SelectObject(mem, old_font);
            DeleteObject(font);
            DeleteDC(mem);
            DeleteDC(screen);
            free(wide);
            return 0;
        }
        old_bmp = SelectObject(mem, bmp);
        memset(bits, 0, (size_t)mw * (size_t)mh * 4u);
        SetBkMode(mem, TRANSPARENT);
        SetTextColor(mem, RGB(255, 255, 255));
        SetTextAlign(mem, TA_TOP | TA_LEFT);
        TextOutW(mem, 1, 1, wide, n);
        out = (unsigned char *)malloc((size_t)mw * (size_t)mh);
        if (out == NULL) {
            SelectObject(mem, old_bmp);
            DeleteObject(bmp);
            SelectObject(mem, old_font);
            DeleteObject(font);
            DeleteDC(mem);
            DeleteDC(screen);
            free(wide);
            return 0;
        }
        for (i = 0; i < mw * mh; ++i) {
            out[i] = (unsigned char)(bits[i] & 0xFFu); /* B=G=R=覆盖率 */
        }
        SelectObject(mem, old_bmp);
        DeleteObject(bmp);
    }
    SelectObject(mem, old_font);
    DeleteObject(font);
    DeleteDC(mem);
    DeleteDC(screen);
    free(wide);
    *mask = out;
    *w = mw;
    *h = mh;
    return 1;
}