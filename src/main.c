/* FVP VM in C -- command line entry point.
 *
 * CLI mirrors the Python reference prototype:
 *   fvm.exe <script.hcb> [--enc cp932|gbk] [--steps N] [--click N]
 *           [--cursor X,Y] [--text-out FILE] [--quiet] [--eol crlf|lf]
 *           [--pctrace FILE] [--savebin FILE] [--dump-globals FILE]
 *
 * The text output file is byte-compatible with Python's:
 *   "[<layer>] <text>\n" per TextPrint, written with CRLF on Windows (the
 *   reference opens the file in text mode, which translates newlines).
 */
#include <errno.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "encoding.h"
#include "fvp.h"
#include "hcb.h"
#include "save.h"
#include "savehist.h"
#include "save_vm.h"
#include "vm.h"

static void fvp_usage(FILE *f, const char *prog)
{
    fprintf(f,
            "usage: %s <script.hcb> [options]\n"
            "  --enc NAME       text encoding: cp932 (default) or gbk\n"
            "  --steps N        instruction budget (default 2000000)\n"
            "  --click N        synthetic click every N steps (default 0)\n"
            "  --click-mask N    synthetic click bitmask (default 0x0B)\n"
            "  --cursor X,Y     synthetic cursor position (default 960,540)\n"
            "  --text-out FILE  write \"[layer] text\" lines (CRLF) to FILE\n"
            "  --quiet          do not echo text / debmess to stdout\n"
            "  --eol crlf|lf    line endings for --text-out (default crlf)\n"
            "  --pctrace FILE   log executed pc values (debug aid)\n"
            "  --savebin FILE   seed globals from a save/save.bin\n"
"  --save-history FILE  restore the dialogue history ring from a save slot\n"
            "  --pc N           start at an explicit pc (research hook)\n"
            "  --timeline       use the virtual clock (ThreadSleep/ThreadWait wake by ms)\n"
            "  --bool-type      jmpcond 只认 type==0 为假、比较类把真假写进类型（原版语义，\n"
            "                   会改变输出；默认关，见 docs/roadmap.md 第二步）\n"
            "  --no-threads     disable coroutine slots 0..31 (A/B against the old stub)\n"
            "  --unknown-all    print every unknown command id (not just the top 15)\n"
            "  --dump-textattr  print every configured text window (color/size/speed)\n"
            "  --dump-savedata  print every SaveData(95) record (kind/idx/value)\n"
            "  --dump-history   print every history record (HistorySet 26 / Get 25)\n"
            "  --dump-globals FILE  write \"index value\" lines for set globals\n"
            "  --stall-after N  stop after N steps without any syscall (default 2000000,\n"
            "                   0 = off): catches scripts that spin in their own loop\n"
            "  --stall-exit     exit with code 6 when a stall was detected\n",
            prog);
}

static int fvp_parse_u64(const char *s, uint64_t *out)
{
    char *end = NULL;
    unsigned long long v;
    if (s == NULL || *s == '\0' || *s == '-') {
        return 0;
    }
    errno = 0;
    v = strtoull(s, &end, 10);
    if (errno != 0 || end == NULL || *end != '\0') {
        return 0;
    }
    *out = (uint64_t)v;
    return 1;
}

static int fvp_parse_i64(const char *s, int64_t *out)
{
    char *end = NULL;
    long long v;
    if (s == NULL || *s == '\0') {
        return 0;
    }
    errno = 0;
    v = strtoll(s, &end, 10);
    if (errno != 0 || end == NULL || *end != '\0') {
        return 0;
    }
    *out = (int64_t)v;
    return 1;
}

static void fvp_write_eol(FILE *f, const char *data, size_t len, int crlf)
{
    size_t start = 0;
    size_t i;
    if (!crlf) {
        fwrite(data, 1, len, f);
        return;
    }
    for (i = 0; i < len; ++i) {
        if (data[i] == '\n') {
            fwrite(data + start, 1, i - start, f);
            fwrite("\r\n", 1, 2, f);
            start = i + 1u;
        }
    }
    fwrite(data + start, 1, len - start, f);
}

