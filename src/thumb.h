/* 存档缩略图缩放：把渲染层的 BGRA 画面缩到 200×113（或任意 W×H）。
 *
 * 引擎侧是 `render_thumb()`（`0x4255C0`）把当前画面缩到 engine+0xFD0/0xFD4 指定的
 * 尺寸，然后按裸 BGRA 写进槽文件（`docs/save-format.md` §2.3），alpha 恒为 0xFF。
 *
 * 这里只做纯函数式的 box filter（面积平均）：缩放比很大时（1920×1080 → 200×113）
 * 采样平均比双线性更接近原图的整体亮度，且不依赖 gfx，方便单测。
 */
#ifndef FVP_THUMB_H
#define FVP_THUMB_H

#ifdef __cplusplus
extern "C" {
#endif

/* src/dst 都是 BGRA、逐行紧排。返回 1 成功，0 = 参数非法（尺寸 <= 0 / NULL）。
 * 输出的 alpha 一律写成 0xFF，与引擎一致。 */
int fvp_thumb_scale_bgra(const unsigned char *src, int sw, int sh,
                         unsigned char *dst, int dw, int dh);

#ifdef __cplusplus
}
#endif

#endif /* FVP_THUMB_H */