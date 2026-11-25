/* FVP 跨平台 OS 缝（seam）——每个平台外壳必须提供的少数系统能力。
 *
 * 为什么需要这一层：引擎核心（vm/syscall/gfx/snd 的纯逻辑部分）不应当直接
 * include <windows.h>。桌面外壳里真正与操作系统打交道的只有这几件事，把它们
 * 收成一份契约后，新增平台（Android / Linux）只需要实现这个头文件 + 自己的
 * 窗口/音频输出，不需要改核心。
 *
 * 目前的实现：
 *   - src/platform/win32/  —— 桌面外壳（窗口、D3D、waveOut、GDI 缺字回退）
 *   - src/platform/portable/platform_portable.c —— 只用 C 标准库的参考实现，
 *     用于单测与"核心不依赖 Win32"的证明（tests/test_platform.c）
 *
 * 注意：窗口/渲染/音频输出**不在**这个最小缝里——它们天然与平台强相关，各自的
 * 外壳直接实现（见 docs/cross-platform.md 的清单）。
 */
#ifndef FVP_PLATFORM_H
#define FVP_PLATFORM_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* 单调时钟（秒）。只用于计时/帧间隔，不要求墙钟时间。 */
double fvp_plat_now_sec(void);

/* 单调时钟（毫秒，整数）。 */
unsigned long fvp_plat_ticks_ms(void);

/* 写一行日志（UTF-8，调用方负责换行）。平台决定落到哪里：桌面外壳写日志文件，
 * 便携实现写 stderr。 */
void fvp_plat_log(const char *utf8);

/* 读整个文件到 malloc 的缓冲（调用方 free）。成功返回 1。 */
int fvp_plat_read_file(const char *path, unsigned char **data, size_t *len);

#ifdef __cplusplus
}
#endif

#endif /* FVP_PLATFORM_H */