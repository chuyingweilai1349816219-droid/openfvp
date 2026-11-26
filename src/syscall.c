/* FVP VM in C -- syscall dispatch.
 *
 * The reference implementation (fvp/ref/fvm_ref.py) implements only a small
 * subset of the 148 commands; everything else is a stub.  This file mirrors
 * that behaviour exactly, including:
 *
 *   - argc comes from cmd_argc.json (id -> argument count), the same file the
 *     Python VM loads via --cmds;
 *   - the return value goes to `temp` (not the operand stack);
 *   - commands whose handler returns None leave `temp` untouched unless the
 *     command name is in the reference's "returns a value" set, in which case
 *     temp becomes 0;
 *   - unimplemented commands whose name ends in "Test" report "finished" (1).
 */
#include "syscall.h"

#include <limits.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "gfx.h"
#include "motion.h"
#include "gen_cmd_table.h"
#include "snd.h"

/* The render layer is optional: fvm.exe links without src/gfx.o, so every
 * gfx entry point this file references is a weak symbol.  They are only
 * called when fvp_vm_attach_gfx() installed a context, and a context can
 * only be installed by a program that links the renderer. */
#pragma weak gfx_graph_load
#pragma weak gfx_gaiji_add
#pragma weak gfx_graph_set_frame
#pragma weak gfx_parts_assign
#pragma weak gfx_parts_load
#pragma weak gfx_parts_select
#pragma weak gfx_prim_group_in
#pragma weak gfx_prim_group_out
#pragma weak gfx_prim_reset
#pragma weak gfx_set_prim
#pragma weak gfx_text_append
#pragma weak gfx_text_clear
#pragma weak gfx_text_set_color
#pragma weak gfx_text_set_out_size
#pragma weak gfx_text_set_buff
#pragma weak gfx_text_set_pos
#pragma weak gfx_text_set_pos_ex
#pragma weak gfx_text_set_size
#pragma weak gfx_text_set_size_ex
#pragma weak gfx_text_set_speed
#pragma weak gfx_text_set_shadow
#pragma weak gfx_text_set_space
#pragma weak gfx_text_set_font
#pragma weak gfx_text_set_format
#pragma weak gfx_text_set_function
#pragma weak gfx_text_set_pause
#pragma weak gfx_text_busy
/* 动画补间（src/motion.c + gfx 侧）：只有链接了 gfx.c 的程序才有这些符号。 */
#pragma weak gfx_get_prim
#pragma weak gfx_prim_hit
#pragma weak gfx_motion_add
#pragma weak gfx_motion_cancel
#pragma weak gfx_motion_enabled

/* Same story for the audio layer (src/snd.c): bin/fvm.exe links neither it
 * nor the BIN package reader, so every symbol is weak here and no audio code
 * can run unless fvp_vm_attach_snd() installed a context. */
#pragma weak snd_is_playing
#pragma weak snd_crossfade
#pragma weak snd_play_ogg
#pragma weak snd_play_wav
#pragma weak snd_set_vol
#pragma weak snd_stop

/* 取 UTF-8 串的第一个码点（外字登记用；非法/空串返回 0）。 */
static uint32_t fvp_utf8_first(const char *s)
{
    const unsigned char *p = (const unsigned char *)s;

    if (p == NULL || p[0] == 0) {
        return 0u;
    }
    if (p[0] < 0x80u) {
        return (uint32_t)p[0];
    }
    if ((p[0] & 0xE0u) == 0xC0u && (p[1] & 0xC0u) == 0x80u) {
        return ((uint32_t)(p[0] & 0x1Fu) << 6) | (uint32_t)(p[1] & 0x3Fu);
    }
    if ((p[0] & 0xF0u) == 0xE0u && (p[1] & 0xC0u) == 0x80u &&
        (p[2] & 0xC0u) == 0x80u) {
        return ((uint32_t)(p[0] & 0x0Fu) << 12) |
               ((uint32_t)(p[1] & 0x3Fu) << 6) | (uint32_t)(p[2] & 0x3Fu);
    }
    if ((p[0] & 0xF8u) == 0xF0u && (p[1] & 0xC0u) == 0x80u &&
        (p[2] & 0xC0u) == 0x80u && (p[3] & 0xC0u) == 0x80u) {
        return ((uint32_t)(p[0] & 0x07u) << 18) |
               ((uint32_t)(p[1] & 0x3Fu) << 12) |
               ((uint32_t)(p[2] & 0x3Fu) << 6) | (uint32_t)(p[3] & 0x3Fu);
    }
    return 0u;
}

static FvpStr *fvp_sys_str(FvpVM *vm, const char *s)
{
    return fvp_arena_str(&vm->arena, s, strlen(s));
}

/* cmd_123 TextPrint(layer, text) */
static void fvp_sys_text_print(FvpVM *vm, const FvpValue *args, uint32_t argc)
{
    FvpValue layer = argc > 0 ? args[0] : fvp_make_int(0);
    FvpValue txt = argc > 1 ? args[1]
                            : fvp_make_str(fvp_sys_str(vm, ""));
    if (txt.type != (uint8_t)FVP_STR) {
        txt = fvp_make_str(fvp_value_to_str(&vm->arena, &txt));
    }
    fvp_vm_add_text(vm, &layer, txt.str);
}

/* Python repr() of a value, best effort (used only by cmd_13 Debmess when
 * stdout is not quiet; not part of the byte-exact contract). */
static void fvp_sys_repr(FvpVM *vm, const FvpValue *v, FILE *out)
{
    if (v->type == (uint8_t)FVP_INT) {
        fprintf(out, "%lld", (long long)v->i);
    } else if (v->type == (uint8_t)FVP_FLOAT) {
        FvpStr *s = fvp_float_repr(&vm->arena, v->f);
        fwrite(s->data, 1, s->len, out);
    } else {
        const FvpStr *s = v->str;
        uint32_t i;
        fputc('\'', out);
        for (i = 0; i < s->len; ++i) {
            unsigned char c = (unsigned char)s->data[i];
            if (c == '\'' || c == '\\') {
                fputc('\\', out);
                fputc((int)c, out);
            } else if (c == '\n') {
                fputs("\\n", out);
            } else if (c == '\r') {
                fputs("\\r", out);
            } else if (c == '\t') {
                fputs("\\t", out);
            } else if (c < 0x20u) {
                fprintf(out, "\\x%02x", (unsigned)c);
            } else {
                fputc((int)c, out);
            }
        }
        fputc('\'', out);
    }
}

/* cmd_13 Debmess([...]) */
static void fvp_sys_debmess(FvpVM *vm, const FvpValue *args, uint32_t argc)
{
    uint32_t i;
    if (vm->quiet) {
        return;
    }
    fputs("[debmess] [", stdout);
    for (i = 0; i < argc; ++i) {
        if (i != 0) {
            fputs(", ", stdout);
        }
        fvp_sys_repr(vm, &args[i], stdout);
    }
    fputs("]\n", stdout);
}

/* Python: self.names.get(cid, '').endswith('Test') */
static int fvp_sys_is_test(uint16_t cid)
{
    const char *name;
    size_t n;
    if (cid >= FVP_CMD_COUNT) {
        return 0;
    }
    name = fvp_cmd_names[cid];
    n = strlen(name);
    return n >= 4 && strcmp(name + n - 4, "Test") == 0;
}

/* ------------------------------------------------------------------ */
/* Optional renderer forwarding (docs/c-gfx.md section 8)              */
/*                                                                     */
/* Every rendering command is a no-op here unless fvp_vm_attach_gfx()   */
/* installed a Gfx context, so bin/fvm.exe keeps its exact prototype    */
/* behaviour (unknown-command counters, temp values, text log).  The    */
/* gfx calls mirror work/render.py's handler table one by one.          */
/* ------------------------------------------------------------------ */

static int fvp_gfx_is_int(const FvpValue *v)
{
    /* render.py's _type(v) == 2: only plain ints are applied; floats and
     * strings are ignored by PrimSet* / Motion* (same as the engine's
     * Variant checks). */
    return v->type == (uint8_t)FVP_INT;
}

/* Python int(v) as render.py's _int() does it: int -> itself, float ->
 * truncate towards zero, everything else -> 0. */
static long long fvp_gfx_ll(const FvpValue *v)
{
    if (v->type == (uint8_t)FVP_INT) {
        return v->i;
    }
    if (v->type == (uint8_t)FVP_FLOAT) {
        double d = v->f;
        if (!isfinite(d)) {
            return 0;
        }
        if (d >= 9223372036854775807.0) {
            return LLONG_MAX;
        }
        if (d <= -9223372036854775808.0) {
            return LLONG_MIN;
        }
        return (long long)d;
    }
    return 0;
}

static int fvp_gfx_int(const FvpValue *v)
{
    long long x = fvp_gfx_ll(v);
    if (x > INT_MAX) {
        return INT_MAX;
    }
    if (x < INT_MIN) {
        return INT_MIN;
    }
    return (int)x;
}

/* gfx_prim_reset() is the engine initialiser minus the Z field (gfx leaves
 * it 0; render.py's Prim.reset() sets 1000).  Add the missing field so the
 * C state matches the Python model exactly. */
static void fvp_gfx_prim_reset(Gfx *g, int id)
{
    if (gfx_prim_reset(g, id) == 0) {
        (void)gfx_set_prim(g, id, GFX_F_Z, 1000);
    }
}

static void fvp_gfx_prim_reset_shadow(FvpGfxBridge *b, int id)
{
    if (id >= 0 && id < FVP_GFX_PRIM_MAX) {
        b->prim_graph[id] = -1;
    }
}

/* render.py loads work/cmd_table.json argc values, while the byte-exact VM
 * uses work/cmd_argc.json (gen_cmd_table.h).  These six ids disagree; the
 * renderer gets the arguments from the *top of the operand stack* instead of
 * from the VM's pop count, so the VM's stack discipline is untouched. */
static int fvp_gfx_argc(uint16_t cid, uint32_t argc)
{
    switch (cid) {
    case 49: /* MotionMoveStop: cmd_table 1, cmd_argc 6 */
        return 1;
    case 51: /* MotionMoveR:     cmd_table 6, cmd_argc 1 */
        return 6;
    case 52: /* MotionMoveRStop: cmd_table 1, cmd_argc 8 */
        return 1;
    case 54: /* MotionMoveS2:    cmd_table 8, cmd_argc 1 */
        return 8;
    case 94: /* SaveThumbSize:   cmd_table 2, cmd_argc 3 */
        return 2;
    case 95: /* SaveData:        cmd_table 3, cmd_argc 2 */
        return 3;
    default:
        return (int)argc;
    }
}

