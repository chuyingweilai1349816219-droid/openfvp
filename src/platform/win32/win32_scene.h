/* FVP Win32 shell -- replay of a dumped render state (see win32_scene.c). */
#ifndef FVP_WIN32_SCENE_H
#define FVP_WIN32_SCENE_H

#include "gfx.h"
#include "win32_window.h"

#ifdef __cplusplus
extern "C" {
#endif

/* 读一帧场景二进制并灌进 gfx（prims / graphs / texts / text_layout）。 */
int fvp_scene_load(Gfx *gfx, const wchar_t *path);

/* 显示一帧，直到窗口关闭（ESC / 关闭按钮）。 */
int fvp_scene_present_loop(Gfx *gfx, FvpWin32Window *win);

#ifdef __cplusplus
}
#endif

#endif /* FVP_WIN32_SCENE_H */