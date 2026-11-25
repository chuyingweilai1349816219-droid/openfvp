#ifndef FVP_WIN32_GDI_TEXT_H
#define FVP_WIN32_GDI_TEXT_H

/* 用 GDI 把一串 UTF-8 文本栅格化成 8 位覆盖率掩码（缺字回退用）。
 * 成功返回 1 并通过 mask/w/h 交回（mask 由调用方 free）；失败返回 0。
 * 只在 Windows 桌面外壳里使用；核心库不依赖它。 */
int win32_gdi_text_advance(const char *utf8, int px);
int win32_gdi_text_mask(const char *utf8, int px, unsigned char **mask, int *w,
                        int *h);

#endif /* FVP_WIN32_GDI_TEXT_H */