static int fvp_sys_gfx(FvpVM *vm, FvpThread *t, const FvpValue *a,
                       uint32_t argc, uint16_t cid)
{
    FvpGfxBridge *b = vm->gfx_bridge;
    Gfx *g = (b != NULL) ? b->gfx : NULL;
    int id;

    if (g == NULL) {
        return 0;
    }
    id = (argc > 0) ? fvp_gfx_int(&a[0]) : 0;

    switch (cid) {
    case 7: /* ColorSet(idx, R, G, B, A): the renderer never samples the
             * palette, render.py only records it */
        break;
    case 20: /* FloatToInt(v): render.py returns int(v); the byte-exact VM
              * leaves this command unimplemented because no text transcript
              * depends on it -- but the picture does (the script feeds
              * coordinates such as FloatToInt(84.375) into PrimSetXY). */
        if (argc > 0 && a[0].type != (uint8_t)FVP_STR) {
            t->temp = fvp_make_int(fvp_gfx_ll(&a[0]));
        } else {
            t->temp = fvp_make_int(0);
        }
        break;
    case 22: /* GraphLoad(slot, name): only a non-empty string loads.  The
              * engine's null release and int/bool placeholders are no-ops in
              * the reference prototype (a "null" has no C value). */
        if (argc > 1 && a[1].type == (uint8_t)FVP_STR && a[1].str->len > 0) {
            (void)gfx_graph_load(g, id, a[1].str->data);
        }
        break;
    case 42: /* MotionAlpha(id, from, to, ms, mode, flag): take `to` */
        if (gfx_motion_enabled != NULL && gfx_motion_enabled(g) && argc > 3 &&
            fvp_gfx_is_int(&a[2]) && fvp_gfx_is_int(&a[3])) {
            int from = 255;

            (void)gfx_get_prim(g, id, GFX_F_ALPHA, &from);
            (void)gfx_motion_add(g, id, FVP_TWEEN_ALPHA, from, 0,
                                 fvp_gfx_int(&a[2]), 0, fvp_gfx_int(&a[3]),
                                 argc > 4 ? fvp_gfx_int(&a[4]) : 1);
        } else if (argc > 2 && fvp_gfx_is_int(&a[2])) {
            (void)gfx_set_prim(g, id, GFX_F_ALPHA, fvp_gfx_int(&a[2]));
        } else if (argc > 1 && fvp_gfx_is_int(&a[1])) {
            (void)gfx_set_prim(g, id, GFX_F_ALPHA, fvp_gfx_int(&a[1]));
        }
        break;
    case 45: /* MotionAnim(id, animId, ms, loop) */
        if (argc > 1 && fvp_gfx_is_int(&a[1]) && id >= 0 &&
            id < FVP_GFX_PRIM_MAX) {
            int slot = (int)b->prim_graph[id];
            if (slot >= 0) {
                (void)gfx_graph_set_frame(g, slot, fvp_gfx_int(&a[1]));
            }
        }
        break;
    case 48: /* MotionMove(id, x, y, z, ?, ms, mode, flag)：开启动画时插补间 */
        if (getenv("FVP_DEBUG_TEXT") != NULL) {
            fprintf(stderr,
                    "MotionMove prim=%d x=%d y=%d z=%d ms=%d mode=%d%s steps=%llu\n", id,
                    argc > 1 ? fvp_gfx_int(&a[1]) : -1,
                    argc > 2 ? fvp_gfx_int(&a[2]) : -1,
                    argc > 3 ? fvp_gfx_int(&a[3]) : -1,
                    argc > 5 ? fvp_gfx_int(&a[5]) : -1,
                    argc > 6 ? fvp_gfx_int(&a[6]) : -1,
                    (gfx_motion_enabled != NULL && gfx_motion_enabled(g))
                        ? " (tween)" : " (instant)",
                    (unsigned long long)vm->steps);
        }
        if (gfx_motion_enabled != NULL && gfx_motion_enabled(g) && argc > 5 &&
            fvp_gfx_is_int(&a[5]) && (fvp_gfx_is_int(&a[1]) ||
                                      fvp_gfx_is_int(&a[2]))) {
            int cx = 0;
            int cy = 0;
            int tx = fvp_gfx_is_int(&a[1]) ? fvp_gfx_int(&a[1]) : 0;
            int ty = fvp_gfx_is_int(&a[2]) ? fvp_gfx_int(&a[2]) : 0;

            (void)gfx_get_prim(g, id, GFX_F_X, &cx);
            (void)gfx_get_prim(g, id, GFX_F_Y, &cy);
            if (!fvp_gfx_is_int(&a[1])) {
                tx = cx;
            }
            if (!fvp_gfx_is_int(&a[2])) {
                ty = cy;
            }
            (void)gfx_motion_add(g, id, FVP_TWEEN_MOVE_XY, cx, cy, tx, ty,
                                 fvp_gfx_int(&a[5]),
                                 argc > 6 ? fvp_gfx_int(&a[6]) : 1);
            if (fvp_gfx_is_int(&a[3])) {
                (void)gfx_set_prim(g, id, GFX_F_Z, fvp_gfx_int(&a[3]));
            }
        } else {
            if (argc > 1 && fvp_gfx_is_int(&a[1])) {
                (void)gfx_set_prim(g, id, GFX_F_X, fvp_gfx_int(&a[1]));
            }
            if (argc > 2 && fvp_gfx_is_int(&a[2])) {
                (void)gfx_set_prim(g, id, GFX_F_Y, fvp_gfx_int(&a[2]));
            }
            if (argc > 3 && fvp_gfx_is_int(&a[3])) {
                (void)gfx_set_prim(g, id, GFX_F_Z, fvp_gfx_int(&a[3]));
            }
        }
        break;
    case 49: /* MotionMoveStop: no renderer state in the prototype */
    case 52: /* MotionMoveRStop */
        break;
    case 51: /* MotionMoveR(id, x, y, ...) */
        if (getenv("FVP_DEBUG_TEXT") != NULL) {
            fprintf(stderr, "MotionMoveR prim=%d x=%d y=%d\n", id,
                    argc > 1 ? fvp_gfx_int(&a[1]) : -1,
                    argc > 2 ? fvp_gfx_int(&a[2]) : -1);
        }
        if (argc > 1 && fvp_gfx_is_int(&a[1])) {
            (void)gfx_set_prim(g, id, GFX_F_X, fvp_gfx_int(&a[1]));
        }
        if (argc > 2 && fvp_gfx_is_int(&a[2])) {
            (void)gfx_set_prim(g, id, GFX_F_Y, fvp_gfx_int(&a[2]));
        }
        break;
    case 54: /* MotionMoveS2(id, sx, sy, ?, ?, ms, mode, flag) */
        if (gfx_motion_enabled != NULL && gfx_motion_enabled(g) && argc > 5 &&
            fvp_gfx_is_int(&a[5]) && (fvp_gfx_is_int(&a[1]) ||
                                      fvp_gfx_is_int(&a[2]))) {
            int csx = 1000;
            int csy = 1000;
            int tsx = fvp_gfx_is_int(&a[1]) ? fvp_gfx_int(&a[1]) : 1000;
            int tsy = fvp_gfx_is_int(&a[2]) ? fvp_gfx_int(&a[2]) : 1000;

            (void)gfx_get_prim(g, id, GFX_F_SCALE_X, &csx);
            (void)gfx_get_prim(g, id, GFX_F_SCALE_Y, &csy);
            if (!fvp_gfx_is_int(&a[1])) {
                tsx = csx;
            }
            if (!fvp_gfx_is_int(&a[2])) {
                tsy = csy;
            }
            (void)gfx_motion_add(g, id, FVP_TWEEN_SCALE, csx, csy, tsx, tsy,
                                 fvp_gfx_int(&a[5]),
                                 argc > 6 ? fvp_gfx_int(&a[6]) : 1);
        } else {
            if (argc > 1 && fvp_gfx_is_int(&a[1])) {
                (void)gfx_set_prim(g, id, GFX_F_SCALE_X, fvp_gfx_int(&a[1]));
            }
            if (argc > 2 && fvp_gfx_is_int(&a[2])) {
                (void)gfx_set_prim(g, id, GFX_F_SCALE_Y, fvp_gfx_int(&a[2]));
            }
        }
        break;
    case 57: /* MotionMoveZ(id, z, ...) */
        if (argc > 1 && fvp_gfx_is_int(&a[1])) {
            (void)gfx_set_prim(g, id, GFX_F_Z, fvp_gfx_int(&a[1]));
        }
        break;
    case 64: /* PartsAssign(partsId, graphSlot) */
        if (argc > 1) {
            (void)gfx_parts_assign(g, id, fvp_gfx_int(&a[1]));
        }
        break;
    case 65: /* PartsLoad(partsId, name): a falsy name drops the part, a
              * non-empty string loads it; other truthy values are ignored
              * (render.py's asset lookup fails and leaves the part alone). */
        if (argc > 1 && a[1].type == (uint8_t)FVP_STR) {
            if (a[1].str->len == 0) {
                (void)gfx_parts_load(g, id, NULL);
            } else {
                (void)gfx_parts_load(g, id, a[1].str->data);
            }
        } else if (argc > 1 && !fvp_value_truth(&a[1])) {
            (void)gfx_parts_load(g, id, NULL);
        }
        break;
    case 71: /* PartsSelect(partsId, frame) */
        if (argc > 1) {
            (void)gfx_parts_select(g, id, fvp_gfx_int(&a[1]));
        }
        break;
    case 72: /* PrimExitGroup: recorded by the engine, unused by the
              * reference compositor */
        break;
    case 73: /* PrimGroupIn(child, parent) */
        if (argc > 1) {
            (void)gfx_prim_group_in(g, id, fvp_gfx_int(&a[1]));
        }
        break;
    case 75: /* PrimGroupOut(id) */
        (void)gfx_prim_group_out(g, id);
        break;
    case 77: /* PrimSetAlpha(id, alpha) */
        if (argc > 1 && fvp_gfx_is_int(&a[1])) {
            (void)gfx_set_prim(g, id, GFX_F_ALPHA, fvp_gfx_int(&a[1]));
        }
        break;
    case 78: /* PrimSetBlend(id, blend) */
        if (argc > 1 && fvp_gfx_is_int(&a[1])) {
            (void)gfx_set_prim(g, id, GFX_F_BLEND, fvp_gfx_int(&a[1]));
        }
        break;
    case 79: /* PrimSetDraw(id, draw) */
        if (argc > 1 && fvp_gfx_is_int(&a[1])) {
            (void)gfx_set_prim(g, id, GFX_F_DRAW, fvp_gfx_int(&a[1]));
        }
        break;
    case 80: /* PrimSetNull(id) */
        fvp_gfx_prim_reset(g, id);
        fvp_gfx_prim_reset_shadow(b, id);
        break;
    case 81: /* PrimSetOP(id, op1, op2) */
        if (argc > 1 && fvp_gfx_is_int(&a[1])) {
            (void)gfx_set_prim(g, id, GFX_F_OP1, fvp_gfx_int(&a[1]));
        }
        if (argc > 2 && fvp_gfx_is_int(&a[2])) {
            (void)gfx_set_prim(g, id, GFX_F_OP2, fvp_gfx_int(&a[2]));
        }
        break;
    case 82: /* PrimSetRS(id, angle, scale) */
        if (argc > 1 && fvp_gfx_is_int(&a[1])) {
            (void)gfx_set_prim(g, id, GFX_F_ANGLE, fvp_gfx_int(&a[1]));
        }
        if (argc > 2 && fvp_gfx_is_int(&a[2])) {
            int s = fvp_gfx_int(&a[2]);
            (void)gfx_set_prim(g, id, GFX_F_SCALE_X, s);
            (void)gfx_set_prim(g, id, GFX_F_SCALE_Y, s);
        }
        break;
    case 83: /* PrimSetRS2(id, angle, sx, sy) */
        if (argc > 1 && fvp_gfx_is_int(&a[1])) {
            (void)gfx_set_prim(g, id, GFX_F_ANGLE, fvp_gfx_int(&a[1]));
        }
        if (argc > 2 && fvp_gfx_is_int(&a[2])) {
            (void)gfx_set_prim(g, id, GFX_F_SCALE_X, fvp_gfx_int(&a[2]));
        }
        if (argc > 3 && fvp_gfx_is_int(&a[3])) {
            (void)gfx_set_prim(g, id, GFX_F_SCALE_Y, fvp_gfx_int(&a[3]));
        }
        break;
    case 85: /* PrimSetSprt(id, graphSlot, x?, y?) */
        fvp_gfx_prim_reset(g, id);
        fvp_gfx_prim_reset_shadow(b, id);
        (void)gfx_set_prim(g, id, GFX_F_TYPE, 4);
        if (argc > 1) {
            (void)gfx_set_prim(g, id, GFX_F_GRAPH, fvp_gfx_int(&a[1]));
            if (id >= 0 && id < FVP_GFX_PRIM_MAX) {
                b->prim_graph[id] = fvp_gfx_int(&a[1]);
            }
        }
        if (argc > 2 && fvp_gfx_is_int(&a[2])) {
            (void)gfx_set_prim(g, id, GFX_F_X, fvp_gfx_int(&a[2]));
        }
        if (argc > 3 && fvp_gfx_is_int(&a[3])) {
            (void)gfx_set_prim(g, id, GFX_F_Y, fvp_gfx_int(&a[3]));
        }
        break;
    case 86: /* PrimSetText(id, textWindow, x?, y?) */
        fvp_gfx_prim_reset(g, id);
        fvp_gfx_prim_reset_shadow(b, id);
        if (getenv("FVP_DEBUG_TEXT") != NULL) {
            fprintf(stderr, "PrimSetText prim=%d tw=%d x=%d y=%d\n", id,
                    argc > 1 ? fvp_gfx_int(&a[1]) : 0,
                    argc > 2 ? fvp_gfx_int(&a[2]) : -1,
                    argc > 3 ? fvp_gfx_int(&a[3]) : -1);
        }
        (void)gfx_set_prim(g, id, GFX_F_TYPE, 5);
        (void)gfx_set_prim(g, id, GFX_F_TW,
                           argc > 1 ? fvp_gfx_int(&a[1]) : 0);
        /* 原版 0x43BDF5：`movb $0xff,0x7(%eax)` —— 建文本图元时 alpha 直接置 255。 */
        (void)gfx_set_prim(g, id, GFX_F_ALPHA, 255);
        if (argc > 2 && fvp_gfx_is_int(&a[2])) {
            (void)gfx_set_prim(g, id, GFX_F_X, fvp_gfx_int(&a[2]));
        }
        if (argc > 3 && fvp_gfx_is_int(&a[3])) {
            (void)gfx_set_prim(g, id, GFX_F_Y, fvp_gfx_int(&a[3]));
        }
        break;
    case 87: /* PrimSetTile(id, tile) */
        fvp_gfx_prim_reset(g, id);
        fvp_gfx_prim_reset_shadow(b, id);
        (void)gfx_set_prim(g, id, GFX_F_TYPE, 2);
        if (argc > 1 && fvp_gfx_is_int(&a[1])) {
            (void)gfx_set_prim(g, id, GFX_F_TILE,
                               fvp_gfx_int(&a[1]) & 0xFF);
        }
        break;
    case 88: /* PrimSetUV(id, u, v) */
        if (argc > 1 && fvp_gfx_is_int(&a[1])) {
            (void)gfx_set_prim(g, id, GFX_F_U, fvp_gfx_int(&a[1]));
        }
        if (argc > 2 && fvp_gfx_is_int(&a[2])) {
            (void)gfx_set_prim(g, id, GFX_F_V, fvp_gfx_int(&a[2]));
        }
        break;
    case 89: /* PrimSetWH(id, w, h) */
        if (getenv("FVP_DEBUG_TEXT") != NULL) {
            fprintf(stderr, "PrimSetWH prim=%d w=%d h=%d\n", id,
                    argc > 1 ? fvp_gfx_int(&a[1]) : -1,
                    argc > 2 ? fvp_gfx_int(&a[2]) : -1);
        }
        if (argc > 1 && fvp_gfx_is_int(&a[1])) {
            (void)gfx_set_prim(g, id, GFX_F_W, fvp_gfx_int(&a[1]));
        }
        if (argc > 2 && fvp_gfx_is_int(&a[2])) {
            (void)gfx_set_prim(g, id, GFX_F_H, fvp_gfx_int(&a[2]));
        }
        break;
    case 90: /* PrimSetXY(id, x, y) */
        if (getenv("FVP_DEBUG_TEXT") != NULL) {
            fprintf(stderr, "PrimSetXY prim=%d x=%d y=%d\n", id,
                    argc > 1 ? fvp_gfx_int(&a[1]) : -1,
                    argc > 2 ? fvp_gfx_int(&a[2]) : -1);
        }
        if (argc > 1 && fvp_gfx_is_int(&a[1])) {
            (void)gfx_set_prim(g, id, GFX_F_X, fvp_gfx_int(&a[1]));
        }
        if (argc > 2 && fvp_gfx_is_int(&a[2])) {
            (void)gfx_set_prim(g, id, GFX_F_Y, fvp_gfx_int(&a[2]));
        }
        break;
    case 91: /* PrimSetZ(id, z) */
        if (argc > 1 && fvp_gfx_is_int(&a[1])) {
            (void)gfx_set_prim(g, id, GFX_F_Z, fvp_gfx_int(&a[1]));
        }
        break;
    case 111: /* TextClear(layer) */
        if (getenv("FVP_DEBUG_TEXT") != NULL) {
            fprintf(stderr, "TextClear layer=%d\n", id);
        }
        gfx_text_clear(g, id);
        if (b->on_text != NULL) {
            b->on_text(b->text_ud, id, NULL, 1);
        }
        break;
    case 112: /* TextColor(layer, R, G, B) */
        if (argc > 3) {
            gfx_text_set_color(g, id, fvp_gfx_int(&a[1]),
                               fvp_gfx_int(&a[2]), fvp_gfx_int(&a[3]));
        }
        break;
    case 120: /* TextOutSize(layer, w, h)：两个分量各自要求 int 才写（原版 handler
               * 0x43C960 逐分量 cmpb type==2 + 范围检查），越界/非 int 保持原值。 */
        if (getenv("FVP_DEBUG_TEXT") != NULL) {
            fprintf(stderr, "TextOutSize: argc=%u layer=%d a1=%lld a2=%lld\n",
                    (unsigned)argc, id, (long long)fvp_gfx_int(&a[1]),
                    (long long)fvp_gfx_int(&a[2]));
        }
        if (argc > 2) {
            int oa = (argc > 1 && fvp_gfx_is_int(&a[1]))
                         ? fvp_gfx_int(&a[1])
                         : -1;
            int ob = fvp_gfx_is_int(&a[2]) ? fvp_gfx_int(&a[2]) : -1;

            gfx_text_set_out_size(g, id, oa, ob);
        }
        break;
    case 122: /* TextPos(layer, x, y)：原版逐轴写 tw+0x5E8/+0x5EA（面板内光标），
               * 每个轴都要求 int；非 int 的那一轴保持原值。 */
        if (getenv("FVP_DEBUG_TEXT") != NULL) {
            fprintf(stderr, "TextPos layer=%d x=%d y=%d\n", id,
                    argc > 1 ? fvp_gfx_int(&a[1]) : -1,
                    argc > 2 ? fvp_gfx_int(&a[2]) : -1);
        }
        if (argc > 2) {
            int has_x = fvp_gfx_is_int(&a[1]);
            int has_y = fvp_gfx_is_int(&a[2]);

            gfx_text_set_pos_ex(g, id, has_x ? fvp_gfx_int(&a[1]) : 0,
                                has_y ? fvp_gfx_int(&a[2]) : 0, has_x, has_y);
        }
        break;
    case 110: /* TextBuff(layer, w, h)：建/改该层的离屏文本面板（原版 0x430EA0）。
               * 图元驱动模式下 gfx 用它当文本区尺寸（换行宽度 / 行数上限）；
               * 旧路径 --text-fixed-layout 不读它，冻结基线不变。 */
        if (getenv("FVP_DEBUG_TEXT") != NULL) {
            fprintf(stderr, "TextBuff layer=%d w=%d h=%d\n", id,
                    argc > 1 ? fvp_gfx_int(&a[1]) : -1,
                    argc > 2 ? fvp_gfx_int(&a[2]) : -1);
        }
        if (argc > 1 && fvp_gfx_is_int(&a[1])) {
            int bw = fvp_gfx_int(&a[1]);
            int bh = (argc > 2 && fvp_gfx_is_int(&a[2]))
                         ? fvp_gfx_int(&a[2])
                         : 8;

            (void)gfx_text_set_buff(g, id, bw, bh);
        }
        break;
    case 123: /* TextPrint(layer, text) */
        if (argc > 1 && a[1].type == (uint8_t)FVP_STR) {
            const char *s = a[1].str->data;
            int n = (int)a[1].str->len;
            /* render.py drops one-byte numeric handles (unresolved text
             * variable signature); real one-digit lines never occur. */
            int numeric = (n == 1 && s[0] >= '0' && s[0] <= '9');
            if (!numeric) {
                gfx_text_append(g, id, s);
                if (b->on_text != NULL) {
                    b->on_text(b->text_ud, id, s, 0);
                }
            }
        }
        break;
    case 126: /* TextSize(layer, w, h)：原版 handler 0x43CD60 逐分量要求 int +
               * 范围（12..64 / 8..32），越界/非 int 的那一个保持原值。 */
        if (getenv("FVP_DEBUG_TEXT") != NULL) {
            fprintf(stderr, "TextSize layer=%d w=%d h=%d\n", id,
                    argc > 1 ? fvp_gfx_int(&a[1]) : -1,
                    argc > 2 ? fvp_gfx_int(&a[2]) : -1);
        }
        if (argc > 1) {
            int has_w = fvp_gfx_is_int(&a[1]);
            int has_h = (argc > 2) && fvp_gfx_is_int(&a[2]);

            gfx_text_set_size_ex(g, id, has_w ? fvp_gfx_int(&a[1]) : 0,
                                 has_h ? fvp_gfx_int(&a[2]) : 0, has_w, has_h);
        }
        break;
    case 129: /* TextSpeed(layer, ms)：存进 32 个窗口属性，同时交给渲染层，
               * 逐字显示（--typewriter）打开时按这个速度推进。 */
        if (argc > 1) {
            gfx_text_set_speed(g, id, fvp_gfx_int(&a[1]));
        }
        break;
    default:
        return 0; /* 主 switch 也没有 case 的命令：仍算未实现 */
    }
    return 1;
}

