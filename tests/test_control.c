/* 控制 / 光标这一组命令（ControlMask 8 / ControlPulse 9 / CursorChange 10 /
 * CursorMove 11）的状态语义测试。
 *
 *   bin/test_control.exe
 *
 * 语义全部来自静态分析（handler 0x437440 / 0x437460 / 0x437470 / 0x4374a0，
 * 见 docs/vm-commands.md §5 补充表）。关键点是"参数类型"而不是"参数值"：
 *
 *   8  ControlMask : engine+0x458 = (参数是 null)  → 本站没有 null 类型，
 *                    1 这支不可达，这里钉住"恒 0 + 记录参数类型"
 *   9  ControlPulse: engine+0xfd8 = 1（只置位，不清零）
 *   10 CursorChange: 参数必须是 int 且 0..3，否则整条命令无效；合法则记 shape
 *   11 CursorMove  : x/y 必须是 int 才移动，第三参数只记录类型
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
    FvpValue i0 = fvp_make_int(0);
    FvpValue i1 = fvp_make_int(1);
    FvpValue i3 = fvp_make_int(3);
    FvpValue i4 = fvp_make_int(4);
    FvpValue im1 = fvp_make_int(-1);
    FvpValue fl = fvp_make_float(1.5);
    FvpValue str = fvp_make_str(NULL);
    FvpValue x10 = fvp_make_int(10);
    FvpValue y20 = fvp_make_int(20);

    /* fvp_vm_init 只依赖 hcb 指针，传 NULL 也能初始化。 */
    fvp_vm_init(&vm, NULL, 1);

    /* --- 0. 初始状态 --- */
    CHECK(vm.control_mask == 0 && vm.pulse == 0, "fresh mask/pulse");
    CHECK(vm.cursor_shape == -1, "fresh cursor shape is -1 (got %d)",
          vm.cursor_shape);
    CHECK(vm.cursor_moved == 0, "fresh cursor not moved");
    CHECK(vm.control_calls[0] == 0 && vm.control_calls[1] == 0 &&
          vm.control_calls[2] == 0 && vm.control_calls[3] == 0,
          "fresh counters");

    /* --- 1. ControlMask(8)：看类型不看值 --- */
    fvp_vm_control_mask(&vm, &i1);
    CHECK(vm.control_mask == 0, "int 1 -> mask 0 (got %d)", vm.control_mask);
    CHECK(vm.control_mask_type == (unsigned char)FVP_INT,
          "records the arg type (got %u)", vm.control_mask_type);
    fvp_vm_control_mask(&vm, &i0);
    CHECK(vm.control_mask == 0, "int 0 -> mask 0 (value is not the criterion)");
    fvp_vm_control_mask(&vm, &str);
    CHECK(vm.control_mask == 0, "string -> mask 0");
    CHECK(vm.control_mask_type == (unsigned char)FVP_STR,
          "records the string type (got %u)", vm.control_mask_type);
    fvp_vm_control_mask(&vm, NULL);
    CHECK(vm.control_mask == 0 && vm.control_mask_type == 0xFF,
          "NULL arg -> mask 0, type 0xFF");
    CHECK(vm.control_calls[0] == 4, "mask call counter (got %llu)",
          (unsigned long long)vm.control_calls[0]);

    /* --- 2. ControlPulse(9)：只置 1 --- */
    fvp_vm_control_pulse(&vm);
    CHECK(vm.pulse == 1, "pulse set");
    fvp_vm_control_pulse(&vm);
    CHECK(vm.pulse == 1, "pulse stays 1");
    CHECK(vm.control_calls[1] == 2, "pulse call counter (got %llu)",
          (unsigned long long)vm.control_calls[1]);

    /* --- 3. CursorChange(10)：合法性判定 --- */
    CHECK(fvp_vm_cursor_change(&vm, &i3) == 1, "int 3 accepted");
    CHECK(vm.cursor_shape == 3, "shape 3 (got %d)", vm.cursor_shape);
    CHECK(fvp_vm_cursor_change(&vm, &i0) == 1, "int 0 accepted");
    CHECK(vm.cursor_shape == 0, "shape 0 (got %d)", vm.cursor_shape);
    CHECK(fvp_vm_cursor_change(&vm, &i4) == 0, "int 4 rejected");
    CHECK(vm.cursor_shape == 0, "shape unchanged after reject (got %d)",
          vm.cursor_shape);
    CHECK(fvp_vm_cursor_change(&vm, &im1) == 0, "int -1 rejected");
    CHECK(vm.cursor_shape == 0, "shape still 0 (got %d)", vm.cursor_shape);
    CHECK(fvp_vm_cursor_change(&vm, &fl) == 0, "float rejected (must be int)");
    CHECK(fvp_vm_cursor_change(&vm, &str) == 0, "string rejected");
    CHECK(vm.cursor_shape == 0, "shape unchanged (got %d)", vm.cursor_shape);
    CHECK(fvp_vm_cursor_change(&vm, NULL) == 0, "NULL arg rejected");
    /* NULL 参数在计数之前就返回了，所以 7 次调用只记 6 次。 */
    CHECK(vm.control_calls[2] == 6, "change call counter (got %llu)",
          (unsigned long long)vm.control_calls[2]);

    /* --- 4. CursorMove(11) --- */
    CHECK(fvp_vm_cursor_move(&vm, &x10, &y20, &i1) == 1, "int x/y accepted");
    CHECK(vm.cursor_x == 10 && vm.cursor_y == 20, "cursor at 10,20 (got %d,%d)",
          vm.cursor_x, vm.cursor_y);
    CHECK(vm.cursor_moved == 1, "moved flag");
    CHECK(vm.cursor_force_type == (unsigned char)FVP_INT,
          "records the third arg type (got %u)", vm.cursor_force_type);
    CHECK(fvp_vm_cursor_move(&vm, &fl, &y20, &i1) == 0, "float x rejected");
    CHECK(vm.cursor_x == 10, "x unchanged after reject (got %d)", vm.cursor_x);
    CHECK(fvp_vm_cursor_move(&vm, &x10, &str, &i1) == 0, "string y rejected");
    CHECK(fvp_vm_cursor_move(&vm, NULL, &y20, &i1) == 0, "NULL x rejected");
    CHECK(fvp_vm_cursor_move(&vm, &x10, NULL, &i1) == 0, "NULL y rejected");
    /* 第三参数为 NULL 时只记类型 0xFF，仍然执行（本站没有 null 类型）。 */
    CHECK(fvp_vm_cursor_move(&vm, &i0, &i3, NULL) == 1, "NULL force still moves");
    CHECK(vm.cursor_x == 0 && vm.cursor_y == 3, "cursor at 0,3 (got %d,%d)",
          vm.cursor_x, vm.cursor_y);
    CHECK(vm.cursor_force_type == 0xFF, "NULL force type 0xFF");
    /* 同上：x/y 为 NULL 的两次在计数之前返回。 */
    CHECK(vm.control_calls[3] == 4, "move call counter (got %llu)",
          (unsigned long long)vm.control_calls[3]);

    /* --- 5. NULL vm 安全 --- */
    fvp_vm_control_mask(NULL, &i1);
    fvp_vm_control_pulse(NULL);
    CHECK(fvp_vm_cursor_change(NULL, &i1) == 0, "NULL vm change");
    CHECK(fvp_vm_cursor_move(NULL, &x10, &y20, &i1) == 0, "NULL vm move");

    fvp_vm_free(&vm);

    printf("\ntest_control: %d passed, %d failed\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}