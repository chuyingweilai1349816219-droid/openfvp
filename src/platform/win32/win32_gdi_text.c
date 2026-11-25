/* 平台层的缺字回退：用 GDI 现场把一串文本栅格化成覆盖率掩码。
 *
 * 为什么需要它：桌面外壳用预生成的位图图集（gfx-font-*.bin）画字，图集是按
 * 目标作品的日文脚本生成的；汉化补丁的存档/文本里会出现图集没有的汉字。
 * 原版 FVP 本身就用 GDI（CreateFontA/GetGlyphOutline）画字，这里用同样的思路
 * 只对"图集缺字"的字符串做现场栅格化，已覆盖的字仍走图集，保证冻结基线不变。
 *
 * 两个实现（见头文件说明）：
 *   抗锯齿路径（默认）：CreateFontW + TextOutW + 32bpp DIB → 灰度覆盖率；
 *   原版协议路径：CreateFontA(SHIFTJIS_CHARSET) + GetGlyphOutlineA(GGO_BITMAP)
 *                 → 1bpp（掩码只有 0/255）。参数来自 ref/exe-gdi-params.json。
 */
#include "win32_gdi_text.h"

#include <stdlib.h>
#include <string.h>

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

/* 原版用的是 SHIFTJIS_CHARSET（0x80），见 ref/exe-gdi-params.json 的
 * invariants.charset_value；字重/质量/间距等其余参数原版全是 0。 */
#define FVP_ORIGINAL_CHARSET 0x80u
#define FVP_CP932 932

/* 原版 GetGlyphOutlineA 三次调用共用的 MAT2（16.16 的单位矩阵）。 */
static const MAT2 g_mat2_identity = {
    { 0, 1 }, { 0, 0 },
    { 0, 0 }, { 0, 1 }
};

/* exe 内置字体注册表第 0 项（docs/original-verification.md §8.3）。 */
static char g_default_face[64] = "\xEF\xBC\xAD\xEF\xBC\xB3 \xE3\x82\xB4\xE3\x82\xB7"
                                 "\xE3\x83\x83\xE3\x82\xAF"; /* "ＭＳ ゴシック" */

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

int win32_gdi_text_advance(const char *utf8, int px)
{
    HDC screen;
    HDC mem;
    HFONT font;
    HGDIOBJ old_font;
    SIZE size;
    wchar_t *wide;
    int n;
    int adv = 0;

    if (utf8 == NULL || px <= 0) {
        return 0;
    }
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
    mem = (screen != NULL) ? CreateCompatibleDC(screen) : NULL;
    font = CreateFontW(-px, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                       DEFAULT_CHARSET, OUT_TT_PRECIS, CLIP_DEFAULT_PRECIS,
                       ANTIALIASED_QUALITY, DEFAULT_PITCH | FF_DONTCARE,
                       L"Microsoft YaHei");
    if (mem != NULL && font != NULL) {
        old_font = SelectObject(mem, font);
        if (GetTextExtentPoint32W(mem, wide, n, &size)) {
            adv = (int)size.cx;
        }
        SelectObject(mem, old_font);
    }
    if (font != NULL) {
        DeleteObject(font);
    }
    if (mem != NULL) {
        DeleteDC(mem);
    }
    if (screen != NULL) {
        DeleteDC(screen);
    }
    free(wide);
    return adv;
}

/* ------------------------------------------------------------------------- *
 * 原版协议路径
 *
 * 依据（全部来自 exe 的静态提取，见 ref/exe-gdi-params.json）：
 *   - CreateFontA(nHeight, 0,0,0, 0 字重, 0,0,0, SHIFTJIS_CHARSET, 0,0,0,0, face)
 *   - GetGlyphOutlineA 只用了 GGO_METRICS(0) 与 GGO_BITMAP(1)
 *   - 三次调用共用同一个 MAT2 = 单位矩阵
 * 原版没有调用过 TextOut/ExtTextOut，字形一律以 1bpp 位图取出自己贴。
 * ------------------------------------------------------------------------- */

/* UTF-8 → cp932；遇到 cp932 表示不了的字符返回 0（原版是 ANSI 程序，同样表示
 * 不了）。used_default 用 WC_NO_BEST_FIT_CHARS 打开，避免"猜"出一个替代字。 */
static int utf8_to_cp932(const char *utf8, char *out, int cap)
{
    wchar_t wide[128];
    BOOL used_default = FALSE;
    int n;

    if (utf8 == NULL || out == NULL || cap <= 0) {
        return 0;
    }
    n = MultiByteToWideChar(CP_UTF8, 0, utf8, -1, wide, 128);
    if (n <= 0) {
        return 0;
    }
    n = WideCharToMultiByte(FVP_CP932, WC_NO_BEST_FIT_CHARS, wide, -1, out, cap,
                            NULL, &used_default);
    if (n <= 0 || used_default) {
        return 0;
    }
    return n - 1;
}