/* ------------------------------------------------------------------ */
/* Optional audio forwarding (docs/04-audio.md, "运行时接线")          */
/*                                                                     */
/* Ids are the hcb command-table ids (cmd_table.json), which is what the */
/* VM dispatches on.  Note they are NOT the exe's alphabetical        */
/* registration indices: the legacy sound API is 100..107 here:       */
/*   0 AudioLoad(2)      1 AudioPlay(2)     2 AudioSilentOn(1)        */
/*   3 AudioState(1)     4 AudioStop(2)     5 AudioType(2)            */
/*   6 AudioVol(3)                                                    */
/*   100 SoundLoad(2)    101 SoundMasterVol(1)  102 SoundPlay(3)      */
/*   103 SoundSilentOn(1) 104 SoundStop(2)  105 SoundType(2)          */
/*   106 SoundTypeVol(2) 107 SoundVol(3)                              */
/*                                                                     */
/* Everything below only runs when fvp_vm_attach_snd() installed a     */
/* bridge; fvm.exe never does, so all of these ids keep the exact      */
/* pre-existing "unimplemented command" behaviour there (counted in     */
/* the unknown-command histogram, temp value untouched).               */
/* ------------------------------------------------------------------ */

static void fvp_snd_emit(FvpSndBridge *b, const FvpSndEvent *ev)
{
    if (b != NULL && b->on_event != NULL) {
        b->on_event(b->event_ud, ev);
    }
}

static int fvp_snd_clamp(int v, int lo, int hi)
{
    if (v < lo) return lo;
    if (v > hi) return hi;
    return v;
}

/* Category implied by the resource name prefix.  The prefixes are unambiguous
 * in this game's script ("bgm/", "bgm2/", "voice/", "se/", "se_env/",
 * "se_sys/"); matching is case-insensitive because both "BGM/050" and
 * "bgm/001" occur. */
