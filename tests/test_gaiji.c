/* GaijiLoad(21) / V3DMotion(142) 的状态测试。
 *
 *   bin/test_gaiji.exe
 *
 * 语义来自静态分析：
 *   GaijiLoad(str1, size 12..0x40, str2)：handler 0x43f4f0。两个字符串参数
 *     （原版 type 4/5）+ 中间那个必须是 int 且 12..64，然后
 *     call 0x43e9c0(engine+0x6a49cc, str1, size, str2)。本站只记录参数。
 *   V3DMotion(arg1..arg3 可选坐标, timeMs 1..0x493e0, mode 1..5, flag)：
 *     handler 0x43cfe0。本站没有 3D 场景，只记录 time/mode。
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
    FvpVM vm;
    FvpValue s_a;
    FvpValue s_b;
    FvpValue n12 = fvp_make_int(12);
    FvpValue n11 = fvp_make_int(11);
    FvpValue n64 = fvp_make_int(64);
    FvpValue n65 = fvp_make_int(65);
    FvpValue n0 = fvp_make_int(0);
    FvpValue n24 = fvp_make_int(24);
    FvpValue fl = fvp_make_float(1.0);
    FvpValue t1 = fvp_make_int(1);
    FvpValue t300000 = fvp_make_int(300000);
    FvpValue t300001 = fvp_make_int(300001);
    FvpValue m5 = fvp_make_int(5);
    FvpValue m6 = fvp_make_int(6);

    fvp_vm_init(&vm, NULL, 1);
    s_a = fvp_make_str(fvp_arena_str(&vm.arena, "gaiji/a", 7));
    s_b = fvp_make_str(fvp_arena_str(&vm.arena, "b", 1));

    /* --- 1. 初始状态 --- */
    CHECK(vm.gaiji_calls == 0 && vm.gaiji_size == 0, "fresh gaiji");
    CHECK(vm.gaiji_name1 == NULL && vm.gaiji_name2 == NULL,
          "fresh gaiji names");
    CHECK(vm.v3d_calls == 0 && vm.v3d_time_ms == 0 && vm.v3d_mode == 0,
          "fresh v3d");

    /* --- 2. GaijiLoad 正常路径 --- */
    CHECK(fvp_vm_gaiji_load(&vm, &s_a, &n24, &s_b) == 1, "valid gaiji");
    CHECK(vm.gaiji_size == 24, "size recorded (%d)", vm.gaiji_size);
    CHECK(vm.gaiji_name1 != NULL && strcmp(vm.gaiji_name1, "gaiji/a") == 0,
          "name1 recorded (%s)", vm.gaiji_name1 != NULL ? vm.gaiji_name1 : "-");
    CHECK(vm.gaiji_name2 != NULL && strcmp(vm.gaiji_name2, "b") == 0,
          "name2 recorded (%s)", vm.gaiji_name2 != NULL ? vm.gaiji_name2 : "-");
    CHECK(vm.gaiji_calls == 1, "gaiji counter (%llu)",
          (unsigned long long)vm.gaiji_calls);

    /* --- 3. size 边界 12..64 --- */
    CHECK(fvp_vm_gaiji_load(&vm, &s_a, &n12, &s_b) == 1, "size 12 ok");
    CHECK(fvp_vm_gaiji_load(&vm, &s_a, &n64, &s_b) == 1, "size 64 ok");
    CHECK(fvp_vm_gaiji_load(&vm, &s_a, &n11, &s_b) == 0, "size 11 rejected");
    CHECK(fvp_vm_gaiji_load(&vm, &s_a, &n65, &s_b) == 0, "size 65 rejected");
    CHECK(vm.gaiji_size == 64, "rejected calls kept the old size (%d)",
          vm.gaiji_size);
    CHECK(vm.gaiji_calls == 3, "gaiji counter after rejects (%llu)",
          (unsigned long long)vm.gaiji_calls);

    /* --- 4. 类型 / NULL --- */
    CHECK(fvp_vm_gaiji_load(&vm, &n24, &n24, &s_b) == 0, "int str1 rejected");
    CHECK(fvp_vm_gaiji_load(&vm, &s_a, &n24, &n24) == 0, "int str2 rejected");
    CHECK(fvp_vm_gaiji_load(&vm, &s_a, &fl, &s_b) == 0, "float size rejected");
    CHECK(fvp_vm_gaiji_load(&vm, NULL, &n24, &s_b) == 0, "NULL str1 rejected");
    CHECK(fvp_vm_gaiji_load(&vm, &s_a, NULL, &s_b) == 0, "NULL size rejected");
    CHECK(fvp_vm_gaiji_load(NULL, &s_a, &n24, &s_b) == 0, "NULL vm rejected");
    CHECK(vm.gaiji_calls == 3, "gaiji counter unchanged (%llu)",
          (unsigned long long)vm.gaiji_calls);

    /* --- 5. V3DMotion --- */
    CHECK(fvp_vm_v3d_motion(&vm, &t300000, &m5) == 1, "valid v3d");
    CHECK(vm.v3d_time_ms == 300000 && vm.v3d_mode == 5,
          "v3d recorded (%d ms, mode %d)", vm.v3d_time_ms, vm.v3d_mode);
    CHECK(vm.v3d_calls == 1, "v3d counter (%llu)",
          (unsigned long long)vm.v3d_calls);
    CHECK(fvp_vm_v3d_motion(&vm, &t1, &t1) == 1, "time 1 / mode 1 ok");
    CHECK(fvp_vm_v3d_motion(&vm, &n0, &m5) == 0, "time 0 rejected");
    CHECK(fvp_vm_v3d_motion(&vm, &t300001, &m5) == 0, "time 300001 rejected");
    CHECK(fvp_vm_v3d_motion(&vm, &t300000, &n0) == 0, "mode 0 rejected");
    CHECK(fvp_vm_v3d_motion(&vm, &t300000, &m6) == 0, "mode 6 rejected");
    CHECK(fvp_vm_v3d_motion(&vm, &t300000, &fl) == 0, "float mode rejected");
    CHECK(fvp_vm_v3d_motion(&vm, NULL, &m5) == 0, "NULL time rejected");
    CHECK(fvp_vm_v3d_motion(NULL, &t1, &m5) == 0, "NULL vm rejected");
    CHECK(vm.v3d_calls == 2, "v3d counter after rejects (%llu)",
          (unsigned long long)vm.v3d_calls);

    fvp_vm_free(&vm);

    printf("\ntest_gaiji: %d passed, %d failed\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}