static void fvp_layer_text(char *buf, size_t buflen, const FvpValue *layer)
{
    if (layer->type == (uint8_t)FVP_INT) {
        snprintf(buf, buflen, "%lld", (long long)layer->i);
        return;
    }
    if (layer->type == (uint8_t)FVP_FLOAT) {
        /* Python's "%d" on a float truncates toward zero. */
        double d = layer->f;
        if (!isfinite(d)) {
            /* Python would raise OverflowError/ValueError for inf/nan. */
            snprintf(buf, buflen, "0");
            return;
        }
        if (d >= 9223372036854775807.0) {
            snprintf(buf, buflen, "%lld", (long long)INT64_MAX);
        } else if (d <= -9223372036854775808.0) {
            snprintf(buf, buflen, "%lld", (long long)INT64_MIN);
        } else {
            snprintf(buf, buflen, "%lld", (long long)d);
        }
        return;
    }
    /* Python would raise TypeError for a string layer; be lenient. */
    if (layer->str != NULL) {
        size_t n = layer->str->len;
        if (n >= buflen) {
            n = buflen - 1u;
        }
        memcpy(buf, layer->str->data, n);
        buf[n] = '\0';
    } else {
        buf[0] = '\0';
    }
}

static int fvp_write_text_file(const char *path, const FvpVM *vm, int crlf)
{
    FILE *f = fopen(path, "wb");
    size_t i;
    if (f == NULL) {
        fprintf(stderr, "fvm: cannot write %s\n", path);
        return 0;
    }
    for (i = 0; i < vm->ntext; ++i) {
        char layer[64];
        const FvpStr *txt = vm->text[i].text;
        fvp_layer_text(layer, sizeof(layer), &vm->text[i].layer);
        fputc('[', f);
        fwrite(layer, 1, strlen(layer), f);
        fwrite("] ", 1, 2, f);
        fvp_write_eol(f, txt->data, txt->len, crlf);
        if (crlf) {
            fwrite("\r\n", 1, 2, f);
        } else {
            fputc('\n', f);
        }
    }
    if (fclose(f) != 0) {
        fprintf(stderr, "fvm: error writing %s\n", path);
        return 0;
    }
    return 1;
}

typedef struct FvpUnkTop {
    uint16_t cid;
    uint32_t count;
} FvpUnkTop;

static int g_unknown_all;
static int g_dump_textattr; /* --dump-textattr: dump every configured text window */
static int g_dump_savedata; /* --dump-savedata: dump every SaveData(95) record */
static int g_dump_history;  /* --dump-history: dump every history record */