static int fvp_snd_cat_from_name(const char *name)
{
    char tok[16];
    size_t n = 0;
    const char *p = name;
    if (name == NULL) {
        return FVP_SND_CAT_UNKNOWN;
    }
    while (*p != '\0' && *p != '/' && *p != '\\' && n + 1u < sizeof tok) {
        char c = *p++;
        if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
        tok[n++] = c;
    }
    tok[n] = '\0';
    if (n == 0) {
        return FVP_SND_CAT_UNKNOWN;
    }
    if (strcmp(tok, "bgm") == 0 || strcmp(tok, "bgm2") == 0) {
        return FVP_SND_CAT_BGM;
    }
    if (strcmp(tok, "voice") == 0 || strcmp(tok, "voice2") == 0) {
        return FVP_SND_CAT_VOICE;
    }
    if (strcmp(tok, "se") == 0 || strcmp(tok, "se_env") == 0 ||
        strcmp(tok, "se_sys") == 0 || strcmp(tok, "sound") == 0 ||
        strcmp(tok, "env") == 0) {
        return FVP_SND_CAT_SE;
    }
    return FVP_SND_CAT_UNKNOWN;
}

/* AudioType(ch, type) / SoundType(idx, type) select one of the engine sound
 * manager's 10 mixing groups (master + group + per-source volume, see the
 * 0x436080 gain computation in work/iroseka_HD.asm).  The traced 900k run
 * shows AudioType(ch,1) used for BGM channels and AudioType(ch,0) for voice
 * channels (0x26500 loads "voice/..." right after AudioType(ch,0)); the
 * legacy SoundType group 0..4 is likewise the SE groups.  Resource name
 * prefixes remain authoritative, this is only the fallback for names that
 * carry no prefix. */
static int fvp_snd_cat_from_type(int type)
{
    if (type == 1) {
        return FVP_SND_CAT_BGM;
    }
    if (type == 0) {
        return FVP_SND_CAT_VOICE;
    }
    if (type >= 2 && type < FVP_SND_TYPES) {
        return FVP_SND_CAT_SE;
    }
    return FVP_SND_CAT_UNKNOWN;
}

/* The prototype VM has no options/save layer, so the script's volume globals
 * are seeded from commands that are stubs here: at run time SoundMasterVol
 * and SoundTypeVol always arrive as 0 and AudioVol/SoundVol as 0 or 1 (the
 * real game reads those options from its save data).  A literal 0 would mute
 * the whole prototype.  Rule used here:
 *
 *   - a master/group value stays at its 100 default until the script sends
 *     that same field a non-zero value (then the option layer is considered
 *     present and every later value, including a real 0 mute, is applied);
 *   - the per-source volume is applied only once the master or any group is
 *     configured, so the "1" the script passes in the prototype (which is
 *     itself a missing option value) does not make BGM 1 % loud.
 *
 * A shell that parses the real options can simply send the real values
 * through the normal commands before the VM runs and the rule becomes a
 * no-op.  Documented in docs/04-audio.md. */
static int fvp_snd_volume_configured(const FvpSndBridge *b)
{
    int i;
    if (b == NULL) {
        return 0;
    }
    if (b->master_configured) {
        return 1;
    }
    for (i = 0; i < FVP_SND_TYPES; i++) {
        if (b->type_configured[i]) {
            return 1;
        }
    }
    return 0;
}

static int fvp_snd_master_effective(const FvpSndBridge *b)
{
    return (b != NULL && b->master_configured) ? b->master_vol : 100;
}

static int fvp_snd_type_effective(const FvpSndBridge *b, int type)
{
    if (b != NULL && type >= 0 && type < FVP_SND_TYPES &&
        b->type_configured[type]) {
        return b->type_vol[type];
    }
    return 100;
}

static int fvp_snd_chan_effective(const FvpSndBridge *b, const FvpSndSlot *sl)
{
    return fvp_snd_volume_configured(b) ? sl->volume : 100;
}

float fvp_snd_gain_calc(int slot_vol, int master_vol, int type_vol, int silent)
{
    double g;

    if (silent) {
        return 0.0f;
    }
    g = ((double)slot_vol / 100.0) * ((double)master_vol / 100.0) *
        ((double)type_vol / 100.0);
    if (g < 0.0) {
        g = 0.0;
    }
    if (g > 1.0) {
        g = 1.0;
    }
    return (float)g;
}

static float fvp_snd_gain(const FvpSndBridge *b, const FvpSndSlot *sl)
{
    int type_vol;

    if (b == NULL || sl == NULL) {
        return 0.0f;
    }
    type_vol = (sl->type >= 0 && sl->type < FVP_SND_TYPES)
                   ? fvp_snd_type_effective(b, sl->type)
                   : 100;
    return fvp_snd_gain_calc(fvp_snd_chan_effective(b, sl),
                             fvp_snd_master_effective(b), type_vol,
                             sl->silent);
}

static void fvp_snd_refresh_gains(FvpSndBridge *b)
{
    int c;
    if (b == NULL || b->snd == NULL || snd_set_vol == NULL) {
        return;
    }
    for (c = 0; c < SND_CH_COUNT; c++) {
        if (b->chan_owner[c] != NULL) {
            snd_set_vol(b->snd, c, fvp_snd_gain(b, b->chan_owner[c]));
        }
    }
}

/* BGM -> the single BGM channel, voice -> the voice channel, everything else
 * (SE) -> round robin over the remaining SND channels, as required by the
 * mixer's channel layout (docs/04-audio.md). */
static int fvp_snd_pick_chan(FvpSndBridge *b, int cat, int fallback)
{
    int span;
    if (cat == FVP_SND_CAT_BGM) {
        /* 两个 BGM 通道轮换：如果当前通道正在响，新曲先装到另一个通道，
         * 由调用方做交叉淡入淡出（docs/04-audio.md）。 */
        if (b->snd != NULL && b->bgm_chan == SND_CH_BGM) {
            return SND_CH_BGM;
        }
        return b->bgm_chan;
    }
    if (cat == FVP_SND_CAT_VOICE) {
        return SND_CH_VOICE;
    }
    if (cat == FVP_SND_CAT_SE) {
        span = SND_CH_COUNT - SND_CH_SE;
        if (span <= 0) {
            return SND_CH_SE;
        }
        if (b->se_next < 0 || b->se_next >= span) {
            b->se_next = 0;
        }
        return SND_CH_SE + b->se_next++;
    }
    if (fallback >= 0 && fallback < SND_CH_COUNT) {
        return fallback;
    }
    return SND_CH_SE;
}

static void fvp_snd_set_name(FvpSndSlot *sl, const char *name)
{
    free(sl->name);
    sl->name = NULL;
    if (name != NULL) {
        sl->name = fvp_xstrdup(name);
    }
}

static void fvp_snd_detach_chan(FvpSndBridge *b, FvpSndSlot *sl)
{
    if (b != NULL && sl->snd_chan >= 0 && sl->snd_chan < SND_CH_COUNT &&
        b->chan_owner[sl->snd_chan] == sl) {
        b->chan_owner[sl->snd_chan] = NULL;
    }
    /* Must be cleared even when the channel had already been taken over by
     * another slot, otherwise a later Stop would silence that new owner. */
    sl->snd_chan = -1;
}

static void fvp_snd_stop_slot(FvpSndBridge *b, FvpSndSlot *sl)
{
    if (b == NULL || sl == NULL) {
        return;
    }
    if (sl->snd_chan >= 0 && sl->snd_chan < SND_CH_COUNT) {
        if (snd_stop != NULL) {
            snd_stop(b->snd, sl->snd_chan);
        }
        fvp_snd_detach_chan(b, sl);
    }
    sl->playing = 0;
}

/* Resolve + decode + hand the PCM to the mixer.  This is the only place that
 * reads a resource package, and it only runs on a Play command: a 116 s Ogg
 * BGM is ~22 MB decoded, so nothing may be decoded at Load time. */
static int fvp_snd_start(FvpVM *vm, uint16_t cid, FvpSndSlot *sl, int slot_index,
                         int cat_hint, int fallback_chan, int loop)
{
    FvpSndBridge *b = vm->snd_bridge;
    FvpSndEvent ev;
    uint8_t *data = NULL;
    size_t size = 0;
    const char *archive = NULL;
    int cat;
    int ch;
    int rc;
    int is_ogg;

    memset(&ev, 0, sizeof ev);
    ev.cid = (int)cid;
    ev.kind = FVP_SND_EV_PLAY;
    ev.slot = slot_index;
    ev.chan = -1;
    ev.category = FVP_SND_CAT_UNKNOWN;
    ev.loop = loop;
    ev.type = sl->type;
    ev.name = sl->name;
    if (b == NULL) {
        return -1;
    }
    b->stats.plays += 1;
    if (sl->name == NULL) {
        /* Play on a slot that was never loaded (the legacy Sound API does
         * this for slots whose name global the missing save layer left
         * empty); count it and stay silent, like an empty engine source. */
        b->stats.unresolved += 1;
        fvp_snd_emit(b, &ev);
        return -1;
    }

    cat = fvp_snd_cat_from_name(sl->name);
    if (cat == FVP_SND_CAT_UNKNOWN) {
        cat = fvp_snd_cat_from_type(sl->type);
    }
    if (cat == FVP_SND_CAT_UNKNOWN) {
        cat = cat_hint;
    }
    ev.category = cat;

    /* A BGM always loops; the script itself passes AudioPlay(ch, true), but
     * a "0" here means the int-literal 0, and the engine's play flag is only
     * about looping, not about starting playback. */
    if (cat == FVP_SND_CAT_BGM) {
        loop = 1;
    }
    ev.loop = loop;

    rc = fvp_snd_bank_read(b, sl->name, &data, &size, &archive);
    if (rc != 0) {
        b->stats.unresolved += 1;
        ev.archive = NULL;
        fvp_snd_emit(b, &ev);
        if (!vm->quiet) {
            fprintf(stderr, "snd: cannot resolve '%s' (rc=%d)\n", sl->name, rc);
        }
        return -1;
    }
    b->stats.resolved += 1;
    ev.archive = archive;

    is_ogg = size >= 4u && memcmp(data, "OggS", 4) == 0;
    /* The engine's start routine clears the per-channel silence flag
     * (4410b0 writes +0x14 = 0), so a Play always becomes audible again. */
    sl->silent = 0;
    ch = fvp_snd_pick_chan(b, cat, fallback_chan);
    if (is_ogg) {
        rc = snd_play_ogg(b->snd, ch, data, size, loop,
                          fvp_snd_gain(b, sl));
    } else {
        rc = snd_play_wav(b->snd, ch, data, size, loop,
                          fvp_snd_gain(b, sl));
    }
    free(data);
    if (rc != 0) {
        b->stats.decode_fail += 1;
        if (!vm->quiet) {
            fprintf(stderr, "snd: cannot decode '%s' from %s (rc=%d)\n",
                    sl->name, archive != NULL ? archive : "?", rc);
        }
        fvp_snd_emit(b, &ev);
        return -1;
    }

    /* BGM 切曲：新曲在 ch 上淡入，旧 BGM 通道淡出（两条通道同时有声）。 */
    if (cat == FVP_SND_CAT_BGM) {
        if (b->bgm_chan != ch && b->snd != NULL && b->bgm_xfade_ms > 0 &&
            snd_is_playing(b->snd, b->bgm_chan)) {
            if (snd_crossfade != NULL) {
                snd_crossfade(b->snd, b->bgm_chan, ch, b->bgm_xfade_ms);
            }
            b->stats.bgm_crossfades += 1;
            if (!vm->quiet) {
                fprintf(stderr, "snd: bgm crossfade %d -> %d (%d ms)\n",
                        b->bgm_chan, ch, b->bgm_xfade_ms);
            }
        }
        b->bgm_chan = ch;
    }

    /* The previous owner of this mixer channel (if any) is now silent. */
    if (b->chan_owner[ch] != NULL && b->chan_owner[ch] != sl) {
        b->chan_owner[ch]->playing = 0;
        b->chan_owner[ch]->snd_chan = -1;
    }
    if (sl->snd_chan >= 0 && sl->snd_chan != ch) {
        fvp_snd_detach_chan(b, sl);
    }
    sl->playing = 1;
    sl->loop = loop;
    sl->snd_chan = ch;
    b->chan_owner[ch] = sl;
    ev.chan = ch;
    if (snd_set_vol != NULL) {
        snd_set_vol(b->snd, ch, fvp_snd_gain(b, sl));
    }
    if (cat == FVP_SND_CAT_BGM) {
        b->stats.plays_bgm += 1;
    } else if (cat == FVP_SND_CAT_VOICE) {
        b->stats.plays_voice += 1;
    } else {
        b->stats.plays_se += 1;
    }
    ev.ok = 1;
    fvp_snd_emit(b, &ev);
    return 0;
}

