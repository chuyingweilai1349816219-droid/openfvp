#ifndef FVP_WIN32_GDI_TEXT_H
#define FVP_WIN32_GDI_TEXT_H

/* 用 GDI 把一串 UTF-8 文本栅格化成 8 位覆盖率掩码（缺字回退用）。
 * 成功返回 1 并通过 mask/w/h 交回（mask 由调用方 free）；失败返回 0。
 * 只在 Windows 桌面外壳里使用；核心库不依赖它。
 *
 * 这里有两套实现，差别是**光栅化协议**，不是参数微调：
 *   win32_gdi_text_*          抗锯齿路径：CreateFontW(ANTIALIASED_QUALITY,
 *                             DEFAULT_CHARSET) + TextOutW 到 32bpp DIB，
 *                             掩码是 0..255 的灰度覆盖率（汉化文本更清晰）。
 *   win32_gdi_text_*_original 原版同协议：CreateFontA(SHIFTJIS_CHARSET, 字重 0,
 *                             质量 0) + GetGlyphOutlineA（GGO_METRICS 取进位、
 *                             GGO_BITMAP 取 1bpp 位图）+ 单位 MAT2；掩码只有
 *                             0 / 255 两值。依据是静态提取出来的
 *                             ref/exe-gdi-params.json（见
 *                             docs/original-verification.md §8）。
 * 原版是 cp932 的 ANSI 程序，所以"原版协议"只对 cp932 能表示的字符成立；
 * cp932 表示不了的字符，原版自己也画不出来 —— 这种情况返回 0。 */
int win32_gdi_text_advance(const char *utf8, int px);
int win32_gdi_text_mask(const char *utf8, int px, unsigned char **mask, int *w,
                        int *h);

/* 原版协议。face 为 UTF-8 的字体族名（原版是数据驱动的，见 §8.3）；
 * face 为 NULL 时用默认的 "ＭＳ ゴシック"（exe 内置注册表的第 0 项）。 */
int win32_gdi_text_mask_original(const char *utf8, const char *face, int px,
                                 unsigned char **mask, int *w, int *h);
int win32_gdi_text_advance_original(const char *utf8, const char *face, int px);

/* 与 GfxGlyphFallback 兼容的适配器（固定用默认族名），方便外壳直接挂上。 */
int win32_gdi_text_mask_cp932(const char *utf8, int px, unsigned char **mask,
                              int *w, int *h);
int win32_gdi_text_advance_cp932(const char *utf8, int px);

/* 换默认族名（UTF-8）。传 NULL 或空串恢复默认。 */
void win32_gdi_text_set_default_face(const char *utf8_face);
const char *win32_gdi_text_default_face(void);

#endif /* FVP_WIN32_GDI_TEXT_H */