static void fvp_print_summary(const FvpVM *vm)
{
    FvpUnkTop *top;
    size_t i;
    size_t j;
    size_t shown;
    printf("\n=== summary ===\n");
    printf("steps      %llu\n", (unsigned long long)vm->steps);
    printf("text lines %llu\n", (unsigned long long)vm->ntext);
    printf("threads    %llu\n", (unsigned long long)vm->nthreads);
    printf("coroutines %s: start %llu, exit %llu, reset %llu\n",
           vm->threads_on ? "on" : "off",
           (unsigned long long)vm->thread_starts,
           (unsigned long long)vm->thread_exits,
           (unsigned long long)vm->thread_resets);
    printf("virtual ms %llu\n", (unsigned long long)vm->virtual_ms);
    printf("text attrs %d window(s) configured\n",
           fvp_textattr_configured_count(vm->textattr, FVP_TEXT_WINDOWS));
    if (g_dump_textattr) {
        int wi;

        for (wi = 0; wi < FVP_TEXT_WINDOWS; ++wi) {
            const FvpTextAttr *ta = &vm->textattr[wi];

            if (ta->set_mask == 0u) {
                continue;
            }
            printf("  tw %2d: mask=0x%x color=%d,%d,%d size=%dx%d speed=%d ms "
                   "shadow=%d space=%d,%d skip=%d font=%d,%d\n"
                   "           fmt=%d,%d,%d,%d,%d,%d func=%d,%d,%d\n",
                   wi, ta->set_mask, ta->r, ta->g, ta->b, ta->w, ta->h,
                   ta->speed_ms, ta->shadow_dist, ta->space_x, ta->space_y,
                   ta->skip_mode, ta->font_id, ta->font_id2,
                   ta->fmt_pad_a, ta->fmt_pad_b, ta->fmt_off_a,
                   ta->fmt_off_b, ta->space_x, ta->space_y,
                   ta->func_a, ta->func_b, ta->func_c);
        }
    }
    printf("save state slot=%d str=%d/%d/%d thumb=%dx%d\n", vm->save_slot,
           vm->save_str[0] != NULL, vm->save_str[1] != NULL,
           vm->save_str[2] != NULL, vm->save_thumb_w, vm->save_thumb_h);
    printf("savedata   %d record(s) of %llu call(s)\n", vm->save_data_n,
           (unsigned long long)vm->save_data_seen);
    if (g_dump_savedata && vm->save_data_n > 0) {
        int si;

        for (si = 0; si < vm->save_data_n; ++si) {
            printf("  savedata %2d: kind=%d idx=%d value=%lld\n", si,
                   vm->save_data[si].kind, vm->save_data[si].idx,
                   (long long)vm->save_data[si].value);
        }
    }
    /* 台词历史（HistorySet 26 / HistoryGet 25）：100 条环形记录，环形槽位见
     * src/history.h。默认只报计数，--dump-history 才逐条打印。 */
    /* ControlMask 8 / ControlPulse 9 / CursorChange 10 / CursorMove 11 */
    printf("control    mask=%d pulse=%d cursor=%d pos=%d,%d "
           "(mask %llu / pulse %llu / change %llu / move %llu)\n",
           (int)vm->control_mask, (int)vm->pulse, vm->cursor_shape, vm->cursor_x,
           vm->cursor_y, (unsigned long long)vm->control_calls[0],
           (unsigned long long)vm->control_calls[1],
           (unsigned long long)vm->control_calls[2],
           (unsigned long long)vm->control_calls[3]);
    printf("parts      motion %llu / pause %llu / stop %llu call(s) ; rgb "
           "%llu call(s) ; group move %llu call(s)\n",
           (unsigned long long)vm->parts_motion_calls,
           (unsigned long long)vm->parts_motion_pause_calls,
           (unsigned long long)vm->parts_motion_stop_calls,
           (unsigned long long)vm->parts_rgb_calls,
           (unsigned long long)vm->prim_group_move_calls);
    printf("snow       layer=%d,%d (start %llu / stop %llu), prim %llu call(s)\n",
           (int)vm->snow_on[0], (int)vm->snow_on[1],
           (unsigned long long)vm->snow_start_calls,
           (unsigned long long)vm->snow_stop_calls,
           (unsigned long long)vm->prim_snow_calls);
    if (vm->snow_calls > 0) {
        printf("snow full  %d call(s), layer=%d, arg2..14 =", vm->snow_calls,
               vm->snow_layer);
        {
            int i;

            for (i = 1; i < 14; ++i) {
                printf(" %d", vm->snow_vals[i]);
            }
            printf(", opt mask=0x%x\n", (unsigned)vm->snow_opt_mask);
        }
    }
    printf("cursor     show=%d ; exit dialog %llu call(s) ; movie stop %llu "
           "call(s)\n", (int)vm->cursor_show,
           (unsigned long long)vm->exit_dialog_calls,
           (unsigned long long)vm->movie_stop_calls);
    printf("input      click=%d repeat=%d flash=%llu setclick=%llu\n",
           (int)vm->input_click, vm->input_repeat,
           (unsigned long long)vm->input_flash_calls,
           (unsigned long long)vm->input_setclick_calls);
    printf("pause      text %llu call(s), motion %llu call(s)\n",
           (unsigned long long)vm->text_pause_calls,
           (unsigned long long)vm->motion_pause_calls);
    printf("fonts      %d name(s), current=%d\n", fvp_vm_font_count(vm),
           fvp_vm_font_get(vm));
    printf("save/load  request slot=%d (load %llu call(s), save %llu "
           "call(s))\n", vm->request_slot,
           (unsigned long long)vm->load_calls,
           (unsigned long long)vm->save_calls);
    printf("rand       state=%u ; text repaint %llu call(s)\n",
           (unsigned)vm->rand_state,
           (unsigned long long)vm->text_repaint_calls);
    printf("threads    raise %llu call(s), last ms=%d, woken %llu\n",
           (unsigned long long)vm->thread_raise_calls, vm->thread_raise_ms,
           (unsigned long long)vm->thread_raise_woken);
    printf("lip        anim %llu call(s) id=%d mode=%d ; sync %llu call(s) "
           "id=%d on=%d\n",
           (unsigned long long)vm->lip_anim_calls, vm->lip_anim_id,
           vm->lip_anim_mode, (unsigned long long)vm->lip_sync_calls,
           vm->lip_sync_id, vm->lip_sync_on);
    /* GaijiLoad 21 / V3DMotion 142 */
    printf("gaiji      %llu call(s), size=%d name=%s / %s\n",
           (unsigned long long)vm->gaiji_calls, vm->gaiji_size,
           vm->gaiji_name1 != NULL ? vm->gaiji_name1 : "-",
           vm->gaiji_name2 != NULL ? vm->gaiji_name2 : "-");
    printf("v3d        %llu call(s), time=%d ms mode=%d\n",
           (unsigned long long)vm->v3d_calls, vm->v3d_time_ms, vm->v3d_mode);
    /* WindowMode 147 / ExitMode 17 */
    printf("window     mode=%d state=%d (%llu call(s)); exit mode=%d "
           "(%llu call(s))\n", vm->window_mode_last, vm->window_state,
           (unsigned long long)vm->window_mode_calls, vm->exit_mode_last,
           (unsigned long long)vm->exit_mode_calls);
    /* TextBuff 110 */
    printf("text buff  %llu call(s)\n",
           (unsigned long long)vm->text_buff_calls);
    /* GraphRGB 23 / SysProjFolder 109 */
    printf("graph rgb  %llu call(s), folder=%s (folder calls %llu)\n",
           (unsigned long long)vm->graph_rgb_calls,
           vm->proj_folder != NULL ? vm->proj_folder : "-",
           (unsigned long long)vm->proj_folder_calls);
    printf("history    count=%u pos=%u filled=%d set=%llu get=%llu\n",
           (unsigned)vm->history.count, (unsigned)vm->history.pos,
           fvp_history_filled(&vm->history),
           (unsigned long long)vm->history.sets,
           (unsigned long long)vm->history.gets);
    /* 卡死侦测：脚本自旋到没有任何 syscall 时由 VM 停下（见 src/vm.h）。 */
    if (fvp_vm_stalled(vm)) {
        printf("stall      pc=0x%x after %llu steps without a syscall\n",
               (unsigned)vm->stall_pc, (unsigned long long)vm->stall_span);
    } else {
        printf("stall      none\n");
    }
    printf("prim hit   %llu call(s), %llu hit(s)\n",
           (unsigned long long)vm->prim_hit_calls,
           (unsigned long long)vm->prim_hit_hits);
    if (g_dump_history) {
        int hi;

        /* 直接扫 100 个环形槽：count 只表示"开过多少条"，本作脚本从不发
         * HistorySet(null)，所以 count 恒 0、pos 恒 0，只有槽 0 有内容。
         * 按槽打印才能看到"引擎当前到底记了什么"。 */
        for (hi = 0; hi < FVP_HISTORY_SLOTS; ++hi) {
            const FvpHistoryEntry *e = &vm->history.entry[hi];

            if (e->text0 == NULL && e->text1 == NULL && e->value == -1) {
                continue;
            }
            printf("  history slot %3d: value=%s%lld text1=\"", hi,
                   (e->value == -1) ? "(unset)" : "",
                   (e->value == -1) ? 0LL : (long long)e->value);
            if (e->text1 != NULL) {
                fwrite(e->text1, 1, strlen(e->text1), stdout);
            }
            printf("\" text0=\"");
            if (e->text0 != NULL) {
                fwrite(e->text0, 1, strlen(e->text0), stdout);
            }
            printf("\"\n");
        }
    }
    {
        size_t ti;

        for (ti = 0; ti < vm->nthreads; ++ti) {
            printf("thread %d: text lines %llu\n", vm->threads[ti]->id,
                   (unsigned long long)vm->threads[ti]->ntext);
        }
    }
    printf("unknown cmds: ");
    if (vm->nunknown == 0) {
        printf("\n");
        return;
    }
    top = (FvpUnkTop *)fvp_xmalloc(vm->nunknown * sizeof(FvpUnkTop));
    for (i = 0; i < vm->nunknown; ++i) {
        top[i].cid = vm->unknown_order[i];
        top[i].count = vm->unknown_count[vm->unknown_order[i]];
    }
    /* stable insertion sort, descending count (Python: sorted(-count)) */
    for (i = 1; i < vm->nunknown; ++i) {
        FvpUnkTop key = top[i];
        j = i;
        while (j > 0 && top[j - 1].count < key.count) {
            top[j] = top[j - 1];
            j -= 1u;
        }
        top[j] = key;
    }
    shown = g_unknown_all ? vm->nunknown
                        : (vm->nunknown < 15u ? vm->nunknown : 15u);
    for (i = 0; i < shown; ++i) {
        if (i != 0) {
            printf(", ");
        }
        printf("%u\xC3\x97%u", (unsigned)top[i].cid, (unsigned)top[i].count);
    }
    printf("\n");
    free(top);
}

