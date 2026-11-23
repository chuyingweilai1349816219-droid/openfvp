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
#include "gen_cmd_table.h"
#include "snd.h"

/* The render layer is optional: fvm.exe links without src/gfx.o, so every
 * gfx entry point this file references is a weak symbol.  They are only
 * called when fvp_vm_attach_gfx() installed a context, and a context can
 * only be installed by a program that links the renderer. */
#pragma weak gfx_graph_load
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
#pragma weak gfx_text_set_pos
#pragma weak gfx_text_set_size

/* Same story for the audio layer (src/snd.c): bin/fvm.exe links neither it
 * nor the BIN package reader, so every symbol is weak here and no audio code
 * can run unless fvp_vm_attach_snd() installed a context. */
#pragma weak snd_is_playing
#pragma weak snd_play_ogg
#pragma weak snd_play_wav
#pragma weak snd_set_vol
#pragma weak snd_stop

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

static void fvp_sys_gfx(FvpVM *vm, FvpThread *t, const FvpValue *a,
                        uint32_t argc, uint16_t cid)
{
    FvpGfxBridge *b = vm->gfx_bridge;
    Gfx *g = (b != NULL) ? b->gfx : NULL;
    int id;

    if (g == NULL) {
        return;
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
        if (argc > 2 && fvp_gfx_is_int(&a[2])) {
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
    case 48: /* MotionMove(id, x, y, z, ...) */
        if (argc > 1 && fvp_gfx_is_int(&a[1])) {
            (void)gfx_set_prim(g, id, GFX_F_X, fvp_gfx_int(&a[1]));
        }
        if (argc > 2 && fvp_gfx_is_int(&a[2])) {
            (void)gfx_set_prim(g, id, GFX_F_Y, fvp_gfx_int(&a[2]));
        }
        if (argc > 3 && fvp_gfx_is_int(&a[3])) {
            (void)gfx_set_prim(g, id, GFX_F_Z, fvp_gfx_int(&a[3]));
        }
        break;
    case 49: /* MotionMoveStop: no renderer state in the prototype */
    case 52: /* MotionMoveRStop */
        break;
    case 51: /* MotionMoveR(id, x, y, ...) */
        if (argc > 1 && fvp_gfx_is_int(&a[1])) {
            (void)gfx_set_prim(g, id, GFX_F_X, fvp_gfx_int(&a[1]));
        }
        if (argc > 2 && fvp_gfx_is_int(&a[2])) {
            (void)gfx_set_prim(g, id, GFX_F_Y, fvp_gfx_int(&a[2]));
        }
        break;
    case 54: /* MotionMoveS2(id, sx, sy, ...) */
        if (argc > 1 && fvp_gfx_is_int(&a[1])) {
            (void)gfx_set_prim(g, id, GFX_F_SCALE_X, fvp_gfx_int(&a[1]));
        }
        if (argc > 2 && fvp_gfx_is_int(&a[2])) {
            (void)gfx_set_prim(g, id, GFX_F_SCALE_Y, fvp_gfx_int(&a[2]));
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
        (void)gfx_set_prim(g, id, GFX_F_TYPE, 5);
        (void)gfx_set_prim(g, id, GFX_F_TW,
                           argc > 1 ? fvp_gfx_int(&a[1]) : 0);
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
        if (argc > 1 && fvp_gfx_is_int(&a[1])) {
            (void)gfx_set_prim(g, id, GFX_F_W, fvp_gfx_int(&a[1]));
        }
        if (argc > 2 && fvp_gfx_is_int(&a[2])) {
            (void)gfx_set_prim(g, id, GFX_F_H, fvp_gfx_int(&a[2]));
        }
        break;
    case 90: /* PrimSetXY(id, x, y) */
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
    case 120: /* TextOutSize(layer, w, h) */
        if (argc > 2) {
            gfx_text_set_out_size(g, id, fvp_gfx_int(&a[1]),
                                  fvp_gfx_int(&a[2]));
        }
        break;
    case 122: /* TextPos(layer, x, y) */
        if (argc > 2) {
            gfx_text_set_pos(g, id, fvp_gfx_int(&a[1]), fvp_gfx_int(&a[2]));
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
    case 126: /* TextSize(layer, w, h) */
        if (argc > 2) {
            gfx_text_set_size(g, id, fvp_gfx_int(&a[1]),
                              fvp_gfx_int(&a[2]));
        }
        break;
    case 129: /* TextSpeed: no gfx entry point (text is drawn as static
               * lines, like render.py); intentionally a no-op */
        break;
    default:
        break;
    }
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

static float fvp_snd_gain(const FvpSndBridge *b, const FvpSndSlot *sl)
{
    double g;
    if (b == NULL || sl == NULL) {
        return 0.0f;
    }
    g = (double)fvp_snd_chan_effective(b, sl) / 100.0;
    g *= (double)fvp_snd_master_effective(b) / 100.0;
    if (sl->type >= 0 && sl->type < FVP_SND_TYPES) {
        g *= (double)fvp_snd_type_effective(b, sl->type) / 100.0;
    }
    if (sl->silent) {
        g = 0.0;
    }
    if (g < 0.0) g = 0.0;
    if (g > 1.0) g = 1.0;
    return (float)g;
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
        return SND_CH_BGM;
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

void fvp_syscall(FvpVM *vm, FvpThread *t, uint16_t cid)
{
    uint32_t argc = cid < FVP_CMD_COUNT ? (uint32_t)fvp_cmd_argc[cid] : 0u;
    FvpValue args[FVP_MAX_ARGC] = {0}; /* zero == fvp_make_int(0) */
    int32_t sp_before = t->sp;
    uint32_t k;

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
            fvp_sys_gfx(vm, t, raw, (uint32_t)rargc, cid);
        } else {
            fvp_sys_gfx(vm, t, args, argc, cid);
        }
    }

    /* Audio commands: only when a context is attached (fvm.exe: never).
     * Handled commands return here, so the unknown-command counter and the
     * temp-value rules keep their exact prototype behaviour otherwise. */
    if (vm->snd_bridge != NULL && fvp_sys_snd(vm, t, args, argc, cid)) {
        return;
    }

    switch (cid) {
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
    case 36: /* InputGetUp */
        t->temp = fvp_make_int(0);
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
    case 137: /* ThreadWait: yield to the scheduler */
        t->blocked = 1;
        return;
    case 13: /* Debmess */
        fvp_sys_debmess(vm, args, argc);
        return;
    case 22: /* GraphLoad */
        t->temp = fvp_make_int(1);
        return;
    default:
        break;
    }

    /* Unimplemented commands. */
    fvp_vm_count_unknown(vm, cid);
    if (fvp_sys_is_test(cid)) {
        t->temp = fvp_make_int(1);
    } else if (cid < FVP_CMD_COUNT && fvp_cmd_returns[cid]) {
        t->temp = fvp_make_int(0);
    }
}