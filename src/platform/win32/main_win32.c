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
#include "win32_gdi_text.h"
#include "../../titlemenu.h"
#include "../../openseq.h"
#include "../../savehist.h"
#include "../../savelist.h"
#include "../../gamemenu.h"
#include "../../titleanim.h"
#include "../../titleenter.h"
#include "../../titlebg.h"

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
    int threads;
    int tex_cache_mb; /* --tex-cache-mb: texture cache cap in MiB, 0 = unlimited */ /* --no-threads: 关协程槽（默认开） */
    int audio_buffers;
    int audio_frames;

    int burst_steps; /* VM steps per frame when not waiting for text/input */
    int stall_after; /* 0 = 关闭；见 src/vm.h 的 stall_* */
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
    int unknown_all; /* --unknown-all: 退出总结打印全部未实现命令 */
    int typewriter; /* --typewriter: 逐字显示（默认关，关时像素基准不变） */
    int text_prims; /* --text-prims: 按文本图元（PrimSetText 86）定位，默认关 */
    int title_menu; /* --title-menu: 先显示引擎侧标题菜单，选了 New Game 才跑脚本 */
    int title_save_list; /* --title-save-list: 直接打开 Continue 的存档列表 */
    /* 调试钩子：脚本自己调 SaveWrite/Load 之前先手动造一个请求，用来端到端
     * 验证"请求 -> 外壳执行"这条链路（-1 = 不发）。 */
    int no_input; /* --no-input：忽略真实鼠标/键盘事件（门禁截图要确定性） */
    /* --glyph-original：缺字回退改用**原版同协议**（cp932 + GGO_BITMAP 1bpp），
     * 默认走抗锯齿路径（汉化文本更清晰）。协议依据见
     * ref/exe-gdi-params.json 与 docs/original-verification.md §8。 */
    int glyph_original;
    wchar_t title_font[PATH_CAP]; /* --title-font：标题菜单专用字库 */
    int pick_seed;  /* --pick N：启动就把第 N 项当作已选中（门禁/截图用） */
    int title_fade_ms; /* 点 New Game 后的白场过渡时长（0 = 直接进，默认 800） */
    int menu_adjust; /* --menu-adjust N：OPTION 面板里连调 N 步音量（±10，门禁用） */
    int menu_pick; /* --menu-pick N：菜单打开后自动激活第 N 项（截图/门禁用） */
    int menu_at; /* --menu-at N：第 N 毫秒自动打开游戏内菜单（截图/门禁用） */
    int load_slot; /* --load-slot N：启动就进"部分恢复"视图（截图/门禁用） */
    int preview_slot; /* --preview-slot N：存档列表里预选第 N 号槽并显示预览 */
    int opening;    /* --opening：启动先跑开场序列（logo 淡入→白场→标题） */
    /* --title-anim：标题进场动画（logo alpha 0..255 / 缩放 10000..1000，
     * 参数来自脚本；--opening 时自动打开）。默认关 → 冻结基线不变。 */
    int title_anim;
    /* --title-bg-anim：标题背景的脚本动画（目前只实现星空：
     * MotionMoveS2 2000→1000 per-mille / 40000 ms，左上锚、无旋转）。
     * 默认关 → 冻结基线不变；--title-anim 不会自动打开它。 */
    int title_bg_anim;
    double shot_at_ms; /* --shot-at N：跑到 N 毫秒时存一张截图（0 = 不存） */
    /* --shot-every MS --shot-prefix P：一次运行里每隔 MS 毫秒抓一张
     * P-NNNN.bmp（做动画 demo/GIF 用；不是像素基线的一部分）。 */
    double shot_every_ms;
    int shot_div; /* --shot-div N：抓帧时 box 降采样 N 倍（1 = 原尺寸） */
    wchar_t shot_prefix[PATH_CAP];
    int title_no_bg; /* --title-no-bg：标题菜单不铺背景（几何测量用） */
    int title_hover_seed; /* --title-hover N：启动就把第 N 项设为 hover（截图/门禁用，-1 = 不动） */
    int title_no_hover; /* --title-no-hover：完全不按鼠标位置算 hover（截图门禁用） */
    int load_restart; /* --load-restart：Load(slot) 时读槽文件 + 复位场景 + 重启脚本
                       * （保留 globals/flags；槽尾快照段还没逆清，暂不应用） */
    int debug_save_request;
    int debug_load_request;
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
/* --startup-trace：在日志打开之前就能落盘的启动追踪                    */
/*                                                                     */
/* 为什么需要它：日志是在解析完参数之后才打开的，如果程序在"进程起来但还  */
/* 没打开日志"之间卡住（COM/DPI/驱动钩子/窗口创建等），日志里什么都看不  */
/* 到。这个开关把启动分成若干步，每步写一行 "耗时秒 步骤名" 并 fflush，   */
/* 卡在哪一步一眼就能看出来。路径按 ASCII 处理（只用于诊断）。           */
/* ------------------------------------------------------------------ */
static FILE *g_trace;
static double g_trace_t0;

static void trace_mark(const char *what)
{
    if (g_trace == NULL) {
        return;
    }
    fprintf(g_trace, "%9.3f  %s\n", now_sec() - g_trace_t0, what);
    fflush(g_trace);
}