/* 整文件读入（调用者 free）。失败返回 NULL。 */
static unsigned char *fvp_read_whole_file(const char *path, size_t *len_out)
{
    FILE *f;
    long n;
    unsigned char *buf;

    if (len_out != NULL) {
        *len_out = 0;
    }
    f = fopen(path, "rb");
    if (f == NULL) {
        return NULL;
    }
    if (fseek(f, 0, SEEK_END) != 0) {
        fclose(f);
        return NULL;
    }
    n = ftell(f);
    if (n <= 0) {
        fclose(f);
        return NULL;
    }
    rewind(f);
    buf = (unsigned char *)malloc((size_t)n);
    if (buf == NULL) {
        fclose(f);
        return NULL;
    }
    if (fread(buf, 1, (size_t)n, f) != (size_t)n) {
        free(buf);
        fclose(f);
        return NULL;
    }
    fclose(f);
    if (len_out != NULL) {
        *len_out = (size_t)n;
    }
    return buf;
}

/* 把非零的 int 全局变量写成 "index value" 行，便于和独立实现比对。 */
static int fvp_dump_globals(const char *path, const FvpVM *vm)
{
    FILE *f;
    int i;
    int written = 0;

    f = fopen(path, "wb");
    if (f == NULL) {
        return 0;
    }
    for (i = 0; i < FVP_GLOBAL_COUNT; ++i) {
        if (vm->globals[i].type != (uint8_t)FVP_INT) {
            continue;
        }
        if (vm->globals[i].i == 0) {
            continue;
        }
        fprintf(f, "%d %lld\n", i, (long long)vm->globals[i].i);
        written += 1;
    }
    fclose(f);
    return written + 1; /* >0 表示成功 */
}

