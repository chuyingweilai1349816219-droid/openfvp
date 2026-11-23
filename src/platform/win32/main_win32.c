/* FVP Win32 desktop shell -- entry point and frame loop.
 *
 *   fvp.exe --game <游戏目录> [--script iroseka.hcb] [--enc cp932|gbk|auto]
 *           [--scale auto|1|2] [--filter bilinear|nearest] [--fullscreen]
 *           [--vm-steps N] [--text-pace auto|off|ms] [--run-seconds S]
 *           [--screenshot out.bmp] [--font FILE] [--no-feed-text]
 *           [--log FILE] [--console] [--verbose]
 *
 * The shell owns everything the engine core deliberately does not: the window,
 * the GDI presentation, the fixed-60-Hz frame loop, mouse/keyboard input,
 * waveOut audio output and the runtime glue (HCB load, encoding detection, VM
 * time slices, text pacing).
 *
 * The core files under fvp/src/ are NOT modified by the shell; this file only
 * uses their public interfaces (hcb.h / vm.h / snd.h) plus the renderer's
 * gfx_* surface (gfx.h).
 */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <mmsystem.h>
#include <shellapi.h>
#include <shlobj.h>

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

#include "encoding.h"
#include "hcb.h"
#include "save.h"
#include "thumb.h"
#include "save_vm.h"
#include "snd.h"
#include "vm.h"

#include "gfx_iface.h"
#include "win32_audio.h"
#include "win32_log.h"
#include "win32_window.h"

#define FVP_WIN32_VERSION "0.1"
#define FVP_CANVAS_W 1920
#define FVP_CANVAS_H 1080
#define FVP_TARGET_FPS 60.0
#define FVP_FRAME_DT (1.0 / FVP_TARGET_FPS)
#define PATH_CAP 1024
#define FONT_MAX 8

/* ------------------------------------------------------------------ */
/* Options                                                             */
/* ------------------------------------------------------------------ */

enum {
    ENC_AUTO = 0,
    ENC_CP932 = 1,
    ENC_GBK = 2
};

enum {
    TEXT_PACE_AUTO = 0,
    TEXT_PACE_OFF = 1,
    TEXT_PACE_FIXED = 2
};

typedef struct Options {
    wchar_t game_dir[PATH_CAP];
    int game_dir_set;
    wchar_t script[PATH_CAP];
    int script_set;

    int enc_mode;
    int scale_mode; /* 0 auto, 1, 2 */
    int filter_mode; /* 0 = nearest, 1 = bilinear */
    int fullscreen;

    int no_audio;
    int motion; /* --motion: 启用补间（默认关） */
    int timeline; /* --timeline: 虚拟时钟由帧间推进（默认关） */
    int tone_hz;
    int audio_buffers;
    int audio_frames;

    int burst_steps; /* VM steps per frame when not waiting for text/input */
    int text_pace_mode;
    int text_pace_ms;

    double run_seconds;
    wchar_t screenshot[PATH_CAP];
    wchar_t thumb_shot[PATH_CAP];
    wchar_t save_template[PATH_CAP];
    wchar_t save_preview[PATH_CAP];
    double save_preview_every; /* >0：运行中每隔这么多秒写一次预览 */
    wchar_t save_encoding[16]; /* "" = 跟随脚本；否则 cp932 / gbk */
    wchar_t log_path[PATH_CAP];
    wchar_t fonts[FONT_MAX][PATH_CAP];
    int nfonts;
    int feed_text; /* temporary shell-side TextPrint -> gfx bridge */
    int console;
    int verbose;
} Options;

/* ------------------------------------------------------------------ */
/* Globals / small helpers                                             */
/* ------------------------------------------------------------------ */

static LARGE_INTEGER g_qpc_freq;
static double g_start_time;

static double now_sec(void)
{
    LARGE_INTEGER c;
    QueryPerformanceCounter(&c);
    return (double)c.QuadPart / (double)g_qpc_freq.QuadPart;
}

static int file_exists_w(const wchar_t *path)
{
    DWORD attr = GetFileAttributesW(path);
    return attr != INVALID_FILE_ATTRIBUTES &&
           (attr & FILE_ATTRIBUTE_DIRECTORY) == 0;
}

static int dir_exists_w(const wchar_t *path)
{
    DWORD attr = GetFileAttributesW(path);
    return attr != INVALID_FILE_ATTRIBUTES &&
           (attr & FILE_ATTRIBUTE_DIRECTORY) != 0;
}

static int is_absolute_w(const wchar_t *path)
{
    if (path == NULL || path[0] == L'\0') {
        return 0;
    }
    if (path[0] == L'\\' || path[0] == L'/') {
        return 1;
    }
    if (path[1] == L':' && (path[2] == L'\\' || path[2] == L'/')) {
        return 1;
    }
    return 0;
}

static int path_join_w(wchar_t *out, size_t cap, const wchar_t *dir,
                       const wchar_t *name)
{
    size_t d;
    size_t n;

    if (out == NULL || cap == 0 || dir == NULL || name == NULL) {
        return 0;
    }
    if (is_absolute_w(name)) {
        n = wcslen(name);
        if (n + 1 > cap) {
            return 0;
        }
        wmemcpy(out, name, n + 1);
        return 1;
    }
    d = wcslen(dir);
    n = wcslen(name);
    if (d + n + 2 > cap) {
        return 0;
    }
    wmemcpy(out, dir, d);
    out[d] = L'\\';
    wmemcpy(out + d + 1, name, n + 1);
    return 1;
}

static int path_dirname_w(const wchar_t *path, wchar_t *out, size_t cap)
{
    size_t n;
    size_t i;

    n = wcslen(path);
    i = n;
    while (i > 0 && path[i - 1] != L'\\' && path[i - 1] != L'/') {
        i -= 1;
    }
    if (i == 0) {
        if (cap < 2) {
            return 0;
        }
        out[0] = L'.';
        out[1] = L'\0';
        return 1;
    }
    while (i > 1 && (path[i - 1] == L'\\' || path[i - 1] == L'/')) {
        i -= 1;
    }
    if (i == 0) {
        i = 1;
    }
    if (i >= cap) {
        return 0;
    }
    wmemcpy(out, path, i);
    out[i] = L'\0';
    return 1;
}

/* Last path component (directory or file name). */
static int path_basename_w(const wchar_t *path, wchar_t *out, size_t cap)
{
    size_t n;
    size_t start;
    size_t len;

    if (path == NULL || out == NULL || cap == 0) {
        return 0;
    }
    n = wcslen(path);
    while (n > 0 && (path[n - 1] == L'\\' || path[n - 1] == L'/')) {
        n -= 1;
    }
    start = n;
    while (start > 0 && path[start - 1] != L'\\' && path[start - 1] != L'/') {
        start -= 1;
    }
    len = n - start;
    if (len == 0 || len >= cap) {
        return 0;
    }
    wmemcpy(out, path + start, len);
    out[len] = L'\0';
    return 1;
}

static int wide_to_utf8(const wchar_t *src, char *dst, size_t cap)
{
    int n;
    if (src == NULL || dst == NULL || cap == 0) {
        return 0;
    }
    n = WideCharToMultiByte(CP_UTF8, 0, src, -1, dst, (int)cap, NULL, NULL);
    return n > 0;
}

static int utf8_to_wide(const char *src, wchar_t *dst, size_t cap)
{
    int n;
    if (src == NULL || dst == NULL || cap == 0) {
        return 0;
    }
    n = MultiByteToWideChar(CP_UTF8, 0, src, -1, dst, (int)cap);
    return n > 0;
}

static int wide_to_ansi(const wchar_t *src, char *dst, size_t cap)
{
    int n;
    wchar_t shortp[PATH_CAP];
    DWORD sn;

    if (src == NULL || dst == NULL || cap == 0) {
        return 0;
    }
    n = WideCharToMultiByte(CP_ACP, 0, src, -1, dst, (int)cap, NULL, NULL);
    if (n > 0) {
        return 1;
    }
    sn = GetShortPathNameW(src, shortp, (DWORD)PATH_CAP);
    if (sn > 0 && sn < (DWORD)PATH_CAP) {
        n = WideCharToMultiByte(CP_ACP, 0, shortp, -1, dst, (int)cap, NULL,
                                NULL);
        return n > 0;
    }
    return 0;
}

/* Wide string -> UTF-8 for the log, safe for NULL. */
static const char *w2u(const wchar_t *src, char *buf, size_t cap)
{
    if (!wide_to_utf8(src, buf, cap)) {
        buf[0] = '\0';
    }
    return buf;
}

static int exe_dir_w(wchar_t *out, size_t cap)
{
    wchar_t path[PATH_CAP];
    DWORD n = GetModuleFileNameW(NULL, path, (DWORD)PATH_CAP);
    if (n == 0 || n >= (DWORD)PATH_CAP) {
        return 0;
    }
    return path_dirname_w(path, out, cap);
}

static void set_dpi_awareness(void)
{
    typedef BOOL(WINAPI * SetDpiCtxFn)(HANDLE);
    HMODULE user32 = GetModuleHandleW(L"user32.dll");
    FARPROC proc = NULL;
    SetDpiCtxFn fn = NULL;

    if (user32 != NULL) {
        proc = GetProcAddress(user32, "SetProcessDpiAwarenessContext");
    }
    if (proc != NULL) {
        memcpy(&fn, &proc, sizeof(fn));
        if (fn != NULL) {
            /* DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2 == (HANDLE)-4 */
            fn((HANDLE)(INT_PTR)-4);
            return;
        }
    }
    SetProcessDPIAware();
}

/* ------------------------------------------------------------------ */
/* Usage / errors                                                      */
/* ------------------------------------------------------------------ */