static void trace_init(void)
{
    const wchar_t *cl = GetCommandLineW();
    const wchar_t *key = L"--startup-trace";
    const wchar_t *p;
    char path[PATH_CAP];
    int i = 0;

    if (cl == NULL) {
        return;
    }
    p = wcsstr(cl, key);
    if (p == NULL) {
        return;
    }
    p += wcslen(key);
    while (*p == L' ' || *p == L'\t') {
        p++;
    }
    if (*p == L'"') {
        p++;
    }
    while (*p != L'\0' && *p != L'"' && i < (int)sizeof(path) - 1) {
        path[i++] = (char)(unsigned char)*p++;
    }
    path[i] = '\0';
    if (i == 0) {
        return;
    }
    g_trace = fopen(path, "w");
    if (g_trace == NULL) {
        return;
    }
    g_trace_t0 = now_sec();
    trace_mark("entry (WinMain start)");
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
           L"  --stall-after N     连续 N 步没有 syscall 就判定脚本自旋并停下"
           L"（默认 2000000，0 = 关）\n"
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
            L"  --no-threads        关闭协程槽 0..31（A/B 用；默认开启，本作脚本行为中立）\n"
            L"  --unknown-all       退出总结打印全部未实现命令（默认只打 top 12）\n"
            L"  --typewriter        逐字显示文本（按 TextSpeed，默认关；关时像素基准不变）\n"
           L"  --text-prims        文本窗口画在 PrimSetText(86) 绑定的图元位置上（默认关）\n"
           L"  --title-menu        先显示引擎侧标题菜单（New Game/Continue/Album/Option/Exit），\n"
           L"                      选 New Game 才开始跑脚本（默认关；几何按原版实测）\n"
            L"  --tex-cache-mb N    texture cache cap in MiB（默认 256；0 = 不限制，按 LRU 淘汰未被引用的图）\n"
           L"  --title-bg-anim     标题背景按脚本做缩放动画（星空 2000->1000/40 s；默认关）\n"           L"  --load-restart      Load(slot) 时读槽文件 + 复位场景 + 重启脚本（保留 globals/flags）\n"
           L"  --shot-every MS     运行中每 MS 毫秒抓一张（配 --shot-prefix；做动画 demo）\n"
           L"  --shot-prefix P     --shot-every 的文件名前缀（写 P-0000.bmp ..）\n"           L"  --shot-div N       抓帧时 box 降采样 N 倍（1..8，默认 1）\n"
           L"  --log FILE          日志文件（默认 <exe目录>\\fvp-win32.log）\n"
           L"  --startup-trace FILE 把启动各阶段写进 FILE（日志打开之前也能落盘，\n"
           L"                      卡启动时用它定位）\n"
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
    o->stall_after = 2000000;
    o->text_pace_mode = TEXT_PACE_AUTO;
    o->run_seconds = 0.0;
    o->save_preview_every = 0.0;
    o->debug_save_request = -1;
    o->title_no_bg = 0;
    o->opening = 0;
    o->title_anim = -1; /* -1 = 未指定，见 main 里的 --opening 联动 */
    o->title_bg_anim = 0;
    o->preview_slot = -1;
    o->load_slot = -1;
    o->menu_at = -1;
    o->menu_pick = -1;
    o->menu_adjust = 0;
    o->pick_seed = -1;
    o->title_fade_ms = 800;
    o->title_font[0] = L'\0';
    o->no_input = 0;
    o->glyph_original = 0;
    o->shot_at_ms = 0.0;
    o->shot_every_ms = 0.0;
    o->shot_div = 1;
    o->shot_prefix[0] = L'\0';
    o->title_hover_seed = -1;
    o->title_no_hover = 0;
    o->load_restart = 0;
    o->debug_load_request = -1;
    o->motion = 0;
    o->timeline = 0;
    o->threads = 1;
    o->tex_cache_mb = 256;
    o->typewriter = 0;
    o->text_prims = 0;
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
        } else if (wcscmp(a, L"--stall-after") == 0) {
            NEED_VALUE();
            if (!parse_int_w(v, &o->stall_after) || o->stall_after < 0) {
                bad_option("--stall-after", v);
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
        } else if (wcscmp(a, L"--pick") == 0 && i + 1 < argc) {
            o->pick_seed = _wtoi(argv[++i]);
        } else if (wcscmp(a, L"--title-fade") == 0 && i + 1 < argc) {
            o->title_fade_ms = _wtoi(argv[++i]);
        } else if (wcscmp(a, L"--title-font") == 0 &&
                   i + 1 < argc) {
            wcsncpy(o->title_font, argv[++i], PATH_CAP - 1);
        } else if (wcscmp(a, L"--menu-adjust") == 0 && i + 1 < argc) {
            o->menu_adjust = _wtoi(argv[++i]);
        } else if (wcscmp(a, L"--menu-pick") == 0 && i + 1 < argc) {
            o->menu_pick = _wtoi(argv[++i]);
        } else if (wcscmp(a, L"--menu-at") == 0 && i + 1 < argc) {
            o->menu_at = _wtoi(argv[++i]);
        } else if (wcscmp(a, L"--load-slot") == 0 && i + 1 < argc) {
            o->load_slot = _wtoi(argv[++i]);
        } else if (wcscmp(a, L"--no-input") == 0) {
            o->no_input = 1;
        } else if (wcscmp(a, L"--glyph-original") == 0) {
            o->glyph_original = 1;
        } else if (wcscmp(a, L"--glyph-face") == 0 && i + 1 < argc) {
            /* 原版族名是数据驱动的（exe 内置注册表 4 项，见 §8.3）；
             * 这里给门禁/对照实验换族名用。 */
            char face_utf8[128];

            ++i;
            if (wide_to_ansi(argv[i], face_utf8, sizeof face_utf8)) {
                win32_gdi_text_set_default_face(face_utf8);
            }
        } else if (wcscmp(a, L"--no-input") == 0) {
            o->no_input = 1;
        } else if (wcscmp(a, L"--preview-slot") == 0 &&
                   i + 1 < argc) {
            o->preview_slot = _wtoi(argv[++i]);
        } else if (wcscmp(a, L"--opening") == 0) {
            o->opening = 1;
        } else if (wcscmp(a, L"--title-anim") == 0) {
            o->title_anim = 1;
        } else if (wcscmp(a, L"--title-bg-anim") == 0) {
            o->title_bg_anim = 1;
        } else if (wcscmp(a, L"--shot-at") == 0 && i + 1 < argc) {
            o->shot_at_ms = wcstod(argv[++i], NULL);
        } else if (wcscmp(a, L"--shot-every") == 0 && i + 1 < argc) {
            o->shot_every_ms = wcstod(argv[++i], NULL);
        } else if (wcscmp(a, L"--shot-div") == 0 && i + 1 < argc) {
            int dv = _wtoi(argv[++i]);

            o->shot_div = dv < 1 ? 1 : (dv > 8 ? 8 : dv);
        } else if (wcscmp(a, L"--shot-prefix") == 0 && i + 1 < argc) {
            wcsncpy(o->shot_prefix, argv[++i], PATH_CAP - 1);
        } else if (wcscmp(a, L"--title-no-bg") == 0) {
            o->title_no_bg = 1;
        } else if (wcscmp(a, L"--title-no-hover") == 0) {
            o->title_no_hover = 1;
        } else if (wcscmp(a, L"--title-hover") == 0 &&
                   i + 1 < argc) {
            o->title_hover_seed = _wtoi(argv[++i]);
        } else if (wcscmp(a, L"--debug-save-request") == 0 &&
                   i + 1 < argc) {
            o->debug_save_request = _wtoi(argv[++i]);
        } else if (wcscmp(a, L"--load-restart") == 0) {
            o->load_restart = 1;
        } else if (wcscmp(a, L"--debug-load-request") == 0 &&
                   i + 1 < argc) {
            o->debug_load_request = _wtoi(argv[++i]);
            if (o->save_preview_every < 0.0) {
                o->save_preview_every = 0.0;
            }
        } else if (wcscmp(a, L"--motion") == 0) {
            o->motion = 1;
        } else if (wcscmp(a, L"--timeline") == 0) {
            o->timeline = 1;
        } else if (wcscmp(a, L"--no-threads") == 0) {
            o->threads = 0;
        } else if (wcscmp(a, L"--unknown-all") == 0) {
            o->unknown_all = 1;
        } else if (wcscmp(a, L"--typewriter") == 0) {
            o->typewriter = 1;
        } else if (wcscmp(a, L"--text-prims") == 0) {
            o->text_prims = 1;
        } else if (wcscmp(a, L"--title-menu") == 0) {
            o->title_menu = 1;
        } else if (wcscmp(a, L"--title-save-list") == 0) {
            o->title_menu = 1;
            o->title_save_list = 1;
        } else if (wcscmp(a, L"--tex-cache-mb") == 0) {
            NEED_VALUE();
            if (!parse_int_w(v, &o->tex_cache_mb) || o->tex_cache_mb < 0 ||
                o->tex_cache_mb > 8192) {
                bad_option("--tex-cache-mb", v);
            }
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

/* 抓帧降采样：div×div 的 box 平均后写 BMP（--shot-div）。 */
static int write_bmp32_scaled(const wchar_t *path, const uint32_t *rgba, int w,
                              int h, int div)
{
    int dw, dh, x, y, dx, dy;
    uint32_t *tmp;
    int ok;

    if (div <= 1) {
        return write_bmp32(path, rgba, w, h);
    }
    dw = w / div;
    dh = h / div;
    if (dw <= 0 || dh <= 0) {
        return 0;
    }
    tmp = (uint32_t *)malloc((size_t)dw * (size_t)dh * sizeof(uint32_t));
    if (tmp == NULL) {
        return 0;
    }
    for (y = 0; y < dh; ++y) {
        for (x = 0; x < dw; ++x) {
            unsigned sr = 0, sg = 0, sb = 0;

            for (dy = 0; dy < div; ++dy) {
                const uint32_t *row = rgba + (size_t)(y * div + dy) * (size_t)w;
                for (dx = 0; dx < div; ++dx) {
                    uint32_t d = row[x * div + dx];

                    sr += (d >> 16) & 0xFFu;
                    sg += (d >> 8) & 0xFFu;
                    sb += d & 0xFFu;
                }
            }
            sr /= (unsigned)(div * div);
            sg /= (unsigned)(div * div);
            sb /= (unsigned)(div * div);
            tmp[(size_t)y * (size_t)dw + (size_t)x] =
                0xFF000000u | (sr << 16) | (sg << 8) | sb;
        }
    }
    ok = write_bmp32(path, tmp, dw, dh);
    free(tmp);
    return ok;
}

/* ------------------------------------------------------------------ */
/* Window sizing / title                                               */
/* ------------------------------------------------------------------ */

/* ------------------------------------------------------------------ */
/* 存档预览：用真实槽当模板，只换「时间戳 + 缩略图」写出一份新槽        */
/* ------------------------------------------------------------------ */

/* 把整个文件读进内存（读档助手用；不依赖日志/其它初始化）。 */
static unsigned char *shell_read_file(const wchar_t *path, size_t *len_out)
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

/* --load-restart：读槽文件 → 复位场景（prims/motion/text）→ 重建 VM 并从入口重启，
 * 保留调用前已灌入的全局变量与 flag。对应 exe 的读档复位序列：
 *   0x438D20（场景/图元树复位） + 0x441350（脚本引擎复位）+ 线程 +0xFD9 重启。
 * 槽尾"引擎快照"目前只回灌**台词历史环**（第 20 段，格式与读档路径 0x43F25A
 * 已从静态分析冻结，见 ref/exe-history.json / src/savehist.c）；其余段的字段
 * 语义还没逆清，仍不应用。 */
static int shell_load_restart(FvpVM *vm, FvpHcb *hcb, Gfx *gfx, Snd *snd,
                              const Options *opts, int slot,
                              FvpEncoding save_enc)
{
    wchar_t path[PATH_CAP];
    char u8[PATH_CAP * 3];
    unsigned char *buf;
    size_t len = 0;
    FvpSaveSlot sd;
    FvpSaveHist hist;
    int slot_ok = 0;
    int hist_ok = 0;
    FvpValue *gcopy = NULL;
    unsigned char *gcset = NULL;
    unsigned char *gflags = NULL;
    int i;

    _snwprintf(path, PATH_CAP, L"%ls\\save\\s%03d.bin", opts->game_dir, slot);
    buf = shell_read_file(path, &len);
    memset(&sd, 0, sizeof sd);
    if (buf != NULL) {
        slot_ok = fvp_save_slot_load(&sd, buf, len, 0, 0);
        fvp_logf("load: slot %d file %s (%lu bytes) -> %s, chapter=%u B, "
                 "line=%u B, thumb=%dx%d, tail=%.1f KB",
                 slot, w2u(path, u8, sizeof(u8)), (unsigned long)len,
                 slot_ok ? "parsed" : "bad slot header",
                 sd.len[0], sd.len[2], sd.thumb_w, sd.thumb_h,
                 (double)sd.tail_len / 1024.0);
        if (slot_ok) {
            /* 槽尾第 20 段 = 台词历史环（u32 count/pos + 100 条记录）。
             * 段偏移还没算出来，所以按结构判据定位（src/savehist.h 顶部）。 */
            fvp_savehist_init(&hist);
            hist_ok = fvp_savehist_locate(&hist, sd.tail, sd.tail_len,
                                          sd.str[2], sd.len[2], save_enc);
            fvp_logf("load: history ring scan -> %s%s (tail=%.1f KB)",
                     hist_ok ? "found" : "not found",
                     hist_ok ? "" : " (history snapshot skipped)",
                     (double)sd.tail_len / 1024.0);
            fvp_save_slot_free(&sd);
        }
        free(buf);
        memset(&sd, 0, sizeof sd);
    } else {
        fvp_logf("load: slot %d file %s cannot be read (restarting anyway)",
                 slot, w2u(path, u8, sizeof(u8)));
    }

    /* 1. 场景复位（exe 0x438D20 + gfx 复位）：prim/motion/文本窗口全清 */
    for (i = 0; i < GFX_PRIM_MAX; ++i) {
        (void)gfx_prim_reset(gfx, i);
    }
    gfx_motion_clear(gfx);
    for (i = 0; i < GFX_TEXT_MAX; ++i) {
        gfx_text_clear(gfx, i);
    }
    (void)gfx_fill_canvas(gfx, 0, 0, 0, 255);

    /* 2. 保留 globals / globals_set / flags，重建 VM（脚本引擎复位） */
    gcopy = (FvpValue *)malloc(sizeof(FvpValue) * (size_t)FVP_GLOBAL_COUNT);
    gcset = (unsigned char *)malloc((size_t)FVP_GLOBAL_COUNT);
    gflags = (unsigned char *)malloc((size_t)FVP_FLAG_BYTES);
    if (gcopy != NULL) {
        memcpy(gcopy, vm->globals, sizeof(FvpValue) * (size_t)FVP_GLOBAL_COUNT);
    }
    if (gcset != NULL) {
        memcpy(gcset, vm->globals_set, (size_t)FVP_GLOBAL_COUNT);
    }
    if (gflags != NULL) {
        memcpy(gflags, vm->flags, (size_t)FVP_FLAG_BYTES);
    }
    fvp_vm_free(vm);
    memset(vm, 0, sizeof *vm);
    fvp_vm_init(vm, hcb, 1);
    fvp_vm_set_stall_limit(vm, (uint64_t)opts->stall_after);
    vm->timeline_on = opts->timeline;
    vm->threads_on = opts->threads;
    if (gcopy != NULL) {
        memcpy(vm->globals, gcopy, sizeof(FvpValue) * (size_t)FVP_GLOBAL_COUNT);
    }
    if (gcset != NULL) {
        memcpy(vm->globals_set, gcset, (size_t)FVP_GLOBAL_COUNT);
    }
    if (gflags != NULL) {
        memcpy(vm->flags, gflags, (size_t)FVP_FLAG_BYTES);
    }
    free(gcopy);
    free(gcset);
    free(gflags);
    if (gfx != NULL) {
        fvp_vm_attach_gfx(vm, gfx);
    }
    if (snd != NULL) {
        fvp_vm_attach_snd(vm, snd);
    }
    if (hist_ok && fvp_savehist_apply(&hist, &vm->history)) {
        const FvpSaveHistRec *r = &hist.rec[hist.pos];
        const char *newest = r->text1 != NULL ? r->text1
                                              : (r->text0 != NULL ? r->text0
                                                                  : "");

        fvp_logf("history: restored count=%u pos=%u filled=%d "
                 "scan=tail+%#x newest=\"%s\"",
                 hist.count, hist.pos, fvp_history_filled(&vm->history),
                 (unsigned)hist.offset, newest);
    } else if (hist_ok) {
        fvp_logf("history: parsed but apply failed (keeping the fresh container)");
    }
    fvp_savehist_free(&hist);
    fvp_vm_start(vm);
    fvp_logf("load: scene reset (prims/motion/text) + script engine restarted "
             "at entry pc=0x%x (globals/flags kept)",
             (unsigned)hcb->main_start);
    return slot_ok;
}

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
/* 引擎侧标题菜单（--title-menu）                                       */
/* ------------------------------------------------------------------ */

/* 量宽回调：走 gfx 的字体，保证"量"和"画"用同一套字形。 */
/* 贴图模式开关：贴图已经绑进图元后，覆盖层就不要再画一遍文字。 */
static int title_textures_on;
/* 标题画面背景是否装上（整屏图，画在菜单之前） */
static int title_bg_on;
/* 标题菜单专用字库（--title-font）：文字模式用它画，脚本正文仍用位图图集 */
static const GfxFont *title_ui_font;
/* 游戏内菜单的 OPTION 面板（只读：音量/文本速度等当前值） */
static int g_menu_option_panel;
static const FvpSndBridge *g_gmenu_bridge;
static int title_logo_on;
static int title_ovl_loaded;
static int title_copy_on;
/* 标题画面进场：**逐元素错开**（脚本 hcb 0x5933E..0x5961A，见
 * src/titleanim.h / src/titleenter.h）：
 *
 *   t=2000  logo(265)      alpha 0→255 / 2000 ms
 *   t=2400  菜单 1501       900 ms + 纵向 +23 px → 0
 *   t=2440..2560 菜单 1508/1515/1522/1529  820 ms + 纵向 +23 px → 0
 *   t=2560  版权行(278)    820 ms + 横向 +30 px → 0
 *   t=4000  整段结束
 *
 * 默认（--title-anim 关闭）alpha 全 255、偏移 0 ⇒ 绘制与静态画面**逐字节相同**；
 * --title-anim / --opening 打开后按上面这条脚本时间轴走。
 * 背景与叠加层不参与（脚本给它们的是 25 s 缩放/旋转，尚未实现）。 */
static int title_logo_alpha = 255;
static int title_copy_alpha = 255;
static int title_copy_dx;
static int title_menu_alpha[FVP_TITLE_ITEMS] = {255, 255, 255, 255, 255};
static int title_menu_dy[FVP_TITLE_ITEMS];
/* 标题背景动画（--title-bg-anim）：参数见 src/titlebg.h；elapsed 用墙钟。 */
static int title_bg_anim_on;
static FvpTitleBgGame title_bg_game = FVP_TITLEBG_GAME_IROSEKA;
static double title_bg_t0;
static double title_bg_now;

static FvpTitleEnter title_enter;
static FvpTitleEnterGame title_enter_game = FVP_TITLEENTER_GAME_IROSEKA;
static double title_enter_t0;
static int title_enter_active;
static int title_enter_started;

/* 把逐元素状态复位成"静态画面"（全部 255 / 偏移 0）。 */
static void title_enter_reset(void)
{
    int i;

    title_logo_alpha = 255;
    title_copy_alpha = 255;
    title_copy_dx = 0;
    for (i = 0; i < FVP_TITLE_ITEMS; ++i) {
        title_menu_alpha[i] = 255;
        title_menu_dy[i] = 0;
    }
}

/* 用**墙钟时间**推进（而不是累加 frame_dt）：标题第一次绘制要现场加载一整套
 * 贴图，那一帧的 dt 可能是几百毫秒，累加会把整段进场在一帧里跳完。 */
static void title_enter_start(double now_s, FvpTitleEnterGame game)
{
    title_enter_game = game;
    fvp_titleenter_init(&title_enter, game);
    title_enter_t0 = now_s;
    title_enter_active = 1;
    title_enter_reset();
    title_logo_alpha = 0;
    title_copy_alpha = 0;
    {
        int i;

        for (i = 0; i < FVP_TITLE_ITEMS; ++i) {
            title_menu_alpha[i] = 0;
        }
    }
}

static void title_enter_advance(double now_s)
{
    int elapsed_ms;
    int i;

    if (!title_enter_active) {
        return;
    }
    elapsed_ms = (int)((now_s - title_enter_t0) * 1000.0 + 0.5);
    fvp_titleenter_set_elapsed(&title_enter, elapsed_ms);
    title_logo_alpha = fvp_titleenter_alpha(&title_enter,
                                            FVP_TITLEENTER_PART_LOGO);
    title_copy_alpha = fvp_titleenter_alpha(&title_enter,
                                            FVP_TITLEENTER_PART_COPY);
    fvp_titleenter_offset(&title_enter, FVP_TITLEENTER_PART_COPY,
                          &title_copy_dx, NULL);
    for (i = 0; i < FVP_TITLE_ITEMS; ++i) {
        title_menu_alpha[i] = fvp_titleenter_alpha(
            &title_enter, FVP_TITLEENTER_PART_MENU0 + i);
        fvp_titleenter_offset(&title_enter, FVP_TITLEENTER_PART_MENU0 + i,
                              NULL, &title_menu_dy[i]);
    }
    if (fvp_titleenter_done(&title_enter)) {
        title_enter_reset();
        title_enter_active = 0;
    }
}

/* --title-no-bg：只画菜单、不铺背景（几何测量/门禁用：背景的亮部会污染
 * 墨迹检测）。 */
static int title_bg_disabled;
/* 背景占用的图形槽 */
#define TITLE_BG_SLOT 0x0FF0
#define TITLE_OVL_SLOT 0x0FF3 /* 背景叠加层 0xFF3..0xFF6 */
#define TITLE_LOGO_SLOT 0x0FF1
#define TITLE_COPY_SLOT 0x0FF2

/* 贴图占用的图形槽（脚本还没开始跑，用 0xF00 之后的高位，避免和脚本槽冲突） */
#define TITLE_SLOT_BASE 0x0F00
/* hover/选中态贴图（五彩斑斓的 title_<item>1）装进 0xF10+i：2026-11-25 由静态
 * 像素比对定出映射（ref/title-variants.json + docs/title-menu.md §12）。 */
#define TITLE_HOVER_SLOT_BASE (TITLE_SLOT_BASE + 16)
/* 开场序列直接用**脚本的槽位号** 250/252/254/255/256/257（见 src/openseq.h 的层表），
 * 不再借私有槽：这样槽位号本身就是脚本事实，门禁能直接比对。脚本此时还没跑，
 * 所以不会和 VM 抢槽（New Game 之后脚本会重新 GraphLoad 这些槽，正好覆盖）。 */
/* 存档列表的预览图（选中槽里记录的背景图） */
#define SAVE_PREVIEW_SLOT 0x0FE2

static int title_measure(void *ctx, const char *utf8, int px, int *width)
{
#ifdef FVP_STUB_GFX
    (void)ctx;
    (void)utf8;
    (void)px;
    (void)width;
    return 0;
#else
    Gfx *g = (Gfx *)ctx;
    int w;

    if (g == NULL) {
        return 0;
    }
    w = gfx_text_width(g, utf8, px);
    if (w <= 0) {
        return 0;
    }
    *width = w;
    return 1;
#endif
}

/* 把菜单画到当前画布上。必须**在 gfx_render() 之后**调用——render 会按图元/
 * 文本窗口重建画布，先画会被抹掉（见 docs/title-menu.md §2.1）。
 *
 * 有 profile 时按**该作品实测的墨迹矩形**画（右对齐 or 居中、行距/字号都按实测），
 * 没有 profile 时退回通用的右对齐布局。 */
/* 游戏内菜单（引擎侧 UI）：压暗背景 + 居中列出各项 + hover 高亮。
 * 文字复用标题菜单的字库（--title-font），没有就用默认位图字体。 */
static void draw_game_menu(Gfx *gfx, const FvpGameMenu *m)
{
    int i;
    int px = 44;

    if (gfx == NULL || m == NULL || !m->active) {
        return;
    }
    (void)gfx_fill_canvas(gfx, 0, 0, 0, 140);
    for (i = 0; i < m->count; ++i) {
        int y = fvp_gamemenu_item_y(m, i);
        int r = 235, g = 235, b = 240;
        int w;

        if (!m->items[i].enabled) {
            r = 120;
            g = 120;
            b = 130;
        } else if (i == m->hover) {
            r = 255;
            g = 232;
            b = 140;
        }
        w = (title_ui_font != NULL)
                ? gfx_text_width_font(gfx, title_ui_font, m->items[i].label, px)
                : gfx_text_width(gfx, m->items[i].label, px);
        if (title_ui_font != NULL) {
            (void)gfx_draw_text_line_font(gfx, title_ui_font, m->items[i].label,
                                          m->x + (m->width - w) / 2, y, px, r,
                                          g, b, 255);
        } else {
            (void)gfx_draw_text_line(gfx, m->items[i].label,
                                     m->x + (m->width - w) / 2, y, px, r, g,
                                     b, 255);
        }
    }
    (void)gfx_draw_text_line(gfx, "ESC: close", 56, 40, 32, 220, 220, 230, 255);
    if (g_menu_option_panel) {
        int y = 760;
        int px = 32;

        (void)gfx_draw_text_line(gfx, "OPTION (read-only)", 120, y, px, 255,
                                 232, 140, 255);
        {
            char vol_line[64];
            int vol = (g_gmenu_bridge != NULL)
                          ? fvp_snd_bridge_master(g_gmenu_bridge)
                          : 100;

            _snprintf(vol_line, sizeof(vol_line) - 1,
                      "master volume : %d%%  (Up/Down adjusts)", vol);
            (void)gfx_draw_text_line(gfx, vol_line, 120, y + 48, px, 220, 220,
                                     230, 255);
        }
        (void)gfx_draw_text_line(gfx, "text speed    : script-controlled",
                                 120, y + 96, px, 220, 220, 230, 255);
        (void)gfx_draw_text_line(gfx, "skip / auto   : not implemented",
                                 120, y + 144, px, 200, 200, 210, 255);
    }
}

/* 背景层：整屏大图按 (bg_src_x, bg_src_y) 裁剪到 1920x1080；
 * --title-bg-anim 且该作背景动画已实现时（目前只有星空），按脚本的
 * MotionMoveS2 缩放：只画"屏幕上可见的源区域"，缩放到整屏。 */
static void title_draw_bg(Gfx *gfx, const FvpTitleProfile *prof)
{
    int bsw = 0;
    int bsh = 0;

    if (!title_bg_on) {
        return;
    }
    if (gfx != NULL && title_bg_anim_on) {
        int elapsed = (int)((title_bg_now - title_bg_t0) * 1000.0 + 0.5);

        if (fvp_titlebg_uses_layers(title_bg_game) && prof != NULL) {
            /* 五彩斑斓：四层各自 path A/B（脚本 MotionMoveS2 + MotionMoveR） */
            GfxAffineLayer layers[FVP_TITLEBG_MAX_LAYERS];
            int n = fvp_titlebg_layer_count(title_bg_game);
            int li;
            int cnt = 0;

            for (li = 0; li < n; ++li) {
                const FvpTitleBgLayerDef *def =
                    fvp_titlebg_layer_def(title_bg_game, li);
                FvpTitleBgLayerState lst;
                GfxAffineLayer *L;

                if (def == NULL ||
                    !fvp_titlebg_layer_state(title_bg_game, li, elapsed,
                                             &lst)) {
                    continue;
                }
                L = &layers[cnt++];
                memset(L, 0, sizeof *L);
                L->slot = (def->slot_tag == 0)
                              ? TITLE_BG_SLOT
                              : TITLE_OVL_SLOT + def->slot_tag - 1;
                L->img_x = def->img_x;
                L->img_y = def->img_y;
                L->op_x = def->op_x;
                L->op_y = def->op_y;
                L->anchor_center = def->anchor_center;
                if (def->anchor_center) {
                    L->anchor_x = prof->client_w / 2;
                    L->anchor_y = prof->client_h / 2;
                } else {
                    L->anchor_x = def->op_x;
                    L->anchor_y = def->op_y;
                }
                L->scale_x1000 = lst.scale_x1000;
                L->scale_y1000 = lst.scale_y1000;
                L->angle_units = lst.angle_units;
                L->alpha = 255;
            }
            if (cnt > 0 &&
                gfx_draw_affine_layers(gfx, layers, cnt, 8)) {
                return;
            }
        } else {
            FvpTitleBgState st;

            fvp_titlebg_state(title_bg_game, elapsed, &st);
            if (st.animated && prof != NULL && st.scale_x1000 > 0 &&
                st.scale_y1000 > 0) {
                (void)gfx_draw_image_zoomed(gfx, TITLE_BG_SLOT,
                                            st.scale_x1000, st.scale_y1000,
                                            prof->client_w, prof->client_h);
                return;
            }
        }
    }
    if (prof != NULL && gfx_image_size(gfx, TITLE_BG_SLOT, &bsw, &bsh) &&
        (bsw != prof->client_w || bsh != prof->client_h)) {
        (void)gfx_draw_image_cropped(gfx, TITLE_BG_SLOT, 0, 0, prof->bg_src_x,
                                     prof->bg_src_y, prof->client_w,
                                     prof->client_h);
    } else {
        (void)gfx_draw_image_at(gfx, TITLE_BG_SLOT, 0, 0);
    }
}

/* 背景叠加层：素材头里的坐标是"未裁剪大图"坐标系，要减掉背景裁剪起点 */
static void title_draw_overlays(Gfx *gfx, const FvpTitleProfile *prof)
{
    int li;

    if (title_ovl_loaded <= 0 || prof == NULL) {
        return;
    }
    for (li = 0; li < prof->bg_overlay_count && li < 4; ++li) {
        int ox = 0;
        int oy = 0;

        if (prof->bg_overlay[li][0] == '\0') {
            continue;
        }
        (void)gfx_image_offset(gfx, TITLE_OVL_SLOT + li, &ox, &oy);
        (void)gfx_draw_image_at(gfx, TITLE_OVL_SLOT + li, ox - prof->bg_src_x,
                                oy - prof->bg_src_y);
    }
}

static void title_draw_overlay(Gfx *gfx, const FvpTitleMenu *m,
                               const FvpTitleProfile *prof, int hover)
{
#ifdef FVP_STUB_GFX
    (void)gfx;
    (void)m;
    (void)prof;
    (void)hover;
#else
    int i;

    if (gfx == NULL || m == NULL) {
        return;
    }
    /* 背景 / logo / 版权行与"菜单用贴图还是文字"无关：先铺背景和 logo，
     * 菜单项画完后再压版权行。位置都取自素材头里的 x/y。 */
    {
        int bg_on_top = (prof != NULL) ? prof->bg_on_top : 0;

        /* 叠放顺序由脚本的 PrimSetZ 决定（ref/title-scene.json 的 layer_z）。
         * 五彩斑斓 A=1000 在最上 → 先画叠加层 D→C→B，再画背景 A；
         * 星空背景在下 → 先背景后叠加层。 */
        if (!bg_on_top) {
            title_draw_bg(gfx, prof);
            title_draw_overlays(gfx, prof);
        } else {
            title_draw_overlays(gfx, prof);
            title_draw_bg(gfx, prof);
        }
    }
    if (title_logo_on && title_logo_alpha > 0) {
        int lx = 0;
        int ly = 0;

        (void)gfx_image_offset(gfx, TITLE_LOGO_SLOT, &lx, &ly);
        if (title_logo_alpha >= 255) {
            (void)gfx_draw_image_at(gfx, TITLE_LOGO_SLOT, lx, ly);
        } else {
            (void)gfx_draw_image_alpha(gfx, TITLE_LOGO_SLOT, lx, ly,
                                       title_logo_alpha);
        }
    }
    if (title_copy_on && title_copy_alpha > 0) {
        int cx = 0;
        int cy = 0;

        (void)gfx_image_offset(gfx, TITLE_COPY_SLOT, &cx, &cy);
        cx += title_copy_dx;
        if (title_copy_alpha >= 255) {
            (void)gfx_draw_image_at(gfx, TITLE_COPY_SLOT, cx, cy);
        } else {
            (void)gfx_draw_image_alpha(gfx, TITLE_COPY_SLOT, cx, cy,
                                       title_copy_alpha);
        }
    }
    if (title_textures_on) {
        for (i = 0; i < FVP_TITLE_ITEMS; ++i) {
            int ix = 0;
            int iy = 0;
            int ia = title_menu_alpha[i];
            int idy = title_menu_dy[i];

            if (prof == NULL || !prof->items[i].asset[0]) {
                continue;
            }
            if (ia <= 0) {
                continue;
            }
            /* 素材头里自带摆放坐标（原版就是这么放的）→ 优先用它；
             * 没有的话退回 profile 里按墨迹框反推的位置。 */
            if (!gfx_image_offset(gfx, TITLE_SLOT_BASE + i, &ix, &iy) ||
                (ix == 0 && iy == 0)) {
                if (!fvp_title_profile_image_pos(prof, i, &ix, &iy)) {
                    continue;
                }
            }
            iy += idy;
            if (i == hover) {
                int gw = 0;
                int gh = 0;

                /* 发光层：整行光带，横向铺满、竖向对准这一项的墨迹中心 */
                if (gfx_image_size(gfx, TITLE_SLOT_BASE + 8 + i, &gw,
                                   &gh) &&
                    gh > 0) {
                    int gy = prof->items[i].ink_y0 -
                                 (gh - (prof->items[i].ink_y1 -
                                        prof->items[i].ink_y0)) /
                                     2;

                    if (ia >= 255) {
                        (void)gfx_draw_image_at(gfx, TITLE_SLOT_BASE + 8 + i,
                                                (prof->client_w - gw) / 2,
                                                gy);
                    } else {
                        (void)gfx_draw_image_alpha(
                            gfx, TITLE_SLOT_BASE + 8 + i,
                            (prof->client_w - gw) / 2, gy, ia);
                    }
                }
            }
            /* hover 时换画高亮态贴图（素材头自带坐标，和常态同一左上角）。 */
            {
                int hv_w = 0;
                int hv_h = 0;

                if (i == hover &&
                    gfx_image_size(gfx, TITLE_HOVER_SLOT_BASE + i, &hv_w,
                                   &hv_h) &&
                    hv_w > 0) {
                    int hx = ix;
                    int hy = iy;

                    (void)gfx_image_offset(gfx, TITLE_HOVER_SLOT_BASE + i, &hx,
                                           &hy);
                    hy += idy;
                    if (ia >= 255) {
                        (void)gfx_draw_image_at(gfx, TITLE_HOVER_SLOT_BASE + i,
                                                hx, hy);
                    } else {
                        (void)gfx_draw_image_alpha(gfx,
                                                   TITLE_HOVER_SLOT_BASE + i,
                                                   hx, hy, ia);
                    }
                } else if (ia >= 255) {
                    (void)gfx_draw_image_at(gfx, TITLE_SLOT_BASE + i, ix, iy);
                } else {
                    (void)gfx_draw_image_alpha(gfx, TITLE_SLOT_BASE + i, ix, iy,
                                               ia);
                }
            }
        }
        return;
    }
    for (i = 0; i < FVP_TITLE_ITEMS; ++i) {
        int x0 = 0, y0 = 0, x1 = 0, y1 = 0;
        int hovered = (i == hover);
        int ia = title_menu_alpha[i];
        int idy = title_menu_dy[i];
        int px;
        const char *label;
        int r = hovered ? 255 : 235;
        int gr = hovered ? 250 : 235;
        int b = hovered ? 255 : 240;

        if (prof != NULL) {
            /* 用 profile 里实测的基色；hover 用同色相提亮一点（原版的
             * hover 具体样式还没实测，这里只做"能看出选中"的近似）。 */
            int k = hovered ? 130 : 100;

            r = (prof->menu_color[0] * k) / 100;
            gr = (prof->menu_color[1] * k) / 100;
            b = (prof->menu_color[2] * k) / 100;
        }
        if (!m->enabled[i]) {
            r /= 2;
            gr /= 2;
            b /= 2; /* 不可用项压暗（例如没存档时的 Continue） */
        }
        if (r > 255) {
            r = 255;
        }
        if (gr > 255) {
            gr = 255;
        }
        if (b > 255) {
            b = 255;
        }
        if (prof != NULL) {
            if (!fvp_title_profile_ink_rect(prof, i, &x0, &y0, &x1, &y1)) {
                continue;
            }
            label = prof->items[i].label;
            /* 墨迹高 ≈ 字号 * 3/4 → 反推字号（iroseka 42→56、hoshimemo 34→45） */
            px = (y1 - y0 + 1) * 4 / 3;
            if (px < 8) {
                px = 8;
            }
        } else {
            if (!fvp_title_item_rect(m, i, title_measure, gfx, &x0, &y0, &x1,
                                     &y1)) {
                continue;
            }
            label = fvp_title_label(i);
            px = m->font_px;
        }
        {
            if (title_ui_font != NULL) {
                int fs = gfx_font_pixel_size(title_ui_font);

                if (fs > 0) {
                    px = fs;
                }
            }
            int tw = (title_ui_font != NULL)
                         ? gfx_text_width_font(gfx, title_ui_font, label, px)
                         : gfx_text_width(gfx, label, px);
            int dx = x0;

            /* 按 profile 的对齐方式落笔：右对齐贴到实测的墨迹右边界，
             * 居中则以实测墨迹中心为准（字体不同宽度必然不同，但对齐
             * 基准必须一致）。 */
            if (tw > 0 && prof != NULL) {
                if (prof->align == FVP_TITLE_ALIGN_RIGHT) {
                    dx = x1 - tw + 1;
                } else {
                    dx = (x0 + x1) / 2 - tw / 2;
                }
            }
            if (ia <= 0) {
                continue;
            }
            if (title_ui_font != NULL) {
                (void)gfx_draw_text_line_font(gfx, title_ui_font, label, dx,
                                              y0 + idy, px, r, gr, b, ia);
            } else {
                (void)gfx_draw_text_line(gfx, label, dx, y0 + idy, px, r, gr,
                                         b, ia);
            }
        }
    }
    /* 版权行压在最上面（原版在屏幕底部） */

#endif
}

static int title_textures_on;

/* 数一下 <game_dir>\save 里有几个存档槽（sNNN.bin）。
 * 标题菜单的 Continue 就靠它决定能不能点。 */
/* 扫描 <game_dir>\save\sNNN.bin，把每个槽读成一条列表记录（只读）。
 * 返回读到的条数。目录枚举在平台层做，savelist.c 只吃"一个槽文件路径"。 */

/* 进入"部分恢复"视图：只还原存档里记录的画面与台词，不恢复任何游戏状态。
 * 这是 ② Load(40) 的诚实中间态，日志明确写 partial。 */

/* 进入"部分恢复"视图：只还原存档里记录的画面与台词，不恢复任何游戏状态。
 * 这是 ② Load(40) 的诚实中间态，日志明确写 partial。 */
static void enter_load_view(int slot_index, const FvpSaveList *list,
                            int *active, int *slot_out, char *graph_out,
                            size_t graph_cap, char *line_out, size_t line_cap)
{
    const FvpSaveEntry *it;

    if (list == NULL || slot_index < 0 || slot_index >= list->count) {
        return;
    }
    it = &list->items[slot_index];
    *active = 1;
    *slot_out = it->slot;
    snprintf(graph_out, graph_cap, "%s", it->graph);
    snprintf(line_out, line_cap, "%s", it->line);
    fvp_logf("load: partial restore slot=%d graph='%s' line='%s' "
             "(visual+text only, game state NOT restored)",
             it->slot, it->graph, it->line);
}

static int title_scan_saves(FvpSaveList *list, const wchar_t *game_dir,
                            const char *game_dir_u8, FvpEncoding enc)
{
    wchar_t pattern[PATH_CAP];
    WIN32_FIND_DATAW fd;
    HANDLE h;
    int n = 0;

    fvp_savelist_init(list);
    if (game_dir == NULL || game_dir[0] == L'\0' || game_dir_u8 == NULL) {
        return 0;
    }
    if (!path_join_w(pattern, PATH_CAP, game_dir, L"save\\s*.bin")) {
        return 0;
    }
    h = FindFirstFileW(pattern, &fd);
    if (h == INVALID_HANDLE_VALUE) {
        return 0;
    }
    do {
        wchar_t wpath[PATH_CAP];
        int slot = -1;
        FILE *sf;
        long fsz;
        unsigned char *sbuf;

        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            continue;
        }
        if (fd.cFileName[0] != L's' ||
            swscanf(fd.cFileName, L"s%d.bin", &slot) != 1) {
            continue;
        }
        if (slot < 0 || slot > 999) {
            continue;
        }
        /* Windows 下 CJK 路径只能走宽字符 API，所以这里自己读字节，
         * 再交给 savelist 的 add_slot_bytes()。 */
        if (!path_join_w(wpath, PATH_CAP, game_dir,
                         L"save") ||
            wcslen(wpath) + wcslen(fd.cFileName) + 2 > PATH_CAP) {
            continue;
        }
        wcscat(wpath, L"\\");
        wcscat(wpath, fd.cFileName);
        sf = _wfopen(wpath, L"rb");
        if (sf == NULL) {
            continue;
        }
        if (fseek(sf, 0, SEEK_END) != 0 || (fsz = ftell(sf)) <= 0 ||
            fseek(sf, 0, SEEK_SET) != 0) {
            fclose(sf);
            continue;
        }
        sbuf = (unsigned char *)malloc((size_t)fsz);
        if (sbuf == NULL) {
            fclose(sf);
            continue;
        }
        if (fread(sbuf, 1, (size_t)fsz, sf) != (size_t)fsz) {
            free(sbuf);
            fclose(sf);
            continue;
        }
        fclose(sf);
        if (fvp_savelist_add_slot_bytes(list, sbuf, (size_t)fsz, slot, enc)) {
            n += 1;
        }
        free(sbuf);
        if (list->count >= FVP_SAVELIST_MAX) {
            break;
        }
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    return n;
}

static int title_count_save_slots(const wchar_t *game_dir)
{
    wchar_t pattern[PATH_CAP];
    WIN32_FIND_DATAW fd;
    HANDLE h;
    int n = 0;

    if (game_dir == NULL || game_dir[0] == L'\0') {
        return 0;
    }
    if (!path_join_w(pattern, PATH_CAP, game_dir, L"save\\s*.bin")) {
        return 0;
    }
    h = FindFirstFileW(pattern, &fd);
    if (h == INVALID_HANDLE_VALUE) {
        return 0;
    }
    do {
        const wchar_t *nm = fd.cFileName;

        /* sNNN.bin（s + 3 位数字 + .bin）；save.bin 不算槽 */
        if ((fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) == 0 &&
            nm[0] == L's' && nm[1] >= L'0' && nm[1] <= L'9' &&
            nm[2] >= L'0' && nm[2] <= L'9' && nm[3] >= L'0' && nm[3] <= L'9' &&
            wcscmp(nm + 4, L".bin") == 0) {
            n += 1;
            if (n >= 999) {
                break;
            }
        }
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    return n;
}

/* ------------------------------------------------------------------ */
/* 引擎侧标题菜单：把 profile 里的贴图装进图形槽，并绑成图元              */
/* ------------------------------------------------------------------ */


/* 返回成功装上的贴图数量。每装上一张就把图元 i+1 绑成 type 4 sprite：
 * graph = 槽、x/y = profile 给的贴图位置、draw = 1。 */
static int title_load_textures(Gfx *gfx, const FvpTitleProfile *prof)
{
#ifdef FVP_STUB_GFX
    (void)gfx;
    (void)prof;
    return 0;
#else
    int i;
    int loaded = 0;
    int glow_loaded = 0;

    if (gfx == NULL || prof == NULL) {
        return 0;
    }
    for (i = 0; i < FVP_TITLE_ITEMS; ++i) {
        const FvpTitleItemDef *d = &prof->items[i];
        int slot = TITLE_SLOT_BASE + i;
        int x = 0;
        int y = 0;
        int prim = i + 1;

        if (d->asset[0] == '\0') {
            continue;
        }
        if (gfx_graph_load(gfx, slot, d->asset) != 0) {
            fvp_logf("title: texture '%s' failed to load (slot 0x%X)",
                     d->asset, slot);
            continue;
        }
        if (!fvp_title_profile_image_pos(prof, i, &x, &y)) {
            continue;
        }
        (void)gfx_set_prim(gfx, prim, GFX_F_TYPE, 4);
        (void)gfx_set_prim(gfx, prim, GFX_F_GRAPH, slot);
        (void)gfx_set_prim(gfx, prim, GFX_F_X, x);
        (void)gfx_set_prim(gfx, prim, GFX_F_Y, y);
        (void)gfx_set_prim(gfx, prim, GFX_F_DRAW, 1);
        (void)gfx_set_prim(gfx, prim, GFX_F_ALPHA, 255);
        loaded += 1;
    }
    /* 发光层（星空的记忆是 title_*_ray 那种整行光带）：装进 0xF08+i，
     * hover 时垫在文字下面。 */
    for (i = 0; i < FVP_TITLE_ITEMS; ++i) {
        const FvpTitleItemDef *d = &prof->items[i];

        if (d->asset_glow[0] == '\0') {
            continue;
        }
        if (gfx_graph_load(gfx, TITLE_SLOT_BASE + 8 + i, d->asset_glow) ==
            0) {
            glow_loaded += 1;
        }
    }
    if (glow_loaded > 0) {
        fvp_logf("title: %d glow texture(s) loaded (slots 0x%X..0x%X)",
                 glow_loaded, TITLE_SLOT_BASE + 8,
                 TITLE_SLOT_BASE + 7 + FVP_TITLE_ITEMS);
    }
    if (prof->bg_asset[0] != '\0' && !title_bg_disabled) {
        if (gfx_graph_load(gfx, TITLE_BG_SLOT, prof->bg_asset) == 0) {
            title_bg_on = 1;
            fvp_logf("title: background '%s' loaded (slot 0x%X)",
                     prof->bg_asset, TITLE_BG_SLOT);
        } else {
            fvp_logf("title: background '%s' failed to load",
                     prof->bg_asset);
        }
    }
    if (!title_bg_disabled) {
        int li;

        for (li = 0; li < prof->bg_overlay_count && li < 4; ++li) {
            if (prof->bg_overlay[li][0] == '\0') {
                continue;
            }
            if (gfx_graph_load(gfx, TITLE_OVL_SLOT + li,
                               prof->bg_overlay[li]) == 0) {
                title_ovl_loaded += 1;
                fvp_logf("title: bg overlay '%s' loaded (slot 0x%X)",
                         prof->bg_overlay[li], TITLE_OVL_SLOT + li);
            }
        }
    }
    if (prof->logo_asset[0] != '\0') {
        if (gfx_graph_load(gfx, TITLE_LOGO_SLOT, prof->logo_asset) ==
            0) {
            title_logo_on = 1;
            fvp_logf("title: logo '%s' loaded (slot 0x%X)",
                     prof->logo_asset, TITLE_LOGO_SLOT);
        }
    }
    if (prof->copyright_asset[0] != '\0') {
        if (gfx_graph_load(gfx, TITLE_COPY_SLOT,
                           prof->copyright_asset) == 0) {
            title_copy_on = 1;
            fvp_logf("title: copyright '%s' loaded (slot 0x%X)",
                     prof->copyright_asset, TITLE_COPY_SLOT);
        }
    }
    /* hover/选中态贴图：五彩斑斓每项有一张高亮图（`title_<item>1`），hover 时
     * 换画它；星空走发光层（asset_hover 为空串）。 */
    for (i = 0; i < FVP_TITLE_ITEMS; ++i) {
        const FvpTitleItemDef *d2 = &prof->items[i];

        if (d2->asset_hover[0] == '\0') {
            continue;
        }
        if (gfx_graph_load(gfx, TITLE_HOVER_SLOT_BASE + i,
                           d2->asset_hover) == 0) {
            fvp_logf("title: hover texture '%s' loaded (slot 0x%X)",
                     d2->asset_hover, TITLE_HOVER_SLOT_BASE + i);
        } else {
            fvp_logf("title: hover texture '%s' failed (slot 0x%X)",
                     d2->asset_hover, TITLE_HOVER_SLOT_BASE + i);
        }
    }
    return loaded;
#endif
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
    double t_start = 0.0; /* 帧循环起点（--shot-at 用） */
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
    FvpTitleMenu title;
    int title_active = 0;
    int title_picked = -1;
    int title_hover = -1;
    const FvpTitleProfile *title_profile = NULL;
    int title_textures = 0;
    FvpSaveList save_list;
    int save_list_active = 0;
    int save_hover = -1;
    int debug_requests_sent = 0;
    wchar_t save_dir[PATH_CAP];
    int save_writes = 0;
    int load_requests = 0;
    FvpOpenSeq opening;
    GfxFont *title_font = NULL;
    int opening_active = 0;
    int opening_layers_loaded = 0;
    double opening_started_at = 0.0;
    int shot_taken = 0;
    int shot_seq = 0;
    double shot_next_ms = 0.0;
    int save_preview_ok = 0;
    int load_view_active = 0;      /* 部分恢复视图（仅画面+文本） */
    FvpGameMenu gmenu;             /* 游戏内菜单（引擎侧 UI） */
    int menu_auto_armed = 0;
    int menu_option_panel = 0;
    double menu_t0 = 0.0;
    int load_view_slot = -1;
    char load_view_graph[64];
    char load_view_line[512];
    int title_fade_active = 0;  /* New Game 后的白场过渡中 */
    double title_fade_elapsed = 0.0;
    int title_fade_started_vm = 0;
    char save_preview_name[64];
    double opening_ms = 0.0;
    double title_shown_at = 0.0;
    double last_report = 0.0;
    PerfStats perf;
    PerfStats wperf; /* 本周期 5 s 窗口内的统计（周期报告用，见 perf_clear） */
    unsigned long long steps_at_report = 0;
    double run_started = 0.0;
    int verbose;

    (void)prev;
    (void)cmdline;
    (void)show;

    trace_init();
    trace_mark("before timing/dpi");
    QueryPerformanceFrequency(&g_qpc_freq);
    g_start_time = now_sec();
    set_dpi_awareness();
    trace_mark("dpi awareness set");
    /* Needed for SHBrowseForFolder (double-click folder picker). */
    (void)CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);
    trace_mark("com initialized (STA)");

    defaults(&opts);
    argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    trace_mark("command line split");
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
    /* 标题菜单：有该作品的 profile 就套用（iroseka 右对齐 / hoshimemo 居中），否则用通用默认。 */
    fvp_title_init(&title);
    title_active = opts.title_menu;
    if (title_active) {
        {
            char hint_u8[PATH_CAP * 4];

            if (wide_to_utf8(opts.game_dir, hint_u8, sizeof(hint_u8))) {
                title_profile = fvp_title_profile_find(hint_u8);
            }
            if (title_profile == NULL &&
                wide_to_utf8(opts.script, hint_u8, sizeof(hint_u8))) {
                title_profile = fvp_title_profile_find(hint_u8);
            }
        }
        {
            int slots = title_count_save_slots(opts.game_dir);

            title.enabled[FVP_TITLE_CONTINUE] = slots > 0 ? 1 : 0;
            fvp_logf("title: continue %s (%d slot(s) under save/)",
                     slots > 0 ? "enabled" : "disabled", slots);
            /* Continue 的列表：把每个槽读成一条记录（只读） */
            fvp_savelist_init(&save_list);
        }
        if (title_profile != NULL) {
            fvp_title_apply_profile(&title, title_profile);
            title.enabled[FVP_TITLE_CONTINUE] =
                title_count_save_slots(opts.game_dir) > 0 ? 1 : 0;
            if (opts.title_hover_seed >= 0 &&
                opts.title_hover_seed < FVP_TITLE_ITEMS) {
                title_hover = opts.title_hover_seed;
                title.hover = title_hover;
            }
            fvp_logf("title: profile '%s' (align=%s, pitch=%d, top=%d)",
                     title_profile->game,
                     title_profile->align == FVP_TITLE_ALIGN_CENTER
                         ? "centre" : "right",
                     title_profile->pitch, title_profile->ink_top0);
        } else {
            fvp_logf("title: no per-title profile, using generic layout");
        }
    }

    /* --opening 打开标题时顺带跑脚本里的进场动画（--title-anim 显式指定优先）。 */
    if (opts.title_anim < 0) {
        opts.title_anim = opts.opening ? 1 : 0;
    }
    /* 开场序列（--opening）：白底 + FAVORITE logo 六层，时间轴来自脚本
     * （见 src/openseq.h / openinganim.h，hcb 0x58096..0x584CB）。
     * 这里只置标志，标题资源的加载仍在上面按 title_active 做；
     * 开场期间不画标题菜单（见帧循环里的 opening_active 判断）。 */
    if (opts.opening) {
        FvpOpenGame open_game = fvp_openseq_game_from_name(
            title_profile != NULL ? title_profile->game : NULL);

        fvp_openseq_init(&opening, open_game);
        opening_active = 1;
        /* 真正的日志在素材载入之后打（这里 log 文件还没打开）。 */
    }
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

    /* Continue 的存档列表：等 game_u8 就绪后再扫描（只读） */
    if (title_active) {
        FvpEncoding slenc = (opts.enc_mode == ENC_GBK)
                                ? FVP_ENC_GBK : FVP_ENC_CP932;

        if (wcscmp(opts.save_encoding, L"gbk") == 0) {
            slenc = FVP_ENC_GBK;
        }
        (void)title_scan_saves(&save_list, opts.game_dir, game_u8,
                               slenc);
        fvp_logf("save list: %d slot(s) readable", save_list.count);
        if (opts.load_slot >= 0) {
            int k;

            for (k = 0; k < save_list.count; ++k) {
                if (save_list.items[k].slot == opts.load_slot) {
                    save_list.selected = k;
                    save_hover = k;
                    save_list_active = 1;
                    enter_load_view(k, &save_list, &load_view_active,
                                    &load_view_slot, load_view_graph,
                                    sizeof(load_view_graph), load_view_line,
                                    sizeof(load_view_line));
                    break;
                }
            }
        }
        if (opts.preview_slot >= 0) {
            int k;

            for (k = 0; k < save_list.count; ++k) {
                if (save_list.items[k].slot == opts.preview_slot) {
                    save_list.selected = k;
                    save_hover = k;
                    save_list_active = 1;
                    fvp_logf("save list: preselect slot %d (preview)",
                             opts.preview_slot);
                    break;
                }
            }
        }
        if (save_list.count > 0) {
            char first[256];
            char last[256];

            fvp_savelist_row_text(&save_list, 0, first, sizeof(first));
            fvp_savelist_row_text(&save_list, save_list.count - 1, last,
                                  sizeof(last));
            fvp_logf("save list: first=%s", first);
            fvp_logf("save list: last=%s", last);
        }
        if (opts.title_save_list && save_list.count > 0) {
            save_list_active = 1;
        }
    }
    {
        static const char *const game_menu_labels[] = {
            "SAVE", "LOAD", "OPTION", "TITLE", "QUIT"
        };

        fvp_gamemenu_init(&gmenu, game_menu_labels, 5, 300, 64);
        fvp_gamemenu_layout(&gmenu, 1920, 240, 40);
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
    fvp_vm_set_stall_limit(&vm, (uint64_t)opts.stall_after);
    vm.timeline_on = opts.timeline;
    vm.threads_on = opts.threads;
    fvp_logf("threads: coroutine slots %s", opts.threads ? "on" : "off (--no-threads)");
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
    if (opts.title_menu) {
        /* 引擎侧标题菜单：先不启动脚本，等用户在菜单里选 New Game。 */
        fvp_logf("title: engine-side menu enabled (script starts on New Game)");
    } else {
        fvp_vm_start(&vm);
        fvp_logf("vm: started at pc=0x%x (quiet text capture)",
                 hcb.main_start);
    }

    snd = snd_create(48000, 2);
    if (snd == NULL) {
        fvp_logf("audio: snd_create failed, running silent");
    }
    trace_mark("log opened + options parsed + script loaded + vm started");
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
        /* 纹理缓存上限：默认 512 MiB，--tex-cache-mb 0 = 不限制。 */
        gfx_tex_set_cap_bytes(gfx, (size_t)opts.tex_cache_mb * 1024u * 1024u);
        fvp_logf("gfx: texture cache cap %d MiB%s", opts.tex_cache_mb,
                 opts.tex_cache_mb == 0 ? " (unlimited)" : "");
        gfx_typewriter_set_enabled(gfx, opts.typewriter);
    gfx_set_text_prims(gfx, opts.text_prims);
    fvp_logf("gfx: text prims %s", opts.text_prims ? "on" : "off");
        fvp_logf("gfx: typewriter %s", opts.typewriter ? "on" : "off");
        if (title_active) {
            title_bg_disabled = opts.title_no_bg;
            title_textures = title_load_textures(gfx, title_profile);
            if (opts.title_bg_anim) {
                title_bg_anim_on = 1;
                title_bg_game = fvp_titlebg_game_from_name(
                    title_profile != NULL ? title_profile->game : NULL);
                title_bg_t0 = now_sec();
                fvp_logf("title: bg zoom started (%s, %d ms; script "
                         "MotionMoveS2%s)",
                         title_bg_game == FVP_TITLEBG_GAME_HOSHIMEMO
                             ? "hoshimemo 2000->1000/40000"
                             : "iroseka 4 layers 1300..4000->800..900, "
                               "7.5..62.5deg->0/25000",
                         fvp_titlebg_total_ms(title_bg_game),
                         fvp_titlebg_uses_layers(title_bg_game)
                             ? "+MotionMoveR, path A/B"
                             : "");
            }
            if (opts.opening) {
                int i;
                int loaded = 0;

                for (i = 0; i < fvp_openseq_layer_count(); ++i) {
                    if (gfx_graph_load(gfx, fvp_openseq_layer_slot(i),
                                       fvp_openseq_layer_name(i)) == 0) {
                        loaded += 1;
                    }
                }
                opening_layers_loaded = loaded;
                if (loaded == fvp_openseq_layer_count()) {
                    fvp_logf("opening: %d/%d layers loaded (script slots "
                             "250/252/254/255/256/257)", loaded,
                             fvp_openseq_layer_count());
                    fvp_logf("opening: script timeline (%s, %d ms): alpha in "
                             "1200/1400, logo out 2500, bg out 3500, cut 100",
                             fvp_openseq_game_name(opening.game),
                             FVP_OPEN_TOTAL_MS);
                    opening_started_at = now_sec();
                } else {
                    fvp_logf("opening: only %d/%d layers loaded, skipping",
                             loaded, fvp_openseq_layer_count());
                    opening_active = 0;
                    title_active = opts.title_menu ? 1 : 0;
                }
            }
            title_textures_on = title_textures > 0;
            if (title_textures > 0) {
                fvp_logf("title: %d/%d menu textures loaded (slots 0x%X..0x%X)",
                         title_textures, FVP_TITLE_ITEMS, TITLE_SLOT_BASE,
                         TITLE_SLOT_BASE + FVP_TITLE_ITEMS - 1);
            } else if (title_profile != NULL) {
                fvp_logf("title: no usable texture, falling back to text");
            }
        }
        {
            /* 缺字回退：图集里没有的码点（汉化文本等）用 GDI 现场栅格化。
             * 两条实现的路数不同（见 win32_gdi_text.h）：
             *   默认 = 抗锯齿（TextOutW + 32bpp DIB，灰度覆盖率）
             *   --glyph-original = 原版同协议（cp932 + GGO_BITMAP，1bpp 二值）
             * 原版同协议的参数是从 exe 里静态提取出来的，见
             * ref/exe-gdi-params.json 与 docs/original-verification.md §8。 */
            static const GfxGlyphFallback gdi_antialiased = {
                win32_gdi_text_mask, win32_gdi_text_advance
            };
            static const GfxGlyphFallback gdi_cp932 = {
                win32_gdi_text_mask_cp932, win32_gdi_text_advance_cp932
            };

            if (opts.glyph_original) {
                gfx_set_glyph_fallback(gfx, &gdi_cp932);
                fvp_logf("gfx: glyph fallback = original GDI protocol "
                         "(SHIFTJIS/GGO_BITMAP, face=%s)",
                         win32_gdi_text_default_face());
            } else {
                gfx_set_glyph_fallback(gfx, &gdi_antialiased);
            }
        }
        nfonts_loaded = load_fonts(gfx, &opts, exe_dir, fonts);
        fvp_logf("gfx: %d font atlas(es) registered", nfonts_loaded);
        if (opts.title_font[0] != L'\0') {
            char tf[PATH_CAP * 3];

            if (wide_to_ansi(opts.title_font, tf, sizeof(tf))) {
                title_font = gfx_font_load(tf);
                if (title_font != NULL) {
                    title_ui_font = title_font;
                    fvp_logf("title: font '%s' size=%d glyphs=%d", tf,
                             gfx_font_pixel_size(title_font),
                             gfx_font_glyph_count(title_font));
                } else {
                    fvp_logf("title: font load failed: %s", tf);
                }
            }
        }
        /* 把实际加载的字集名字注册进 VM 的字体表（TextFontCount/Name/Set/Get
         * 114..117）。名字按像素尺寸给（gfx-font-32 / -34 / -56），因为外层文件
         * 就叫 gfx-font-<size>.bin；原版的字体名来自 exe 注册表，我们没有那份表。 */
        {
            int fi;

            for (fi = 0; fi < nfonts_loaded; ++fi) {
                char fname[48];

                snprintf(fname, sizeof fname, "gfx-font-%d",
                         gfx_font_pixel_size(fonts[fi]));
                (void)fvp_vm_font_register(&vm, fname);
            }
        }
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
    trace_mark("audio started");
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
    trace_mark("gfx created + fonts loaded");
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

    trace_mark("window created");
    t_ready = now_sec();
    fvp_logf("startup: script+vm+gfx+window ready in %.1f ms",
             (t_ready - g_start_time) * 1000.0);
    trace_mark("ready (entering frame loop)");

    /* ---------------- frame loop ---------------- */
    memset(&perf, 0, sizeof(perf));
    perf_clear(&wperf);
    last_frame_time = now_sec();
    next_frame = last_frame_time;
    fps_t0 = last_frame_time;
    run_started = last_frame_time;
    last_report = last_frame_time;
    steps_at_report = vm.steps;

    t_start = now_sec();
    menu_t0 = t_start; /* --menu-at 的计时基准：外壳主循环起点 */
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
        title_bg_now = now;
    if (opts.shot_every_ms > 0.0 && shot_next_ms <= 0.0) {
        shot_next_ms = opts.shot_every_ms;
    }

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
        /* 菜单/标题都要吃 ESC：capture_nav 常开，ESC 不再直接退出窗口。
         * 游戏中按 ESC 打开游戏内菜单（原版的右键菜单等价物）。 */
        in->capture_nav = 1;
        if (opts.no_input) {
            /* 门禁/截图：把真实输入抹掉，画面只由程序状态决定 */
            in->mouse_pressed = 0;
            in->mouse_released = 0;
            in->mouse_down = 0;
            in->key_down = 0;
            in->key_pressed = 0;
            in->nav = 0;
        }
        vm.cx = in->canvas_x;
        vm.cy = in->canvas_y;
        click_edges = in->mouse_pressed + in->key_pressed;
        in->mouse_pressed = 0;
        in->key_pressed = 0;
        /* 引擎侧标题菜单：菜单在的时候点击/回车归菜单用，不喂给 VM。 */
        if (title_active && save_list_active) {
            /* 读档列表：左键选中一行就记日志；右键/退出键回标题菜单 */
            save_hover = fvp_savelist_hit(&save_list, vm.cx, vm.cy);
            {
                /* 选中槽里如果记了背景图名，就把那张图装进预览槽 */
                int sel = save_list.selected >= 0 ? save_list.selected
                                                  : save_hover;

                if (sel >= 0 && sel < save_list.count && gfx != NULL) {
                    const char *gname = save_list.items[sel].graph;

                    if (gname[0] != '\0' &&
                        strcmp(gname, save_preview_name) != 0) {
                        snprintf(save_preview_name,
                                 sizeof(save_preview_name), "%s", gname);
                        save_preview_ok =
                            gfx_graph_load(gfx, SAVE_PREVIEW_SLOT, gname) ==
                            0;
                        fvp_logf("save list: preview graph '%s' %s",
                                 gname, save_preview_ok ? "loaded"
                                                        : "missing");
                    }
                }
            }
            if (in->nav & FVP_NAV_DOWN) {
                save_hover = (save_hover < 0) ? 0
                             : (save_hover + 1) % save_list.count;
                save_list.selected = save_hover;
            }
            if (in->nav & FVP_NAV_UP) {
                save_hover = (save_hover < 0)
                                 ? save_list.count - 1
                                 : (save_hover + save_list.count - 1) %
                                       save_list.count;
                save_list.selected = save_hover;
            }
            if (in->nav & FVP_NAV_ESC) {
                if (load_view_active) {
                    load_view_active = 0;
                    fvp_logf("load: ESC -> back to save list");
                } else {
                    save_list_active = 0;
                    fvp_logf("save list: ESC -> back to title menu");
                }
                in->nav = 0;
            }
            if ((in->nav & FVP_NAV_ENTER) && save_hover >= 0 &&
                save_list.count > 0) {
                char row[256];

                fvp_savelist_row_text(&save_list, save_hover, row,
                                      sizeof(row));
                save_list.selected = save_hover;
                fvp_logf("save list: selected %s (chapter='%s')", row,
                         save_list.items[save_hover].chapter);
                enter_load_view(save_hover, &save_list, &load_view_active,
                                &load_view_slot, load_view_graph,
                                sizeof(load_view_graph), load_view_line,
                                sizeof(load_view_line));
                in->nav = 0;
            }
            if (click_edges > 0 && save_hover >= 0) {
                char row[256];

                fvp_savelist_row_text(&save_list, save_hover, row,
                                      sizeof(row));
                save_list.selected = save_hover;
                fvp_logf("save list: selected %s (chapter='%s' ", row,
                         save_list.items[save_hover].chapter);
                enter_load_view(save_hover, &save_list, &load_view_active,
                                &load_view_slot, load_view_graph,
                                sizeof(load_view_graph), load_view_line,
                                sizeof(load_view_line));
                fvp_logf("save list: snapshot restore is not implemented "
                         "yet, staying in the menu");
                click_edges = 0;
            } else if (click_edges > 0) {
                save_list_active = 0; /* 点空白处回标题菜单 */
                fvp_logf("save list: back to title menu");
                click_edges = 0;
            }
        }
        if (title_active && !save_list_active) {
            int pick = -1;

            if (in->nav & FVP_NAV_DOWN) {
                title_hover = fvp_title_nav(&title, +1);
            }
            if (in->nav & FVP_NAV_UP) {
                title_hover = fvp_title_nav(&title, -1);
            }
            if (in->nav & FVP_NAV_ENTER) {
                pick = fvp_title_activate(&title);
                title_hover = title.hover;
                in->nav = 0;
            }
            if ((in->nav & FVP_NAV_ESC) && !title_active) {
                in->nav = 0;
            }
            if (title_profile != NULL) {
                /* 有该作品的 profile：直接用它实测的墨迹矩形命中。
                 * --title-hover 预置的 hover 要保留到鼠标真的动过
                 * 为止（截图/门禁需要稳定的 hover 态）。 */
                if (opts.title_no_hover) {
                    title_hover = -1;
                } else if (opts.title_hover_seed < 0) {
                    title_hover = fvp_title_profile_hit(title_profile,
                                                        vm.cx, vm.cy);
                }
                if (title.hover != title_hover) {
                    fvp_logf("title: hover -> %d (mouse %d,%d)",
                             title_hover, vm.cx, vm.cy);
                }
                title.hover = title_hover;
            } else {
                (void)fvp_title_move(&title, vm.cx, vm.cy, title_measure,
                                     gfx);
                title_hover = title.hover;
            }
            if (opts.pick_seed >= 0) {
                /* 门禁/截图：把第 N 项当成"已经点了"（只发一次） */
                pick = opts.pick_seed;
                opts.pick_seed = -1;
            }
            if (pick < 0 && click_edges > 0) {
                if (title_profile != NULL) {
                    pick = title_hover;
                } else {
                    pick = fvp_title_click(&title, vm.cx, vm.cy,
                                           title_measure, gfx);
                }
                if (pick < 0 && title_hover >= 0) {
                    pick = title_hover;
                }
            }
            if (pick < 0 && in->key_down && title_hover < 0) {
                pick = FVP_TITLE_NEW_GAME; /* 回车且没悬停：直接开新游戏 */
            }
            if (pick >= 0) {
                title_picked = pick;
                title_active = 0;
                fvp_logf("title: picked %s (item %d at %d ms)",
                         fvp_title_label(pick), pick,
                         (int)((now - title_shown_at) * 1000.0));
                if (pick == FVP_TITLE_EXIT) {
                    win.quit = 1;
                } else if (pick == FVP_TITLE_CONTINUE) {
                    /* Continue → 打开读档列表（只读：快照恢复还没做） */
                    save_list_active = 1;
                    title_active = 1;
                    title_picked = -1;
                    fvp_logf("title: continue -> save list (%d slot(s))",
                             save_list.count);
                } else if (opts.title_fade_ms > 0) {
                    /* 原版点 New Game 后是白屏过渡再进 OP/正文：先淡到白，
                     * 白场峰值处启动脚本，再淡出。 */
                    title_fade_active = 1;
                    title_fade_elapsed = 0.0;
                    title_fade_started_vm = 0;
                    fvp_logf("title: start transition (%d ms, white)",
                             opts.title_fade_ms);
                } else {
                    fvp_vm_start(&vm);
                    fvp_logf("vm: started at pc=0x%x (from title menu)",
                             hcb.main_start);
                }
            }
            click_edges = 0;
        }
        /* ---- 游戏内菜单（引擎侧 UI）：ESC 开关，方向键/回车/点击操作 ---- */
        if (!title_active && gfx != NULL) {
            if (opts.menu_at >= 0 && !menu_auto_armed &&
                (now - menu_t0) * 1000.0 >= (double)opts.menu_at) {
                menu_auto_armed = 1;
                fvp_gamemenu_open(&gmenu);
                fvp_logf("menu: auto-opened at %.0f ms (%d item(s))",
                         (now - menu_t0) * 1000.0, gmenu.count);
            }
            if (in->nav & FVP_NAV_ESC) {
                in->nav &= ~FVP_NAV_ESC;
                if (gmenu.active) {
                    fvp_gamemenu_close(&gmenu);
                    fvp_logf("menu: closed");
                } else {
                    fvp_gamemenu_open(&gmenu);
                    fvp_logf("menu: opened with %d item(s)", gmenu.count);
                }
            }
            g_gmenu_bridge = vm.snd_bridge;
            if (g_menu_option_panel && gmenu.active) {
                /* OPTION 面板打开时，↑/↓ 直接调主音量 */
                if (in->nav & FVP_NAV_UP) {
                    int before = fvp_snd_bridge_master(vm.snd_bridge);

                    in->nav &= ~FVP_NAV_UP;
                    fvp_logf("menu: master volume -> %d%%",
                             fvp_snd_bridge_set_master(vm.snd_bridge,
                                                       before + 10));
                }
                if (in->nav & FVP_NAV_DOWN) {
                    int before = fvp_snd_bridge_master(vm.snd_bridge);

                    in->nav &= ~FVP_NAV_DOWN;
                    fvp_logf("menu: master volume -> %d%%",
                             fvp_snd_bridge_set_master(vm.snd_bridge,
                                                       before - 10));
                }
            }
            if (opts.menu_pick >= 0 && gmenu.active) {
                int pk = opts.menu_pick;

                opts.menu_pick = -1;
                if (pk >= 0 && pk < gmenu.count) {
                    gmenu.hover = pk;
                    {
                        int act = fvp_gamemenu_activate(&gmenu);

                        if (act >= 0) {
                            fvp_logf("menu: auto-pick item %d ('%s')", act,
                                     gmenu.items[act].label);
                            if (act == 4) {
                                win.quit = 1;
                            } else if (act == 3) {
                                fvp_gamemenu_close(&gmenu);
                                title_active = 1;
                                save_list_active = 0;
                                fvp_logf("menu: TITLE -> title screen");
                            } else if (act == 0) {
                                if (opts.save_template[0] != L'\0' &&
                                    opts.save_preview[0] != L'\0' &&
                                    gfx != NULL) {
                                    int cw2 = 0, ch2 = 0;
                                    const uint32_t *cv = gfx_canvas(
                                        gfx, &cw2, &ch2);
                                    char u8b[PATH_CAP * 3];

                                    if (cv != NULL &&
                                        write_save_preview(
                                            opts.save_template,
                                            opts.save_preview,
                                            (const unsigned char *)cv, cw2,
                                            ch2, &vm, opts.enc_mode)) {
                                        fvp_logf("menu: SAVE -> %s (template "
                                                 "slot, game save dir "
                                                 "untouched)",
                                                 w2u(opts.save_preview, u8b,
                                                     sizeof(u8b)));
                                    }
                                    fvp_gamemenu_close(&gmenu);
                                } else {
                                    fvp_logf("menu: SAVE needs "
                                             "--save-template and "
                                             "--save-preview");
                                }
                            } else if (act == 1) {
                                char gd[PATH_CAP * 3];

                                fvp_gamemenu_close(&gmenu);
                                if (wide_to_ansi(opts.game_dir, gd, sizeof(gd))) {
                                    (void)title_scan_saves(&save_list,
                                                           opts.game_dir, gd,
                                                           opts.enc_mode);
                                }
                                save_list_active = 1;
                                title_active = 1;
                                fvp_logf("menu: LOAD -> save list (%d "
                                         "slot(s))", save_list.count);
                            } else if (act == 2) {
                                int step;

                                menu_option_panel = !menu_option_panel;
                                g_menu_option_panel = menu_option_panel;
                                fvp_logf("menu: OPTION panel %s", 
                                         menu_option_panel ? "opened"
                                                           : "closed");
                                /* --menu-adjust N：连调 N 步主音量（门禁/截图用） */
                                for (step = 0; step < abs(opts.menu_adjust);
                                     ++step) {
                                    int before = vm.snd_bridge != NULL
                                                     ? fvp_snd_bridge_master(
                                                           vm.snd_bridge)
                                                     : 100;
                                    int want = before +
                                               (opts.menu_adjust > 0 ? 10 : -10);
                                    int after = fvp_snd_bridge_set_master(
                                        vm.snd_bridge, want);

                                    fvp_logf("menu: master volume -> %d%% "
                                             "(was %d%%, bridge=%s)",
                                             after, before,
                                             vm.snd_bridge != NULL ? "live"
                                                                   : "none");
                                    if (after < 0) {
                                        break;
                                    }
                                }
                            }
                        }
                    }
                }
            }
            if (gmenu.active) {
                if (in->nav & FVP_NAV_DOWN) {
                    in->nav &= ~FVP_NAV_DOWN;
                    (void)fvp_gamemenu_nav(&gmenu, 1);
                    fvp_logf("menu: hover -> %d", gmenu.hover);
                }
                if (in->nav & FVP_NAV_UP) {
                    in->nav &= ~FVP_NAV_UP;
                    (void)fvp_gamemenu_nav(&gmenu, -1);
                    fvp_logf("menu: hover -> %d", gmenu.hover);
                }
                fvp_gamemenu_move(&gmenu, vm.cx, vm.cy);
                if ((in->nav & FVP_NAV_ENTER) || click_edges > 0) {
                    int pick = (in->nav & FVP_NAV_ENTER)
                                   ? fvp_gamemenu_activate(&gmenu)
                                   : fvp_gamemenu_click(&gmenu, vm.cx, vm.cy);

                    in->nav &= ~FVP_NAV_ENTER;
                    click_edges = 0;
                    if (pick >= 0) {
                        fvp_logf("menu: picked item %d ('%s')", pick,
                                 gmenu.items[pick].label);
                        if (pick == 4) {
                            win.quit = 1;
                        } else if (pick == 3) {
                            fvp_gamemenu_close(&gmenu);
                            title_active = 1;
                            save_list_active = 0;
                            fvp_logf("menu: TITLE -> title screen");
                        } else if (pick == 0) {
                            /* SAVE：用模板槽 + 当前画面写一份真实存档文件，但**只写到
                             * --save-preview 指定的路径**（不会碰游戏自己的 save/）。 */
                            if (opts.save_template[0] != L'\0' &&
                                opts.save_preview[0] != L'\0' &&
                                gfx != NULL) {
                                int cw2 = 0, ch2 = 0;
                                const uint32_t *cv = gfx_canvas(gfx, &cw2,
                                                                &ch2);
                                char u8b[PATH_CAP * 3];

                                if (cv != NULL &&
                                    write_save_preview(
                                        opts.save_template, opts.save_preview,
                                        (const unsigned char *)cv, cw2, ch2,
                                        &vm, opts.enc_mode)) {
                                    fvp_logf("menu: SAVE -> %s (template slot, "
                                             "game save dir untouched)",
                                             w2u(opts.save_preview, u8b,
                                                 sizeof(u8b)));
                                } else {
                                    fvp_logf("menu: SAVE failed (canvas or "
                                             "template unavailable)");
                                }
                                fvp_gamemenu_close(&gmenu);
                            } else {
                                fvp_logf("menu: SAVE needs --save-template and "
                                         "--save-preview (no silent write into "
                                         "the game save dir)");
                            }
                        } else if (pick == 1) {
                            /* LOAD：打开已有的存档列表（只读预览；真读档仍是
                             * "部分恢复"，见 docs/save-snapshot.md §4.6）。 */
                            fvp_gamemenu_close(&gmenu);
                            save_list_active = 1;
                            title_active = 1;
                            fvp_logf("menu: LOAD -> save list (%d slot(s))",
                                     save_list.count);
                        } else if (pick == 2) {
                            menu_option_panel = !menu_option_panel;
                            g_menu_option_panel = menu_option_panel;
                            fvp_logf("menu: OPTION panel %s (read-only: "
                                     "master=%d%%, type[0]=%d%%)",
                                     menu_option_panel ? "opened" : "closed",
                                     100, 100);
                        }
                    }
                }
            }
        }
        if (!title_active) {
            in->nav = 0; /* 菜单不在时导航键没有消费者 */
        }
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
                if (fvp_vm_stalled(&vm)) {
                    break; /* 脚本自旋：VM 已停下，别再往里塞步数 */
                }
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
            /* 标题菜单是引擎侧 UI 覆盖层，必须在 render 之后画。
             * 开场序列期间不画标题（脚本是等开场函数返回后才排标题的逐元素进场，
             * 见 src/titleenter.h，所以 title_anim 也不能在开场期间启动）。 */
            if (title_active && !opening_active) {
                if (title_shown_at == 0.0) {
                    /* 几何/可用性已在启动时按 profile 设好；
                     * 这里只记录菜单开始显示的时刻。 */
                    title_shown_at = now;
                }
                if (opts.title_anim && !title_enter_started) {
                    FvpTitleEnterGame game = fvp_titleenter_game_from_name(
                        title_profile != NULL ? title_profile->game : NULL);

                    title_enter_start(now, game);
                    title_enter_started = 1;
                    fvp_logf("title: script entrance started (%s: logo 0->255 "
                             "over %d ms; %d menu items staggered by %d ms; "
                             "total %d ms)",
                             fvp_titleenter_game_name(game),
                             FVP_TITLEENTER_LOGO_MS,
                             FVP_TITLEENTER_MENU_COUNT,
                             FVP_TITLEENTER_MENU_STEP_MS,
                             fvp_titleenter_total_ms(&title_enter));
                }
                if (title_enter_active) {
                    title_enter_advance(now);
                }
                if (load_view_active) {
                    /* 部分恢复：整屏背景图 + 存档记的那句台词 + 明确标注 */
                    if (load_view_graph[0] != '\0' && save_preview_ok) {
                        (void)gfx_draw_image_scaled(gfx, SAVE_PREVIEW_SLOT,
                                                    0, 0, 1920, 1080);
                    } else {
                        (void)gfx_fill_canvas(gfx, 0, 0, 0, 255);
                    }
                    if (load_view_line[0] != '\0') {
                        (void)gfx_draw_text_line(gfx, load_view_line, 120, 900,
                                                 32, 255, 255, 255, 255);
                    }
                    (void)gfx_draw_text_line(
                        gfx, "[PARTIAL RESTORE] visual + text only", 56, 40,
                        32, 255, 210, 120, 255);
                } else if (save_list_active) {
                    int li;

                    /* 预览：选中槽记录的背景图缩到右下角，底下再写那条台词 */
                    if (save_preview_ok) {
                        int sel = save_list.selected;

                        (void)gfx_draw_image_scaled(gfx, SAVE_PREVIEW_SLOT,
                                                    1200, 640, 640, 360);
                        if (sel >= 0 && sel < save_list.count &&
                            save_list.items[sel].line[0] != '\0') {
                            (void)gfx_draw_text_line(
                                gfx, save_list.items[sel].line, 120,
                                1010, 26, 255, 255, 255, 255);
                        }
                    }
                    for (li = 0; li < save_list.count; ++li) {
                        char row[256];
                        int ry = save_list.y + li * save_list.row_h + 18;
                        int rr = 225, rg = 225, rb = 230;

                        fvp_savelist_row_text(&save_list, li, row,
                                              sizeof(row));
                        if (li == save_hover || li == save_list.selected) {
                            rr = 255;
                            rg = 250;
                            rb = 200;
                        }
                        (void)gfx_draw_text_line(gfx, row,
                                                 save_list.x + 16, ry, 28,
                                                 rr, rg, rb, 255);
                    }
                } else {
                    title_draw_overlay(gfx, &title, title_profile,
                                       title_hover);
                }
            } else {
                title_enter_started = 0;
                title_enter_active = 0;
                title_enter_reset();
            }
            if (gmenu.active) {
                draw_game_menu(gfx, &gmenu);
            }
            canvas = gfx_canvas(gfx, &cw, &ch);
            render_ms = (now_sec() - r0) * 1000.0;
        }
        /* ---- 开场序列（--opening，脚本 0x58096..0x584CB）----
         * 六层素材按脚本的 alpha/缩放/旋转/位移/OP 轴心画；时间轴由 openseq
         * 从墙钟算（不用累加 frame_dt：开场第一帧要解码六张图，dt 可能有
         * 几百毫秒，累加会让 1.2 s 的淡入在一帧里跳完）。 */
        if (opening_active) {
            opening_ms = (now - opening_started_at) * 1000.0;
            fvp_openseq_set_elapsed(&opening, (int)opening_ms);
            if (gfx != NULL && canvas != NULL) {
                int i;

                /* 脚本先建标题场景再跑开场（hcb 0x590CC `call 0x584D8` 载入
                 * title_bg1_* 并排好 25 s 的慢缩放/旋转，0x591EC 才 `call`
                 * 开场函数），所以**开场白底后面已经有标题背景**：白底 250
                 * 淡出时露出来的是标题背景，不是黑屏。开场期间标题的
                 * logo/菜单 alpha 还没开始排（它们的 MotionAlpha 在开场返回
                 * 后才排，hcb 0x5933E 起），所以这里强制 title_fade=0：
                 * 只画背景/叠加层，不画 logo/版权/菜单。 */
                if (title_active) {
                    int saved_logo = title_logo_alpha;
                    int saved_copy = title_copy_alpha;
                    int saved_dx = title_copy_dx;
                    int saved_menu[FVP_TITLE_ITEMS];
                    int saved_dy[FVP_TITLE_ITEMS];
                    int k;

                    for (k = 0; k < FVP_TITLE_ITEMS; ++k) {
                        saved_menu[k] = title_menu_alpha[k];
                        saved_dy[k] = title_menu_dy[k];
                        title_menu_alpha[k] = 0;
                        title_menu_dy[k] = 0;
                    }
                    title_logo_alpha = 0;
                    title_copy_alpha = 0;
                    title_copy_dx = 0;
                    title_draw_overlay(gfx, &title, title_profile, title_hover);
                    title_logo_alpha = saved_logo;
                    title_copy_alpha = saved_copy;
                    title_copy_dx = saved_dx;
                    for (k = 0; k < FVP_TITLE_ITEMS; ++k) {
                        title_menu_alpha[k] = saved_menu[k];
                        title_menu_dy[k] = saved_dy[k];
                    }
                }
                for (i = 0; i < fvp_openseq_layer_count(); ++i) {
                    FvpOpenLayerState st;

                    if (!fvp_openseq_layer_state(&opening, i, &st)) {
                        continue;
                    }
                    if (st.alpha <= 0) {
                        continue;
                    }
                    (void)gfx_draw_image_op_xform(
                        gfx, fvp_openseq_layer_slot(i), st.dx, st.dy, st.op_x,
                        st.op_y, st.scale_x1000, st.scale_y1000, st.angle_units,
                        st.alpha);
                }
            }
            if (fvp_openseq_done(&opening)) {
                opening_active = 0;
                title_active = opts.title_menu ? 1 : 0;
                title_shown_at = now;
                fvp_logf("opening: finished in %.0f ms (%d layers) -> "
                         "title menu %s",
                         opening_ms, opening_layers_loaded,
                         title_active ? "on" : "off (script)");
            }
        }
        /* ---- New Game 的白场过渡 ---- */
        if (title_fade_active) {
            double total = (double)opts.title_fade_ms;
            double half = total / 2.0;
            double p;

            title_fade_elapsed += frame_dt * 1000.0;
            if (!title_fade_started_vm && title_fade_elapsed >= half) {
                title_fade_started_vm = 1;
                fvp_vm_start(&vm);
                fvp_logf("vm: started at pc=0x%x (white transition peak)",
                         hcb.main_start);
                fvp_logf("title: start transition -> vm started at %.0f ms",
                         title_fade_elapsed);
            }
            p = title_fade_elapsed / half;
            if (p > 1.0) {
                p = (total - title_fade_elapsed) / half;
            }
            if (p < 0.0) {
                p = 0.0;
            }
            if (p > 1.0) {
                p = 1.0;
            }
            if (gfx != NULL && canvas != NULL) {
                gfx_fill_canvas(gfx, 255, 255, 255, (int)(p * 255.0));
            }
            if (title_fade_elapsed >= total) {
                title_fade_active = 0;
                fvp_logf("title: start transition done (%.0f ms)",
                         title_fade_elapsed);
            }
        }
        /* ---- 存档/读档请求（Load 40 / SaveWrite 96）----
         * VM 只记录"请求 + 槽号"，真正读写文件在这一侧做：
         *   SaveWrite → 用 --save-template 当模板 + 当前画布缩略图 + 脚本暂存的
         *               三条字符串，写成 <game_dir>\save\sNNN.bin；
         *               没有模板就**拒绝写**（槽尾语义未解，硬造只会写坏档）。
         *   Load      → 只记日志：槽尾快照的字段语义还没解完，恢复不了。 */
        if (canvas != NULL) {
            if (!debug_requests_sent) {
                if (opts.debug_save_request >= 0) {
                    FvpValue dv;

                    memset(&dv, 0, sizeof dv);
                    dv.type = FVP_INT;
                    dv.i = opts.debug_save_request;
                    (void)fvp_vm_request_save(&vm, &dv);
                }
                if (opts.debug_load_request >= 0) {
                    FvpValue dv;

                    memset(&dv, 0, sizeof dv);
                    dv.type = FVP_INT;
                    dv.i = opts.debug_load_request;
                    (void)fvp_vm_request_load(&vm, &dv);
                }
                debug_requests_sent = 1;
            }
            {
                int sreq = fvp_vm_take_save_request(&vm);

                if (sreq >= 0) {
                    if (opts.save_template[0] == L'\0') {
                        fvp_logf("save: slot %d requested but --save-template "
                                 "was not given (refusing to invent a snapshot)",
                                 sreq);
                    } else if (path_join_w(save_dir, PATH_CAP, opts.game_dir,
                                           L"save")) {
                        wchar_t slot_path[PATH_CAP];
                        wchar_t fname[32];
                        FvpEncoding senc2 =
                            (opts.enc_mode == ENC_GBK) ? FVP_ENC_GBK
                                                       : FVP_ENC_CP932;
                        int wrote = 0;

                        (void)CreateDirectoryW(save_dir, NULL);
                        swprintf(fname, 32, L"\\s%03d.bin", sreq);
                        wcscpy(slot_path, save_dir);
                        if (wcslen(slot_path) + wcslen(fname) + 1 < PATH_CAP) {
                            wcscat(slot_path, fname);
                            wrote = write_save_preview(
                                opts.save_template, slot_path,
                                (const unsigned char *)canvas, cw, ch, &vm,
                                senc2);
                        }
                        if (wrote) {
                            char u8p[PATH_CAP * 3];

                            fvp_logf("save: wrote slot %d -> %s", sreq,
                                     w2u(slot_path, u8p, sizeof(u8p)));
                            save_writes += 1;
                        } else {
                            fvp_logf("save: slot %d FAILED", sreq);
                        }
                    } else {
                        fvp_logf("save: cannot build the save dir path");
                    }
                }
            }
            {
                int lreq = fvp_vm_take_load_request(&vm);

                if (lreq >= 0) {
                    if (opts.load_restart) {
                        (void)shell_load_restart(&vm, &hcb, gfx, snd, &opts,
                                                 lreq, save_enc);
                    } else {
                        fvp_logf("load: slot %d requested; snapshot restore is "
                                 "not implemented yet (staying in the current "
                                 "scene; pass --load-restart to restart)",
                                 lreq);
                    }
                    load_requests += 1;
                }
            }
        }
        if (canvas == NULL) {
            fvp_logf("gfx: gfx_canvas returned NULL, stopping");
            exit_code = 6;
            break;
        }
        {
            double p0 = now_sec();
        /* --shot-at N：跑到 N 毫秒时存一张（开场序列/淡入的中间态截图用） */
        if (!shot_taken && opts.shot_at_ms > 0.0 && canvas != NULL &&
            opts.screenshot[0] != L'\0' &&
            (now - t_start) * 1000.0 >= opts.shot_at_ms) {
            char u8s[PATH_CAP * 3];

            shot_taken = 1;
            if (write_bmp32(opts.screenshot, canvas, cw, ch)) {
                if (opening_active) {
                    /* 开场期间把"开场时间轴上的时刻"一并写进日志：--shot-at 是
                     * 墙钟（含窗口创建/解码素材的开销），门禁要用开场自己的 t。 */
                    fvp_logf("screenshot: %s 1920x1080 at %.0f ms "
                             "(shot-at; opening t=%.0f ms)",
                             w2u(opts.screenshot, u8s, sizeof(u8s)),
                             opts.shot_at_ms, opening_ms);
                } else {
                    fvp_logf("screenshot: %s 1920x1080 at %.0f ms (shot-at)",
                             w2u(opts.screenshot, u8s, sizeof(u8s)),
                             opts.shot_at_ms);
                }
            }
        }
        /* --shot-every：按固定间隔抓一串（demo/GIF 用） */
        if (opts.shot_every_ms > 0.0 && opts.shot_prefix[0] != L'\0' &&
            canvas != NULL && (now - t_start) * 1000.0 >= shot_next_ms) {
            wchar_t path[PATH_CAP];
            char u8e[PATH_CAP * 3];

            _snwprintf(path, PATH_CAP, L"%ls-%04d.bmp", opts.shot_prefix,
                       shot_seq);
            if (write_bmp32_scaled(path, canvas, cw, ch, opts.shot_div)) {
                fvp_logf("screenshot: %s at %.0f ms (shot-every)",
                         w2u(path, u8e, sizeof(u8e)),
                         (now - t_start) * 1000.0);
            }
            shot_seq += 1;
            shot_next_ms += opts.shot_every_ms;
            if (shot_next_ms < (now - t_start) * 1000.0) {
                shot_next_ms = (now - t_start) * 1000.0 + opts.shot_every_ms;
            }
        }
            fvp_win32_present(&win, canvas, cw, ch);
            present_ms = (now_sec() - p0) * 1000.0;
        }
        /* 动画补间：默认关闭时只走时间轴，不改图元字段。 */
        gfx_motion_advance(gfx, (int)(frame_dt * 1000.0));
        /* 逐字显示：用同一帧 dt 推进；关闭时 gfx_text_advance() 直接返回。 */
        gfx_text_advance(gfx, (int)(frame_dt * 1000.0));
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
            fvp_logf("tex: %d image(s), %.1f MiB cached (cap %.0f MiB, %lu evicted)",
                     gfx_tex_count(gfx),
                     (double)gfx_tex_bytes(gfx) / (1024.0 * 1024.0),
                     (double)gfx_tex_cap_bytes(gfx) / (1024.0 * 1024.0),
                     gfx_tex_evictions(gfx));
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

    if (gfx != NULL) {
        const GfxStats *gs = gfx_stats(gfx);

        if (gs != NULL && gs->glyph_fb_runs > 0) {
            fvp_logf("gfx: glyph fallback used %lu time(s) (atlas missing "
                     "codepoints)", gs->glyph_fb_runs);
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
        /* 截图也要带上引擎侧 UI 覆盖层（和屏幕上看到的一致）。 */
        if (title_active) {
            if (title_fade_active) {
                double half = (double)opts.title_fade_ms / 2.0;
                double p = (half > 0.0) ? title_fade_elapsed / half : 1.0;

                if (p > 1.0) {
                    p = ((double)opts.title_fade_ms - title_fade_elapsed) /
                        half;
                }
                if (p < 0.0) {
                    p = 0.0;
                }
                if (p > 1.0) {
                    p = 1.0;
                }
                gfx_fill_canvas(gfx, 255, 255, 255, (int)(p * 255.0));
            } else if (load_view_active) {
                if (load_view_graph[0] != '\0' && save_preview_ok) {
                    (void)gfx_draw_image_scaled(gfx, SAVE_PREVIEW_SLOT, 0, 0,
                                                1920, 1080);
                } else {
                    (void)gfx_fill_canvas(gfx, 0, 0, 0, 255);
                }
                if (load_view_line[0] != '\0') {
                    (void)gfx_draw_text_line(gfx, load_view_line, 120, 900, 32,
                                             255, 255, 255, 255);
                }
                (void)gfx_draw_text_line(gfx,
                                         "[PARTIAL RESTORE] visual + text only",
                                         56, 40, 32, 255, 210, 120, 255);
            } else if (save_list_active) {
                int li;

                /* 预览图与台词也要出现在截图里（和帧循环里同一套画法） */
                if (save_preview_ok) {
                    int sel = save_list.selected;

                    (void)gfx_draw_image_scaled(gfx, SAVE_PREVIEW_SLOT,
                                                1200, 640, 640, 360);
                    if (sel >= 0 && sel < save_list.count &&
                        save_list.items[sel].line[0] != '\0') {
                        (void)gfx_draw_text_line(gfx,
                                                 save_list.items[sel].line,
                                                 120, 1010, 26, 255, 255,
                                                 255, 255);
                    }
                }
                for (li = 0; li < save_list.count; ++li) {
                    char row[256];

                    fvp_savelist_row_text(&save_list, li, row,
                                          sizeof(row));
                    (void)gfx_draw_text_line(gfx, row,
                                             save_list.x + 16,
                                             save_list.y + li * save_list.row_h + 18,
                                             28, 225, 225, 230, 255);
                }
            } else {
                title_draw_overlay(gfx, &title, title_profile, title_hover);
            }
        }
        canvas = gfx_canvas(gfx, &cw, &ch);
        /* --shot-at 已经存过中间态时，结尾不要再覆盖同一路径 */
        if (gmenu.active && gfx != NULL) {
            draw_game_menu(gfx, &gmenu);
        }
        if (canvas != NULL && opts.screenshot[0] != L'\0' && !shot_taken) {
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
        /* PrimHit(76)：脚本拿鼠标位置做命中测试的频率与命中率（真机才有意义）。 */
        APPEND("prim hit       %llu call(s), %llu hit(s)\n",
               (unsigned long long)vm.prim_hit_calls,
               (unsigned long long)vm.prim_hit_hits);
        /* 字形兜底：某套图集缺字时从别的图集缩放取（tofu=0 表示没有黑方块）。 */
        APPEND("glyphs         %d tofu, %d from fallback atlas\n",
               gfx_stats(gfx)->tofu_glyphs,
               gfx_stats(gfx)->fallback_glyphs);
        APPEND("text prims     %d drawn, %d bound (PrimSetText 86, --text-prims %s)\n",
               gfx_stats(gfx)->text_prims, gfx_text_prim_bindings(gfx),
               opts.text_prims ? "on" : "off");
        APPEND("title menu     %s (profile=%s, align=%s, %d item(s), "
               "pitch %d, continue=%s (%d slot(s), %d readable), picked %s)\n",
               opts.title_menu ? "enabled" : "off",
               title_profile != NULL ? title_profile->game : "generic",
               (title_profile != NULL &&
                title_profile->align == FVP_TITLE_ALIGN_CENTER)
                   ? "centre" : "right",
               FVP_TITLE_ITEMS, title.pitch,
               title.enabled[FVP_TITLE_CONTINUE] ? "enabled" : "disabled",
               title_count_save_slots(opts.game_dir), save_list.count,
               title_picked >= 0
                   ? (title_profile != NULL
                          ? title_profile->items[title_picked].label
                          : fvp_title_label(title_picked))
                   : "-");
        if (gfx_stats(gfx)->ntofu_cp > 0) {
            int ti;

            APPEND("glyphs missing");
            for (ti = 0; ti < gfx_stats(gfx)->ntofu_cp; ++ti) {
                APPEND(" U+%04X", (unsigned)gfx_stats(gfx)->tofu_cp[ti]);
            }
            APPEND("\n");
        }
        APPEND("control        mask=%d pulse=%d cursor=%d pos=%d,%d "
               "(mask %llu / pulse %llu / change %llu / move %llu)\n",
               (int)vm.control_mask, (int)vm.pulse, vm.cursor_shape,
               vm.cursor_x, vm.cursor_y,
               (unsigned long long)vm.control_calls[0],
               (unsigned long long)vm.control_calls[1],
               (unsigned long long)vm.control_calls[2],
               (unsigned long long)vm.control_calls[3]);
        APPEND("parts          motion %llu / pause %llu / stop %llu call(s) "
               "; rgb %llu call(s) ; group move %llu call(s)\n",
               (unsigned long long)vm.parts_motion_calls,
               (unsigned long long)vm.parts_motion_pause_calls,
               (unsigned long long)vm.parts_motion_stop_calls,
               (unsigned long long)vm.parts_rgb_calls,
               (unsigned long long)vm.prim_group_move_calls);
        APPEND("snow           layer=%d,%d (start %llu / stop %llu), prim "
               "%llu call(s)\n", (int)vm.snow_on[0], (int)vm.snow_on[1],
               (unsigned long long)vm.snow_start_calls,
               (unsigned long long)vm.snow_stop_calls,
               (unsigned long long)vm.prim_snow_calls);
        if (vm.snow_calls > 0) {
            int si;

            APPEND("snow full      %d call(s), layer=%d, arg2..14 =",
                   vm.snow_calls, vm.snow_layer);
            for (si = 1; si < 14; ++si) {
                APPEND(" %d", vm.snow_vals[si]);
            }
            APPEND(", opt mask=0x%x\n", (unsigned)vm.snow_opt_mask);
        }
        APPEND("cursor         show=%d ; exit dialog %llu call(s) ; movie "
               "stop %llu call(s)\n", (int)vm.cursor_show,
               (unsigned long long)vm.exit_dialog_calls,
               (unsigned long long)vm.movie_stop_calls);
        APPEND("input          click=%d repeat=%d flash=%llu setclick=%llu\n",
               (int)vm.input_click, vm.input_repeat,
               (unsigned long long)vm.input_flash_calls,
               (unsigned long long)vm.input_setclick_calls);
        APPEND("pause          text %llu call(s), motion %llu call(s)\n",
               (unsigned long long)vm.text_pause_calls,
               (unsigned long long)vm.motion_pause_calls);
        APPEND("fonts          %d name(s), current=%d\n",
               fvp_vm_font_count(&vm), fvp_vm_font_get(&vm));
        APPEND("save/load      request slot=%d (load %llu call(s), save "
               "%llu call(s)); shell did %d write(s), refused %d load(s)\n",
               vm.request_slot, (unsigned long long)vm.load_calls,
               (unsigned long long)vm.save_calls, save_writes,
               load_requests);
        APPEND("rand           state=%u ; text repaint %llu call(s)\n",
               (unsigned)vm.rand_state,
               (unsigned long long)vm.text_repaint_calls);
        APPEND("threads        raise %llu call(s), last ms=%d, woken %llu\n",
               (unsigned long long)vm.thread_raise_calls, vm.thread_raise_ms,
               (unsigned long long)vm.thread_raise_woken);
        APPEND("lip            anim %llu call(s) id=%d mode=%d stop %llu ; sync %llu "
               "call(s) id=%d on=%d\n", (unsigned long long)vm.lip_anim_calls,
               vm.lip_anim_id, vm.lip_anim_mode,
               (unsigned long long)vm.lip_anim_stop_calls,
               (unsigned long long)vm.lip_sync_calls, vm.lip_sync_id,
               vm.lip_sync_on);
        APPEND("gaiji          %llu call(s), size=%d name=%s / %s\n",
               (unsigned long long)vm.gaiji_calls, vm.gaiji_size,
               vm.gaiji_name1 != NULL ? vm.gaiji_name1 : "-",
               vm.gaiji_name2 != NULL ? vm.gaiji_name2 : "-");
        APPEND("v3d            %llu call(s), time=%d ms mode=%d\n",
               (unsigned long long)vm.v3d_calls, vm.v3d_time_ms, vm.v3d_mode);
        APPEND("window         mode=%d state=%d (%llu call(s)); exit mode=%d "
               "(%llu call(s))\n", vm.window_mode_last, vm.window_state,
               (unsigned long long)vm.window_mode_calls, vm.exit_mode_last,
               (unsigned long long)vm.exit_mode_calls);
        APPEND("text buff      %llu call(s)\n",
               (unsigned long long)vm.text_buff_calls);
        {
            int li;

            for (li = 0; li < GFX_TEXT_MAX; ++li) {
                int oa = 0;
                int ob = 0;

                if (gfx_text_get_out_size(gfx, li, &oa, &ob)) {
                    APPEND("text out       layer %d: a=%d b=%d (TextOutSize)\n",
                           li, oa, ob);
                }
            }
        }
        APPEND("graph rgb      %llu call(s), folder=%s (folder calls %llu)\n",
               (unsigned long long)vm.graph_rgb_calls,
               vm.proj_folder != NULL ? vm.proj_folder : "-",
               (unsigned long long)vm.proj_folder_calls);
        APPEND("history        count=%u pos=%u filled=%d set=%llu\n",
               (unsigned)vm.history.count, (unsigned)vm.history.pos,
               fvp_history_filled(&vm.history),
               (unsigned long long)vm.history.sets);
        APPEND("audio          ok=%d submitted=%d completed=%d underruns=%d\n",
               a_ok, a_sub, a_done, a_under);
        /* 真机上仍然没有语义的命令（fvm 里的直方图会把 gfx 命令也算进去，
         * 因为 fvm 不挂 gfx；这里才是「接了 gfx/音频之后还剩哪些桩」）。 */
        if (vm.nunknown > 0) {
            size_t ui;
            size_t shown_u = opts.unknown_all ? vm.nunknown : 12u;

            if (shown_u > vm.nunknown) {
                shown_u = vm.nunknown;
            }
            if (fvp_vm_stalled(&vm)) {
            APPEND("stall          pc=0x%x after %llu steps without a syscall\n",
                   (unsigned)vm.stall_pc, (unsigned long long)vm.stall_span);
        } else {
            APPEND("stall          none\n");
        }
        APPEND("unimpl cmds    %llu distinct:", (unsigned long long)vm.nunknown);
            for (ui = 0; ui < shown_u; ++ui) {
                uint16_t cid = vm.unknown_order[ui];

                APPEND(" %u×%lu", (unsigned)cid,
                       (unsigned long)vm.unknown_count[cid]);
            }
            APPEND("%s\n", opts.unknown_all ? "" : " (top 12)");
        } else {
            APPEND("unimpl cmds    none\n");
        }
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