/* 从 save.bin 灌全局变量；返回 1 成功。 */
static int fvp_seed_globals(const char *path, FvpVM *vm)
{
    unsigned char *buf;
    size_t len = 0;
    FvpSaveBin sb;
    FvpSaveGlobalsStats st;
    int written;

    buf = fvp_read_whole_file(path, &len);
    if (buf == NULL) {
        fprintf(stderr, "fvm: cannot read %s\n", path);
        return 0;
    }
    memset(&sb, 0, sizeof(sb));
    if (!fvp_save_bin_load(&sb, buf, len)) {
        fprintf(stderr, "fvm: %s is not a valid save.bin\n", path);
        free(buf);
        return 0;
    }
    written = fvp_save_globals_apply(&sb, vm->globals, FVP_GLOBAL_COUNT, &st);
    {
        int flag_bytes = fvp_save_flags_apply(&sb, vm->flags, FVP_FLAG_BYTES);

        printf("save.bin: %d records, %d int applied, %d unknown(type1), "
               "%d unset, %d flags bit(s) set\n",
               st.total, written, st.unknown, st.unset,
               fvp_flag_count(vm->flags));
        (void)flag_bytes;
    }
    fvp_save_bin_free(&sb);
    free(buf);
    return 1;
}

/* 从槽文件回灌**台词历史环**（槽尾第 20 段）：exe 读档路径 0x43F25A 起先读
 * count/pos 再 100 次调 0x437EE0。段偏移还没算出来，所以按结构判据定位
 * （src/savehist.h 顶部）。返回 1 成功。 */