static const wchar_t *usage_text(void)
{
    return L"fvp.exe " L"" FVP_WIN32_VERSION
           L" -- FVP 引擎桌面运行器\n"
           L"\n"
           L"用法:\n"
           L"  fvp.exe --game <游戏目录> [选项]\n"
           L"  把 fvp.exe 放进游戏目录后直接双击也可以。\n"
           L"\n"
           L"选项:\n"
           L"  --game DIR          游戏数据目录（找 iroseka.hcb 或目录内第一个 .hcb）\n"
           L"  --script NAME       脚本文件名（默认 iroseka.hcb，相对游戏目录）\n"
           L"  --enc MODE          文本编码 cp932 | gbk | auto（默认 auto）\n"
           L"  --scale auto|1|2    窗口 auto（1280x720，超出桌面时缩小）| 1 | 2\n"
           L"  --filter MODE       缩放过滤 nearest（默认，60fps）| bilinear（更平滑更慢）\n"
           L"  --fullscreen        启动即全屏（游戏中 F11 切换）\n"
           L"  --vm-steps N        每帧 VM 指令预算（默认 40000）\n"
           L"  --text-pace MODE    文本停留 auto（默认）| off | 毫秒数\n"
           L"  --no-audio          不打开音频设备\n"
           L"  --audio-tone HZ     自检音（例如 440），用于验证 waveOut 通路\n"
           L"  --audio-buffers N   waveOut 缓冲区数（默认 3）\n"
           L"  --run-seconds S     运行 S 秒后自动退出（回归测试用）\n"
           L"  --screenshot FILE   退出时把最后一帧写成 32 位 BMP\n"
            L"  --thumb-shot FILE   退出时写 200x113 的存档缩略图 BMP\n"
            L"  --save-template FILE  用这个真实存档槽当模板（配 --save-preview）\n"
            L"  --save-preview FILE   退出时写出「模板 + 当前时间 + 当前画面缩略图」的槽\n"
            L"  --save-preview-every N  运行中每 N 秒写一次预览（文件名加 .1/.2 序号）\n"
            L"  --save-encoding ENC  存档字符串编码 cp932|gbk（默认跟随脚本编码）\n"
            L"  --motion             启用动画补间（默认关闭：保持像素基准）\n"
            L"  --timeline           虚拟时钟随帧推进（ThreadSleep/Wait 按毫秒唤醒）\n"
           L"  --font FILE         加载 GFXF 字体图集（可重复；默认自动找 exe 目录 gfx-font-*.bin）\n"
           L"  --no-feed-text      关闭壳的临时文本桥（等 syscall 层接好 gfx 后用）\n"
           L"  --log FILE          日志文件（默认 <exe目录>\\fvp-win32.log）\n"
           L"  --console           同时输出到启动它的控制台窗口\n"
           L"  --verbose           更详细的日志\n"
           L"  --help / --version\n"
           L"\n"
           L"按键: 鼠标左键/Enter/空格 = 确定，F11 = 全屏，ESC = 退出。\n"
           L"滚轮事件会记进日志（VM 的 InputGetWheel syscall 尚未实现）。\n";
}

static void show_error(const wchar_t *title, const wchar_t *text)
{
    char u8[2048];
    w2u(text, u8, sizeof(u8));
    fvp_logf("ERROR: %s", u8);
    MessageBoxW(NULL, text, title, MB_OK | MB_ICONERROR);
}

static void show_info(const wchar_t *title, const wchar_t *text)
{
    MessageBoxW(NULL, text, title, MB_OK | MB_ICONINFORMATION);
}

/* ------------------------------------------------------------------ */
/* Command line                                                        */
/* ------------------------------------------------------------------ */

static int parse_int_w(const wchar_t *s, int *out)
{
    wchar_t *end = NULL;
    long v;
    if (s == NULL || *s == L'\0') {
        return 0;
    }
    v = wcstol(s, &end, 10);
    if (end == NULL || *end != L'\0') {
        return 0;
    }
    *out = (int)v;
    return 1;
}

static int parse_double_w(const wchar_t *s, double *out)
{
    wchar_t *end = NULL;
    double v;
    if (s == NULL || *s == L'\0') {
        return 0;
    }
    v = wcstod(s, &end);
    if (end == NULL || *end != L'\0') {
        return 0;
    }
    *out = v;
    return 1;
}

static void defaults(Options *o)
{
    memset(o, 0, sizeof(*o));
    o->enc_mode = ENC_AUTO;
    o->scale_mode = 0;
    o->filter_mode = 0;
    o->audio_buffers = 3;
    o->audio_frames = 480; /* 10 ms at 48 kHz */
    o->burst_steps = 40000;
    o->text_pace_mode = TEXT_PACE_AUTO;
    o->run_seconds = 0.0;
    o->save_preview_every = 0.0;
    o->motion = 0;
    o->timeline = 0;
    o->feed_text = 1;
}

static void bad_option(const char *what, const wchar_t *value)
{
    char u8[512];
    fvp_logf("args: %s %s", what, w2u(value, u8, sizeof(u8)));
}

/* Returns 0 on success, 1 when help/version was shown, 2 on a bad option. */
static int parse_args(int argc, wchar_t **argv, Options *o)
{
    int i;

    for (i = 1; i < argc; ++i) {
        const wchar_t *a = argv[i];
        const wchar_t *v = NULL;

        if (wcscmp(a, L"--help") == 0 || wcscmp(a, L"-h") == 0) {
            wchar_t title[64];
            wcscpy(title, L"fvp.exe ");
            wcscat(title, L"" FVP_WIN32_VERSION);
            show_info(title, usage_text());
            return 1;
        }
        if (wcscmp(a, L"--version") == 0) {
            show_info(L"fvp.exe",
                      L"fvp win32 shell " L"" FVP_WIN32_VERSION);
            return 1;
        }

#define NEED_VALUE()                                                         \
    do {                                                                     \
        if (i + 1 >= argc) {                                                 \
            fvp_logf("args: option needs a value");                          \
            return 2;                                                        \
        }                                                                    \
        v = argv[++i];                                                       \
    } while (0)

        if (wcscmp(a, L"--game") == 0) {
            NEED_VALUE();
            wcsncpy(o->game_dir, v, PATH_CAP - 1);
            o->game_dir_set = 1;
        } else if (wcscmp(a, L"--script") == 0) {
            NEED_VALUE();
            wcsncpy(o->script, v, PATH_CAP - 1);
            o->script_set = 1;
        } else if (wcscmp(a, L"--enc") == 0) {
            NEED_VALUE();
            if (wcscmp(v, L"auto") == 0) {
                o->enc_mode = ENC_AUTO;
            } else if (wcscmp(v, L"cp932") == 0) {
                o->enc_mode = ENC_CP932;
            } else if (wcscmp(v, L"gbk") == 0) {
                o->enc_mode = ENC_GBK;
            } else {
                bad_option("--enc", v);
                return 2;
            }
        } else if (wcscmp(a, L"--scale") == 0) {
            NEED_VALUE();
            if (wcscmp(v, L"auto") == 0) {
                o->scale_mode = 0;
            } else if (wcscmp(v, L"1") == 0) {
                o->scale_mode = 1;
            } else if (wcscmp(v, L"2") == 0) {
                o->scale_mode = 2;
            } else {
                bad_option("--scale", v);
                return 2;
            }
        } else if (wcscmp(a, L"--filter") == 0) {
            NEED_VALUE();
            if (wcscmp(v, L"bilinear") == 0 || wcscmp(v, L"halftone") == 0) {
                o->filter_mode = 1;
            } else if (wcscmp(v, L"nearest") == 0 ||
                       wcscmp(v, L"auto") == 0) {
                o->filter_mode = 0;
            } else {
                bad_option("--filter", v);
                return 2;
            }
        } else if (wcscmp(a, L"--fullscreen") == 0) {
            o->fullscreen = 1;
        } else if (wcscmp(a, L"--no-audio") == 0) {
            o->no_audio = 1;
        } else if (wcscmp(a, L"--audio-tone") == 0) {
            NEED_VALUE();
            if (!parse_int_w(v, &o->tone_hz) || o->tone_hz < 0 ||
                o->tone_hz > 20000) {
                bad_option("--audio-tone", v);
                return 2;
            }
        } else if (wcscmp(a, L"--audio-buffers") == 0) {
            NEED_VALUE();
            if (!parse_int_w(v, &o->audio_buffers) || o->audio_buffers < 2 ||
                o->audio_buffers > 16) {
                bad_option("--audio-buffers", v);
                return 2;
            }
        } else if (wcscmp(a, L"--vm-steps") == 0) {
            NEED_VALUE();
            if (!parse_int_w(v, &o->burst_steps) || o->burst_steps < 1 ||
                o->burst_steps > 5000000) {
                bad_option("--vm-steps", v);
                return 2;
            }
        } else if (wcscmp(a, L"--text-pace") == 0) {
            NEED_VALUE();
            if (wcscmp(v, L"auto") == 0) {
                o->text_pace_mode = TEXT_PACE_AUTO;
            } else if (wcscmp(v, L"off") == 0) {
                o->text_pace_mode = TEXT_PACE_OFF;
            } else if (parse_int_w(v, &o->text_pace_ms) &&
                       o->text_pace_ms >= 0) {
                o->text_pace_mode = TEXT_PACE_FIXED;
            } else {
                bad_option("--text-pace", v);
                return 2;
            }
        } else if (wcscmp(a, L"--run-seconds") == 0) {
            NEED_VALUE();
            if (!parse_double_w(v, &o->run_seconds) || o->run_seconds < 0.0) {
                bad_option("--run-seconds", v);
                return 2;
            }
        } else if (wcscmp(a, L"--screenshot") == 0) {
            NEED_VALUE();
            wcsncpy(o->screenshot, v, PATH_CAP - 1);
        } else if (wcscmp(a, L"--thumb-shot") == 0) {
            NEED_VALUE();
            wcsncpy(o->thumb_shot, v, PATH_CAP - 1);
        } else if (wcscmp(a, L"--save-template") == 0) {
            NEED_VALUE();
            wcsncpy(o->save_template, v, PATH_CAP - 1);
        } else if (wcscmp(a, L"--save-preview") == 0) {
            NEED_VALUE();
            wcsncpy(o->save_preview, v, PATH_CAP - 1);
        } else if (wcscmp(a, L"--save-preview-every") == 0) {
            NEED_VALUE();
            o->save_preview_every = wcstod(v, NULL);
            if (o->save_preview_every < 0.0) {
                o->save_preview_every = 0.0;
            }
        } else if (wcscmp(a, L"--motion") == 0) {
            o->motion = 1;
        } else if (wcscmp(a, L"--timeline") == 0) {
            o->timeline = 1;
        } else if (wcscmp(a, L"--save-encoding") == 0) {
            NEED_VALUE();
            wcsncpy(o->save_encoding, v, 15);
        } else if (wcscmp(a, L"--font") == 0) {
            NEED_VALUE();
            if (o->nfonts < FONT_MAX) {
                wcsncpy(o->fonts[o->nfonts], v, PATH_CAP - 1);
                o->nfonts += 1;
            } else {
                fvp_logf("args: more than %d --font options ignored", FONT_MAX);
            }
        } else if (wcscmp(a, L"--no-feed-text") == 0) {
            o->feed_text = 0;
        } else if (wcscmp(a, L"--log") == 0) {
            NEED_VALUE();
            wcsncpy(o->log_path, v, PATH_CAP - 1);
        } else if (wcscmp(a, L"--console") == 0) {
            o->console = 1;
        } else if (wcscmp(a, L"--verbose") == 0) {
            o->verbose = 1;
        } else if (a[0] == L'-' && a[1] != L'\0') {
            bad_option("unknown option", a);
            return 2;
        } else if (!o->game_dir_set) {
            wcsncpy(o->game_dir, a, PATH_CAP - 1);
            o->game_dir_set = 1;
        } else {
            bad_option("unexpected extra argument", a);
            return 2;
        }
#undef NEED_VALUE
    }
    return 0;
}

