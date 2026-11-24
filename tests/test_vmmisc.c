/* Rand(92) / TextRepaint(124) 的单元测试。
 *
 *   bin/test_vmmisc.exe
 *
 * 语义来自静态分析：
 *   Rand(92)  handler 0x437a20：retval 标成 int，`call 0x4483da`（MSVC CRT rand()），
 *             返回值 = 0..RAND_MAX(0x7FFF)。本站用固定种子的 xorshift32 实现，
 *             保证同一次运行可复现。
 *   TextRepaint(124) handler 0x437a50：`call 0x441790(engine)` 让文本重画；本站每帧
 *             整张画布重画，所以是真空操作，只记次数。
 *   字体表 TextFontCount/Get/Name/Set(114..117)：原版是 engine+0x94 的
 *             vector<font>；本站用外壳注册的名字表模拟（fvm 不注册 → 空表）。
 *             Set 的下界是 -5（原版行为，取自静态分析），Name 只收 0..count-1。
 *   Load(40) / SaveWrite(96) handler 0x43a870 / 0x43c1e0：槽号必须是 int 0..999，
 *             然后只把"请求 + 槽号"记进引擎字段（+0xfcb / +0xfca / +0xfcc），
 *             文件读写由外壳做。本站用 take 语义取走请求（和 InputGetWheel 一致）。
 */
#include <stdio.h>
#include <string.h>

#include "../src/vm.h"

static int g_fail = 0;
static int g_pass = 0;

#define CHECK(cond, ...)                                                      \
    do {                                                                      \
        if (cond) {                                                           \
            ++g_pass;                                                         \
        } else {                                                              \
            ++g_fail;                                                         \
            printf("FAIL %s:%d: ", __FILE__, __LINE__);                       \
            printf(__VA_ARGS__);                                              \
            printf("\n");                                                     \
        }                                                                     \
    } while (0)