static int fvp_seed_history(const char *path, FvpVM *vm, FvpEncoding enc)
{
    unsigned char *buf;
    size_t len = 0;
    FvpSaveSlot sd;
    FvpSaveHist sh;
    int ok = 0;

    buf = fvp_read_whole_file(path, &len);
    if (buf == NULL) {
        fprintf(stderr, "fvm: cannot read %s\n", path);
        return 0;
    }
    memset(&sd, 0, sizeof sd);
    fvp_savehist_init(&sh);
    if (!fvp_save_slot_load(&sd, buf, len, 0, 0)) {
        fprintf(stderr, "fvm: %s is not a valid save slot\n", path);
    } else if (!fvp_savehist_locate(&sh, sd.tail, sd.tail_len, sd.str[2],
                                    sd.len[2], enc)) {
        fprintf(stderr, "fvm: %s: no history ring found in the slot tail "
                "(or more than one candidate)\n", path);
    } else if (!fvp_savehist_apply(&sh, &vm->history)) {
        fprintf(stderr, "fvm: %s: history apply failed\n", path);
    } else {
        const FvpSaveHistRec *r = &sh.rec[sh.pos];

        printf("save slot: history ring at tail+%#x, count=%u pos=%u, "
               "newest=\"%s\"\n", (unsigned)sh.offset, sh.count, sh.pos,
               r->text1 != NULL ? r->text1
                                : (r->text0 != NULL ? r->text0 : ""));
        ok = 1;
    }
    fvp_savehist_free(&sh);
    fvp_save_slot_free(&sd);
    free(buf);
    return ok;
}

