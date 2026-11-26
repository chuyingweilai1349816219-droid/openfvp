/* FVP Win32 shell -- replay one dumped render state in a real window.
 *
 * 用途：`bin/fvp.exe --scene out/c/scene.bin` 把 work/scene_to_bin.py 导出的
 * 一帧渲染状态（prims / graphs / texts / text_layout）灌进 src/gfx.c，然后在
 * 真正的 1920x1080 窗口里显示出来。它不跑 VM，只用于"人眼验收"某一帧：
 * 位置、对话框、字号、层序都来自 dump，和离线 PNG 是同一份状态。
 *
 * 场景二进制格式见 work/scene_to_bin.py 顶部（小端，magic "FVPS"）。
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>

#include "gfx.h"
#include "win32_scene.h"
#include "win32_window.h"

typedef struct SceneReader {
    FILE *fp;
    int bad;
} SceneReader;

static int rd_i32(SceneReader *r, int *v)
{
    unsigned char b[4];

    if (r->bad || fread(b, 1, 4, r->fp) != 4) {
        r->bad = 1;
        return 0;
    }
    *v = (int)((unsigned)b[0] | ((unsigned)b[1] << 8) |
               ((unsigned)b[2] << 16) | ((unsigned)b[3] << 24));
    return 1;
}

static char *rd_str(SceneReader *r)
{
    int n = 0;
    char *s;

    if (!rd_i32(r, &n) || n < 0 || n > (1 << 20)) {
        r->bad = 1;
        return NULL;
    }
    s = (char *)malloc((size_t)n + 1);
    if (s == NULL) {
        r->bad = 1;
        return NULL;
    }
    if (n > 0 && fread(s, 1, (size_t)n, r->fp) != (size_t)n) {
        free(s);
        r->bad = 1;
        return NULL;
    }
    s[n] = '\0';
    return s;
}

/* 读一帧场景并灌进 gfx。返回 1 = 成功。 */
int fvp_scene_load(Gfx *gfx, const wchar_t *path)
{
    static const int prim_fields[] = {
        GFX_F_TYPE, GFX_F_DRAW, GFX_F_ALPHA, GFX_F_BLEND, GFX_F_Z,
        GFX_F_X, GFX_F_Y, GFX_F_W, GFX_F_H, GFX_F_U, GFX_F_V,
        GFX_F_OP1, GFX_F_OP2, GFX_F_ANGLE, GFX_F_SCALE_X, GFX_F_SCALE_Y,
        GFX_F_GRAPH, GFX_F_TW, GFX_F_PARENT
    };
    SceneReader r;
    char magic[5];
    int version = 0;
    int i;
    int n = 0;

    if (gfx == NULL || path == NULL) {
        return 0;
    }
    r.fp = _wfopen(path, L"rb");
    r.bad = 0;
    if (r.fp == NULL) {
        return 0;
    }
    memset(magic, 0, sizeof magic);
    if (fread(magic, 1, 4, r.fp) != 4 || memcmp(magic, "FVPS", 4) != 0 ||
        !rd_i32(&r, &version) || (version != 1 && version != 2)) {
        fclose(r.fp);
        return 0;
    }
    /* canvas / background：canvas 取自窗口侧（1920x1080 固定），这里只吃 bg。 */
    {
        int cw = 0, ch = 0, bg[4];

        if (!rd_i32(&r, &cw) || !rd_i32(&r, &ch)) {
            fclose(r.fp);
            return 0;
        }
        for (i = 0; i < 4; ++i) {
            if (!rd_i32(&r, &bg[i])) {
                fclose(r.fp);
                return 0;
            }
        }
        gfx_set_background(gfx, bg[0], bg[1], bg[2], bg[3]);
    }
    /* graphs */
    if (!rd_i32(&r, &n)) {
        fclose(r.fp);
        return 0;
    }
    for (i = 0; i < n && !r.bad; ++i) {
        int slot = 0, w = 0, h = 0, gx = 0, gy = 0, frames = 1, frame = 0;
        char *name = NULL;

        if (!rd_i32(&r, &slot)) {
            break;
        }
        name = rd_str(&r);
        if (r.bad) {
            break;
        }
        if (!rd_i32(&r, &w) || !rd_i32(&r, &h) || !rd_i32(&r, &gx) ||
            !rd_i32(&r, &gy) || !rd_i32(&r, &frames) ||
            !rd_i32(&r, &frame)) {
            free(name);
            break;
        }
        if (slot >= 0 && slot < GFX_GRAPH_MAX && name != NULL &&
            name[0] != '\0') {
            (void)gfx_graph_load(gfx, slot, name);
            if (frames > 1) {
                (void)gfx_graph_set_frame(gfx, slot, frame);
            }
        }
        free(name);
    }
    /* prims */
    if (!r.bad && rd_i32(&r, &n)) {
        for (i = 0; i < n && !r.bad; ++i) {
            int v[20];
            int k;

            for (k = 0; k < 20; ++k) {
                if (!rd_i32(&r, &v[k])) {
                    break;
                }
            }
            if (r.bad) {
                break;
            }
            if (v[0] < 0 || v[0] >= GFX_PRIM_MAX) {
                continue;
            }
            gfx_prim_reset(gfx, v[0]);
            for (k = 0; k < 19; ++k) {
                gfx_set_prim(gfx, v[0], prim_fields[k], v[k + 1]);
            }
        }
    }
    /* texts */
    if (!r.bad && rd_i32(&r, &n)) {
        for (i = 0; i < n && !r.bad; ++i) {
            int idx = 0, col[3], size[2], out[2], has_pos = 0, pos[2] = {0, 0};
            int panel[2] = {0, 0};
            int nlines = 0, k;

            if (!rd_i32(&r, &idx)) {
                break;
            }
            for (k = 0; k < 3; ++k) {
                if (!rd_i32(&r, &col[k])) {
                    break;
                }
            }
            for (k = 0; k < 2; ++k) {
                if (!rd_i32(&r, &size[k])) {
                    break;
                }
            }
            for (k = 0; k < 2; ++k) {
                if (!rd_i32(&r, &out[k])) {
                    break;
                }
            }
            if (version >= 2) {
                if (!rd_i32(&r, &panel[0]) || !rd_i32(&r, &panel[1])) {
                    break;
                }
            }
            if (r.bad || !rd_i32(&r, &has_pos) || !rd_i32(&r, &pos[0]) ||
                !rd_i32(&r, &pos[1]) || !rd_i32(&r, &nlines)) {
                break;
            }
            if (idx >= 0 && idx < GFX_TEXT_MAX) {
                gfx_text_clear(gfx, idx);
                gfx_text_set_color(gfx, idx, col[0], col[1], col[2]);
                gfx_text_set_size(gfx, idx, size[0], size[1]);
                gfx_text_set_out_size(gfx, idx, out[0], out[1]);
                if (has_pos) {
                    gfx_text_set_pos(gfx, idx, pos[0], pos[1]);
                }
                if (panel[0] > 0 && panel[1] > 0) {
                    (void)gfx_text_set_buff(gfx, idx, panel[0], panel[1]);
                }
            }
            for (k = 0; k < nlines && !r.bad; ++k) {
                char *line = rd_str(&r);

                if (line == NULL) {
                    break;
                }
                if (idx >= 0 && idx < GFX_TEXT_MAX) {
                    gfx_text_append(gfx, idx, line);
                }
                free(line);
            }
        }
    }
    /* text_layout */
    if (!r.bad && rd_i32(&r, &n)) {
        for (i = 0; i < n && !r.bad; ++i) {
            int idx = 0, x = 0, y = 0, maxw = 1920, size = 32, lines = 1;
            GfxTextLayout lay;

            if (!rd_i32(&r, &idx) || !rd_i32(&r, &x) || !rd_i32(&r, &y) ||
                !rd_i32(&r, &maxw) || !rd_i32(&r, &size) ||
                !rd_i32(&r, &lines)) {
                break;
            }
            if (idx < 0 || idx >= GFX_TEXT_MAX) {
                continue;
            }
            lay.x = x;
            lay.y = y;
            lay.max_w = maxw;
            lay.size = size;
            lay.max_lines = lines;
            (void)gfx_text_set_layout(gfx, idx, &lay);
        }
    }
    fclose(r.fp);
    return r.bad ? 0 : 1;
}

/* 只要窗口还活着就一直重画这一帧（不推进任何状态）。 */
int fvp_scene_present_loop(Gfx *gfx, FvpWin32Window *win)
{
    if (gfx == NULL || win == NULL) {
        return 0;
    }
    while (!win->quit) {
        const uint32_t *canvas;
        int cw = 0;
        int ch = 0;

        if (!fvp_win32_pump(win)) {
            break;
        }
        gfx_render(gfx);
        canvas = gfx_canvas(gfx, &cw, &ch);
        if (canvas != NULL) {
            (void)fvp_win32_present(win, canvas, cw, ch);
        }
        Sleep(16);
    }
    return 1;
}