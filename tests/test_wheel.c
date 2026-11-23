/* 鼠标滚轮（InputGetWheel, 命令 36）的累加/取走语义测试。
 *
 *   bin/test_wheel.exe
 *
 * VM 只做「累加 + 取走」：外壳每帧把窗口收到的 wheel_delta 加进去，脚本调用
 * InputGetWheel 时取走并清零。所以真正要钉住的是：累加正确、取走清零、多帧复位、
 * 正负方向、NULL 安全。
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

    /* fvp_vm_init 只依赖 hcb 指针，传 NULL 也能初始化（不跑脚本）。 */
    fvp_vm_init(&vm, NULL, 1);

    CHECK(fvp_vm_wheel_take(&vm) == 0, "fresh wheel");

    fvp_vm_wheel_add(&vm, 120);
    CHECK(fvp_vm_wheel_take(&vm) == 120, "one notch");
    CHECK(fvp_vm_wheel_take(&vm) == 0, "take clears");

    fvp_vm_wheel_add(&vm, 120);
    fvp_vm_wheel_add(&vm, 240);
    CHECK(fvp_vm_wheel_take(&vm) == 360, "accumulates within a frame");

    fvp_vm_wheel_add(&vm, -120);
    CHECK(fvp_vm_wheel_take(&vm) == -120, "negative direction");

    fvp_vm_wheel_add(&vm, 0);
    CHECK(fvp_vm_wheel_take(&vm) == 0, "adding zero is a no-op");

    /* 取走之后再来一轮，确认不会残留。 */
    fvp_vm_wheel_add(&vm, 360);
    (void)fvp_vm_wheel_take(&vm);
    fvp_vm_wheel_add(&vm, 120);
    CHECK(fvp_vm_wheel_take(&vm) == 120, "no residue between frames");

    /* NULL 安全。 */
    fvp_vm_wheel_add(NULL, 120);
    CHECK(fvp_vm_wheel_take(NULL) == 0, "NULL vm is safe");

    fvp_vm_free(&vm);

    printf("\ntest_wheel: %d passed, %d failed\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}