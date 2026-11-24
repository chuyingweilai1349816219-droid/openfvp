/* TextBuff(110) 的状态测试。
 *
 *   bin/test_textbuff.exe
 *
 * 语义来自静态分析（handler 0x43c830 → setter 0x430ea0）：
 *   TextBuff(twIdx 0..31, a, b)
 *     * twIdx 必须是 int 且在 0..31，否则整条命令无效；
 *     * a / b 各自"是 int 且 >= 0"才采用，否则用默认 8；
 *     * 任一分量 > 0x400 时 setter 直接放弃 → 整条命令无效（连记录都不留）；
 *     * 生效时写 u16 tw+0x5dc / tw+0x5de。
 * 本站只记录尺寸，不重建缓冲（我们的文本是按行存的动态串）。
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
    FvpValue wi0 = fvp_make_int(0);
    FvpValue wi8 = fvp_make_int(8);
    FvpValue wi31 = fvp_make_int(31);
    FvpValue wi32 = fvp_make_int(32);
    FvpValue wi_neg = fvp_make_int(-1);
    FvpValue fl = fvp_make_float(3.0);
    FvpValue n40 = fvp_make_int(40);
    FvpValue n30 = fvp_make_int(30);
    FvpValue n0 = fvp_make_int(0);
    FvpValue n400 = fvp_make_int(0x400);
    FvpValue n401 = fvp_make_int(0x401);
    FvpValue nneg = fvp_make_int(-3);

    fvp_vm_init(&vm, NULL, 1);

    /* --- 1. 初始状态 --- */
    CHECK(vm.text_buff_calls == 0, "fresh counter");
    CHECK(vm.text_buff[0].set == 0 && vm.text_buff[8].set == 0,
          "fresh windows unset");

    /* --- 2. 正常设置 --- */
    CHECK(fvp_vm_text_buff(&vm, &wi0, &n40, &n30) == 1, "valid accepted");
    CHECK(vm.text_buff[0].set == 1, "window 0 marked");
    CHECK(vm.text_buff[0].w == 40 && vm.text_buff[0].h == 30,
          "size stored (%u,%u)", (unsigned)vm.text_buff[0].w,
          (unsigned)vm.text_buff[0].h);
    CHECK(vm.text_buff[8].set == 0, "other windows untouched");
    CHECK(vm.text_buff_calls == 1, "counter (%llu)",
          (unsigned long long)vm.text_buff_calls);

    /* --- 3. 缺省 / 非法分量 → 8 --- */
    CHECK(fvp_vm_text_buff(&vm, &wi8, NULL, NULL) == 1, "NULL a/b accepted");
    CHECK(vm.text_buff[8].w == 8 && vm.text_buff[8].h == 8,
          "defaults are 8/8 (%u,%u)", (unsigned)vm.text_buff[8].w,
          (unsigned)vm.text_buff[8].h);
    CHECK(fvp_vm_text_buff(&vm, &wi8, &fl, &nneg) == 1,
          "float / negative components fall back");
    CHECK(vm.text_buff[8].w == 8 && vm.text_buff[8].h == 8,
          "still 8/8 (%u,%u)", (unsigned)vm.text_buff[8].w,
          (unsigned)vm.text_buff[8].h);

    /* --- 4. 0 合法、0x400 合法、0x401 整条放弃 --- */
    CHECK(fvp_vm_text_buff(&vm, &wi31, &n0, &n400) == 1, "0 and 0x400 ok");
    CHECK(vm.text_buff[31].w == 0 && vm.text_buff[31].h == 0x400,
          "edge stored (%u,%u)", (unsigned)vm.text_buff[31].w,
          (unsigned)vm.text_buff[31].h);
    {
        uint64_t before = vm.text_buff_calls;

        CHECK(fvp_vm_text_buff(&vm, &wi31, &n401, &n30) == 0,
              "0x401 rejected");
        CHECK(vm.text_buff_calls == before,
              "rejected call not counted (%llu)",
              (unsigned long long)vm.text_buff_calls);
        CHECK(vm.text_buff[31].w == 0 && vm.text_buff[31].h == 0x400,
              "rejected call left the old size");
    }

    /* --- 5. 窗口号越界 / 非 int / NULL --- */
    CHECK(fvp_vm_text_buff(&vm, &wi32, &n40, &n30) == 0, "idx 32 rejected");
    CHECK(fvp_vm_text_buff(&vm, &wi_neg, &n40, &n30) == 0, "idx -1 rejected");
    CHECK(fvp_vm_text_buff(&vm, &fl, &n40, &n30) == 0, "float idx rejected");
    CHECK(fvp_vm_text_buff(&vm, NULL, &n40, &n30) == 0, "NULL idx rejected");
    CHECK(fvp_vm_text_buff(NULL, &wi0, &n40, &n30) == 0, "NULL vm rejected");

    fvp_vm_free(&vm);

    printf("\ntest_textbuff: %d passed, %d failed\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}