int main(int argc, char **argv)
{
    const char *script = NULL;
    const char *encname = "cp932";
    const char *text_out = NULL;
    const char *pctrace_path = NULL;
    const char *savebin_path = NULL;
    const char *save_history_path = NULL;
    long start_pc = -1;
    const char *dump_globals_path = NULL;
    uint64_t steps = 2000000u;
    uint64_t stall_after = 2000000u;
    int stall_exit = 0;
    int64_t click = 0;
    int64_t click_mask = 0x0B; /* 与 headless 默认一致；--click-mask 可改 */
    int64_t cx = 960;
    int64_t cy = 540;
    int quiet = 0;
    int timeline = 0;
    int bool_type = 0;
    int threads = 1; /* 1/0 = 开/关协程槽；-1 表示命令行显式 --no-threads */
    int crlf = 1;
    int i;
    FvpEncoding enc;
    FvpHcb hcb;
    FvpVM vm;
    char err[256];
    int stalled;

    for (i = 1; i < argc; ++i) {
        const char *a = argv[i];
        if (strcmp(a, "--help") == 0 || strcmp(a, "-h") == 0) {
            fvp_usage(stdout, argv[0]);
            return 0;
        } else if (strcmp(a, "--version") == 0) {
            printf("fvm (C) 0.1 -- FVP script VM prototype\n");
            return 0;
        } else if (strcmp(a, "--enc") == 0) {
            if (i + 1 >= argc) {
                fvp_usage(stderr, argv[0]);
                return 2;
            }
            encname = argv[++i];
        } else if (strcmp(a, "--steps") == 0) {
            if (i + 1 >= argc || !fvp_parse_u64(argv[i + 1], &steps)) {
                fprintf(stderr, "fvm: bad --steps value\n");
                return 2;
            }
            i += 1;
        } else if (strcmp(a, "--stall-after") == 0) {
            if (i + 1 >= argc || !fvp_parse_u64(argv[i + 1], &stall_after)) {
                fprintf(stderr, "fvm: bad --stall-after value\n");
                return 2;
            }
            i += 1;
        } else if (strcmp(a, "--stall-exit") == 0) {
            stall_exit = 1;
        } else if (strcmp(a, "--click") == 0) {
            if (i + 1 >= argc || !fvp_parse_i64(argv[i + 1], &click)) {
                fprintf(stderr, "fvm: bad --click value\n");
                return 2;
            }
            i += 1;
        } else if (strcmp(a, "--cursor") == 0) {
            const char *v;
            if (i + 1 >= argc) {
                fprintf(stderr, "fvm: missing --cursor value\n");
                return 2;
            }
            v = argv[++i];
            if (sscanf(v, "%lld,%lld", (long long *)&cx, (long long *)&cy) != 2) {
                fprintf(stderr, "fvm: bad --cursor value\n");
                return 2;
            }
        } else if (strcmp(a, "--click-mask") == 0) {
            if (i + 1 >= argc ||
                !fvp_parse_i64(argv[i + 1], &click_mask)) {
                fprintf(stderr, "fvm: bad --click-mask value\n");
                return 2;
            }
            ++i;
        } else if (strcmp(a, "--text-out") == 0) {
            if (i + 1 >= argc) {
                fprintf(stderr, "fvm: missing --text-out value\n");
                return 2;
            }
            text_out = argv[++i];
        } else if (strcmp(a, "--pctrace") == 0) {
            if (i + 1 >= argc) {
                fprintf(stderr, "fvm: missing --pctrace value\n");
                return 2;
            }
            pctrace_path = argv[++i];
        } else if (strcmp(a, "--timeline") == 0) {
            timeline = 1;
        } else if (strcmp(a, "--bool-type") == 0) {
            bool_type = 1;
        } else if (strcmp(a, "--threads") == 0) {
            threads = 1;
        } else if (strcmp(a, "--no-threads") == 0) {
            threads = -1;
        } else if (strcmp(a, "--unknown-all") == 0) {
            g_unknown_all = 1;
        } else if (strcmp(a, "--dump-textattr") == 0) {
            g_dump_textattr = 1;
        } else if (strcmp(a, "--dump-savedata") == 0) {
            g_dump_savedata = 1;
        } else if (strcmp(a, "--dump-history") == 0) {
            g_dump_history = 1;
        } else if (strcmp(a, "--pc") == 0) {
            if (i + 1 >= argc) {
                fprintf(stderr, "fvm: missing --pc value\n");
                return 2;
            }
            start_pc = strtol(argv[++i], NULL, 0);
        } else if (strcmp(a, "--save-history") == 0) {
            if (i + 1 >= argc) {
                fprintf(stderr, "fvm: missing --save-history value\n");
                return 2;
            }
            save_history_path = argv[++i];
        } else if (strcmp(a, "--savebin") == 0) {
            if (i + 1 >= argc) {
                fprintf(stderr, "fvm: missing --savebin value\n");
                return 2;
            }
            savebin_path = argv[++i];
        } else if (strcmp(a, "--dump-globals") == 0) {
            if (i + 1 >= argc) {
                fprintf(stderr, "fvm: missing --dump-globals value\n");
                return 2;
            }
            dump_globals_path = argv[++i];
        } else if (strcmp(a, "--quiet") == 0) {
            quiet = 1;
        } else if (strcmp(a, "--eol") == 0) {
            const char *v;
            if (i + 1 >= argc) {
                fprintf(stderr, "fvm: missing --eol value\n");
                return 2;
            }
            v = argv[++i];
            if (strcmp(v, "crlf") == 0) {
                crlf = 1;
            } else if (strcmp(v, "lf") == 0) {
                crlf = 0;
            } else {
                fprintf(stderr, "fvm: bad --eol value (crlf|lf)\n");
                return 2;
            }
        } else if (a[0] == '-' && a[1] != '\0') {
            fprintf(stderr, "fvm: unknown option %s\n", a);
            fvp_usage(stderr, argv[0]);
            return 2;
        } else if (script == NULL) {
            script = a;
        } else {
            fprintf(stderr, "fvm: unexpected extra argument %s\n", a);
            return 2;
        }
    }
    if (script == NULL) {
        fvp_usage(stderr, argv[0]);
        return 2;
    }
    if (!fvp_encoding_from_name(encname, &enc)) {
        fprintf(stderr, "fvm: unknown encoding %s (cp932|gbk)\n", encname);
        return 2;
    }
    if (click < 0) {
        click = 0;
    }

    if (!fvp_hcb_load(&hcb, script, enc, err, sizeof(err))) {
        fprintf(stderr, "fvm: %s\n", err);
        fvp_hcb_free(&hcb);
        return 3;
    }

    fvp_vm_init(&vm, &hcb, quiet);
    vm.timeline_on = timeline;
    vm.bool_type = bool_type;
    vm.threads_on = (threads < 0) ? 0 : threads;
    if (save_history_path != NULL &&
        !fvp_seed_history(save_history_path, &vm, enc)) {
        fprintf(stderr, "fvm: history restore from %s failed\n",
                save_history_path);
        fvp_vm_free(&vm);
        fvp_hcb_free(&hcb);
        return 2;
    }
    if (savebin_path != NULL && !fvp_seed_globals(savebin_path, &vm)) {
        fvp_vm_free(&vm);
        fvp_hcb_free(&hcb);
        return 5;
    }
    vm.click_period = click;
    vm.click_mask = (int)click_mask;
    vm.cx = cx;
    vm.cy = cy;
    if (start_pc >= 0) {
        fvp_vm_set_stall_limit(&vm, stall_after);
        fvp_vm_start_at(&vm, (unsigned int)start_pc);
        printf("start pc -> 0x%x (explicit, not main_start)\n",
               (unsigned)start_pc);
        (void)fvp_vm_run(&vm, steps);
        fvp_print_summary(&vm);
        fvp_vm_free(&vm);
        fvp_hcb_free(&hcb);
        return 0;
    }
    if (pctrace_path != NULL) {
        vm.pctrace = fopen(pctrace_path, "wb");
        if (vm.pctrace == NULL) {
            fprintf(stderr, "fvm: cannot write %s\n", pctrace_path);
            fvp_vm_free(&vm);
            fvp_hcb_free(&hcb);
            return 2;
        }
    }
    fvp_vm_set_stall_limit(&vm, stall_after);
    fvp_vm_start(&vm);
    (void)fvp_vm_run(&vm, steps);

    fvp_print_summary(&vm);
    if (dump_globals_path != NULL) {
        if (!fvp_dump_globals(dump_globals_path, &vm)) {
            fprintf(stderr, "fvm: cannot write %s\n", dump_globals_path);
            fvp_vm_free(&vm);
            fvp_hcb_free(&hcb);
            return 4;
        }
        printf("globals -> %s\n", dump_globals_path);
    }
    if (text_out != NULL) {
        if (!fvp_write_text_file(text_out, &vm, crlf)) {
            fvp_vm_free(&vm);
            fvp_hcb_free(&hcb);
            return 4;
        }
        printf("text -> %s\n", text_out);
    }

    stalled = fvp_vm_stalled(&vm);
    fvp_vm_free(&vm);
    fvp_hcb_free(&hcb);
    return (stalled && stall_exit) ? 6 : 0;
}