/* cp932 字节串按 DBCS 拆成 GetGlyphOutlineA 的 uChar 序列。
 * 返回字符个数；out 容量不足返回 -1。 */
static int cp932_units(const char *cp, int len, UINT *out, int cap)
{
    int i = 0;
    int n = 0;

    while (i < len) {
        unsigned char b = (unsigned char)cp[i];

        if (n >= cap) {
            return -1;
        }
        if ((b >= 0x81u && b <= 0x9Fu) || (b >= 0xE0u && b <= 0xFCu)) {
            if (i + 1 >= len) {
                return -1;
            }
            out[n++] = (UINT)((UINT)b << 8) | (UINT)(unsigned char)cp[i + 1];
            i += 2;
        } else {
            out[n++] = (UINT)b;
            i += 1;
        }
    }
    return n;
}

static HFONT make_font_cp932(const char *face, int px)
{
    char ansi[128];

    if (!utf8_to_cp932(face != NULL ? face : g_default_face, ansi, (int)sizeof ansi)) {
        return NULL;
    }
    /* 参数顺序/取值与原版一致：字重 0、质量 0、字符集 SHIFTJIS。 */
    return CreateFontA(-px, 0, 0, 0, 0, FALSE, FALSE, FALSE,
                       (DWORD)FVP_ORIGINAL_CHARSET, 0, 0, 0, 0, ansi);
}

/* 取一个字的 GGO_BITMAP；成功返回 1，bits/stride 由调用方 free。 */
static int glyph_bitmap_cp932(HDC dc, UINT ch, unsigned char **bits,
                              int *stride, GLYPHMETRICS *gm)
{
    DWORD size;

    size = GetGlyphOutlineA(dc, ch, GGO_BITMAP, gm, 0, NULL, &g_mat2_identity);
    if (size == GDI_ERROR || size == 0 || gm->gmBlackBoxX == 0
        || gm->gmBlackBoxY == 0) {
        return 0;
    }
    *bits = (unsigned char *)malloc((size_t)size);
    if (*bits == NULL) {
        return 0;
    }
    if (GetGlyphOutlineA(dc, ch, GGO_BITMAP, gm, size, *bits, &g_mat2_identity)
        == GDI_ERROR) {
        free(*bits);
        *bits = NULL;
        return 0;
    }
    /* GGO_BITMAP 的行按 4 字节对齐 */
    *stride = (int)(((gm->gmBlackBoxX + 31u) / 32u) * 4u);
    return 1;
}

int win32_gdi_text_advance_original(const char *utf8, const char *face, int px)
{
    char cp[256];
    UINT units[128];
    HDC dc;
    HFONT font;
    HGDIOBJ old_font;
    int len;
    int n;
    int i;
    int advance = 0;

    if (utf8 == NULL || *utf8 == '\0' || px <= 0) {
        return 0;
    }
    len = utf8_to_cp932(utf8, cp, (int)sizeof cp);
    if (len <= 0) {
        return 0;
    }
    n = cp932_units(cp, len, units, 128);
    if (n <= 0) {
        return 0;
    }
    dc = CreateCompatibleDC(NULL);
    if (dc == NULL) {
        return 0;
    }
    font = make_font_cp932(face, px);
    if (font == NULL) {
        DeleteDC(dc);
        return 0;
    }
    old_font = SelectObject(dc, font);
    for (i = 0; i < n; ++i) {
        GLYPHMETRICS gm;

        if (GetGlyphOutlineA(dc, units[i], GGO_METRICS, &gm, 0, NULL,
                             &g_mat2_identity) != GDI_ERROR) {
            advance += (int)gm.gmCellIncX;
        }
    }
    SelectObject(dc, old_font);
    DeleteObject(font);
    DeleteDC(dc);
    return advance;
}

