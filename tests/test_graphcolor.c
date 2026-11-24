/* GraphRGB(23) / SysProjFolder(109) 的状态测试。
 *
 *   bin/test_graphcolor.exe
 *
 * 语义来自静态分析（handler 0x43a3d0 / 0x43c7f0）：
 *   23 GraphRGB(graphId 0..0xFFF, r, g, b)：槽号必须是 int 且在 0..0xFFF；
 *      三个分量各自"是 int 且在 0..200"才采用，否则**默认 100**（不调制）。
 *   109 SysProjFolder(str)：只接受字符串参数（原版 type 4/5），其它一律不动。
 * 本站两者都只记录状态，不参与渲染/文件访问（见 src/vm.h 的注释）。
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
    FvpValue slot7 = fvp_make_int(7);
    FvpValue slot4095 = fvp_make_int(4095);
    FvpValue slot4096 = fvp_make_int(4096);
    FvpValue slot_neg = fvp_make_int(-1);
    FvpValue fl = fvp_make_float(2.0);
    FvpValue ten = fvp_make_int(10);
    FvpValue fifty = fvp_make_int(50);
    FvpValue two_hundred = fvp_make_int(200);
    FvpValue over = fvp_make_int(201);
    FvpValue neg = fvp_make_int(-5);
    FvpValue str_empty;

    fvp_vm_init(&vm, NULL, 1);
    /* 空串用 arena 造一个真 FvpStr（len=0），不是 NULL 指针。 */
    str_empty = fvp_make_str(fvp_arena_str(&vm.arena, "", 0));

    /* --- 1. 初始状态 --- */
    CHECK(vm.graph_rgb_calls == 0, "fresh graph_rgb counter");
    CHECK(vm.proj_folder == NULL && vm.proj_folder_calls == 0,
          "fresh project folder");
    CHECK(vm.graph_rgb_set[7] == 0, "slot 7 not set yet");

    /* --- 2. 合法槽 + 合法分量 --- */
    CHECK(fvp_vm_graph_rgb(&vm, &slot7, &ten, &fifty, &two_hundred) == 1,
          "valid GraphRGB accepted");
    CHECK(vm.graph_rgb[7][0] == 10 && vm.graph_rgb[7][1] == 50 &&
          vm.graph_rgb[7][2] == 200, "rgb stored (%d,%d,%d)",
          vm.graph_rgb[7][0], vm.graph_rgb[7][1], vm.graph_rgb[7][2]);
    CHECK(vm.graph_rgb_set[7] == 1, "slot 7 marked");
    CHECK(vm.graph_rgb[8][0] == 0 && vm.graph_rgb_set[8] == 0,
          "other slots untouched");

    /* --- 3. 分量越界 / 非 int → 该分量回落到 100 --- */
    CHECK(fvp_vm_graph_rgb(&vm, &slot7, &over, &neg, &fl) == 1,
          "out-of-range components still accepted");
    CHECK(vm.graph_rgb[7][0] == 100 && vm.graph_rgb[7][1] == 100 &&
          vm.graph_rgb[7][2] == 100, "clamped to default 100 (%d,%d,%d)",
          vm.graph_rgb[7][0], vm.graph_rgb[7][1], vm.graph_rgb[7][2]);

    /* --- 4. 边界：0 与 200 合法，201 / -1 非法 --- */
    CHECK(fvp_vm_graph_rgb(&vm, &slot4095, &ten, &two_hundred, &over) == 1,
          "slot 4095 accepted");
    CHECK(vm.graph_rgb[4095][0] == 10 && vm.graph_rgb[4095][1] == 200 &&
          vm.graph_rgb[4095][2] == 100, "edge values (%d,%d,%d)",
          vm.graph_rgb[4095][0], vm.graph_rgb[4095][1],
          vm.graph_rgb[4095][2]);

    /* --- 5. 槽号越界 / 非 int / NULL → 不受理、不计数 --- */
    CHECK(fvp_vm_graph_rgb(&vm, &slot4096, &ten, &ten, &ten) == 0,
          "slot 4096 rejected");
    CHECK(fvp_vm_graph_rgb(&vm, &slot_neg, &ten, &ten, &ten) == 0,
          "negative slot rejected");
    CHECK(fvp_vm_graph_rgb(&vm, &fl, &ten, &ten, &ten) == 0,
          "float slot rejected");
    CHECK(fvp_vm_graph_rgb(&vm, NULL, &ten, &ten, &ten) == 0, "NULL slot");
    CHECK(vm.graph_rgb_calls == 3, "only accepted calls counted (%llu)",
          (unsigned long long)vm.graph_rgb_calls);

    /* --- 6. SysProjFolder --- */
    CHECK(fvp_vm_set_proj_folder(&vm, &str_empty) == 1,
          "empty string accepted (it is still a string)");
    CHECK(vm.proj_folder != NULL && vm.proj_folder[0] == '\0',
          "empty folder stored");
    CHECK(fvp_vm_set_proj_folder(&vm, &ten) == 0, "int rejected");
    CHECK(fvp_vm_set_proj_folder(&vm, &fl) == 0, "float rejected");
    CHECK(fvp_vm_set_proj_folder(&vm, NULL) == 0, "NULL rejected");
    CHECK(vm.proj_folder_calls == 1, "folder call counter (%llu)",
          (unsigned long long)vm.proj_folder_calls);

    /* --- 7. NULL vm 安全 --- */
    CHECK(fvp_vm_graph_rgb(NULL, &slot7, &ten, &ten, &ten) == 0, "NULL vm rgb");
    CHECK(fvp_vm_set_proj_folder(NULL, &str_empty) == 0, "NULL vm folder");

    fvp_vm_free(&vm);

    printf("\ntest_graphcolor: %d passed, %d failed\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}