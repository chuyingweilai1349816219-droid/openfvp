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
#include "save_vm.h"
#include "vm.h"

static void fvp_usage(FILE *f, const char *prog)
{
    fprintf(f,
            "usage: %s <script.hcb> [options]\n"
            "  --enc NAME       text encoding: cp932 (default) or gbk\n"
            "  --steps N        instruction budget (default 2000000)\n"
            "  --click N        synthetic click every N steps (default 0)\n"
            "  --cursor X,Y     synthetic cursor position (default 960,540)\n"
            "  --text-out FILE  write \"[layer] text\" lines (CRLF) to FILE\n"
            "  --quiet          do not echo text / debmess to stdout\n"
            "  --eol crlf|lf    line endings for --text-out (default crlf)\n"
            "  --pctrace FILE   log executed pc values (debug aid)\n"
            "  --savebin FILE   seed globals from a save/save.bin\n"
            "  --timeline       use the virtual clock (ThreadSleep/ThreadWait wake by ms)\n"
            "  --threads        enable coroutine slots 0..31 (ThreadStart/ThreadExit)\n"
            "  --unknown-all    print every unknown command id (not just the top 15)\n"
            "  --dump-globals FILE  write \"index value\" lines for set globals\n",
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
    printf("save state slot=%d str=%d/%d/%d thumb=%dx%d\n", vm->save_slot,
           vm->save_str[0] != NULL, vm->save_str[1] != NULL,
           vm->save_str[2] != NULL, vm->save_thumb_w, vm->save_thumb_h);
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

int main(int argc, char **argv)
{
    const char *script = NULL;
    const char *encname = "cp932";
    const char *text_out = NULL;
    const char *pctrace_path = NULL;
    const char *savebin_path = NULL;
    const char *dump_globals_path = NULL;
    uint64_t steps = 2000000u;
    int64_t click = 0;
    int64_t cx = 960;
    int64_t cy = 540;
    int quiet = 0;
    int timeline = 0;
    int threads = 0;
    int crlf = 1;
    int i;
    FvpEncoding enc;
    FvpHcb hcb;
    FvpVM vm;
    char err[256];

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
        } else if (strcmp(a, "--threads") == 0) {
            threads = 1;
        } else if (strcmp(a, "--unknown-all") == 0) {
            g_unknown_all = 1;
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
    vm.threads_on = threads;
    if (savebin_path != NULL && !fvp_seed_globals(savebin_path, &vm)) {
        fvp_vm_free(&vm);
        fvp_hcb_free(&hcb);
        return 5;
    }
    vm.click_period = click;
    vm.cx = cx;
    vm.cy = cy;
    if (pctrace_path != NULL) {
        vm.pctrace = fopen(pctrace_path, "wb");
        if (vm.pctrace == NULL) {
            fprintf(stderr, "fvm: cannot write %s\n", pctrace_path);
            fvp_vm_free(&vm);
            fvp_hcb_free(&hcb);
            return 2;
        }
    }
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

    fvp_vm_free(&vm);
    fvp_hcb_free(&hcb);
    return 0;
}