static int fvp_snd_state(FvpSndBridge *b, const FvpSndSlot *sl)
{
    int playing;
    if (b == NULL || sl == NULL) {
        return 0;
    }
    playing = sl->playing;
    if (playing && sl->snd_chan >= 0 && snd_is_playing != NULL) {
        playing = snd_is_playing(b->snd, sl->snd_chan);
    }
    return playing ? 1 : 0;
}

/* Returns 1 when the command was handled (bridge attached), 0 to let the
 * normal dispatch path run -- which is what binaries without an audio
 * context (bin/fvm.exe) always get. */
static int fvp_sys_snd(FvpVM *vm, FvpThread *t, const FvpValue *a, uint32_t argc,
                       uint16_t cid)
{
    FvpSndBridge *b = vm->snd_bridge;
    FvpSndSlot *sl;
    FvpSndEvent ev;
    int arg0;
    int arg1;
    const char *name;

    if (b == NULL || snd_play_ogg == NULL) {
        return 0; /* not attached / not linked: keep the old no-op path */
    }
    arg0 = argc > 0 ? fvp_gfx_int(&a[0]) : 0;
    arg1 = argc > 1 ? fvp_gfx_int(&a[1]) : 0;
    name = (argc > 1 && a[1].type == (uint8_t)FVP_STR && a[1].str != NULL)
               ? a[1].str->data
               : NULL;

    memset(&ev, 0, sizeof ev);
    ev.cid = (int)cid;
    ev.slot = -1;
    ev.category = FVP_SND_CAT_UNKNOWN;
    ev.ok = 1;
    ev.type = -1;

    switch (cid) {
    case 0: /* AudioLoad(ch, name): remember the slot -> name mapping */
        if (arg0 < 0 || arg0 >= FVP_SND_AUDIO_SLOTS) {
            return 1;
        }
        sl = &b->audio[arg0];
        fvp_snd_stop_slot(b, sl);
        fvp_snd_set_name(sl, name);
        b->stats.loads += 1;
        ev.kind = FVP_SND_EV_LOAD;
        ev.slot = arg0;
        ev.name = sl->name;
        ev.category = fvp_snd_cat_from_name(sl->name);
        ev.ok = name != NULL;
        fvp_snd_emit(b, &ev);
        return 1;
    case 1: /* AudioPlay(ch, loopFlag) */
        if (arg0 < 0 || arg0 >= FVP_SND_AUDIO_SLOTS) {
            return 1;
        }
        (void)fvp_snd_start(vm, cid, &b->audio[arg0], arg0,
                            FVP_SND_CAT_UNKNOWN, arg0, arg1 != 0);
        return 1;
    case 2: /* AudioSilentOn(ch) */
        if (arg0 < 0 || arg0 >= FVP_SND_AUDIO_SLOTS) {
            return 1;
        }
        sl = &b->audio[arg0];
        sl->silent = 1;
        b->stats.silent_on += 1;
        fvp_snd_refresh_gains(b);
        ev.kind = FVP_SND_EV_SILENT;
        ev.slot = arg0;
        ev.name = sl->name;
        fvp_snd_emit(b, &ev);
        return 1;
    case 3: /* AudioState(ch) -> bool: still playing? */
        if (arg0 < 0 || arg0 >= FVP_SND_AUDIO_SLOTS) {
            t->temp = fvp_make_int(0);
            return 1;
        }
        sl = &b->audio[arg0];
        b->stats.states += 1;
        t->temp = fvp_make_int(fvp_snd_state(b, sl));
        ev.kind = FVP_SND_EV_STATE;
        ev.slot = arg0;
        ev.ok = t->temp.i != 0;
        ev.name = sl->name;
        fvp_snd_emit(b, &ev);
        return 1;
    case 4: /* AudioStop(ch, fadeMs): fade is ignored (hard stop) */
        if (arg0 < 0 || arg0 >= FVP_SND_AUDIO_SLOTS) {
            return 1;
        }
        sl = &b->audio[arg0];
        fvp_snd_stop_slot(b, sl);
        b->stats.stops += 1;
        ev.kind = FVP_SND_EV_STOP;
        ev.slot = arg0;
        ev.name = sl->name;
        fvp_snd_emit(b, &ev);
        return 1;
    case 5: /* AudioType(ch, type 0..9) */
        if (arg0 < 0 || arg0 >= FVP_SND_AUDIO_SLOTS) {
            return 1;
        }
        sl = &b->audio[arg0];
        sl->type = fvp_snd_clamp(arg1, 0, FVP_SND_TYPES - 1);
        b->stats.types += 1;
        fvp_snd_refresh_gains(b);
        ev.kind = FVP_SND_EV_TYPE;
        ev.slot = arg0;
        ev.type = sl->type;
        ev.category = fvp_snd_cat_from_type(sl->type);
        ev.name = sl->name;
        fvp_snd_emit(b, &ev);
        return 1;
    case 6: /* AudioVol(ch, vol 0..100, fadeMs) */
        if (arg0 < 0 || arg0 >= FVP_SND_AUDIO_SLOTS) {
            return 1;
        }
        sl = &b->audio[arg0];
        sl->volume = fvp_snd_clamp(arg1, 0, 100);
        b->stats.vols += 1;
        fvp_snd_refresh_gains(b);
        ev.kind = FVP_SND_EV_VOL;
        ev.slot = arg0;
        ev.volume = sl->volume;
        ev.name = sl->name;
        fvp_snd_emit(b, &ev);
        return 1;

    case 100: /* SoundLoad(idx 0..255, name) */
        if (arg0 < 0 || arg0 >= FVP_SND_SOUND_SLOTS) {
            return 1;
        }
        sl = &b->sound[arg0];
        fvp_snd_stop_slot(b, sl);
        fvp_snd_set_name(sl, name);
        b->stats.loads += 1;
        ev.kind = FVP_SND_EV_LOAD;
        ev.slot = arg0;
        ev.name = sl->name;
        ev.category = fvp_snd_cat_from_name(sl->name);
        ev.ok = name != NULL;
        fvp_snd_emit(b, &ev);
        return 1;
    case 101: /* SoundMasterVol(vol 0..100) */
        b->master_vol = fvp_snd_clamp(arg0, 0, 100);
        if (b->master_vol != 0) {
            b->master_configured = 1;
        }
        b->stats.vols += 1;
        fvp_snd_refresh_gains(b);
        ev.kind = FVP_SND_EV_VOL;
        ev.volume = b->master_vol;
        fvp_snd_emit(b, &ev);
        return 1;
    case 102: /* SoundPlay(idx, playFlag, fadeMs) */
        if (arg0 < 0 || arg0 >= FVP_SND_SOUND_SLOTS) {
            return 1;
        }
        (void)fvp_snd_start(vm, cid, &b->sound[arg0], arg0,
                            FVP_SND_CAT_SE, -1, arg1 != 0);
        return 1;
    case 103: /* SoundSilentOn(idx) */
        if (arg0 < 0 || arg0 >= FVP_SND_SOUND_SLOTS) {
            return 1;
        }
        sl = &b->sound[arg0];
        sl->silent = 1;
        b->stats.silent_on += 1;
        fvp_snd_refresh_gains(b);
        ev.kind = FVP_SND_EV_SILENT;
        ev.slot = arg0;
        ev.name = sl->name;
        fvp_snd_emit(b, &ev);
        return 1;
    case 104: /* SoundStop(idx, fadeMs) */
        if (arg0 < 0 || arg0 >= FVP_SND_SOUND_SLOTS) {
            return 1;
        }
        sl = &b->sound[arg0];
        fvp_snd_stop_slot(b, sl);
        b->stats.stops += 1;
        ev.kind = FVP_SND_EV_STOP;
        ev.slot = arg0;
        ev.name = sl->name;
        fvp_snd_emit(b, &ev);
        return 1;
    case 105: /* SoundType(idx, type 0..9) */
        if (arg0 < 0 || arg0 >= FVP_SND_SOUND_SLOTS) {
            return 1;
        }
        sl = &b->sound[arg0];
        sl->type = fvp_snd_clamp(arg1, 0, FVP_SND_TYPES - 1);
        b->stats.types += 1;
        fvp_snd_refresh_gains(b);
        ev.kind = FVP_SND_EV_TYPE;
        ev.slot = arg0;
        ev.type = sl->type;
        ev.category = fvp_snd_cat_from_type(sl->type);
        ev.name = sl->name;
        fvp_snd_emit(b, &ev);
        return 1;
    case 106: /* SoundTypeVol(type 0..9, vol 0..100) */
        if (arg0 < 0 || arg0 >= FVP_SND_TYPES) {
            return 1;
        }
        b->type_vol[arg0] = fvp_snd_clamp(arg1, 0, 100);
        if (b->type_vol[arg0] != 0) {
            b->type_configured[arg0] = 1;
        }
        b->stats.vols += 1;
        fvp_snd_refresh_gains(b);
        ev.kind = FVP_SND_EV_VOL;
        ev.type = arg0;
        ev.volume = b->type_vol[arg0];
        fvp_snd_emit(b, &ev);
        return 1;
    case 107: /* SoundVol(idx, vol 0..100, fadeMs) */
        if (arg0 < 0 || arg0 >= FVP_SND_SOUND_SLOTS) {
            return 1;
        }
        sl = &b->sound[arg0];
        sl->volume = fvp_snd_clamp(arg1, 0, 100);
        b->stats.vols += 1;
        fvp_snd_refresh_gains(b);
        ev.kind = FVP_SND_EV_VOL;
        ev.slot = arg0;
        ev.volume = sl->volume;
        ev.name = sl->name;
        fvp_snd_emit(b, &ev);
        return 1;
    default:
        break;
    }
    return 0;
}

int fvp_snd_bridge_set_master(FvpSndBridge *b, int vol)
{
    if (b == NULL) {
        return -1;
    }
    vol = fvp_snd_clamp(vol, 0, 100);
    b->master_vol = vol;
    b->master_configured = 1;
    fvp_snd_refresh_gains(b);
    return vol;
}

int fvp_snd_bridge_master(const FvpSndBridge *b)
{
    return fvp_snd_master_effective(b);
}