/* ------------------------------------------------------------------ */
/* Script discovery                                                    */
/* ------------------------------------------------------------------ */

static int find_first_hcb(const wchar_t *dir, wchar_t *out, size_t cap)
{
    wchar_t pattern[PATH_CAP];
    WIN32_FIND_DATAW fd;
    HANDLE h;

    if (!path_join_w(pattern, PATH_CAP, dir, L"*.hcb")) {
        return 0;
    }
    h = FindFirstFileW(pattern, &fd);
    if (h == INVALID_HANDLE_VALUE) {
        return 0;
    }
    do {
        if ((fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) == 0) {
            if (path_join_w(out, cap, dir, fd.cFileName)) {
                FindClose(h);
                return 1;
            }
        }
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    return 0;
}

/* Look for a game directory next to the exe, in <exe>\game, ..\game or the
 * current directory (double-click friendly). */
static int find_game_dir_default(wchar_t *out, size_t cap)
{
    wchar_t exe_dir[PATH_CAP];
    const wchar_t *suffixes[4] = {L"", L"game", L"..\\game", L""};
    int i;

    if (!exe_dir_w(exe_dir, PATH_CAP)) {
        return 0;
    }
    for (i = 0; i < 4; ++i) {
        wchar_t cand[PATH_CAP];
        wchar_t probe[PATH_CAP];
        if (i == 3) {
            wcscpy(cand, L".");
        } else if (suffixes[i][0] == L'\0') {
            wcsncpy(cand, exe_dir, PATH_CAP - 1);
            cand[PATH_CAP - 1] = L'\0';
        } else if (!path_join_w(cand, PATH_CAP, exe_dir, suffixes[i])) {
            continue;
        }
        if (!dir_exists_w(cand)) {
            continue;
        }
        if (path_join_w(probe, PATH_CAP, cand, L"iroseka.hcb") &&
            file_exists_w(probe)) {
            wcsncpy(out, cand, cap - 1);
            out[cap - 1] = L'\0';
            return 1;
        }
    }
    return 0;
}

/* Folder picker fallback: double-clicking fvp.exe without --game asks the
 * user for the game directory instead of failing. */
static int pick_game_dir(wchar_t *out, size_t cap)
{
    BROWSEINFOW bi;
    LPITEMIDLIST pidl;
    int ok = 0;

    memset(&bi, 0, sizeof(bi));
    bi.hwndOwner = NULL;
    bi.lpszTitle = L"请选择游戏目录（包含 iroseka.hcb 的目录）";
    bi.ulFlags = BIF_RETURNONLYFSDIRS | BIF_USENEWUI;
    pidl = SHBrowseForFolderW(&bi);
    if (pidl != NULL) {
        if (SHGetPathFromIDListW(pidl, out)) {
            out[cap - 1] = L'\0';
            ok = 1;
        }
        CoTaskMemFree(pidl);
    }
    return ok;
}

static int resolve_paths(Options *o, wchar_t *script_path, wchar_t *game_dir)
{
    wchar_t exe_dir[PATH_CAP];
    char u8[PATH_CAP * 3];

    if (!exe_dir_w(exe_dir, PATH_CAP)) {
        wcscpy(exe_dir, L".");
    }

    if (!o->game_dir_set) {
        wchar_t cand[PATH_CAP];
        const wchar_t *suffixes[4] = {L"", L"game", L"..\\game", L""};
        int i;
        for (i = 0; i < 4; ++i) {
            if (i == 3) {
                wcscpy(cand, L".");
            } else if (suffixes[i][0] == L'\0') {
                wcsncpy(cand, exe_dir, PATH_CAP - 1);
                cand[PATH_CAP - 1] = L'\0';
            } else {
                if (!path_join_w(cand, PATH_CAP, exe_dir, suffixes[i])) {
                    continue;
                }
            }
            if (dir_exists_w(cand)) {
                wchar_t probe[PATH_CAP];
                if (path_join_w(probe, PATH_CAP, cand, L"iroseka.hcb") &&
                    file_exists_w(probe)) {
                    wcsncpy(o->game_dir, cand, PATH_CAP - 1);
                    o->game_dir_set = 1;
                    break;
                }
            }
        }
        if (!o->game_dir_set) {
            wcsncpy(o->game_dir, exe_dir, PATH_CAP - 1);
            o->game_dir_set = 1;
        }
    }
    if (!dir_exists_w(o->game_dir)) {
        fvp_logf("script: game directory does not exist: %s",
                 w2u(o->game_dir, u8, sizeof(u8)));
        return 0;
    }
    wcsncpy(game_dir, o->game_dir, PATH_CAP - 1);
    game_dir[PATH_CAP - 1] = L'\0';

    if (o->script_set) {
        if (!path_join_w(script_path, PATH_CAP, game_dir, o->script)) {
            return 0;
        }
    } else if (path_join_w(script_path, PATH_CAP, game_dir, L"iroseka.hcb") &&
               file_exists_w(script_path)) {
        /* default name found */
    } else if (find_first_hcb(game_dir, script_path, PATH_CAP)) {
        fvp_logf("script: iroseka.hcb not found, using %s",
                 w2u(script_path, u8, sizeof(u8)));
    } else {
        fvp_logf("script: no .hcb script in %s",
                 w2u(game_dir, u8, sizeof(u8)));
        return 0;
    }

    if (!file_exists_w(script_path)) {
        fvp_logf("script: file does not exist: %s",
                 w2u(script_path, u8, sizeof(u8)));
        return 0;
    }
    return 1;
}

/* ------------------------------------------------------------------ */
/* Encoding detection                                                  */
/* ------------------------------------------------------------------ */

static void count_script_chars(const FvpHcb *hcb, long *kana, long *han,
                               long *fffd)
{
    uint32_t i;
    *kana = 0;
    *han = 0;
    *fffd = 0;
    for (i = 0; i < hcb->ninstr; ++i) {
        const FvpStr *s = hcb->instrs[i].str;
        uint32_t p;
        if (s == NULL) {
            continue;
        }
        for (p = 0; p + 2u < s->len;) {
            const unsigned char b0 = (unsigned char)s->data[p];
            const unsigned char b1 = (unsigned char)s->data[p + 1u];
            const unsigned char b2 = (unsigned char)s->data[p + 2u];
            if (b0 == 0xEFu && b1 == 0xBFu && b2 == 0xBDu) {
                *fffd += 1;
                p += 3u;
            } else if (b0 == 0xE3u && (b1 == 0x81u || b1 == 0x82u) &&
                       b2 >= 0x80u && b2 <= 0xBFu) {
                *kana += 1;
                p += 3u;
            } else if (b0 >= 0xE3u && b0 <= 0xE9u) {
                *han += 1;
                p += 3u;
            } else {
                p += 1u;
            }
        }
    }
}

static int load_script(FvpHcb *hcb, const char *path_a, int enc_mode,
                       char *err, size_t errlen, int verbose)
{
    FvpEncoding use = FVP_ENC_CP932;
    long kana = 0;
    long han = 0;
    long fffd = 0;

    if (enc_mode == ENC_GBK) {
        use = FVP_ENC_GBK;
    }

    if (!fvp_hcb_load(hcb, path_a, use, err, errlen)) {
        return 0;
    }
    if (enc_mode == ENC_CP932) {
        fvp_logf("script: encoding cp932 (forced)");
        return 1;
    }
    if (enc_mode == ENC_GBK) {
        fvp_logf("script: encoding gbk (forced)");
        return 1;
    }

    count_script_chars(hcb, &kana, &han, &fffd);
    if (kana >= 8) {
        fvp_logf("script: encoding cp932 (auto: kana=%ld han=%ld fffd=%ld)",
                 kana, han, fffd);
        return 1;
    }
    if (fffd == 0 && kana == 0) {
        /* No kana and no replacement characters: ASCII or kanji-only text
         * decodes identically under both tables, keep cp932. */
        fvp_logf("script: encoding cp932 (auto: kana=0 han=%ld fffd=0)", han);
        return 1;
    }
    if (verbose) {
        fvp_logf("script: cp932 probe kana=%ld han=%ld fffd=%ld -> trying gbk",
                 kana, han, fffd);
    }
    fvp_hcb_free(hcb);
    if (!fvp_hcb_load(hcb, path_a, FVP_ENC_GBK, err, errlen)) {
        return 0;
    }
    count_script_chars(hcb, &kana, &han, &fffd);
    fvp_logf("script: encoding gbk (auto: gbk probe kana=%ld han=%ld "
             "fffd=%ld)",
             kana, han, fffd);
    return 1;
}

/* ------------------------------------------------------------------ */
/* Text helpers                                                        */
/* ------------------------------------------------------------------ */

static void layer_string(char *buf, size_t cap, const FvpValue *v)
{
    if (cap == 0) {
        return;
    }
    if (v->type == (uint8_t)FVP_INT) {
        snprintf(buf, cap, "%lld", (long long)v->i);
        buf[cap - 1] = '\0';
        return;
    }
    if (v->type == (uint8_t)FVP_FLOAT) {
        snprintf(buf, cap, "%.3f", v->f);
        buf[cap - 1] = '\0';
        return;
    }
    if (v->str != NULL) {
        size_t n = v->str->len;
        if (n >= cap) {
            n = cap - 1;
        }
        memcpy(buf, v->str->data, n);
        buf[n] = '\0';
        return;
    }
    buf[0] = '\0';
}

static int utf8_chars(const FvpStr *s)
{
    uint32_t i;
    int n = 0;
    if (s == NULL) {
        return 0;
    }
    for (i = 0; i < s->len; ++i) {
        unsigned char c = (unsigned char)s->data[i];
        if ((c & 0xC0u) != 0x80u) {
            n += 1;
        }
    }
    return n;
}

static double text_hold_seconds(const Options *o, const FvpTextLine *line)
{
    int chars;
    long layer = -1;
    double ms;

    if (o->text_pace_mode == TEXT_PACE_OFF) {
        return 0.0;
    }
    if (o->text_pace_mode == TEXT_PACE_FIXED) {
        return (double)o->text_pace_ms / 1000.0;
    }
    if (line->layer.type == (uint8_t)FVP_INT) {
        layer = (long)line->layer.i;
    }
    if (layer == 8) {
        return 0.35; /* speaker name: the dialogue follows immediately */
    }
    if (layer == 9) {
        return 1.8; /* chapter / location title */
    }
    chars = utf8_chars(line->text);
    ms = 700.0 + 26.0 * (double)chars;
    if (ms < 900.0) {
        ms = 900.0;
    }
    if (ms > 4500.0) {
        ms = 4500.0;
    }
    return ms / 1000.0;
}

static void log_text_line(const FvpTextLine *line, uint64_t steps)
{
    char layer[32];
    char *printable;
    size_t n;
    size_t i;
    size_t out;

    layer_string(layer, sizeof(layer), &line->layer);
    n = line->text != NULL ? line->text->len : 0u;
    printable = (char *)malloc(n + 1u);
    if (printable == NULL) {
        return;
    }
    out = 0;
    for (i = 0; i < n && out < 400u; ++i) {
        unsigned char c = (unsigned char)line->text->data[i];
        if (c == '\n') {
            printable[out++] = '\\';
            printable[out++] = 'n';
        } else if (c == '\r') {
            printable[out++] = '\\';
            printable[out++] = 'r';
        } else if (c < 0x20u) {
            printable[out++] = ' ';
        } else {
            printable[out++] = (char)c;
        }
    }
    printable[out] = '\0';
    fvp_logf("text: [%s] %s (steps=%llu)", layer, printable,
             (unsigned long long)steps);
    free(printable);
}

/* ------------------------------------------------------------------ */
/* BMP screenshot                                                      */
/* ------------------------------------------------------------------ */

static int write_bmp32(const wchar_t *path, const uint32_t *rgba, int w,
                       int h)
{
    BITMAPFILEHEADER fh;
    BITMAPINFOHEADER ih;
    FILE *f;
    unsigned char *row;
    DWORD row_bytes;
    DWORD image_bytes;
    int y;
    int x;
    int ok = 1;
    char u8[PATH_CAP * 3];

    if (path == NULL || rgba == NULL || w <= 0 || h <= 0) {
        return 0;
    }
    f = _wfopen(path, L"wb");
    if (f == NULL) {
        fvp_logf("screenshot: cannot open %s",
                 w2u(path, u8, sizeof(u8)));
        return 0;
    }
    row_bytes = (DWORD)w * 4u;
    image_bytes = row_bytes * (DWORD)h;

    memset(&fh, 0, sizeof(fh));
    fh.bfType = 0x4D42; /* "BM" */
    fh.bfOffBits = (DWORD)(sizeof(fh) + sizeof(ih));
    fh.bfSize = fh.bfOffBits + image_bytes;

    memset(&ih, 0, sizeof(ih));
    ih.biSize = sizeof(ih);
    ih.biWidth = w;
    ih.biHeight = h; /* bottom-up */
    ih.biPlanes = 1;
    ih.biBitCount = 32;
    ih.biCompression = BI_RGB;
    ih.biSizeImage = image_bytes;

    if (fwrite(&fh, sizeof(fh), 1, f) != 1 ||
        fwrite(&ih, sizeof(ih), 1, f) != 1) {
        fclose(f);
        return 0;
    }
    row = (unsigned char *)malloc(row_bytes);
    if (row == NULL) {
        fclose(f);
        return 0;
    }
    for (y = h - 1; y >= 0; --y) {
        const uint32_t *src = rgba + (size_t)y * (size_t)w;
        for (x = 0; x < w; ++x) {
            uint32_t d = src[x];
            row[x * 4 + 0] = (unsigned char)((d >> 16) & 0xFFu); /* B */
            row[x * 4 + 1] = (unsigned char)((d >> 8) & 0xFFu);  /* G */
            row[x * 4 + 2] = (unsigned char)(d & 0xFFu);         /* R */
            row[x * 4 + 3] = 0xFFu;                              /* X */
        }
        if (fwrite(row, row_bytes, 1, f) != 1) {
            ok = 0;
            break;
        }
    }
    free(row);
    if (fclose(f) != 0) {
        ok = 0;
    }
    fvp_logf("screenshot: %s %dx%d (%lu bytes) %s",
             w2u(path, u8, sizeof(u8)), w, h, (unsigned long)image_bytes,
             ok ? "ok" : "FAILED");
    return ok;
}

/* ------------------------------------------------------------------ */
/* Window sizing / title                                               */
/* ------------------------------------------------------------------ */

/* ------------------------------------------------------------------ */
/* 存档预览：用真实槽当模板，只换「时间戳 + 缩略图」写出一份新槽        */
/* ------------------------------------------------------------------ */

static unsigned char *read_file_w(const wchar_t *path, size_t *len_out)
{
    FILE *f;
    long n;
    unsigned char *buf;

    if (len_out != NULL) {
        *len_out = 0;
    }
    f = _wfopen(path, L"rb");
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

/* 返回 1 表示写出成功。字符串段：脚本通过 SaveCreate 准备的那几条（VM 里是 UTF-8）
 * 会按脚本编码（CP932 / GBK）编码后写进槽里；脚本没准备就沿用模板的字符串。 */
static int write_save_preview(const wchar_t *tmpl, const wchar_t *out_path,
                              const unsigned char *canvas, int cw, int ch,
                              FvpVM *vm, FvpEncoding enc)
{
    unsigned char *buf;
    size_t len = 0;
    FvpSaveSlot slot;
    SYSTEMTIME st;
    char u8[PATH_CAP * 3];
    int i;
    int nstr = 0;

    buf = read_file_w(tmpl, &len);
    if (buf == NULL) {
        fvp_logf("save-preview: cannot read template %s",
                 w2u(tmpl, u8, sizeof(u8)));
        return 0;
    }
    memset(&slot, 0, sizeof(slot));
    if (!fvp_save_slot_load(&slot, buf, len, FVP_SAVE_THUMB_W_DEFAULT,
                            FVP_SAVE_THUMB_H_DEFAULT)) {
        fvp_logf("save-preview: %s is not a valid save slot",
                 w2u(tmpl, u8, sizeof(u8)));
        free(buf);
        return 0;
    }
    free(buf);

    GetLocalTime(&st);
    slot.year = st.wYear;
    slot.month = (unsigned char)st.wMonth;
    slot.day = (unsigned char)st.wDay;
    slot.wday = (unsigned char)st.wDayOfWeek;
    slot.hour = (unsigned char)st.wHour;
    slot.minute = (unsigned char)st.wMinute;

    if (canvas != NULL && slot.thumb != NULL && slot.thumb_w > 0 &&
        slot.thumb_h > 0) {
        if (fvp_thumb_scale_bgra(canvas, cw, ch, slot.thumb, slot.thumb_w,
                                 slot.thumb_h)) {
            fvp_logf("save-preview: thumbnail replaced (%dx%d)",
                     slot.thumb_w, slot.thumb_h);
        }
    }

    /* 脚本准备的字符串（UTF-8）→ 脚本编码 → 写进槽。 */
    if (vm != NULL) {
        for (i = 0; i < 3; ++i) {
            if (vm->save_str[i] == NULL || vm->save_str[i]->len == 0) {
                continue;
            }
            {
                FvpStr *enc_str = fvp_encode(&vm->arena, enc,
                                             vm->save_str[i]->data,
                                             vm->save_str[i]->len);
                unsigned char *copy;

                if (enc_str->len == 0 || enc_str->len > 0xFFFFu) {
                    continue;
                }
                copy = (unsigned char *)malloc(enc_str->len);
                if (copy == NULL) {
                    continue;
                }
                memcpy(copy, enc_str->data, enc_str->len);
                free(slot.str[i]);
                slot.str[i] = copy;
                slot.len[i] = enc_str->len;
                nstr += 1;
            }
        }
    }

    {
        size_t need = fvp_save_slot_size(&slot);
        unsigned char *out = need > 0 ? (unsigned char *)malloc(need) : NULL;
        int ok = 0;

        if (out != NULL && fvp_save_slot_store(&slot, out, need) == need) {
            FILE *f = _wfopen(out_path, L"wb");

            if (f != NULL) {
                ok = fwrite(out, 1, need, f) == need;
                fclose(f);
            }
        }
        free(out);
        fvp_save_slot_free(&slot);
        if (!ok) {
            fvp_logf("save-preview: cannot write %s",
                     w2u(out_path, u8, sizeof(u8)));
            return 0;
        }
        fvp_logf("save-preview: wrote %s (%zu bytes, timestamp %04u-%02u-%02u "
                 "%02u:%02u, %d string(s) from the script, thumbnail from the "
                 "live canvas)",
                 w2u(out_path, u8, sizeof(u8)), need, (unsigned)st.wYear,
                 (unsigned)st.wMonth, (unsigned)st.wDay, (unsigned)st.wHour,
                 (unsigned)st.wMinute, nstr);
    }
    return 1;
}

static void work_area_size(int *w, int *h)
{
    RECT r;
    if (SystemParametersInfoW(SPI_GETWORKAREA, 0, &r, 0)) {
        *w = r.right - r.left;
        *h = r.bottom - r.top;
    } else {
        *w = 1920;
        *h = 1080;
    }
}

static void choose_window_size(const Options *o, int *w, int *h)
{
    int wa_w;
    int wa_h;
    int cw;
    int ch;

    work_area_size(&wa_w, &wa_h);
    if (o->scale_mode == 2) {
        cw = 2560;
        ch = 1440;
    } else {
        cw = 1280;
        ch = 720;
    }
    if (cw > wa_w || ch > wa_h) {
        double s = (double)wa_w / (double)cw;
        if ((double)wa_h / (double)ch < s) {
            s = (double)wa_h / (double)ch;
        }
        cw = (int)((double)cw * s);
        ch = (int)((double)ch * s);
        if (cw < 640) {
            cw = wa_w > 640 ? 640 : wa_w;
        }
        if (ch < 360) {
            ch = wa_h > 360 ? 360 : wa_h;
        }
    }
    *w = cw;
    *h = ch;
}

static void make_title(wchar_t *out, size_t cap, const wchar_t *game, int fps,
                       int fullscreen)
{
    wchar_t num[32];
    char a[32];

    snprintf(a, sizeof(a), "%d", fps);
    if (!utf8_to_wide(a, num, 32)) {
        wcscpy(num, L"?");
    }
    out[0] = L'\0';
    if (game != NULL && game[0] != L'\0') {
        wcsncat(out, game, cap - 1);
    } else {
        wcsncat(out, L"FVP", cap - 1);
    }
    wcsncat(out, L"  -  ", cap - 1);
    wcsncat(out, num, cap - 1);
    wcsncat(out, L" FPS", cap - 1);
    if (fullscreen) {
        wcsncat(out, L"  [fullscreen]", cap - 1);
    }
    out[cap - 1] = L'\0';
}

/* ------------------------------------------------------------------ */
/* Renderer helpers                                                    */
/* ------------------------------------------------------------------ */

#if !defined(FVP_STUB_GFX)
static void feed_text_line(Gfx *g, const FvpTextLine *line)
{
    long layer = 0;

    if (line == NULL || line->text == NULL) {
        return;
    }
    if (line->layer.type == (uint8_t)FVP_INT) {
        layer = (long)line->layer.i;
    }
    if (layer < 0 || layer >= GFX_TEXT_MAX) {
        return;
    }
    /* The reference engine keeps one visible line per text window for the
     * layers this game uses; clear-then-append mirrors TextClear+TextPrint.
     * This is a temporary bridge until the syscall layer feeds gfx directly. */
    gfx_text_clear(g, (int)layer);
    gfx_text_append(g, (int)layer, line->text->data);
}

/* Load explicit --font files, or every gfx-font-*.bin next to the exe. */
static int load_fonts(Gfx *g, const Options *o, const wchar_t *exe_dir,
                      GfxFont *out[FONT_MAX])
{
    int n = 0;
    int i;

    if (g == NULL) {
        return 0;
    }
    if (o->nfonts > 0) {
        for (i = 0; i < o->nfonts && n < FONT_MAX; ++i) {
            char a[PATH_CAP * 3];
            GfxFont *f;
            if (!wide_to_ansi(o->fonts[i], a, sizeof(a))) {
                fvp_logf("font: cannot convert path %s",
                         w2u(o->fonts[i], a, sizeof(a)));
                continue;
            }
            f = gfx_font_load(a);
            if (f == NULL) {
                fvp_logf("font: load failed: %s", a);
                continue;
            }
            if (gfx_add_font(g, f) == 0) {
                out[n++] = f;
                fvp_logf("font: %s size=%d glyphs=%d", a,
                         gfx_font_pixel_size(f), gfx_font_glyph_count(f));
            } else {
                gfx_font_free(f);
                fvp_logf("font: gfx_add_font rejected %s", a);
            }
        }
        return n;
    }

    {
        wchar_t pattern[PATH_CAP];
        WIN32_FIND_DATAW fd;
        HANDLE h;
        if (!path_join_w(pattern, PATH_CAP, exe_dir, L"gfx-font-*.bin")) {
            return 0;
        }
        h = FindFirstFileW(pattern, &fd);
        if (h == INVALID_HANDLE_VALUE) {
            return 0;
        }
        do {
            wchar_t path[PATH_CAP];
            char a[PATH_CAP * 3];
            GfxFont *f;
            if (n >= FONT_MAX) {
                break;
            }
            if (!path_join_w(path, PATH_CAP, exe_dir, fd.cFileName)) {
                continue;
            }
            if (!wide_to_ansi(path, a, sizeof(a))) {
                continue;
            }
            f = gfx_font_load(a);
            if (f == NULL) {
                fvp_logf("font: load failed: %s", a);
                continue;
            }
            if (gfx_add_font(g, f) == 0) {
                out[n++] = f;
                fvp_logf("font: %s size=%d glyphs=%d", a,
                         gfx_font_pixel_size(f), gfx_font_glyph_count(f));
            } else {
                gfx_font_free(f);
            }
        } while (FindNextFileW(h, &fd));
        FindClose(h);
    }
    return n;
}
#endif /* !FVP_STUB_GFX */

/* ------------------------------------------------------------------ */
/* Present-time histogram                                              */
/* ------------------------------------------------------------------ */

#define PRESENT_BUCKETS 512
#define PRESENT_BUCKET_MS 0.5

typedef struct PerfStats {
    unsigned long frames;
    double present_sum;
    double present_max;
    unsigned long present_buckets[PRESENT_BUCKETS];
    double frame_sum;
    double frame_max;
    unsigned long slow_frames;
} PerfStats;

static void perf_add(PerfStats *p, double present_ms, double frame_ms)
{
    int bucket;
    p->frames += 1;
    p->present_sum += present_ms;
    if (present_ms > p->present_max) {
        p->present_max = present_ms;
    }
    bucket = (int)(present_ms / PRESENT_BUCKET_MS);
    if (bucket < 0) {
        bucket = 0;
    }
    if (bucket >= PRESENT_BUCKETS) {
        bucket = PRESENT_BUCKETS - 1;
    }
    p->present_buckets[bucket] += 1;
    p->frame_sum += frame_ms;
    if (frame_ms > p->frame_max) {
        p->frame_max = frame_ms;
    }
    if (present_ms > 20.0) {
        p->slow_frames += 1;
    }
}

/* 窗口统计清零：周期报告用的是「本窗口」的累计值，不是全程累计值。 */
static void perf_clear(PerfStats *p)
{
    memset(p, 0, sizeof(*p));
}

static double perf_percentile(const PerfStats *p, double pct)
{
    unsigned long long total = 0;
    unsigned long long want;
    unsigned long long acc = 0;
    int i;

    for (i = 0; i < PRESENT_BUCKETS; ++i) {
        total += p->present_buckets[i];
    }
    if (total == 0) {
        return 0.0;
    }
    want = (unsigned long long)((double)total * pct);
    for (i = 0; i < PRESENT_BUCKETS; ++i) {
        acc += p->present_buckets[i];
        if (acc >= want) {
            return ((double)i + 1.0) * PRESENT_BUCKET_MS;
        }
    }
    return (double)PRESENT_BUCKETS * PRESENT_BUCKET_MS;
}

/* ------------------------------------------------------------------ */
/* WinMain                                                             */
/* ------------------------------------------------------------------ */

int WINAPI WinMain(HINSTANCE inst, HINSTANCE prev, LPSTR cmdline, int show)
{
    Options opts;
    int argc = 0;
    wchar_t **argv = NULL;
    wchar_t script_path[PATH_CAP];
    wchar_t game_dir[PATH_CAP];
    wchar_t exe_dir[PATH_CAP];
    wchar_t log_path[PATH_CAP];
    wchar_t title_game[PATH_CAP];
    char script_a[PATH_CAP * 3];
    char game_u8[PATH_CAP * 3];
    char u8[PATH_CAP * 3];
    FvpHcb hcb;
    FvpVM vm;
    Snd *snd = NULL;
    FvpAudio *audio = NULL;
    Gfx *gfx = NULL;
#if !defined(FVP_STUB_GFX)
    GfxArchiveHost *archive_host = NULL;
    GfxFont *fonts[FONT_MAX];
    int nfonts_loaded = 0;
#endif
    FvpWin32Window win;
    int parsed;
    int script_ok;
    int win_w = 1280;
    int win_h = 720;
    char err[512];
    int exit_code = 0;
    double t_load;
    double t_ready;
    double t_first_frame = 0.0;
    int have_first_frame = 0;
    FvpEncoding save_enc; /* 存档字符串用哪种编码写（默认跟随脚本） */

    uint64_t text_logged = 0;
    int frame = 0;
    int fps = 0;
    int fps_frames = 0;
    double fps_t0 = 0.0;
    double last_frame_time = 0.0;
    double next_frame = 0.0;
    double text_hold_until = 0.0;
    int click_armed = 0;
    double click_armed_at = 0.0;
    double last_auto_click = 0.0;
    int click_flash = 0;
    int exit_auto = 0;
    double last_preview_at = 0.0;
    int preview_seq = 0;
    int exit_script = 0;
    double last_report = 0.0;
    PerfStats perf;
    PerfStats wperf; /* 本周期 5 s 窗口内的统计（周期报告用，见 perf_clear） */
    unsigned long long steps_at_report = 0;
    double run_started = 0.0;
    int verbose;

    (void)prev;
    (void)cmdline;
    (void)show;

    QueryPerformanceFrequency(&g_qpc_freq);
    g_start_time = now_sec();
    set_dpi_awareness();
    /* Needed for SHBrowseForFolder (double-click folder picker). */
    (void)CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);

    defaults(&opts);
    argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    if (argv == NULL) {
        MessageBoxW(NULL, L"CommandLineToArgvW failed", L"fvp.exe",
                    MB_OK | MB_ICONERROR);
        return 2;
    }

    if (!exe_dir_w(exe_dir, PATH_CAP)) {
        wcscpy(exe_dir, L".");
    }
    /* Open the default log before parsing so option errors are recorded; a
     * --log option re-opens it below. */
    if (!path_join_w(log_path, PATH_CAP, exe_dir, L"fvp-win32.log")) {
        wcscpy(log_path, L"fvp-win32.log");
    }
    if (!fvp_log_open(log_path)) {
        wchar_t tmp[PATH_CAP];
        DWORD n = GetTempPathW((DWORD)PATH_CAP, tmp);
        if (n > 0 && n < (DWORD)PATH_CAP &&
            path_join_w(log_path, PATH_CAP, tmp, L"fvp-win32.log")) {
            fvp_log_open(log_path);
        }
    }
    fvp_logf("fvp win32 shell %s starting (pid %lu)", FVP_WIN32_VERSION,
             (unsigned long)GetCurrentProcessId());

    parsed = parse_args(argc, argv, &opts);
    if (parsed != 0) {
        if (parsed == 2) {
            show_info(L"fvp.exe",
                      L"参数错误。\n请运行 fvp.exe --help 查看用法。");
        }
        fvp_log_close();
        LocalFree(argv);
        return parsed == 1 ? 0 : 2;
    }
    verbose = opts.verbose;

    /* 存档字符串编码：显式 --save-encoding 优先，否则跟随脚本编码。 */
    save_enc = (opts.enc_mode == ENC_GBK) ? FVP_ENC_GBK : FVP_ENC_CP932;
    if (wcscmp(opts.save_encoding, L"gbk") == 0) {
        save_enc = FVP_ENC_GBK;
    } else if (wcscmp(opts.save_encoding, L"cp932") == 0) {
        save_enc = FVP_ENC_CP932;
    } else if (opts.save_encoding[0] != L'\0') {
        fvp_logf("save: unknown --save-encoding '%ls' (use cp932|gbk), "
                 "following the script encoding instead",
                 opts.save_encoding);
    }

    if (opts.log_path[0] != L'\0') {
        fvp_log_close();
        if (!fvp_log_open(opts.log_path)) {
            show_error(L"fvp.exe", L"无法创建 --log 指定的日志文件。");
            LocalFree(argv);
            return 2;
        }
        fvp_logf("fvp win32 shell %s starting (pid %lu)", FVP_WIN32_VERSION,
                 (unsigned long)GetCurrentProcessId());
    }
    if (opts.console) {
        fvp_log_attach_console();
    }
    fvp_logf("args: argc=%d", argc);
    if (verbose) {
        int i;
        for (i = 0; i < argc; ++i) {
            fvp_logf("args[%d]=%s", i, w2u(argv[i], u8, sizeof(u8)));
        }
    }

    timeBeginPeriod(1);

    if (!opts.game_dir_set) {
        wchar_t found[PATH_CAP];
        if (find_game_dir_default(found, PATH_CAP)) {
            wcsncpy(opts.game_dir, found, PATH_CAP - 1);
            opts.game_dir_set = 1;
            fvp_logf("game dir auto-detected: %s",
                     w2u(found, u8, sizeof(u8)));
        } else if (pick_game_dir(found, PATH_CAP)) {
            wcsncpy(opts.game_dir, found, PATH_CAP - 1);
            opts.game_dir_set = 1;
            fvp_logf("game dir picked: %s", w2u(found, u8, sizeof(u8)));
        }
    }

    if (!resolve_paths(&opts, script_path, game_dir)) {
        show_error(L"fvp.exe", L"找不到可运行的 .hcb 脚本。\n"
                               L"请用 --game <游戏目录> 指定游戏目录。");
        fvp_log_close();
        timeEndPeriod(1);
        LocalFree(argv);
        return 2;
    }
    if (!wide_to_ansi(script_path, script_a, sizeof(script_a))) {
        show_error(L"fvp.exe", L"脚本路径无法转换为系统编码。");
        fvp_log_close();
        timeEndPeriod(1);
        LocalFree(argv);
        return 2;
    }
    if (!wide_to_utf8(game_dir, game_u8, sizeof(game_u8))) {
        show_error(L"fvp.exe", L"游戏目录路径无法转换为 UTF-8。");
        fvp_log_close();
        timeEndPeriod(1);
        LocalFree(argv);
        return 2;
    }
    fvp_logf("game dir: %s", w2u(game_dir, u8, sizeof(u8)));
    fvp_logf("script:   %s", w2u(script_path, u8, sizeof(u8)));

    /* ---------------- script + VM ---------------- */
    t_load = now_sec();
    memset(&hcb, 0, sizeof(hcb));
    script_ok = load_script(&hcb, script_a, opts.enc_mode, err, sizeof(err),
                            verbose);
    if (!script_ok) {
        wchar_t msg[1024];
        wchar_t werr[512];
        if (!utf8_to_wide(err, werr, 512)) {
            wcscpy(werr, L"(unknown error)");
        }
        msg[0] = L'\0';
        wcsncat(msg, L"脚本加载失败:\n", 1024 - 1);
        wcsncat(msg, script_path, 1024 - 1);
        wcsncat(msg, L"\n\n", 1024 - 1);
        wcsncat(msg, werr, 1024 - 1);
        show_error(L"fvp.exe", msg);
        fvp_log_close();
        timeEndPeriod(1);
        LocalFree(argv);
        return 3;
    }
    fvp_logf("script: loaded in %.1f ms (%u instrs, script_len=%u)",
             (now_sec() - t_load) * 1000.0, (unsigned)hcb.ninstr,
             (unsigned)hcb.script_len);

    memset(&vm, 0, sizeof(vm));
    fvp_vm_init(&vm, &hcb, 1);
    vm.timeline_on = opts.timeline;
    if (opts.timeline) {
        fvp_logf("time: virtual clock enabled (advanced by frame dt)");
    }

    /* 全局存档 save/save.bin 里存的是设置 / flag / 已读文本位图，属于运行时状态，
     * 必须在脚本跑起来之前灌进 VM。只灌已确证的 type==2 整型（docs/save-format.md §7.1）。 */
    {
        char *save_path = fvp_save_find_file(game_u8);

        if (save_path == NULL) {
            fvp_logf("save: no save/save.bin under the game directory "
                     "(starting from defaults)");
        } else {
            FvpSaveBin sb;

            if (fvp_save_bin_load_file(&sb, save_path)) {
                FvpSaveGlobalsStats st;
                unsigned int tw = (unsigned int)sb.hdr[12] |
                                  ((unsigned int)sb.hdr[13] << 8) |
                                  ((unsigned int)sb.hdr[14] << 16) |
                                  ((unsigned int)sb.hdr[15] << 24);
                unsigned int th = (unsigned int)sb.hdr[16] |
                                  ((unsigned int)sb.hdr[17] << 8) |
                                  ((unsigned int)sb.hdr[18] << 16) |
                                  ((unsigned int)sb.hdr[19] << 24);
                int applied = fvp_save_globals_apply(&sb, vm.globals,
                                                     FVP_GLOBAL_COUNT, &st);
                int flag_bytes = fvp_save_flags_apply(&sb, vm.flags,
                                                     FVP_FLAG_BYTES);

                fvp_logf("save: %s -> %d/%d globals applied (%d int, "
                         "%d unknown type1, %d unset), %d flags bit(s) set, "
                         "thumb %ux%u, readmap %u bytes",
                         save_path, applied, st.total, st.ints, st.unknown,
                         st.unset, fvp_flag_count(vm.flags), tw, th,
                         FVP_SAVE_READMAP_SIZE);
                (void)flag_bytes;
                fvp_save_bin_free(&sb);
            } else {
                fvp_logf("save: %s is not a valid save.bin (ignored)",
                         save_path);
            }
            free(save_path);
        }
    }
    fvp_vm_start(&vm);
    fvp_logf("vm: started at pc=0x%x (quiet text capture)", hcb.main_start);

    snd = snd_create(48000, 2);
    if (snd == NULL) {
        fvp_logf("audio: snd_create failed, running silent");
    }
    if (!opts.no_audio) {
        audio = fvp_audio_start(snd, 48000, 2, opts.audio_buffers,
                                opts.audio_frames, opts.tone_hz);
        if (audio == NULL) {
            fvp_logf("audio: output disabled (shell continues without sound)");
        }
    } else {
        fvp_logf("audio: disabled by --no-audio");
    }

    /* ---------------- renderer ---------------- */
#if defined(FVP_STUB_GFX)
    gfx = gfx_create(FVP_CANVAS_W, FVP_CANVAS_H, NULL);
    fvp_logf("gfx: canvas %dx%d (stub test pattern)",
             FVP_CANVAS_W, FVP_CANVAS_H);
#else
    {
        const GfxHost *host = NULL;
        archive_host = gfx_archive_host_create(game_u8);
        if (archive_host != NULL) {
            host = gfx_archive_host_iface(archive_host);
            fvp_logf("gfx: archive host ready for %s (%d images indexed)",
                     game_u8, gfx_archive_host_image_count(archive_host));
        } else {
            fvp_logf("gfx: archive host unavailable (textures disabled)");
        }
        gfx = gfx_create(FVP_CANVAS_W, FVP_CANVAS_H, host);
        fvp_logf("gfx: canvas %dx%d (src/gfx.c)", FVP_CANVAS_W, FVP_CANVAS_H);
        nfonts_loaded = load_fonts(gfx, &opts, exe_dir, fonts);
        fvp_logf("gfx: %d font atlas(es) registered", nfonts_loaded);
        fvp_logf("gfx: text bridge %s (temporary until the syscall layer "
                 "feeds gfx)",
                 opts.feed_text ? "ON" : "OFF");
    }
#endif
    if (gfx == NULL) {
        show_error(L"fvp.exe", L"渲染层 gfx_create 失败。");
        fvp_audio_stop(audio);
        snd_destroy(snd);
        fvp_vm_free(&vm);
        fvp_hcb_free(&hcb);
        fvp_log_close();
        timeEndPeriod(1);
        LocalFree(argv);
        return 4;
    }

#if !defined(FVP_STUB_GFX)
    /*
     * 把渲染层挂到虚拟机上：此后 GraphLoad / PrimSet* / Text* 这些命令
     * 由 syscall 直接驱动 gfx，不再需要壳层手动喂文本。
     */
    fvp_vm_attach_gfx(&vm, gfx);
    opts.feed_text = 0; /* 旧的临时文本桥关闭，否则文本会被追加两次 */
    fvp_logf("gfx: attached to vm (syscall -> gfx bridge active)");
    /* 动画补间：默认关闭，--motion 才让 Motion* 真正改图元字段。 */
    gfx_motion_set_enabled(gfx, opts.motion);
    fvp_logf("motion: %s",
             opts.motion ? "enabled (tweens modify prims)"
                         : "disabled (static prims, pixel baseline kept)");
#endif

    /*
     * 把音频挂到虚拟机上：AudioPlay / AudioLoad 等命令会真的解码并混音，
     * 由 audio 线程从 snd_mix 取数据送到 waveOut。
     */
    if (snd != NULL) {
        int added;
        fvp_vm_attach_snd(&vm, snd);
        added = fvp_vm_snd_add_audio_dir(&vm, game_u8);
        fvp_logf("snd: attached to vm (%d audio archive(s) registered)", added);
        if (added <= 0) {
            fvp_logf("snd: no audio package found in %s (BGM/voice will be silent)",
                     game_u8);
        }
    }

    /* ---------------- window ---------------- */
    choose_window_size(&opts, &win_w, &win_h);
    if (!path_basename_w(game_dir, title_game, PATH_CAP) ||
        title_game[0] == L'\0') {
        if (!path_basename_w(script_path, title_game, PATH_CAP)) {
            wcscpy(title_game, L"FVP");
        }
    }
    {
        wchar_t title[256];
        make_title(title, 256, title_game, 60, 0);
        if (!fvp_win32_create(&win, inst, title, win_w, win_h, FVP_CANVAS_W,
                              FVP_CANVAS_H, opts.filter_mode,
                              opts.fullscreen)) {
            show_error(L"fvp.exe", L"创建窗口失败。");
            gfx_destroy(gfx);
            fvp_audio_stop(audio);
            snd_destroy(snd);
            fvp_vm_free(&vm);
            fvp_hcb_free(&hcb);
            fvp_log_close();
            timeEndPeriod(1);
            LocalFree(argv);
            return 5;
        }
    }

    t_ready = now_sec();
    fvp_logf("startup: script+vm+gfx+window ready in %.1f ms",
             (t_ready - g_start_time) * 1000.0);

    /* ---------------- frame loop ---------------- */
    memset(&perf, 0, sizeof(perf));
    perf_clear(&wperf);
    last_frame_time = now_sec();
    next_frame = last_frame_time;
    fps_t0 = last_frame_time;
    run_started = last_frame_time;
    last_report = last_frame_time;
    steps_at_report = vm.steps;

    while (!win.quit) {
        FvpWin32Input *in = &win.input;
        double now;
        double frame_dt;
        double render_ms;
        double present_ms;
        size_t text_before;
        int new_text = 0;
        int want_skip_text = 0;
        int click_edges;
        int cw = FVP_CANVAS_W;
        int ch = FVP_CANVAS_H;
        const uint32_t *canvas;

        if (!fvp_win32_pump(&win)) {
            break;
        }
        now = now_sec();

        if (opts.run_seconds > 0.0 && now - run_started >= opts.run_seconds) {
            exit_auto = 1;
            fvp_logf("run: --run-seconds %.1f reached, exiting",
                     opts.run_seconds);
            break;
        }

        /* 运行中周期性写存档预览（相当于「按 F5 存档」的自动化版本）。 */
        if (opts.save_preview_every > 0.0 && gfx != NULL &&
            opts.save_template[0] != L'\0' &&
            opts.save_preview[0] != L'\0' &&
            now - last_preview_at >= opts.save_preview_every) {
            int cw2 = FVP_CANVAS_W;
            int ch2 = FVP_CANVAS_H;
            const uint32_t *canvas2 = gfx_canvas(gfx, &cw2, &ch2);

            if (canvas2 != NULL) {
                wchar_t numbered[PATH_CAP * 2];

                preview_seq += 1;
                _snwprintf(numbered, PATH_CAP * 2 - 1, L"%s.%d",
                           opts.save_preview, preview_seq);
                write_save_preview(opts.save_template, numbered,
                                   (const unsigned char *)canvas2, cw2, ch2,
                                   &vm,
                                   save_enc);
                last_preview_at = now;
            }
        }

        /* Fixed 60 Hz pacing with a 1 ms timer resolution. */
        if (now < next_frame) {
            double remaining = next_frame - now;
            if (remaining > 0.0015) {
                Sleep((DWORD)((remaining - 0.001) * 1000.0));
            } else {
                /* Busy-wait the last ~1.5 ms: Sleep() granularity would cost
                 * ~1.5 fps and add jitter to the frame interval. */
                while (now_sec() < next_frame) {
                    /* spin */
                }
            }
            continue;
        }
        frame_dt = now - last_frame_time;
        if (opts.timeline && frame_dt > 0.0) {
            vm.virtual_ms += (uint64_t)(frame_dt * 1000.0);
        }
        if (frame_dt < 0.0) {
            frame_dt = 0.0;
        }
        if (frame_dt > 0.25) {
            frame_dt = 0.25;
        }
        if (frame_dt * 1000.0 > 30.0) {
            fvp_logf("perf: slow frame dt=%.1f ms (frame %d)", frame_dt * 1000.0,
                     frame);
        }
        last_frame_time = now;
        next_frame += FVP_FRAME_DT;
        if (next_frame < now - FVP_FRAME_DT) {
            next_frame = now; /* drop accumulated catch-up after a stall */
        }

        /* ---------------- input -> VM ---------------- */
        vm.cx = in->canvas_x;
        vm.cy = in->canvas_y;
        click_edges = in->mouse_pressed + in->key_pressed;
        in->mouse_pressed = 0;
        in->key_pressed = 0;
        if (click_edges > 0) {
            want_skip_text = 1;
            click_flash = 10;
            if (!click_armed) {
                click_armed = 1;
                click_armed_at = now;
                fvp_logf("input: click edge at canvas (%d,%d), steps=%llu",
                         in->canvas_x, in->canvas_y,
                         (unsigned long long)vm.steps);
            }
        }
        if (in->mouse_down || in->key_down) {
            /* Holding the button / Enter behaves like a VN fast-forward:
             * repeat about eight times per second. */
            if (now - last_auto_click >= 0.125) {
                last_auto_click = now;
                if (!click_armed) {
                    click_armed = 1;
                    click_armed_at = now;
                }
            }
        }
        if (in->wheel_delta != 0) {
            fvp_logf("input: wheel delta=%d at canvas (%d,%d)",
                     in->wheel_delta, in->canvas_x, in->canvas_y);
            /* 交给 VM：脚本用 InputGetWheel(36) 取走（120 = 一格）。 */
            fvp_vm_wheel_add(&vm, in->wheel_delta);
            in->wheel_delta = 0;
        }

        /* ---------------- VM time slice ---------------- */
        text_before = vm.ntext;
        if (want_skip_text) {
            text_hold_until = 0.0;
        }
        if (text_hold_until <= now || click_armed) {
            uint64_t target = vm.steps + (uint64_t)opts.burst_steps;
            int polls = 0;

            while (vm.steps < target && vm.nthreads > 0 && !win.quit) {
                int is_poll = 0;
                int will_poll = 0;
                uint64_t before;

                if (vm.nthreads > 0) {
                    FvpThread *t = vm.threads[0];
                    if (t != NULL && !t->blocked) {
                        const FvpInstr *ins = fvp_hcb_at(&hcb, t->pc);
                        if (ins != NULL && ins->op == 0x03 && ins->w == 31) {
                            is_poll = 1;
                            will_poll = click_armed;
                        }
                    }
                }
                if (will_poll) {
                    /* InputGetDown (cmd 31) is the script's click poll.  The
                     * syscall stub reports "down" only while vm->click_period
                     * is set, so hold it for exactly the one instruction that
                     * polls: a real mouse press becomes a single edge-triggered
                     * click the script can see, with no busy-wait. */
                    vm.click_period = 2;
                }
                before = vm.steps;
                fvp_vm_run(&vm, before + 1u);
                if (will_poll) {
                    vm.click_period = 0;
                    if (vm.steps == before + 1u) {
                        click_armed = 0;
                        fvp_logf("input: click consumed by InputGetDown at "
                                 "steps=%llu",
                                 (unsigned long long)vm.steps);
                    }
                }
                if (vm.steps == before) {
                    break; /* no thread made progress */
                }
                if (vm.ntext > text_before) {
                    new_text = 1;
                    break;
                }
                if (is_poll) {
                    polls += 1;
                    if (polls > 96) {
                        break; /* script is waiting for a click */
                    }
                }
            }
        }

        /* ---------------- text pacing / text bridge ---------------- */
        if (new_text) {
            uint64_t i;
            for (i = text_logged; i < vm.ntext; ++i) {
                log_text_line(&vm.text[i], vm.steps);
#if !defined(FVP_STUB_GFX)
                if (opts.feed_text) {
                    feed_text_line(gfx, &vm.text[i]);
                }
#endif
            }
            text_logged = vm.ntext;
            if (vm.ntext > 0) {
                double hold = text_hold_seconds(&opts, &vm.text[vm.ntext - 1]);
                text_hold_until = hold > 0.0 ? now + hold : 0.0;
            }
        }
        if (click_armed && now - click_armed_at > 3.0) {
            fvp_logf("input: click expired (script did not poll InputGetDown)");
            click_armed = 0;
        }
        if (click_flash > 0) {
            click_flash -= 1;
        }

        /* ---------------- render + present ---------------- */
#ifdef FVP_STUB_GFX
        fvp_stub_overlay(in->canvas_x, in->canvas_y, in->mouse_down,
                         (unsigned long long)vm.steps, fps, frame,
                         click_flash);
#endif
        {
            double r0 = now_sec();
            gfx_render(gfx);
            canvas = gfx_canvas(gfx, &cw, &ch);
            render_ms = (now_sec() - r0) * 1000.0;
        }
        if (canvas == NULL) {
            fvp_logf("gfx: gfx_canvas returned NULL, stopping");
            exit_code = 6;
            break;
        }
        {
            double p0 = now_sec();
            fvp_win32_present(&win, canvas, cw, ch);
            present_ms = (now_sec() - p0) * 1000.0;
        }
        /* 动画补间：默认关闭时只走时间轴，不改图元字段。 */
        gfx_motion_advance(gfx, (int)(frame_dt * 1000.0));
        perf_add(&perf, present_ms, frame_dt * 1000.0);
        perf_add(&wperf, present_ms, frame_dt * 1000.0);
        frame += 1;
        (void)frame; /* consumed by the stub renderer only */
        fps_frames += 1;

        if (!have_first_frame) {
            have_first_frame = 1;
            t_first_frame = now_sec();
            fvp_logf("startup: first frame presented %.1f ms after process "
                     "start (render %.2f ms, present %.2f ms = fill %.2f + "
                     "stretch %.2f + blit %.2f)",
                     (t_first_frame - g_start_time) * 1000.0, render_ms,
                     present_ms, win.t_fill_ms, win.t_stretch_ms,
                     win.t_blit_ms);
        }

        /* ---------------- periodic reporting ---------------- */
        now = now_sec();
        if (now - fps_t0 >= 0.5) {
            double dt = now - fps_t0;
            wchar_t title[256];
            fps = (int)((double)fps_frames / dt + 0.5);
            fps_frames = 0;
            fps_t0 = now;
            make_title(title, 256, title_game, fps, win.fullscreen);
            fvp_win32_set_title(&win, title);
        }
        if (now - last_report >= 5.0) {
            double secs = now - last_report;
            unsigned long frames = wperf.frames;
            unsigned long long steps = vm.steps - steps_at_report;
            int a_ok = 0;
            int a_sub = 0;
            int a_done = 0;
            int a_under = 0;
            unsigned long a_err = 0;
            fvp_audio_stats(audio, &a_ok, &a_sub, &a_done, &a_under, &a_err);
            fvp_logf("perf: fps=%.1f frame_avg=%.2fms frame_max=%.2fms "
                     "present_avg=%.2fms present_max=%.2fms slow=%lu",
                     (double)frames / secs,
                     frames > 0 ? wperf.frame_sum / (double)frames : 0.0,
                     wperf.frame_max,
                     frames > 0 ? wperf.present_sum / (double)frames : 0.0,
                     wperf.present_max, wperf.slow_frames);
            fvp_logf("vm:   %llu steps (%.0f steps/s), text lines %llu, "
                     "threads %llu",
                     (unsigned long long)steps, (double)steps / secs,
                     (unsigned long long)vm.ntext,
                     (unsigned long long)vm.nthreads);
            fvp_logf("tex: %d image(s), %.1f MiB cached",
                     gfx_tex_count(gfx),
                     (double)gfx_tex_bytes(gfx) / (1024.0 * 1024.0));
            fvp_logf("time: virtual %llu ms (timeline %s)",
                     (unsigned long long)vm.virtual_ms,
                     opts.timeline ? "on" : "off");
            fvp_logf("motion: %d active tween(s), %s", gfx_motion_active(gfx),
                     gfx_motion_enabled(gfx) ? "enabled" : "disabled");
            fvp_logf("audio: %s submitted=%d completed=%d underruns=%d "
                     "err=%lu",
                     fvp_audio_status(audio), a_sub, a_done, a_under, a_err);
            if (verbose) {
                fvp_logf("input: mouse=(%d,%d) canvas=(%d,%d) down=%d "
                         "armed=%d hold=%.2fs",
                         in->client_x, in->client_y, in->canvas_x,
                         in->canvas_y, in->mouse_down, click_armed,
                         text_hold_until > now ? text_hold_until - now : 0.0);
            }
            last_report = now;
            steps_at_report = vm.steps;
            perf_clear(&wperf); /* 下一个窗口重新计数 */
        }
        if (vm.nthreads == 0 && !exit_script) {
            exit_script = 1;
            fvp_logf("vm: all threads exited (script finished at steps=%llu)",
                     (unsigned long long)vm.steps);
        }
    }

    /* ---------------- screenshot ---------------- */
    if ((opts.screenshot[0] != L'\0' || opts.thumb_shot[0] != L'\0' ||
         (opts.save_template[0] != L'\0' && opts.save_preview[0] != L'\0')) &&
        gfx != NULL) {
        int cw = FVP_CANVAS_W;
        int ch = FVP_CANVAS_H;
        const uint32_t *canvas;
        gfx_render(gfx);
        canvas = gfx_canvas(gfx, &cw, &ch);
        if (canvas != NULL && opts.screenshot[0] != L'\0') {
            write_bmp32(opts.screenshot, canvas, cw, ch);
        }
        /* 存档缩略图：把当前画面按 200x113 缩出来写 BMP（通道顺序对 box filter
         * 无所谓，写 BMP 时再按 BGR 排）。真存档里是裸 BGRA，这里只为人工对照。 */
        if (canvas != NULL && opts.thumb_shot[0] != L'\0') {
            const int tw = FVP_SAVE_THUMB_W_DEFAULT;
            const int th = FVP_SAVE_THUMB_H_DEFAULT;
            unsigned char *tmp =
                (unsigned char *)malloc((size_t)tw * th * 4u);
            uint32_t *pix =
                (uint32_t *)malloc((size_t)tw * th * sizeof(uint32_t));

            if (tmp != NULL && pix != NULL &&
                fvp_thumb_scale_bgra((const unsigned char *)canvas, cw, ch, tmp,
                                     tw, th)) {
                memcpy(pix, tmp, (size_t)tw * th * 4u);
                if (write_bmp32(opts.thumb_shot, pix, tw, th)) {
                    char u8[PATH_CAP * 3];

                    fvp_logf("thumb: wrote %s (%dx%d, 4 bytes per pixel)",
                             w2u(opts.thumb_shot, u8, sizeof(u8)), tw, th);
                }
            } else {
                fvp_logf("thumb: scaling failed (out of memory?)");
            }
            free(tmp);
            free(pix);
        }
        /* 存档预览：模板槽 + 当前时间 + 当前画面缩略图。 */
        if (canvas != NULL && opts.save_template[0] != L'\0' &&
            opts.save_preview[0] != L'\0') {
            write_save_preview(opts.save_template, opts.save_preview,
                               (const unsigned char *)canvas, cw, ch, &vm,
                               save_enc);
        }
    }

    /* ---------------- summary ---------------- */
    {
        int a_ok = 0;
        int a_sub = 0;
        int a_done = 0;
        int a_under = 0;
        unsigned long a_err = 0;
        char buf[2048];
        int n = 0;
        double run_seconds = now_sec() - run_started;
        double avg_present =
            perf.frames > 0 ? perf.present_sum / (double)perf.frames : 0.0;
        double avg_frame =
            perf.frames > 0 ? perf.frame_sum / (double)perf.frames : 0.0;

#define APPEND(...)                                                          \
    do {                                                                     \
        if (n < (int)sizeof(buf) - 1) {                                      \
            int m = snprintf(buf + n, sizeof(buf) - (size_t)n, __VA_ARGS__); \
            if (m > 0) {                                                     \
                n += m;                                                      \
            }                                                                \
            if (n > (int)sizeof(buf) - 1) {                                  \
                n = (int)sizeof(buf) - 1;                                    \
            }                                                                \
        }                                                                    \
    } while (0)

        fvp_audio_stats(audio, &a_ok, &a_sub, &a_done, &a_under, &a_err);
        APPEND("\n=== win32 shell summary ===\n");
        APPEND("run            %.2f s, %lu frames (%.2f fps)\n", run_seconds,
               perf.frames,
               run_seconds > 0 ? (double)perf.frames / run_seconds : 0.0);
        APPEND("startup        ready %.1f ms, first frame %.1f ms\n",
               (t_ready - g_start_time) * 1000.0,
               have_first_frame ? (t_first_frame - g_start_time) * 1000.0
                                : 0.0);
        APPEND("frame pacing   avg %.2f ms, max %.2f ms (target %.2f ms)\n",
               avg_frame, perf.frame_max, FVP_FRAME_DT * 1000.0);
        APPEND("gdi present    avg %.2f ms, p95 %.2f ms, max %.2f ms, "
               "slow(>20ms) %lu\n",
               avg_present, perf_percentile(&perf, 0.95), perf.present_max,
               perf.slow_frames);
        APPEND("vm             %llu steps total, %llu text lines, %llu threads "
               "left\n",
               (unsigned long long)vm.steps, (unsigned long long)vm.ntext,
               (unsigned long long)vm.nthreads);
        APPEND("audio          ok=%d submitted=%d completed=%d underruns=%d\n",
               a_ok, a_sub, a_done, a_under);
        APPEND("exit           %s\n",
               exit_auto ? "run-seconds" :
               exit_script ? "script-finished" :
               win.quit ? "window/ESC" : "error");
        buf[sizeof(buf) - 1] = '\0';
        fvp_log_raw(buf);
#undef APPEND
    }
    fvp_logf("exit: code=%d threads=%llu steps=%llu", exit_code,
             (unsigned long long)vm.nthreads,
             (unsigned long long)vm.steps);

    /* ---------------- cleanup ---------------- */
    fvp_audio_stop(audio);
    if (gfx != NULL) {
        gfx_destroy(gfx);
        gfx = NULL;
    }
#if !defined(FVP_STUB_GFX)
    if (archive_host != NULL) {
        gfx_archive_host_destroy(archive_host);
        archive_host = NULL;
    }
    {
        int i;
        for (i = 0; i < nfonts_loaded; ++i) {
            gfx_font_free(fonts[i]);
        }
    }
#endif
    fvp_win32_destroy(&win);
    fvp_vm_free(&vm);
    fvp_hcb_free(&hcb);
    snd_destroy(snd);
    timeEndPeriod(1);
    fvp_log_close();
    LocalFree(argv);
    (void)verbose;
    CoUninitialize();
    return exit_code;
}