int main(void)
{
    FvpVM a;
    FvpVM b;
    int i;
    int saw_variation = 0;
    int prev;
    FvpValue slot5 = fvp_make_int(5);
    FvpValue slot999 = fvp_make_int(999);
    FvpValue slot1000 = fvp_make_int(1000);
    FvpValue slot_neg = fvp_make_int(-1);
    FvpValue fl = fvp_make_float(3.0);

    fvp_vm_init(&a, NULL, 1);
    fvp_vm_init(&b, NULL, 1);

    /* --- 1. 范围：0..0x7FFF（MSVC RAND_MAX） --- */
    prev = fvp_vm_rand(&a);
    CHECK(prev >= 0 && prev <= 0x7FFF, "first rand in range (%d)", prev);
    for (i = 0; i < 2000; i++) {
        int v = fvp_vm_rand(&a);

        CHECK(v >= 0 && v <= 0x7FFF, "rand %d in range (%d)", i, v);
        if (v != prev) {
            saw_variation = 1;
        }
        prev = v;
    }
    CHECK(saw_variation, "sequence is not constant");

    /* --- 2. 可复现：两个同初始状态的 VM 序列完全一致（a 先重置）--- */
    fvp_vm_free(&a);
    fvp_vm_init(&a, NULL, 1);
    for (i = 0; i < 64; i++) {
        int va = fvp_vm_rand(&a);
        int vb = fvp_vm_rand(&b);

        CHECK(va == vb, "deterministic at step %d (%d vs %d)", i, va, vb);
    }

    /* --- 3. NULL 安全 --- */
    CHECK(fvp_vm_rand(NULL) == 0, "NULL vm rand");

    /* --- 4. TextRepaint 只计数 --- */
    CHECK(a.text_repaint_calls == 0, "fresh repaint counter");
    a.text_repaint_calls += 1u;
    a.text_repaint_calls += 1u;
    CHECK(a.text_repaint_calls == 2, "repaint counter (%llu)",
          (unsigned long long)a.text_repaint_calls);

    /* --- 4f. PartsMotion(66/67/68) / PartsRGB(70) / PrimGroupMove(74) --- */
    {
        FvpValue p0 = fvp_make_int(0);
        FvpValue p1 = fvp_make_int(1);
        FvpValue p63 = fvp_make_int(63);
        FvpValue p64 = fvp_make_int(64);
        FvpValue on1 = fvp_make_int(1);
        FvpValue on2 = fvp_make_int(2);
        FvpValue n50 = fvp_make_int(50);
        FvpValue n200 = fvp_make_int(200);
        FvpValue n201 = fvp_make_int(201);
        FvpValue n256 = fvp_make_int(256);
        FvpValue t0 = fvp_make_int(0);
        FvpValue t1000 = fvp_make_int(1000);
        FvpValue t300000 = fvp_make_int(300000);
        FvpValue t300001 = fvp_make_int(300001);
        FvpValue g4095 = fvp_make_int(4095);
        FvpValue g4096 = fvp_make_int(4096);

        CHECK(a.parts_motion_calls == 0 && a.parts_rgb_calls == 0 &&
              a.prim_group_move_calls == 0, "fresh parts counters");
        CHECK(fvp_vm_parts_motion(&a, &p0, &n50, &t1000) == 1,
              "parts motion ok");
        CHECK(a.parts_motion_calls == 1, "motion counter (%llu)",
              (unsigned long long)a.parts_motion_calls);
        CHECK(a.parts_motion[0] == 50u && a.parts_motion_time[0] == 1000u,
              "记录 kind/时长 (%u/%u)", (unsigned)a.parts_motion[0],
              a.parts_motion_time[0]);
        CHECK(fvp_vm_parts_motion(&a, &p63, &n200, &t300000) == 1,
              "partsId 63 + 时长上限 300000 ok");
        CHECK(fvp_vm_parts_motion(&a, &p64, &n50, &t1000) == 0,
              "partsId 64 拒绝");
        CHECK(fvp_vm_parts_motion(&a, &slot_neg, &n50, &t1000) == 0,
              "partsId -1 拒绝");
        CHECK(fvp_vm_parts_motion(&a, &p1, NULL, &t1000) == 0,
              "NULL kind 拒绝");
        CHECK(fvp_vm_parts_motion(&a, &p1, &fl, &t1000) == 0,
              "float kind 拒绝");
        CHECK(fvp_vm_parts_motion(&a, &p1, &n256, &t1000) == 0,
              "kind 256 拒绝");
        CHECK(fvp_vm_parts_motion(&a, &p1, &n50, &t0) == 0, "时长 0 拒绝");
        CHECK(fvp_vm_parts_motion(&a, &p1, &n50, &t300001) == 0,
              "时长 300001 拒绝");
        CHECK(fvp_vm_parts_motion(&a, &p1, &n50, NULL) == 0, "NULL 时长 拒绝");
        CHECK(a.parts_motion_calls == 2, "motion counter 只算成功 (%llu)",
              (unsigned long long)a.parts_motion_calls);

        CHECK(fvp_vm_parts_motion_pause(&a, &p1, &on1) == 1, "pause ok");
        CHECK(a.parts_motion_pause_calls == 1, "pause counter (%llu)",
              (unsigned long long)a.parts_motion_pause_calls);
        CHECK(fvp_vm_parts_motion_pause(&a, &p1, &on2) == 0, "on=2 拒绝");
        CHECK(fvp_vm_parts_motion_pause(&a, &p1, NULL) == 0, "NULL on 拒绝");
        CHECK(fvp_vm_parts_motion_pause(&a, &p64, &on1) == 0,
              "pause partsId 64 拒绝");
        CHECK(a.parts_motion_pause_calls == 1, "pause counter 只算成功");
        CHECK(fvp_vm_parts_motion_stop(&a, &p0) == 0, "stop partsId 0 拒绝");
        CHECK(fvp_vm_parts_motion_stop(&a, &p1) == 1, "stop partsId 1 ok");
        CHECK(fvp_vm_parts_motion_stop(&a, &p63) == 1, "stop partsId 63 ok");
        CHECK(fvp_vm_parts_motion_stop(&a, &p64) == 0, "stop partsId 64 拒绝");
        CHECK(a.parts_motion_stop_calls == 2, "stop counter (%llu)",
              (unsigned long long)a.parts_motion_stop_calls);

        CHECK(fvp_vm_parts_rgb(&a, &p1, &n50, &n200, &n201) == 1,
              "parts rgb ok");
        CHECK(a.parts_rgb[1][0] == 50 && a.parts_rgb[1][1] == 200 &&
              a.parts_rgb[1][2] == 100, "分量：201 回落到 100 (%d,%d,%d)",
              a.parts_rgb[1][0], a.parts_rgb[1][1], a.parts_rgb[1][2]);
        CHECK(a.parts_rgb_set[1] == 1u, "parts rgb set");
        CHECK(fvp_vm_parts_rgb(&a, &p64, &n50, &n50, &n50) == 0,
              "partsId 64 拒绝 (rgb)");
        CHECK(a.parts_rgb_calls == 1, "rgb counter (%llu)",
              (unsigned long long)a.parts_rgb_calls);

        CHECK(fvp_vm_prim_group_move(&a, &p0, &p1) == 0, "group id 0 拒绝");
        CHECK(fvp_vm_prim_group_move(&a, &g4095, &p1) == 1, "group id 4095 ok");
        CHECK(a.prim_group_move_last[0] == 4095 &&
              a.prim_group_move_last[1] == 1, "recorded id/target (%d/%d)",
              a.prim_group_move_last[0], a.prim_group_move_last[1]);
        CHECK(fvp_vm_prim_group_move(&a, &g4096, &p1) == 0,
              "group id 4096 拒绝");
        CHECK(fvp_vm_prim_group_move(&a, &p1, &p0) == 0, "target 0 拒绝");
        CHECK(fvp_vm_prim_group_move(&a, &p1, &g4096) == 0,
              "target 4096 拒绝");
        CHECK(fvp_vm_prim_group_move(&a, &p1, &fl) == 0, "float target 拒绝");
        CHECK(fvp_vm_prim_group_move(&a, &p1, NULL) == 0, "NULL target 拒绝");
        CHECK(a.prim_group_move_calls == 1, "group move counter (%llu)",
              (unsigned long long)a.prim_group_move_calls);
        CHECK(fvp_vm_parts_motion(NULL, &p1, &n50, &t1000) == 0, "NULL vm");
        CHECK(fvp_vm_parts_rgb(NULL, &p1, &n50, &n50, &n50) == 0, "NULL vm rgb");
        CHECK(fvp_vm_prim_group_move(NULL, &p1, &p1) == 0, "NULL vm group");
    }

    /* --- 4g. Snow(97)：18 参数大命令（arg1..14 必填、arg15..18 可选） --- */
    {
        /* 基准取值：落在 handler 0x43C230 各条 cmp 的区间内部 */
        const int base[18] = { 0, 100, 200, 300, 2, 2, 1, 10, 100,
                               1, 0,   0,   0,   0, 0, 0, 0,   0 };
        FvpValue v[18];
        FvpValue bad[18];
        int i;

        for (i = 0; i < 18; ++i) {
            v[i] = fvp_make_int(base[i]);
        }
        CHECK(a.snow_calls == 0, "fresh snow(97) counter");
        CHECK(fvp_vm_snow_ex(&a, v, 14) == 1, "14 个必填参数 ok");
        CHECK(a.snow_calls == 1, "snow(97) counter (%d)", a.snow_calls);
        CHECK(a.snow_layer == 0 && a.snow_vals[1] == 100 &&
              a.snow_vals[8] == 100 && a.snow_vals[13] == 0,
              "记录 layer 与 arg2..14");
        CHECK(a.snow_opt_mask == 0u, "没给可选参数 -> mask 0");

        v[14] = fvp_make_int(0xFF);
        v[15] = fvp_make_int(0xFF);
        v[16] = fvp_make_int(0x2710);
        v[17] = fvp_make_int(0xFF);
        CHECK(fvp_vm_snow_ex(&a, v, 18) == 1, "18 个参数 ok");
        CHECK(a.snow_opt_mask == 0xFu, "可选参数全给 -> mask 0xf (%u)",
              (unsigned)a.snow_opt_mask);
        CHECK(a.snow_vals[16] == 0x2710, "记录 arg17 (%d)", a.snow_vals[16]);
        CHECK(a.snow_calls == 2, "snow(97) counter (%d)", a.snow_calls);

        v[14] = fvp_make_int(256); /* arg15 越界 -> 只丢这一个可选参数 */
        CHECK(fvp_vm_snow_ex(&a, v, 18) == 1, "可选参数越界仍生效");
        CHECK(a.snow_opt_mask == 0xEu, "bit0 清掉 -> mask 0xe (%u)",
              (unsigned)a.snow_opt_mask);
        CHECK(a.snow_vals[14] == 0, "arg15 缺省记 0");

        for (i = 0; i < 18; ++i) {
            bad[i] = v[i];
        }
        bad[0] = fvp_make_int(2);
        CHECK(fvp_vm_snow_ex(&a, bad, 18) == 0, "layer 2 拒绝");
        bad[0] = v[0];
        bad[3] = fvp_make_int(0x1000);
        CHECK(fvp_vm_snow_ex(&a, bad, 18) == 0, "arg4 0x1000 拒绝");
        bad[3] = v[3];
        bad[4] = fvp_make_int(1);
        CHECK(fvp_vm_snow_ex(&a, bad, 18) == 0, "arg5 1 拒绝（下界 2）");
        bad[4] = v[4];
        bad[6] = fvp_make_int(0);
        CHECK(fvp_vm_snow_ex(&a, bad, 18) == 0, "arg7 0 拒绝（下界 1）");
        bad[6] = v[6];
        bad[8] = fvp_make_int(0x2711);
        CHECK(fvp_vm_snow_ex(&a, bad, 18) == 0, "arg9 10001 拒绝");
        bad[8] = v[8];
        bad[7] = fvp_make_int(50);
        bad[8] = fvp_make_int(10);
        CHECK(fvp_vm_snow_ex(&a, bad, 18) == 0, "arg8 > arg9 拒绝");
        bad[7] = v[7];
        bad[8] = v[8];
        bad[9] = fvp_make_int(0);
        CHECK(fvp_vm_snow_ex(&a, bad, 18) == 0, "arg10 0 拒绝（下界 1）");
        bad[9] = v[9];
        bad[10] = fvp_make_int(-0x1001);
        CHECK(fvp_vm_snow_ex(&a, bad, 18) == 0, "arg11 -4097 拒绝");
        bad[10] = v[10];
        bad[13] = fvp_make_int(0x401);
        CHECK(fvp_vm_snow_ex(&a, bad, 18) == 0, "arg14 1025 拒绝");
        bad[13] = v[13];
        bad[1] = fl;
        CHECK(fvp_vm_snow_ex(&a, bad, 18) == 0, "float 参数拒绝");
        CHECK(fvp_vm_snow_ex(&a, v, 13) == 0, "只有 13 个参数拒绝");
        CHECK(fvp_vm_snow_ex(&a, NULL, 18) == 0, "NULL args 拒绝");
        CHECK(fvp_vm_snow_ex(NULL, v, 18) == 0, "NULL vm 拒绝");
        CHECK(a.snow_calls == 3, "失败不进计数 (%d)", a.snow_calls);
    }

    /* --- 4h. ThreadRaise(134)：只叫醒 sleep 时长匹配的线程 --- */
    {
        FvpThread t0;
        FvpThread t1;
        FvpThread t2;
        FvpValue ms100 = fvp_make_int(100);
        FvpValue ms250 = fvp_make_int(250);

        memset(&t0, 0, sizeof t0);
        memset(&t1, 0, sizeof t1);
        memset(&t2, 0, sizeof t2);
        t0.blocked = 1; /* 槽 3：sleep 100 -> 会被叫醒 */
        t0.timed = 1;
        t0.sleep_ms = 100;
        t0.wait = 5;
        t1.blocked = 1; /* 槽 4：sleep 250 -> 时长不匹配 */
        t1.timed = 1;
        t1.sleep_ms = 250;
        t2.blocked = 1; /* 槽 5：ThreadWait（sleep_ms = -1）-> 不认 */
        t2.timed = 1;
        t2.sleep_ms = -1;

        CHECK(fvp_vm_thread_raise(&a, &ms100) == 0, "空槽表叫醒 0 个");
        CHECK(a.thread_raise_calls == 1 && a.thread_raise_woken == 0,
              "空槽表也计数 (%llu/%llu)",
              (unsigned long long)a.thread_raise_calls,
              (unsigned long long)a.thread_raise_woken);
        a.thread_slots[3] = &t0;
        a.thread_slots[4] = &t1;
        a.thread_slots[5] = &t2;
        CHECK(fvp_vm_thread_raise(&a, &ms100) == 1, "时长 100 叫醒 1 个");
        CHECK(t0.blocked == 0 && t0.timed == 0 && t0.wait == 0,
              "匹配的线程被完全唤醒");
        CHECK(t1.blocked == 1 && t1.timed == 1, "时长 250 的继续睡");
        CHECK(t2.blocked == 1 && t2.timed == 1, "ThreadWait 的不被叫醒");
        CHECK(fvp_vm_thread_raise(&a, &ms250) == 1, "时长 250 叫醒 1 个");
        CHECK(t1.blocked == 0, "250 的被叫醒");
        CHECK(fvp_vm_thread_raise(&a, &ms100) == 0, "已醒的不会重复计数");
        CHECK(fvp_vm_thread_raise(&a, &fl) == -1, "float 参数拒绝");
        CHECK(fvp_vm_thread_raise(&a, NULL) == -1, "NULL 参数拒绝");
        CHECK(fvp_vm_thread_raise(NULL, &ms100) == -1, "NULL vm 拒绝");
        CHECK(a.thread_raise_calls == 4u && a.thread_raise_ms == 100 &&
              a.thread_raise_woken == 2u,
              "只算合法调用 (%llu/%d/%llu)",
              (unsigned long long)a.thread_raise_calls, a.thread_raise_ms,
              (unsigned long long)a.thread_raise_woken);
        a.thread_slots[3] = NULL;
        a.thread_slots[4] = NULL;
        a.thread_slots[5] = NULL;
    }

    /* --- 4e. SnowStart(98) / SnowStop(99) / PrimSetSnow(84) --- */
    {
        FvpValue l0 = fvp_make_int(0);
        FvpValue l1 = fvp_make_int(1);
        FvpValue l2 = fvp_make_int(2);
        FvpValue on1 = fvp_make_int(1);
        FvpValue off0 = fvp_make_int(0);
        FvpValue p2 = fvp_make_int(2);
        FvpValue p4096 = fvp_make_int(4096);

        CHECK(a.snow_on[0] == 0u && a.snow_on[1] == 0u, "fresh snow off");
        CHECK(fvp_vm_snow(&a, &l0, 1) == 1, "snow layer 0 on");
        CHECK(a.snow_on[0] == 1u && a.snow_on[1] == 0u, "layer 0 only");
        CHECK(fvp_vm_snow(&a, &l1, 1) == 1, "snow layer 1 on");
        CHECK(a.snow_on[1] == 1u, "layer 1 on");
        CHECK(fvp_vm_snow(&a, &l0, 0) == 1, "snow layer 0 off");
        CHECK(a.snow_on[0] == 0u, "layer 0 off");
        CHECK(fvp_vm_snow(&a, &l2, 1) == 0, "layer 2 rejected");
        CHECK(fvp_vm_snow(&a, &slot_neg, 1) == 0, "layer -1 rejected");
        CHECK(fvp_vm_snow(&a, &fl, 1) == 0, "float layer rejected");
        CHECK(fvp_vm_snow(&a, NULL, 1) == 0, "NULL layer rejected");
        CHECK(a.snow_start_calls == 2 && a.snow_stop_calls == 1,
              "snow counters (%llu/%llu)",
              (unsigned long long)a.snow_start_calls,
              (unsigned long long)a.snow_stop_calls);
        CHECK(fvp_vm_snow(NULL, &l0, 1) == 0, "NULL vm snow");

        CHECK(a.prim_snow_calls == 0, "fresh prim snow counter");
        CHECK(fvp_vm_prim_snow(&a, &p2, &on1) == 1, "prim snow on");
        CHECK(a.prim_snow[2] == 1u, "prim 2 snow on");
        CHECK(fvp_vm_prim_snow(&a, &p2, &off0) == 1, "prim snow off");
        CHECK(a.prim_snow[2] == 0u, "prim 2 snow off");
        CHECK(fvp_vm_prim_snow(&a, &slot_neg, &on1) == 0, "prim 0 rejected");
        CHECK(fvp_vm_prim_snow(&a, &p4096, &on1) == 0, "prim 4096 rejected");
        CHECK(fvp_vm_prim_snow(&a, &p2, NULL) == 1, "NULL on -> 0 但仍生效");
        CHECK(a.prim_snow[2] == 0u, "NULL on stored as 0");
        CHECK(a.prim_snow_calls == 3, "prim snow counter (%llu)",
              (unsigned long long)a.prim_snow_calls);
        CHECK(fvp_vm_prim_snow(NULL, &p2, &on1) == 0, "NULL vm prim snow");
    }

    /* --- 4d. CursorShow(12) / ExitDialog(16) / MovieStop(63) --- */
    {
        FvpValue on1 = fvp_make_int(1);
        FvpValue off0 = fvp_make_int(0);

        CHECK(a.cursor_show == 1u, "cursor shown by default (%u)",
              (unsigned)a.cursor_show);
        CHECK(fvp_vm_cursor_show(&a, &off0) == 1, "CursorShow(0) accepted");
        CHECK(a.cursor_show == 1u,
              "值非 null -> 1（本站没有 null，恒 1）");
        CHECK(fvp_vm_cursor_show(&a, &on1) == 1, "CursorShow(1) accepted");
        CHECK(fvp_vm_cursor_show(&a, NULL) == 0, "NULL arg rejected");
        CHECK(fvp_vm_cursor_show(NULL, &on1) == 0, "NULL vm rejected");

        CHECK(a.exit_dialog_calls == 0 && a.movie_stop_calls == 0,
              "fresh dialog/movie counters");
        fvp_vm_exit_dialog(&a);
        fvp_vm_exit_dialog(&a);
        CHECK(a.exit_dialog_calls == 2, "exit dialog counter (%llu)",
              (unsigned long long)a.exit_dialog_calls);
        fvp_vm_movie_stop(&a);
        CHECK(a.movie_stop_calls == 1, "movie stop counter (%llu)",
              (unsigned long long)a.movie_stop_calls);
        fvp_vm_exit_dialog(NULL);
        fvp_vm_movie_stop(NULL);
        CHECK(1, "NULL calls survive");
    }

    /* --- 4c. InputFlash(27) / InputGetRepeat(33) / InputSetClick(37) --- */
    {
        FvpValue on1 = fvp_make_int(1);
        FvpValue on2 = fvp_make_int(2);
        FvpValue off0 = fvp_make_int(0);

        CHECK(a.input_click == 0u && a.input_repeat == 0,
              "fresh input state (click=%u repeat=%d)",
              (unsigned)a.input_click, a.input_repeat);
        CHECK(fvp_vm_input_repeat(&a) == 0, "repeat default 0");
        CHECK(fvp_vm_input_set_click(&a, &on1) == 1, "set click 1");
        CHECK(a.input_click == 1u, "click = 1 (%u)", (unsigned)a.input_click);
        CHECK(fvp_vm_input_set_click(&a, &off0) == 1, "set click 0");
        CHECK(a.input_click == 0u, "click = 0");
        CHECK(fvp_vm_input_set_click(&a, &on2) == 0, "click 2 rejected");
        CHECK(a.input_click == 0u, "click unchanged after reject");
        CHECK(fvp_vm_input_set_click(&a, &fl) == 0, "float click rejected");
        CHECK(fvp_vm_input_set_click(&a, NULL) == 0, "NULL click rejected");
        CHECK(a.input_setclick_calls == 2, "setclick counter (%llu)",
              (unsigned long long)a.input_setclick_calls);
        CHECK(fvp_vm_input_set_click(NULL, &on1) == 0, "NULL vm setclick");
        fvp_vm_input_flash(&a);
        fvp_vm_input_flash(&a);
        CHECK(a.input_flash_calls == 2, "flash counter (%llu)",
              (unsigned long long)a.input_flash_calls);
        fvp_vm_input_flash(NULL);
        CHECK(fvp_vm_input_repeat(NULL) == 0, "NULL vm repeat");
    }

    /* --- 4a. MotionPause(60) / TextPause(121) --- */
    {
        FvpValue p2 = fvp_make_int(2);
        FvpValue p100 = fvp_make_int(100);
        FvpValue p4095 = fvp_make_int(4095);
        FvpValue p4096 = fvp_make_int(4096);
        FvpValue tw31 = fvp_make_int(31);
        FvpValue tw32 = fvp_make_int(32);
        FvpValue on1 = fvp_make_int(1);
        FvpValue off0 = fvp_make_int(0);
        FvpValue on2 = fvp_make_int(2);

        CHECK(a.motion_pause_calls == 0 && a.text_pause_calls == 0,
              "fresh pause counters");
        CHECK(fvp_vm_motion_pause(&a, &p2, &on1) == 1, "motion pause on");
        CHECK(a.prim_pause[2] == 1u, "prim 2 paused");
        CHECK(fvp_vm_motion_pause(&a, &p2, &off0) == 1, "motion pause off");
        CHECK(a.prim_pause[2] == 0u, "prim 2 resumed");
        CHECK(fvp_vm_motion_pause(&a, &p4095, &on1) == 1, "prim 4095 ok");
        CHECK(fvp_vm_motion_pause(&a, &p4096, &on1) == 0, "prim 4096 rejected");
        CHECK(fvp_vm_motion_pause(&a, &slot_neg, &on1) == 0, "prim -1 rejected");
        CHECK(fvp_vm_motion_pause(&a, &fl, &on1) == 0, "float prim rejected");
        CHECK(fvp_vm_motion_pause(&a, NULL, &on1) == 0, "NULL prim rejected");
        CHECK(a.motion_pause_calls == 3, "motion pause counter (%llu)",
              (unsigned long long)a.motion_pause_calls);
        /* 非法 on：按 0 处理，但仍算一次有效调用 */
        CHECK(fvp_vm_motion_pause(&a, &p100, &on2) == 1, "bad on still applies");
        CHECK(a.prim_pause[100] == 0u, "bad on -> 0");

        CHECK(fvp_vm_text_pause(&a, &tw31, &on1) == 1, "text pause on");
        CHECK(a.tw_pause[31] == 1u, "tw 31 paused");
        CHECK(fvp_vm_text_pause(&a, &tw31, &off0) == 1, "text pause off");
        CHECK(a.tw_pause[31] == 0u, "tw 31 resumed");
        CHECK(fvp_vm_text_pause(&a, &tw32, &on1) == 0, "tw 32 rejected");
        CHECK(fvp_vm_text_pause(&a, &slot_neg, &on1) == 0, "tw -1 rejected");
        CHECK(fvp_vm_text_pause(NULL, &tw31, &on1) == 0, "NULL vm rejected");
        CHECK(a.text_pause_calls == 2, "text pause counter (%llu)",
              (unsigned long long)a.text_pause_calls);
    }

    /* --- 4b. 字体表（TextFont* 114..117） --- */
    {
        FvpValue f0 = fvp_make_int(0);
        FvpValue f1 = fvp_make_int(1);
        FvpValue fm5 = fvp_make_int(-5);
        FvpValue fm6 = fvp_make_int(-6);
        FvpValue f3 = fvp_make_int(3);

        CHECK(fvp_vm_font_count(&a) == 0, "empty font table");
        CHECK(fvp_vm_font_get(&a) == -1, "no current font yet (%d)",
              fvp_vm_font_get(&a));
        CHECK(fvp_vm_font_register(&a, "font-a") == 1, "register 1st");
        CHECK(fvp_vm_font_register(&a, "font-b") == 2, "register 2nd");
        CHECK(fvp_vm_font_register(&a, "font-c") == 3, "register 3rd");
        CHECK(fvp_vm_font_count(&a) == 3, "count 3 (%d)",
              fvp_vm_font_count(&a));
        CHECK(fvp_vm_font_name(&a, &f0) != NULL &&
              strcmp(fvp_vm_font_name(&a, &f0), "font-a") == 0, "name 0");
        CHECK(fvp_vm_font_name(&a, &f1) != NULL &&
              strcmp(fvp_vm_font_name(&a, &f1), "font-b") == 0, "name 1");
        CHECK(fvp_vm_font_name(&a, &f3) == NULL, "name 3 (out of range)");
        CHECK(fvp_vm_font_name(&a, &fm5) == NULL, "name -5 rejected");
        CHECK(fvp_vm_font_name(&a, NULL) == NULL, "name NULL rejected");

        CHECK(fvp_vm_font_set(&a, &f1) == 1, "set 1");
        CHECK(fvp_vm_font_get(&a) == 1, "get 1 (%d)", fvp_vm_font_get(&a));
        CHECK(fvp_vm_font_set(&a, &fm5) == 1, "set -5 allowed (原版下界)");
        CHECK(fvp_vm_font_get(&a) == -5, "get -5");
        CHECK(fvp_vm_font_set(&a, &fm6) == 0, "set -6 rejected");
        CHECK(fvp_vm_font_get(&a) == -5, "unchanged after reject");
        CHECK(fvp_vm_font_set(&a, &f3) == 0, "set 3 rejected (count-1 = 2)");
        CHECK(fvp_vm_font_set(&a, NULL) == 0, "set NULL rejected");
        CHECK(fvp_vm_font_register(NULL, "x") == -1, "NULL vm register");
        CHECK(fvp_vm_font_count(NULL) == 0, "NULL count");
        CHECK(fvp_vm_font_get(NULL) == 0, "NULL get");
        CHECK(fvp_vm_font_set(NULL, &f0) == 0, "NULL set");
        CHECK(fvp_vm_font_name(NULL, &f0) == NULL, "NULL name");
    }

    /* --- 5. Load(40) / SaveWrite(96)：请求 + 槽号 --- */
    CHECK(a.request_slot == -1 && a.load_request == 0u && a.save_request == 0u,
          "fresh save/load state");
    CHECK(fvp_vm_take_load_request(&a) == -1, "no load request yet");
    CHECK(fvp_vm_take_save_request(&a) == -1, "no save request yet");

    CHECK(fvp_vm_request_load(&a, &slot5) == 1, "load slot 5 accepted");
    CHECK(a.load_request == 1u && a.request_slot == 5,
          "load flag/slot (%u/%d)", (unsigned)a.load_request, a.request_slot);
    CHECK(a.load_calls == 1, "load counter (%llu)",
          (unsigned long long)a.load_calls);
    CHECK(fvp_vm_take_load_request(&a) == 5, "take load slot");
    CHECK(a.load_request == 0u, "take clears the load flag");
    CHECK(fvp_vm_take_load_request(&a) == -1, "second take gets nothing");

    CHECK(fvp_vm_request_save(&a, &slot999) == 1, "save slot 999 accepted");
    CHECK(a.save_request == 1u && a.request_slot == 999,
          "save flag/slot (%u/%d)", (unsigned)a.save_request, a.request_slot);
    CHECK(fvp_vm_take_save_request(&a) == 999, "take save slot");
    CHECK(a.save_request == 0u, "take clears the save flag");

    CHECK(fvp_vm_request_load(&a, &slot1000) == 0, "slot 1000 rejected");
    CHECK(fvp_vm_request_load(&a, &slot_neg) == 0, "slot -1 rejected");
    CHECK(fvp_vm_request_load(&a, &fl) == 0, "float slot rejected");
    CHECK(fvp_vm_request_load(&a, NULL) == 0, "NULL slot rejected");
    CHECK(fvp_vm_request_save(NULL, &slot5) == 0, "NULL vm rejected");
    CHECK(a.load_calls == 1 && a.save_calls == 1,
          "rejected calls are not counted (%llu/%llu)",
          (unsigned long long)a.load_calls,
          (unsigned long long)a.save_calls);
    CHECK(fvp_vm_take_load_request(NULL) == -1, "NULL vm take load");
    CHECK(fvp_vm_take_save_request(NULL) == -1, "NULL vm take save");

    fvp_vm_free(&a);
    fvp_vm_free(&b);

    printf("\ntest_vmmisc: %d passed, %d failed\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}