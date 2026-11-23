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
#include <string.h>

#include "gfx.h"
#include "gen_cmd_table.h"

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