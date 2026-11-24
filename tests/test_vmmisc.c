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