void fvp_syscall(FvpVM *vm, FvpThread *t, uint16_t cid)
{
    uint32_t argc = cid < FVP_CMD_COUNT ? (uint32_t)fvp_cmd_argc[cid] : 0u;
    FvpValue args[FVP_MAX_ARGC] = {0}; /* zero == fvp_make_int(0) */
    int32_t sp_before = t->sp;
    uint32_t k;

    vm->gfx_handled = 0;
    if (argc > (uint32_t)FVP_MAX_ARGC) {
        argc = (uint32_t)FVP_MAX_ARGC;
    }
    /* Reference: for _ in range(argc): args.insert(0, pop(t)) */
    for (k = argc; k > 0; --k) {
        args[k - 1] = fvp_thread_pop(t);
    }

    /* Rendering commands: forward to the attached gfx context, if any.
     * gfx_set_prim is the weak-symbol probe (all renderer symbols are weak,
     * and no context exists in binaries that do not link src/gfx.c). */
    if (vm->gfx_bridge != NULL && gfx_set_prim != NULL) {
        int rargc = fvp_gfx_argc(cid, argc);
        if (rargc > (int)FVP_MAX_ARGC) {
            rargc = (int)FVP_MAX_ARGC;
        }
        if (rargc != (int)argc) {
            /* The renderer's own argc table disagrees with the VM's; take
             * the call's arguments from the top of the operand stack without
             * changing the VM's pop behaviour (see fvp_gfx_argc()). */
            FvpValue raw[FVP_MAX_ARGC];
            int32_t depth = sp_before;
            int j;
            if (depth > t->scap) {
                depth = t->scap;
            }
            for (j = 0; j < rargc; j++) {
                int32_t idx = depth - rargc + j;
                raw[j] = (t->stack != NULL && idx >= 0 && idx < depth)
                             ? t->stack[idx]
                             : fvp_make_int(0);
            }
            if (fvp_sys_gfx(vm, t, raw, (uint32_t)rargc, cid)) {
                vm->gfx_handled = 1;
            }
        } else {
            if (fvp_sys_gfx(vm, t, args, argc, cid)) {
                vm->gfx_handled = 1;
            }
        }
    }

    /* Audio commands: only when a context is attached (fvm.exe: never).
     * Handled commands return here, so the unknown-command counter and the
     * temp-value rules keep their exact prototype behaviour otherwise. */
    if (vm->snd_bridge != NULL && fvp_sys_snd(vm, t, args, argc, cid)) {
        return;
    }

    switch (cid) {
    /* 24 IntToText(value, opt?): 把整数格式化成十进制字符串并**返回字符串句柄**。
     * 静态分析 handler 0x437646：`push engine+0x481374; call wsprintfA`（格式串
     * 0x472c4c = "%d"），然后 `call 0x4445e0` 分配字符串句柄、`movb $0x5,0x814`
     * 把返回类型标成字符串。本站用 arena 造同内容的 FvpStr 当句柄。
     * 第 2 个参数（文档里记作 opt 1..10）没有证据说明它影响格式，先忽略。 */
    case 24: {
        char buf[32];
        int len;

        if (argc >= 1 && args[0].type == (uint8_t)FVP_INT) {
            len = snprintf(buf, sizeof buf, "%lld", (long long)args[0].i);
            if (len < 0) {
                len = 0;
            }
            t->temp = fvp_make_str(
                fvp_arena_str(&vm->arena, buf, (uint32_t)len));
        } else {
            t->temp = fvp_make_str(fvp_arena_str(&vm->arena, "", 0));
        }
        return;
    }
    case 123: /* TextPrint(layer, text) */
        fvp_sys_text_print(vm, args, argc);
        return;
    case 111: /* TextClear(layer) */
        return;
    case 31: /* InputGetDown: synthetic click every click_period steps */
        if (vm->click_period > 0 &&
            (vm->steps % (uint64_t)vm->click_period) < 2u) {
            t->temp = fvp_make_int(1);
        } else {
            t->temp = fvp_make_int(0);
        }
        return;
    case 35: /* InputGetUp（cmd_table: 35 = InputGetUp, 36 = InputGetWheel） */
        t->temp = fvp_make_int(0);
        return;
    case 36: /* InputGetWheel: 取走外壳累计的滚轮增量（120 = 一格） */
        t->temp = fvp_make_int(fvp_vm_wheel_take(vm));
        return;
    case 29: /* InputGetCursX */
        t->temp = fvp_make_int(vm->cx);
        return;
    case 30: /* InputGetCursY */
        t->temp = fvp_make_int(vm->cy);
        return;
    case 34: /* InputGetState */
        t->temp = fvp_make_int(vm->key_mask);
        return;
    case 32: /* InputGetEvent */
        t->temp = fvp_make_int(0);
        return;
    case 28: /* InputGetCursIn */
        t->temp = fvp_make_int(1);
        return;
    /* 18 FlagGet(flagIdx 0..0x7FF): 返回全局 flag 位图的一位（bool）。
     * 位图位置/长度见 docs/save-format.md §3.5；save.bin 里那 256 字节就是它。 */
    case 18:
        t->temp = fvp_make_int(
            fvp_flag_get(vm->flags, argc > 0 ? (int)args[0].i : -1));
        return;
    /* 19 FlagSet(flagIdx, on): arg2 为假（int 0）则清位，否则置位。 */
    case 19:
        if (argc >= 2) {
            int on = !(args[1].type == (uint8_t)FVP_INT && args[1].i == 0);

            fvp_flag_set(vm->flags, (int)args[0].i, on);
        }
        return;
    /* 112 TextColor(twIdx, R, G, B) / 126 TextSize(twIdx, w, h) /
     * 129 TextSpeed(twIdx, ms)：只记录到 VM 的 32 个窗口属性里，按引擎的
     * 范围钳制（docs/vm-commands.md §5）。渲染层暂时不消费，所以已验收的帧不变。 */
    /* 93 SaveCreate(id, arg) / 94 SaveThumbSize(w, h)：存档暂存区。
     * id 0/1/2 = 章节名 / 未用 / 当前对话行（docs/save-format.md §2.2），
     * id 3 选槽号；缩略图尺寸钳制到 20..200 x 15..150（同文档 §2.3）。 */
    case 93:
        if (argc >= 2) {
            int id = (int)args[0].i;

            if (id >= 0 && id < 3) {
                vm->save_str[id] = (args[1].type == (uint8_t)FVP_STR)
                                       ? args[1].str
                                       : NULL;
            } else if (id == 3) {
                vm->save_slot = (int)args[1].i;
            }
        }
        return;
    case 94:
        if (argc >= 2) {
            int w = (int)args[0].i;
            int h = (int)args[1].i;

            if (w < 20) w = 20;
            if (w > 200) w = 200;
            if (h < 15) h = 15;
            if (h > 150) h = 150;
            vm->save_thumb_w = w;
            vm->save_thumb_h = h;
        }
        return;
    /* 95 SaveData(id, idx, value?)：脚本"把游戏状态摊平成可存数据"的入口。
     * .hcb 内嵌命令表声明 argc=3，但本作编译进的 argc 表只有 2 —— 所以 VM 层
     * 能拿到的是前两个操作数。这里只记录、判断不了语义（docs/save-format.md §4
     * 的 kind 0..13 仍未解），但把它记下来就能回答"脚本到底想存什么"。 */
    case 95:
        if (argc >= 1) {
            vm->save_data_seen += 1u;
            if (vm->save_data_n < FVP_SAVE_DATA_MAX) {
                FvpSaveData *d = &vm->save_data[vm->save_data_n];

                d->kind = (int)args[0].i;
                d->idx = (argc >= 2) ? (int)args[1].i : 0;
                d->value = (argc >= 3) ? (int64_t)args[2].i
                                       : (int64_t)d->idx;
                vm->save_data_n += 1;
            }
        }
        return;
    case 112:
        if (argc >= 4) {
            fvp_textattr_set_color(vm->textattr, FVP_TEXT_WINDOWS,
                                   (int)args[0].i, (int)args[1].i,
                                   (int)args[2].i, (int)args[3].i);
        }
        return;
    case 126:
        if (argc >= 3) {
            fvp_textattr_set_size(vm->textattr, FVP_TEXT_WINDOWS,
                                  (int)args[0].i, (int)args[1].i,
                                  (int)args[2].i);
        }
        return;
    case 129:
        if (argc >= 2) {
            fvp_textattr_set_speed(vm->textattr, FVP_TEXT_WINDOWS,
                                   (int)args[0].i, (int)args[1].i);
        }
        return;
    /* 125 TextShadowDist(twIdx, dist)：**阴影距离**，静态分析 handler **0x43CBB0**
     * （2026-11-24 修正：以前把 0x43CE10 当成这个命令，那其实是 TextSpace）：
     *   arg0 必须是 int 0..0x1F；arg1 不是 int 时按 0 处理，是 int 时 <0 当 0、
     *   >12 当 12，然后转 0x4248B0(tw, dist) → `mov %al,0x597(%ecx)`（只存一字节）。
     * 只记录、不渲染：原版阴影的**形状/颜色/透明度**都还没有证据（只知道距离），
     * 参考实现 work/render.py 也只存不画。见 docs/render-model.md §7.1。 */
    /* 118 TextFormat(twIdx, a, b, c, d, e, f)：静态分析 handler **0x43CC10**，
     * 六个排版参数各自独立判定范围（逐字段 jl/jg，超范围的字段单独跳过）：
     *   a/b → tw+0x5A8 / tw+0x5AA（-32..32，**与 TextSpace 是同一对字段**）
     *   c   → tw+0x5AE（0..64）
     *   d   → tw+0x5AC（0..64；且会把 tw+0x5E8 抬到不小于 d）
     *   e   → tw+0x5B4（-16..16）、f → tw+0x5B0（-16..16）
     * 只记录、不参与排版（原版怎么用这些值没有证据）。实测 tw0 = (32,32,64,64,16,16)、
     * tw8 = (32,1,1,1,1,1)、tw9 = 同 tw0 之外的一档。 */
    case 118:
        if (argc >= 1 && args[0].type == (uint8_t)FVP_INT) {
            int id = (int)args[0].i;
            int v[6];
            int k;

            for (k = 0; k < 6; ++k) {
                int ai = k + 1;

                v[k] = (ai < (int)argc && args[ai].type == (uint8_t)FVP_INT)
                           ? (int)args[ai].i
                           : 0;
            }
            if (fvp_textattr_set_format(vm->textattr, FVP_TEXT_WINDOWS, id, v[0],
                                        v[1], v[2], v[3], v[4], v[5]) &&
                vm->gfx_bridge != NULL && gfx_text_set_format != NULL &&
                id >= 0 && id < FVP_TEXT_WINDOWS) {
                const FvpTextAttr *ta = &vm->textattr[id];

                gfx_text_set_format(vm->gfx_bridge->gfx, id, ta->space_x,
                                    ta->space_y, ta->fmt_pad_a, ta->fmt_pad_b,
                                    ta->fmt_off_a, ta->fmt_off_b);
            }
        }
        return;
    /* 119 TextFunction(twIdx, a, b, c)：静态分析 handler **0x43CCF0**，三个开关
     * 各自独立判定：a → tw+0x5A5（0..1）、b → tw+0x5A4（0..2）、c → tw+0x5A6（0..2）。
     * 只记录（三个开关的语义未知）。实测 tw0 = (1,2,2)。 */
    case 119:
        if (argc >= 1 && args[0].type == (uint8_t)FVP_INT) {
            int id = (int)args[0].i;
            int a1 = (argc >= 2 && args[1].type == (uint8_t)FVP_INT)
                         ? (int)args[1].i
                         : 0;
            int a2 = (argc >= 3 && args[2].type == (uint8_t)FVP_INT)
                         ? (int)args[2].i
                         : 0;
            int a3 = (argc >= 4 && args[3].type == (uint8_t)FVP_INT)
                         ? (int)args[3].i
                         : 0;

            if (fvp_textattr_set_function(vm->textattr, FVP_TEXT_WINDOWS, id,
                                          a1, a2, a3) &&
                vm->gfx_bridge != NULL && gfx_text_set_function != NULL &&
                id >= 0 && id < FVP_TEXT_WINDOWS) {
                gfx_text_set_function(vm->gfx_bridge->gfx, id,
                                      vm->textattr[id].func_a,
                                      vm->textattr[id].func_b,
                                      vm->textattr[id].func_c);
            }
        }
        return;
    /* 113 TextFont(twIdx, fontId, fontId2)：**字体选择**，静态分析 handler **0x43DA70**：
     *   arg0 必须是 int 0..0x1F（twIdx）；
     *   arg1 = int 且 **-5 <= v <= (引擎字体数-1)** → 0x4247F0 写 `tw+0x590`（u16）；
     *   arg2 = int 且同一区间 → 0x424860 写 `tw+0x593`（u8）；
     *   两个分量各自独立判定，超范围的那个跳过。
     * 只记录、不渲染：引擎的字体号 -1/-2 对应哪套字模没有证据（我们只有 4 个图集），
     * 所以渲染层仍按字号挑字体。实测两部作品只用到 -1（正文）/ -2（章节标题）/ 1。 */
    case 113:
        if (argc >= 1 && args[0].type == (uint8_t)FVP_INT) {
            int id = (int)args[0].i;
            int fid = (argc >= 2 && args[1].type == (uint8_t)FVP_INT)
                          ? (int)args[1].i
                          : 0;
            int fid2 = (argc >= 3 && args[2].type == (uint8_t)FVP_INT)
                           ? (int)args[2].i
                           : 0;

            if (fvp_textattr_set_font(vm->textattr, FVP_TEXT_WINDOWS, id, fid,
                                      fid2) &&
                vm->gfx_bridge != NULL && gfx_text_set_font != NULL &&
                id >= 0 && id < FVP_TEXT_WINDOWS) {
                gfx_text_set_font(vm->gfx_bridge->gfx, id,
                                  vm->textattr[id].font_id,
                                  vm->textattr[id].font_id2);
            }
        }
        return;
    case 125:
        if (argc >= 1 && args[0].type == (uint8_t)FVP_INT) {
            int id = (int)args[0].i;
            int dist = (argc >= 2 && args[1].type == (uint8_t)FVP_INT)
                           ? (int)args[1].i
                           : 0;

            if (fvp_textattr_set_shadow(vm->textattr, FVP_TEXT_WINDOWS, id,
                                        dist) &&
                vm->gfx_bridge != NULL && gfx_text_set_shadow != NULL &&
                id >= 0 && id < FVP_TEXT_WINDOWS) {
                gfx_text_set_shadow(vm->gfx_bridge->gfx, id,
                                    vm->textattr[id].shadow_dist);
            }
        }
        return;
    /* 128 TextSpace(twIdx, x, y)：**字间距 / 行间距**，静态分析 handler **0x43CE10**：
     *   arg1 / arg2 各自独立判定，落在 -32..32 才写 tw+0x5A8 / tw+0x5AA（i16），
     *   超范围的那一个分量跳过、另一个仍然生效。
     * 只记录、不渲染（参考实现同样只存不画）。 */
    case 128:
        if (argc >= 1 && args[0].type == (uint8_t)FVP_INT) {
            int id = (int)args[0].i;
            int sx = (argc >= 2 && args[1].type == (uint8_t)FVP_INT)
                         ? (int)args[1].i
                         : 0;
            int sy = (argc >= 3 && args[2].type == (uint8_t)FVP_INT)
                         ? (int)args[2].i
                         : 0;

            if (fvp_textattr_set_space(vm->textattr, FVP_TEXT_WINDOWS, id, sx,
                                       sy) &&
                vm->gfx_bridge != NULL && gfx_text_set_space != NULL &&
                id >= 0 && id < FVP_TEXT_WINDOWS) {
                gfx_text_set_space(vm->gfx_bridge->gfx, id,
                                   vm->textattr[id].space_x,
                                   vm->textattr[id].space_y);
            }
        }
        return;
    /* 127 TextSkip(twIdx, mode 0..3)：静态分析 0x43CDD0，写 tw+0x5d7。 */
    case 127:
        if (argc >= 2) {
            fvp_textattr_set_skip(vm->textattr, FVP_TEXT_WINDOWS,
                                  (int)args[0].i, (int)args[1].i);
        }
        return;
    case 15:  /* DissolveWait: already finished */
    case 44:  /* MotionAlphaTest */
    case 47:  /* MotionAnimTest */
    case 50:  /* MotionMoveTest */
    case 53:  /* MotionMoveRTest */
    case 56:  /* MotionMoveS2Test */
    case 59:  /* MotionMoveZTest */
    case 145: /* V3DMotionTest */
    case 62:  /* MovieState: pretend the WMV already finished */
        t->temp = fvp_make_int(1);
        return;
    /* 41 MenuMessSkip / 108 SysAtSkipName / 141 TitleMenu：静态分析 0x437950 就是
     * 一条 `ret $0x4` —— 原版这三个 handler 是**纯空操作**（不读参数、不写状态、
     * 不设返回值）。所以这里的空桩不是"还没做"，而是与引擎一致。 */
    case 41:
    case 108:
    case 141:
        return;
    case 144: /* V3DMotionStop */
    case 143: /* V3DMotionPause */
    case 146: /* V3DSet */
    case 61:  /* Movie: skip the WMV in the prototype */
    case 64:  /* MovieStop */
    case 14:  /* Dissolve */
        return;
    case 131: /* TextTest */
        t->temp = fvp_make_int(0);
        return;
    case 133: /* ThreadNext: yield to the scheduler */
        t->blocked = 1;
        return;
    /* 136 ThreadStart(threadNo 0..31, entryOffset)：在槽 threadNo 里从
     * entryOffset 起一个协程；threadNo==0 先把 32 个槽全部重置。关着
     * （默认）时保持空桩。 */
    case 136:
        if (argc >= 2 &&
            fvp_vm_thread_start(vm, (int)args[0].i, (uint32_t)args[1].i)) {
            return;
        }
        break;
    /* 132 ThreadExit(which)：which==0 收掉所有子线程（自己除外），否则收掉
     * which 号槽。关着（默认）时保持空桩。 */
    case 132:
        if (fvp_vm_thread_exit(vm, t, argc >= 1 ? (int)args[0].i : 0)) {
            return;
        }
        break;
    /* 130 TextSuspendChr(layer)：等这个文本窗口把字逐字显示完。
     * 只在渲染层开了逐字显示（外壳 --typewriter）且窗口还没打完时才挂起；
     * 默认关闭时 gfx_text_busy() 在链接了 gfx 的程序里也返回 0，所以旧的
     * 「立即继续」行为不变（fvm.exe 没链接 gfx.c，弱符号为空）。 */
    case 130:
        if (argc >= 1 && vm->gfx_bridge != NULL && gfx_text_busy != NULL &&
            gfx_text_busy(vm->gfx_bridge->gfx, (int)args[0].i)) {
            t->blocked = 1;
        }
        return;
    /* 139 TimerSet(timerId 0..15, ms 1..100000)：启动 16 个定时器之一。
     * 138 TimerGet(timerId, opt 1..10000)：返回已过毫秒；给了 opt 则按
     * elapsed*opt/duration 换算（docs/vm-commands.md §5）。
     * 140 TimerSuspend(arg)：arg==1 暂停、0 恢复（engine+0x6a66bc）。
     * 已过时间一律由虚拟时钟推导（--timeline 关闭时恒 0）。 */
    case 139:
        if (argc >= 2) {
            (void)fvp_vm_timer_set(vm, (int)args[0].i, (int)args[1].i);
        }
        return;
    case 138:
        if (argc >= 1) {
            int id = (int)args[0].i;
            int opt = (argc >= 2) ? (int)args[1].i : 0;

            t->temp = fvp_make_int(fvp_vm_timer_get(vm, id, opt));
            return;
        }
        t->temp = fvp_make_int(0);
        return;
    case 140:
        if (argc >= 1) {
            fvp_vm_timer_suspend(vm, (int)args[0].i == 1);
        }
        return;
    /* 135 ThreadSleep(ms) / 137 ThreadWait(ms)：
     * 时间模型开启时按虚拟时钟定时唤醒，否则保持旧的 yield 语义。
     * `sleep_ms` 记原版 ctx+0x80C 的时长：ThreadSleep 的会被 ThreadRaise(134)
     * 按值匹配叫醒，ThreadWait 的不认（原版唤醒函数只看 sleep 位）。 */
    case 135:
    case 137:
    {
        int64_t ms = (argc >= 1) ? args[0].i : 0;

        if (ms < 0) {
            ms = 0;
        }
        if (ms > 300000) {
            ms = 300000;
        }
        t->sleep_ms = (cid == 135) ? (int)ms : -1;
        if (vm->timeline_on && argc >= 1) {
            t->timed = 1;
            t->wake_ms = vm->virtual_ms + (uint64_t)ms;
        }
        t->blocked = 1;
        return;
    }
    case 38: /* LipAnim(id 1..0xFFF, mode 0..3, …)：6 必填 + 2 可选 */
        if (argc >= 6) {
            (void)fvp_vm_lip_anim(vm, args, (int)argc);
            return;
        }
        break;
    case 39: /* LipSync(id 1..0xFFF, on)：原版只看第二参数是不是 null */
        if (argc >= 1) {
            (void)fvp_vm_lip_sync(vm, &args[0], argc >= 2 ? &args[1] : NULL);
            return;
        }
        break;
    case 134: /* ThreadRaise(ms)：叫醒睡眠时长 == ms 的线程（原版 0x43 7B80） */
        if (argc >= 1) {
            (void)fvp_vm_thread_raise(vm, &args[0]);
            return;
        }
        break;
    case 13: /* Debmess */
        fvp_sys_debmess(vm, args, argc);
        return;
    /* 76 PrimHit(primId 1..0xFFF, flag)：**返回 bool** —— 光标是否命中该图元。
     * 静态分析 0x43B9C0：arg0 必须是整型且在 1..0xFFF；flag = arg1 非空；
     * 命中后返回值类型置 1（bool）。三层算法（父链累加 / 矩形判定 / 掩码）
     * 见 docs/render-model.md §8，实现在 gfx.c 的 gfx_prim_hit()。
     * 没有 gfx 上下文时（fvm.exe）保持旧行为：落到下面的"未实现"分支，返回值 0
     * —— 冻结基线因此一个字节都不动。 */
    /* 43 MotionAlphaStop(primId) / 46 MotionAnimStop(primId) /
     * 55 MotionMoveS2Stop(primId) / 58 MotionMoveZStop(primId)：撤掉该图元的
     * 对应一类补间。语义见 docs/vm-commands.md §5：
     *   43 → alpha 补间、55 → 缩放补间；46（帧动画）与 58（Z）本引擎没有对应补间，
     *   所以是空操作 —— 原版这两条也只是"撤掉那种补间"，空表上撤即为空操作。
     * 没有 gfx 上下文时（fvm.exe）落到下面的"未实现"分支，行为与以前一致。 */
    case 43:
    case 46:
    case 55:
    case 58:
        if (vm->gfx_bridge != NULL && gfx_motion_cancel != NULL && argc >= 1 &&
            args[0].type == (uint8_t)FVP_INT) {
            int kind = -1;

            if (cid == 43) {
                kind = 1; /* FVP_TWEEN_ALPHA */
            } else if (cid == 55) {
                kind = 2; /* FVP_TWEEN_SCALE */
            }
            (void)gfx_motion_cancel(vm->gfx_bridge->gfx, (int)args[0].i, kind);
            return;
        }
        break;
    /* 8 ControlMask(argc 1) / 9 ControlPulse(argc 0) / 10 CursorChange(argc 1) /
     * 11 CursorMove(argc 3)：写 engine 的 +0x458 / +0xfd8 / +0x88 和鼠标坐标。
     * handler 依次 0x437440 / 0x437460 / 0x437470 / 0x4374a0；只记录状态，
     * 不猜下游（见 src/vm.h 的 fvp_vm_control_mask 注释）。 */
    /* 23 GraphRGB(graphId 0..0xFFF, r, g, b)：handler 0x43a3d0，分量 0..200、
     * 缺省 100；只记录（见 src/vm.h 的 graph_rgb 注释）。
     * 109 SysProjFolder(str)：handler 0x43c7f0，只接受字符串参数。 */
    /* 110 TextBuff(twIdx 0..31, a, b)：handler 0x43c830 → call 0x430ea0(tw,a,b)，
     * a/b 缺省 8、上限 0x400，写 tw+0x5dc/0x5de。只记录。 */
    /* 147 WindowMode(mode -1..6) / 17 ExitMode(mode 0..4)：handler
     * 0x43d0d0 / 0x43a1a0，真正改的是窗口层字段，本站只记录（见 vm.h）。 */
    /* 21 GaijiLoad(str1, size 12..0x40, str2)：handler 0x43f4f0，
     * call 0x43e9c0(engine+0x6a49cc, str1, size, str2)。只记录。
     * 142 V3DMotion(..., timeMs 1..300000, mode 1..5, flag)：handler 0x43cfe0。
     * 本站没有 3D 场景，只记录。 */
    /* 92 Rand()：handler 0x437a20 → 返回 CRT rand()（0..0x7FFF）。
     * 124 TextRepaint()：handler 0x437a50 → 让文本重画；本站每帧全画，空操作。 */
    /* 40 Load(slot 0..999) / 96 SaveWrite(slot 0..999)：handler 0x43a870 /
     * 0x43c1e0 —— 只把"请求 + 槽号"记进引擎字段（+0xfcb / +0xfca / +0xfcc），
     * 真正的文件读写由外壳/界面那一侧做。 */
    /* 114 TextFontCount() / 115 TextFontGet() / 116 TextFontName(idx) /
     * 117 TextFontSet(idx)：原版操作 engine+0x94 的字体表（见 src/vm.h）。 */
    /* 60 MotionPause(primId 0..0xFFF, on 0..1)：handler 0x43AB60 → prim+9。
     * 121 TextPause(twIdx 0..31, on 0..1)：handler 0x43C9C0 → tw+0x5DA，
     *     同时转给 gfx（打字机推进时跳过暂停的窗口）。 */
    /* 27 InputFlash() / 33 InputGetRepeat() / 37 InputSetClick(v 0/1)。 */
    /* 12 CursorShow(v) / 16 ExitDialog() / 63 MovieStop()。 */
    /* 98 SnowStart(layer, …) / 99 SnowStop(layer, …) / 84 PrimSetSnow(primId, on, …)。 */
    /* 66 PartsMotion / 67 PartsMotionPause / 68 PartsMotionStop / 70 PartsRGB /
     * 74 PrimGroupMove：都是部件/组级别的记录（本站没有对应渲染语义）。 */
    case 66:
        if (argc >= 2) {
            (void)fvp_vm_parts_motion(vm, &args[0], &args[1],
                                      argc >= 3 ? &args[2] : NULL);
            return;
        }
        break;
    case 67:
        if (argc >= 2) {
            (void)fvp_vm_parts_motion_pause(vm, &args[0], &args[1]);
            return;
        }
        break;
    case 68:
        if (argc >= 1) {
            (void)fvp_vm_parts_motion_stop(vm, &args[0]);
            return;
        }
        break;
    case 70:
        if (argc >= 4) {
            (void)fvp_vm_parts_rgb(vm, &args[0], &args[1], &args[2], &args[3]);
            return;
        }
        break;
    case 74:
        if (argc >= 2) {
            (void)fvp_vm_prim_group_move(vm, &args[0], &args[1]);
            return;
        }
        break;
    case 97: /* Snow: 18-arg command, arg1..arg14 required (0x43C230) */
        if (argc >= 14) {
            (void)fvp_vm_snow_ex(vm, args, (int)argc);
            return;
        }
        break;
    case 98:
        if (argc >= 1) {
            (void)fvp_vm_snow(vm, &args[0], 1);
            return;
        }
        break;
    case 99:
        if (argc >= 1) {
            (void)fvp_vm_snow(vm, &args[0], 0);
            return;
        }
        break;
    case 84:
        if (argc >= 2) {
            (void)fvp_vm_prim_snow(vm, &args[0], &args[1]);
            return;
        }
        break;
    case 12:
        if (argc >= 1) {
            (void)fvp_vm_cursor_show(vm, &args[0]);
            return;
        }
        break;
    case 16:
        fvp_vm_exit_dialog(vm);
        return;
    case 63:
        fvp_vm_movie_stop(vm);
        return;
    case 27:
        fvp_vm_input_flash(vm);
        return;
    case 33:
        t->temp = fvp_make_int(fvp_vm_input_repeat(vm));
        return;
    case 37:
        if (argc >= 1) {
            (void)fvp_vm_input_set_click(vm, &args[0]);
            return;
        }
        break;
    case 60:
        if (argc >= 2) {
            (void)fvp_vm_motion_pause(vm, &args[0], &args[1]);
            return;
        }
        break;
    case 121:
        if (argc >= 2) {
            if (fvp_vm_text_pause(vm, &args[0], &args[1]) &&
                vm->gfx_bridge != NULL && gfx_text_set_pause != NULL) {
                (void)gfx_text_set_pause(vm->gfx_bridge->gfx,
                                         (int)args[0].i, (int)args[1].i);
            }
            return;
        }
        break;
    case 114:
        t->temp = fvp_make_int(fvp_vm_font_count(vm));
        return;
    case 115:
        t->temp = fvp_make_int(fvp_vm_font_get(vm));
        return;
    case 116: {
        const char *name =
            fvp_vm_font_name(vm, argc >= 1 ? &args[0] : NULL);

        t->temp = fvp_make_str(fvp_arena_str(&vm->arena, name != NULL ? name : "",
                                             name != NULL ? strlen(name) : 0u));
        return;
    }
    case 117:
        if (argc >= 1) {
            (void)fvp_vm_font_set(vm, &args[0]);
            return;
        }
        break;
    case 40:
        if (argc >= 1) {
            (void)fvp_vm_request_load(vm, &args[0]);
            return;
        }
        break;
    case 96:
        if (argc >= 1) {
            (void)fvp_vm_request_save(vm, &args[0]);
            return;
        }
        break;
    case 92:
        t->temp = fvp_make_int(fvp_vm_rand(vm));
        return;
    case 124:
        vm->text_repaint_calls += 1u;
        return;
    case 21:
        if (argc >= 3) {
            if (fvp_vm_gaiji_load(vm, &args[0], &args[1], &args[2]) &&
                vm->gfx_bridge != NULL && gfx_graph_load != NULL &&
                gfx_gaiji_add != NULL && vm->gaiji_name2 != NULL) {
                /* 外字图装进一个高号图形槽（0x0F00 起，最多 GFX_MAX_GAIJI 个；
                 * 这个区间不会与脚本自己的图元/图形槽冲突），然后登记
                 * 「码点 → 槽」；绘制时由 gfx.c 的 draw_text_run 接管。 */
                int slot = 0x0F00 + (int)((vm->gaiji_calls - 1u) % 64u);
                uint32_t cp = fvp_utf8_first(vm->gaiji_name1);

                if (cp != 0u &&
                    gfx_graph_load(vm->gfx_bridge->gfx, slot,
                                   vm->gaiji_name2) == 0) {
                    (void)gfx_gaiji_add(vm->gfx_bridge->gfx, cp,
                                        vm->gaiji_size, slot);
                }
            }
            return;
        }
        break;
    case 142:
        if (argc >= 5) {
            (void)fvp_vm_v3d_motion(vm, &args[3], &args[4]);
            return;
        }
        break;
    case 147:
        if (argc >= 1) {
            (void)fvp_vm_window_mode(vm, &args[0]);
            return;
        }
        break;
    case 17:
        if (argc >= 1) {
            (void)fvp_vm_exit_mode(vm, &args[0]);
            return;
        }
        break;
    case 110:
        if (argc >= 1) {
            (void)fvp_vm_text_buff(vm, &args[0], argc >= 2 ? &args[1] : NULL,
                                    argc >= 3 ? &args[2] : NULL);
            return;
        }
        break;
    case 23:
        if (argc >= 4) {
            (void)fvp_vm_graph_rgb(vm, &args[0], &args[1], &args[2], &args[3]);
            return;
        }
        break;
    case 109:
        if (argc >= 1) {
            (void)fvp_vm_set_proj_folder(vm, &args[0]);
            return;
        }
        break;
    case 8:
        if (argc >= 1) {
            fvp_vm_control_mask(vm, &args[0]);
            return;
        }
        break;
    case 9:
        fvp_vm_control_pulse(vm);
        return;
    case 10:
        if (argc >= 1) {
            (void)fvp_vm_cursor_change(vm, &args[0]);
            return;
        }
        break;
    case 11:
        if (argc >= 3) {
            (void)fvp_vm_cursor_move(vm, &args[0], &args[1], &args[2]);
            return;
        }
        break;
    case 76:
        if (vm->gfx_bridge == NULL || gfx_prim_hit == NULL || argc < 1 ||
            args[0].type != (uint8_t)FVP_INT) {
            break;
        }
        {
            int id = (int)args[0].i;

            if (id < 1 || id > 0xFFF) {
                break; /* 原版对非法 id 不设返回值 */
            }
            {
                int hit = gfx_prim_hit(vm->gfx_bridge->gfx, id,
                                       argc >= 2 ? 1 : 0, (int)vm->cx,
                                       (int)vm->cy);

                vm->prim_hit_calls += 1u;
                if (hit) {
                    vm->prim_hit_hits += 1u;
                }
                t->temp = fvp_make_int(hit ? 1 : 0);
            }
        }
        return;
    /* 26 HistorySet(kind, value)：台词历史（回想）的写入。静态分析 0x43A5E0：
     *   arg0 **type 0（null）** → "开新的一条"：count = min(count+1,100)、
     *                    pos = (pos+1)%100、该槽清成 {NULL, NULL, -1}
     *   arg0 == 0 / 1 → 仅当 arg1 是**字符串**时写 text0 / text1（整串替换）
     *   arg0 == 2     → 仅当 arg1 是整型且 >= 0 时写 value
     *   其它 kind 或类型不符 → 整条命令被忽略（不报错）
     * 判定细节（含"int 0 不等于 null"这条）集中在 fvp_vm_history_set()，
     * 由 tests/test_history.c 的 VM 级用例钉住。 */
    case 26:
        (void)fvp_vm_history_set(vm, args, (int)argc);
        return;
    /* 25 HistoryGet(kind, idx)：读台词历史。静态分析 0x43A490 / 0x438090 /
     * 0x4380C0 / 0x4380F0：
     *   arg0 **type 0（null）** → 返回条数 count（整型）
     *   arg0 = 0/1    → 返回该条 text0 / text1 的**字符串**（字段未设置时是空串，
     *                   不是 null）；idx 越界返回"无返回值"
     *   arg0 = 2      → 返回 value（整型）；字段为 -1（未设置）或 idx 越界返回"无"
     *   idx 必须 0 <= idx < count；槽位是 pos - idx（负数回绕 +100），即 idx 0
     *   是最近一条。判定集中在 fvp_vm_history_get()。 */
    case 25:
        {
            FvpValue v;

            if (fvp_vm_history_get(vm, args, (int)argc, &v)) {
                t->temp = v;
            }
        }
        return;
    case 22: /* GraphLoad */
        t->temp = fvp_make_int(1);
        return;
    default:
        break;
    }

    /* Unimplemented commands.  gfx 已处理（但主 switch 没有 case）的命令不再计入
     * 未实现直方图 —— 否则外壳日志会把 PrimSetNull 这类实现过的命令报成缺口。 */
    if (!vm->gfx_handled) {
        fvp_vm_count_unknown(vm, cid);
    }
    if (fvp_sys_is_test(cid)) {
        t->temp = fvp_make_int(1);
    } else if (cid < FVP_CMD_COUNT && fvp_cmd_returns[cid]) {
        t->temp = fvp_make_int(0);
    }
}