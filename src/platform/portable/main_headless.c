/* FVP 无窗口（headless）外壳 —— 跨平台验证用的参考平台外壳。
 *
 * 它做三件事，全部与 Win32 无关：
 *   1. 用 src/platform/platform.h 的 OS 缝（时钟/日志/读文件）；
 *   2. 加载 .hcb、初始化 VM、**挂上可移植渲染层** src/gfx.c（离屏 1920x1080）；
 *   3. 跑 N 条指令后把画布写成 BMP，并可把文本行写成文件。
 *
 * 用途：
 *   - 证明"核心 + 渲染层"在没有窗口/没有 D3D/没有 Win32 的环境里也能跑；
 *   - 给 CI 一条不依赖桌面的冒烟路径（`make headless`）；
 *   - 和 `fvm` 的文本输出做逐字节对照（同一份脚本、同样步数）。
 *
 * 注意：没有窗口、没有输入、没有音频。真正的桌面体验仍由 src/platform/win32 提供。
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "encoding.h"
#include "gfx.h"
#include "hcb.h"
#include "platform.h"
#include "save.h"
#include "save_vm.h"
#include "vm.h"

#define CANVAS_W 1920
#define CANVAS_H 1080

static void usage(FILE *f, const char *argv0)
{
    fprintf(f, "usage: %s <script.hcb> [options]\n", argv0);
    fprintf(f, "  --enc cp932|gbk   script text encoding (default cp932)\n");
    fprintf(f, "  --steps N         instruction budget (default 200000)\n");
    fprintf(f, "  --text-out FILE   write the text lines as they appear\\n");
    fprintf(f, "  --pctrace FILE    write one executed pc per line (diagnostics)\\n");
    fprintf(f, "  --screenshot FILE write the canvas as a 32-bit BMP\n");
    fprintf(f, "  --savebin FILE    seed globals from a save/save.bin\n");
    fprintf(f, "  --dump-scene FILE export the FVPS v2 scene "
               "(input for bin/fvp.exe --scene)\n");
    fprintf(f, "  --quiet           do not echo text\n");
    fprintf(f, "  --bool-type       faithful bool semantics "
               "(comparisons push FVP_NULL for false; changes output)\n");
    fprintf(f, "  --faithful-ops    execute op 0x08/0x09, pushtemp and the "
               "DissolveWait/Motion*Test returns as the exe bytes say "
               "(ref/exe-vm-ops.json)\n");
    fprintf(f, "  --primhit-all     PrimHit always answers hit "
               "(diagnostic: push the script past the title menu)\n");
    fprintf(f, "  --click N         synthetic click every N steps\n");
    fprintf(f, "  --text-fixed-layout  legacy text layout (synthetic box;\n");
    fprintf(f, "                    used by tests/verify_headless.py to exercise\n");
    fprintf(f, "                    the renderer without an asset host)\n");
}

/* 32 位 BMP（自上而下，BGRA 源画布转 BGR，alpha 丢）。返回 1 成功。 */
static int write_bmp(const char *path, const unsigned char *rgba, int w, int h)
{
    FILE *f;
    unsigned char hdr[54];
    int stride = (w * 3 + 3) & ~3;
    int bytes = stride * h;
    unsigned char pad[3] = {0, 0, 0};
    int y;

    if (path == NULL || rgba == NULL || w <= 0 || h <= 0) {
        return 0;
    }
    f = fopen(path, "wb");
    if (f == NULL) {
        return 0;
    }
    memset(hdr, 0, sizeof hdr);
    hdr[0] = 'B';
    hdr[1] = 'M';
    hdr[2] = (unsigned char)((54 + bytes) & 0xFF);
    hdr[3] = (unsigned char)(((54 + bytes) >> 8) & 0xFF);
    hdr[4] = (unsigned char)(((54 + bytes) >> 16) & 0xFF);
    hdr[5] = (unsigned char)(((54 + bytes) >> 24) & 0xFF);
    hdr[10] = 54;
    hdr[14] = 40;
    hdr[18] = (unsigned char)(w & 0xFF);
    hdr[19] = (unsigned char)((w >> 8) & 0xFF);
    hdr[20] = (unsigned char)((w >> 16) & 0xFF);
    hdr[21] = (unsigned char)((w >> 24) & 0xFF);
    hdr[22] = (unsigned char)(h & 0xFF);
    hdr[23] = (unsigned char)((h >> 8) & 0xFF);
    hdr[24] = (unsigned char)((h >> 16) & 0xFF);
    hdr[25] = (unsigned char)((h >> 24) & 0xFF);
    hdr[26] = 1;
    hdr[28] = 24; /* bpp */
    hdr[34] = (unsigned char)(bytes & 0xFF);
    hdr[35] = (unsigned char)((bytes >> 8) & 0xFF);
    hdr[36] = (unsigned char)((bytes >> 16) & 0xFF);
    hdr[37] = (unsigned char)((bytes >> 24) & 0xFF);
    if (fwrite(hdr, 1, sizeof hdr, f) != sizeof hdr) {
        fclose(f);
        return 0;
    }
    for (y = h - 1; y >= 0; --y) {
        const unsigned char *row = rgba + (size_t)y * (size_t)w * 4u;
        int x;

        for (x = 0; x < w; ++x) {
            unsigned char bgr[3];

            bgr[0] = row[x * 4 + 2];
            bgr[1] = row[x * 4 + 1];
            bgr[2] = row[x * 4 + 0];
            if (fwrite(bgr, 1, 3, f) != 3) {
                fclose(f);
                return 0;
            }
        }
        if (stride > w * 3) {
            if (fwrite(pad, 1, (size_t)(stride - w * 3), f) !=
                (size_t)(stride - w * 3)) {
                fclose(f);
                return 0;
            }
        }
    }
    fclose(f);
    return 1;
}