int win32_gdi_text_mask_original(const char *utf8, const char *face, int px,
                                 unsigned char **mask, int *w, int *h)
{
    char cp[256];
    UINT units[128];
    HDC dc;
    HFONT font;
    HGDIOBJ old_font;
    TEXTMETRIC tm;
    unsigned char *canvas;
    int len;
    int n;
    int i;
    int baseline;
    int pen;
    int right;
    int cw;
    int chh;
    int rc = 0;

    if (mask == NULL || w == NULL || h == NULL) {
        return 0;
    }
    *mask = NULL;
    *w = 0;
    *h = 0;
    if (utf8 == NULL || px <= 0) {
        return 0;
    }
    len = utf8_to_cp932(utf8, cp, (int)sizeof cp);
    if (len <= 0) {
        return 0;
    }
    n = cp932_units(cp, len, units, 128);
    if (n <= 0) {
        return 0;
    }
    dc = CreateCompatibleDC(NULL);
    if (dc == NULL) {
        return 0;
    }
    font = make_font_cp932(face, px);
    if (font == NULL) {
        DeleteDC(dc);
        return 0;
    }
    old_font = SelectObject(dc, font);
    memset(&tm, 0, sizeof tm);
    if (!GetTextMetricsA(dc, &tm) || tm.tmHeight <= 0) {
        tm.tmHeight = px;
        tm.tmAscent = px;
    }
    baseline = (int)tm.tmAscent + 1;
    chh = (int)tm.tmHeight + 2;
    if (chh <= 0) {
        chh = px + 2;
    }

    /* 第一遍：量出画布宽度（进位之和 + 最大右侧超界）。 */
    pen = 0;
    right = 0;
    for (i = 0; i < n; ++i) {
        GLYPHMETRICS gm;

        if (GetGlyphOutlineA(dc, units[i], GGO_METRICS, &gm, 0, NULL,
                             &g_mat2_identity) == GDI_ERROR) {
            pen += px / 2; /* 量不到就按半个字宽推进，避免叠在一起 */
            continue;
        }
        if (gm.gmBlackBoxX > 0 && gm.gmBlackBoxY > 0) {
            int over = pen + (int)gm.gmptGlyphOrigin.x + (int)gm.gmBlackBoxX;

            if (over > right) {
                right = over;
            }
        }
        pen += (int)gm.gmCellIncX;
    }
    cw = (pen > right ? pen : right) + 2;
    if (cw <= 2) {
        SelectObject(dc, old_font);
        DeleteObject(font);
        DeleteDC(dc);
        return 0;
    }
    canvas = (unsigned char *)calloc((size_t)cw * (size_t)chh, 1u);
    if (canvas == NULL) {
        SelectObject(dc, old_font);
        DeleteObject(font);
        DeleteDC(dc);
        return 0;
    }

    /* 第二遍：把每个字的 1bpp 位图贴到画布上（只有 0 / 255 两值）。 */
    pen = 0;
    for (i = 0; i < n; ++i) {
        GLYPHMETRICS gm;
        unsigned char *bits = NULL;
        int stride = 0;

        if (!glyph_bitmap_cp932(dc, units[i], &bits, &stride, &gm)) {
            GLYPHMETRICS adv;

            if (GetGlyphOutlineA(dc, units[i], GGO_METRICS, &adv, 0, NULL,
                                 &g_mat2_identity) != GDI_ERROR) {
                pen += (int)adv.gmCellIncX;
            } else {
                pen += px / 2;
            }
            continue;
        }
        {
            int x0 = pen + (int)gm.gmptGlyphOrigin.x;
            int y0 = baseline - (int)gm.gmptGlyphOrigin.y;
            int gy;

            for (gy = 0; gy < (int)gm.gmBlackBoxY; ++gy) {
                int dy = y0 + gy;
                int gx;

                if (dy < 0 || dy >= chh) {
                    continue;
                }
                for (gx = 0; gx < (int)gm.gmBlackBoxX; ++gx) {
                    int dx = x0 + gx;
                    unsigned char byte;
                    int bit;

                    if (dx < 0 || dx >= cw) {
                        continue;
                    }
                    byte = bits[(size_t)gy * (size_t)stride + (size_t)(gx >> 3)];
                    bit = (byte >> (7 - (gx & 7))) & 1;
                    if (bit != 0) {
                        canvas[(size_t)dy * (size_t)cw + (size_t)dx] = 255u;
                    }
                }
            }
        }
        free(bits);
        pen += (int)gm.gmCellIncX;
    }

    SelectObject(dc, old_font);
    DeleteObject(font);
    DeleteDC(dc);
    rc = 1;
    *mask = canvas;
    *w = cw;
    *h = chh;
    return rc;
}

int win32_gdi_text_mask_cp932(const char *utf8, int px, unsigned char **mask,
                              int *w, int *h)
{
    return win32_gdi_text_mask_original(utf8, NULL, px, mask, w, h);
}

int win32_gdi_text_advance_cp932(const char *utf8, int px)
{
    return win32_gdi_text_advance_original(utf8, NULL, px);
}

void win32_gdi_text_set_default_face(const char *utf8_face)
{
    if (utf8_face == NULL || *utf8_face == '\0') {
        return;
    }
    strncpy(g_default_face, utf8_face, sizeof g_default_face - 1u);
    g_default_face[sizeof g_default_face - 1u] = '\0';
}

const char *win32_gdi_text_default_face(void)
{
    return g_default_face;
}