/* 收集 on_text 回调的文本行，最后一次性写文件（与 fvm --text-out 的格式一致）。 */
typedef struct {
    FILE *f;
    int lines;
    FILE *steps;      /* --text-out-steps：每行带 VM 步数，便于定位某句台词出现在哪一步 */
    FvpVM *vm;
} TextSink;

static void text_sink(void *ud, int layer, const char *text, int clear)
{
    TextSink *sink = (TextSink *)ud;

    (void)clear;
    if (sink == NULL || sink->f == NULL || text == NULL) {
        return;
    }
    fprintf(sink->f, "[%d] %s\r\n", layer, text);
    if (sink->steps != NULL) {
        fprintf(sink->steps, "%llu\t[%d] %s\n",
                (unsigned long long)sink->vm->steps, layer, text);
    }
    sink->lines += 1;
}

/* --savebin：把 save/save.bin 里已确证的全局变量灌进 VM（与 fvm 的 --savebin 同义）。
 * 用来验证“原版首次启动会读存档”这个环境差异是否影响脚本分支。 */
static int headless_seed_globals(const char *path, FvpVM *vm)
{
    FvpSaveBin sb;
    FvpSaveGlobalsStats st;
    int written;

    memset(&sb, 0, sizeof sb);
    if (!fvp_save_bin_load_file(&sb, path)) {
        fprintf(stderr, "fvp-headless: %s is not a valid save.bin\n", path);
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
    return 1;
}

int main(int argc, char **argv)
{
    const char *script = NULL;
    const char *encname = "cp932";
    const char *text_out = NULL;
    const char *pctrace = NULL;
    const char *shot = NULL;
    const char *text_steps = NULL;
    const char *savebin = NULL;
    const char *dump_scene = NULL; /* --dump-scene: 导出 FVPS v2 场景（--scene 的输入） */
    TextSink sink;
    unsigned long long steps = 200000ULL;
    unsigned long long click = 0;
    int quiet = 0;
    int text_fixed = 0;
    int primhit_all = 0;
    int bool_type = 0; /* --bool-type：原版的“类型即真假”语义（默认关，影响冻结基线） */ /* --primhit-all：诊断用，PrimHit 一律命中 */
    int cmp_matrix = 0; /* --cmp-matrix：只换 0x22..0x27 的类型对矩阵 */
    int faithful_ops = 0; /* --faithful-ops：按 exe 字节执行 0x08/0x09 等（见 ref/exe-vm-ops.json） */
    int click_mask = 0x0B;
    int frame_ms = 16;
    int i;
    FvpEncoding enc = FVP_ENC_CP932;
    char err[512];
    FvpHcb hcb;
    FvpVM vm;
    Gfx *gfx;
    double t0;
    uint64_t done;

    for (i = 1; i < argc; ++i) {
        const char *a = argv[i];

        if (strcmp(a, "--enc") == 0 && i + 1 < argc) {
            encname = argv[++i];
        } else if (strcmp(a, "--steps") == 0 && i + 1 < argc) {
            steps = strtoull(argv[++i], NULL, 10);
        } else if (strcmp(a, "--pctrace") == 0 && i + 1 < argc) {
        pctrace = argv[++i];
    } else if (strcmp(a, "--text-out") == 0 && i + 1 < argc) {
            text_out = argv[++i];
        } else if (strcmp(a, "--screenshot") == 0 && i + 1 < argc) {
            shot = argv[++i];
        } else if (strcmp(a, "--text-out-steps") == 0 && i + 1 < argc) {
            text_steps = argv[++i];
        } else if (strcmp(a, "--savebin") == 0 && i + 1 < argc) {
            savebin = argv[++i];
        } else if (strcmp(a, "--dump-scene") == 0 && i + 1 < argc) {
            dump_scene = argv[++i];
        } else if (strcmp(a, "--frame-ms") == 0 && i + 1 < argc) {
            frame_ms = (int)strtol(argv[++i], NULL, 0);
        } else if (strcmp(a, "--click-mask") == 0 && i + 1 < argc) {
            click_mask = (int)strtol(argv[++i], NULL, 0);
        } else if (strcmp(a, "--click") == 0 && i + 1 < argc) {
            click = strtoull(argv[++i], NULL, 10);
        } else if (strcmp(a, "--text-fixed-layout") == 0) {
            text_fixed = 1;
        } else if (strcmp(a, "--primhit-all") == 0) {
            primhit_all = 1;
        } else if (strcmp(a, "--bool-type") == 0) {
            bool_type = 1;
        } else if (strcmp(a, "--cmp-matrix") == 0) {
            cmp_matrix = 1;
        } else if (strcmp(a, "--faithful-ops") == 0) {
            faithful_ops = 1;
        } else if (strcmp(a, "--quiet") == 0) {
            quiet = 1;
        } else if (a[0] == '-' && a[1] != '\0') {
            fprintf(stderr, "fvp-headless: unknown option %s\n", a);
            usage(stderr, argv[0]);
            return 2;
        } else if (script == NULL) {
            script = a;
        } else {
            fprintf(stderr, "fvp-headless: unexpected argument %s\n", a);
            return 2;
        }
    }
    if (script == NULL) {
        usage(stderr, argv[0]);
        return 2;
    }
    if (!fvp_encoding_from_name(encname, &enc)) {
        fprintf(stderr, "fvp-headless: unknown encoding %s\n", encname);
        return 2;
    }

    fvp_plat_log("fvp-headless: platform seam = portable (no Win32)\n");

    memset(&hcb, 0, sizeof hcb);
    if (!fvp_hcb_load(&hcb, script, enc, err, sizeof err)) {
        fprintf(stderr, "fvp-headless: %s\n", err);
        return 3;
    }
    memset(&sink, 0, sizeof sink);
    sink.vm = &vm;
    if (text_steps != NULL) {
        sink.steps = fopen(text_steps, "wb");
        if (sink.steps == NULL) {
            fprintf(stderr, "fvp-headless: cannot write %s\n",
                    text_steps);
        }
    }
    memset(&vm, 0, sizeof vm);
    fvp_vm_init(&vm, &hcb, quiet);
    vm.click_period = (int)click;
    /* 合成点击的位掩码：原版 InputGetDown 返回 engine+0x43C 那张位掩码
     * （0x4310A4 处就有 `test %edx,0x43c(%ecx)` 测位），脚本又用 `test`
     * （(a & (1<<b)) != 0）逐位取。扫 iroseka.hcb 里 syscall 31 附近的
     * pushint8 位号：bit1 41 次、bit3 16 次、bit0 5 次、其余(2/4/8/10) 1-3 次。
     * 只给 bit0 时那些等 bit1/bit3 的循环永远退不出来（CHANGELOG 四十九续）。
     * 键位与按钮的完整对应还没逆完，这里只保证"点击能满足脚本轮询的位"，
     * 属于合成输入模型的一部分；--click-mask 可改。 */
    vm.click_mask = click_mask;
    /* 打开 VM 的虚拟时钟（--timeline 等价）：`fvp_vm_run` 只在 timeline_on 时
     * 把所有线程都阻塞的等待推进到下一个唤醒点（timeline_advance）。headless 没有
     * 真实帧循环，关着时钟时 `ThreadRaise(ms)`/定时器语义只能"醒来即继续"，
     * 脚本里等时间的循环就永远等不到（CHANGELOG 六十二续的候选 3）。 */
    if (getenv("FVP_NO_TIMELINE") == NULL) {
        vm.timeline_on = 1;
    }
    /* 光标默认放画面中央（原版靠鼠标，headless 没有鼠标）：PrimHit 需要坐标，
     * 放在 (0,0) 时菜单项永远不命中，脚本停在标题（CHANGELOG 五十九续）。 */
    vm.cx = CANVAS_W / 2;
    vm.cy = CANVAS_H / 2;
    /* FVP_CURSOR="x,y"：把光标放到指定画布坐标，用来试菜单命中
     * （headless 没有鼠标；PrimHit 需要 vm.cx/cy）。 */
    {
        const char *cur = getenv("FVP_CURSOR");
        int cx0 = 0;
        int cy0 = 0;

        if (cur != NULL && sscanf(cur, "%d,%d", &cx0, &cy0) == 2) {
            vm.cx = cx0;
            vm.cy = cy0;
            fprintf(stderr, "fvp-headless: cursor=%d,%d\n", cx0, cy0);
        }
    }

    {
        /* 可选素材宿主：`FVP_GAME_DIR=<游戏目录>` 时用 src/gfx.c 里已有的
         * GfxArchiveHost（读 .bin + HZC1 解码成 RGBA，见 gfx_archive_host_create）。
         * 没有素材时菜单图元没有尺寸，PrimHit(76) 永远不命中，脚本会停在标题
         * （CHANGELOG 五十六续）；给了素材才可能走进正文。 */
        static GfxArchiveHost *arc_host = NULL;
        const char *gd = getenv("FVP_GAME_DIR");
        char dirbuf[4096];
        const GfxHost *host = NULL;

        if (gd == NULL && script != NULL) {
            /* 没给 FVP_GAME_DIR 时，从脚本路径推目录：脚本路径是命令行实参，
             * 它的编码链路是可靠的（中文目录下的 iroseka.hcb 一直能读），
             * 而环境变量那一跳会走 CRT 的 ANSI 转换，中文会坏（CHANGELOG 五十八续）。 */
            const char *p = script;
            const char *slash = NULL;
            size_t n;

            while (*p != '\0') {
                if (*p == '/' || *p == '\\') {
                    slash = p;
                }
                ++p;
            }
            if (slash != NULL) {
                n = (size_t)(slash - script);
                if (n > 0 && n < sizeof dirbuf) {
                    memcpy(dirbuf, script, n);
                    dirbuf[n] = '\0';
                    gd = dirbuf;
                }
            }
        }

        if (gd != NULL && gd[0] != '\0') {
            arc_host = gfx_archive_host_create_ex(gd, 1);
            if (arc_host != NULL) {
                host = gfx_archive_host_iface(arc_host);
                fprintf(stderr, "fvp-headless: assets <- %s (%d images)\n", gd,
                        gfx_archive_host_image_count(arc_host));
            } else {
                fprintf(stderr, "fvp-headless: cannot open assets in %s\n", gd);
            }
        }
        gfx = gfx_create(CANVAS_W, CANVAS_H, host);
    }
    if (gfx == NULL) {
        fprintf(stderr, "fvp-headless: gfx_create failed\n");
        fvp_vm_free(&vm);
        fvp_hcb_free(&hcb);
        return 4;
    }
    /* 旧版面（合成对话框 + 每层按版面表画）只在显式要求时打开：
     * 默认是图元驱动（原版行为），而这个外壳没有图片/字库宿主，
     * 图元驱动下画不出任何东西（见 tests/verify_headless.py）。 */
    if (text_fixed) {
        gfx_set_text_fixed_layout(gfx, 1);
    }
    /* 打开补间表：关着时 Motion* 只沿时间轴走、不登记到可查询状态，连
     * MotionXxxTest 也永远答"没有在跑的动作"，与脚本期待相反。headless 用
     * ThreadNext 计数当帧时钟（见下），所以这里必须真记录。 */
    gfx_motion_set_enabled(gfx, 1);
    if (primhit_all) {
        gfx_set_prim_hit_all(gfx, 1);
    }
    vm.bool_type = bool_type;
    vm.faithful_ops = faithful_ops;
    vm.cmp_matrix = cmp_matrix;
    fvp_vm_attach_gfx(&vm, gfx);
    if (text_out != NULL) {
        sink.f = fopen(text_out, "wb");
        if (sink.f == NULL) {
            fprintf(stderr, "fvp-headless: cannot write %s\n", text_out);
            gfx_destroy(gfx);
            fvp_vm_free(&vm);
            fvp_hcb_free(&hcb);
            return 5;
        }
    }
    /* 只要打开了任意一个文本输出（含 --text-out-steps），就挂上文本回调：
     * 带步数的落盘依赖同一个回调。 */
    if ((sink.f != NULL || sink.steps != NULL) && vm.gfx_bridge != NULL) {
        vm.gfx_bridge->on_text = text_sink;
        vm.gfx_bridge->text_ud = &sink;
    }

    t0 = fvp_plat_now_sec();
    if (pctrace != NULL) {
        vm.pctrace = fopen(pctrace, "wb");
    }
    if (savebin != NULL) {
        (void)headless_seed_globals(savebin, &vm);
    }

    /* 允许从指定脚本 pc 起步（环境变量 FVP_START_PC，十六进制/十进制均可）：
     * 没有素材宿主时脚本会停在标题/菜单（PrimHit 永远不命中，见 CHANGELOG 五十六续），
     * 想验证正文渲染就得绕过标题。仅诊断/验证用，默认仍从 main_start 起跑。 */
    {
        const char *sp = getenv("FVP_START_PC");

        if (sp != NULL && sp[0] != '\0') {
            fvp_vm_start_at(&vm, (unsigned)strtoul(sp, NULL, 0));
        } else {
            fvp_vm_start(&vm);
        }
    }
    /* 无逐帧循环外壳的动画时钟：原版主循环里所有线程都让出一次就是一帧，
     * 脚本用 ThreadNext(133) 让出。这里按分块跑，把每块新增的 ThreadNext 次数
     * 换算成帧并推进补间表（每帧 --frame-ms 毫秒，默认 16 = 60 fps 假设）。
     * 不做这件事时 MotionXxxTest 永远答还在跑，脚本卡在 ThreadNext 轮询里
     * （200k 步里 8061 次 ThreadNext，见 CHANGELOG 五十一续）。 */
    {
        uint64_t target = 0;
        uint64_t prev_next = vm.thread_next_calls;

        while (vm.steps < steps) {
            target += 2000u;
            if (target > steps) {
                target = steps;
            }
            (void)fvp_vm_run(&vm, target);
            if (vm.thread_next_calls != prev_next) {
                uint64_t d = vm.thread_next_calls - prev_next;

                prev_next = vm.thread_next_calls;
                if (d > 100000u) {
                    d = 100000u;
                }
                gfx_motion_advance(gfx, (int)(d * (uint64_t)frame_ms));
            }
            if (vm.steps < target) {
                break; /* 线程都退出了 */
            }
        }
        done = vm.steps;
    }
    gfx_render(gfx);
    if (dump_scene != NULL) {
        if (gfx_scene_export(gfx, dump_scene)) {
            printf("headless: scene -> %s\n", dump_scene);
        } else {
            fprintf(stderr, "fvp-headless: cannot write %s\n",
                    dump_scene);
        }
    }
    printf("headless: primhit=%llu/%llu threadnext=%llu motion_active=%d "
           "steps=%llu in %.1f ms\n",
           (unsigned long long)vm.prim_hit_calls,
           (unsigned long long)vm.prim_hit_hits,
           (unsigned long long)vm.thread_next_calls, gfx_motion_active(gfx),
           (unsigned long long)done,
           (fvp_plat_now_sec() - t0) * 1000.0);

    if (shot != NULL) {
        int cw = 0;
        int ch = 0;
        const uint8_t *rgba = gfx_canvas_rgba(gfx, &cw, &ch);

        if (rgba == NULL) {
            const uint32_t *px = gfx_canvas(gfx, &cw, &ch);

            if (px != NULL) {
                /* gfx 内部画布是 0xAARRGGBB，按字节写成 BMP（BGRA→BGR） */
                if (write_bmp(shot, (const unsigned char *)px, cw, ch)) {
                    printf("headless: screenshot -> %s (%dx%d)\n", shot, cw,
                           ch);
                }
            }
        } else if (write_bmp(shot, rgba, cw, ch)) {
            printf("headless: screenshot -> %s (%dx%d)\n", shot, cw, ch);
        }
    }

    if (sink.f != NULL) {
        fclose(sink.f);
        sink.f = NULL;
        printf("headless: text lines -> %s (%d line(s))\n", text_out,
               sink.lines);
    }
    gfx_destroy(gfx);
    fvp_vm_free(&vm);
    fvp_hcb_free(&hcb);
    return 0;
}