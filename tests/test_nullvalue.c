/* 值模型的 null / type-0 语义（原版 Variant type 0）单元测试。
 *
 *   bin/test_nullvalue.exe
 *
 * 背景：原版的值是一个 8 字节 Variant `{i32 type, i32 value}`，
 *   type 0 = false/null/「可选参数省略」（pushfalse，0x444940 写 type=0），
 *   type 1 = true，type 2 = int，type 3 = float，type 4/5/6 = 字符串三种形态。
 * 本站以前把 pushfalse 压成 int 0，于是 LipAnim 的「只停」分支、
 * LipSync 的 on=0、ControlMask / CursorShow 的 null 分支全都不可达。
 * 现在 pushfalse 压 FVP_NULL，同时保持它在数值/字符串上下文里与 int 0 等价——
 * 后者是硬要求：冻结基准 ref/ref-900k.txt 是 Python 参考实现产出的，
 * 那份实现里 pushfalse 就是 0，脚本里高频的 `syscall; pushtemp; pushfalse; eq`
 * 一旦改变结果，900k 步基准立刻崩。
 *
 * 覆盖：
 *   1. fvp_make_null / fvp_value_is_null / truth
 *   2. eq / cmp / to_str 与 int 0 等价（这是基准不变的根据）
 *   3. LipSync(39)：第二参数 null → on=0，其它 → on=1
 *   4. LipAnim(38)：arg2 null → 「只停」分支，不写 mode/arg
 *   5. ControlMask(8)：参数 null → control_mask=1
 *   6. CursorShow：参数 null → 0
 *   7. 端到端：opcode 0x09(pushfalse) 真的压出 FVP_NULL（跑一个最小 .hcb）
 */
#include <stdio.h>
#include <string.h>

#include "../src/hcb.h"
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

/* 最小 .hcb：u32 script_len | code | u32 main_start。
 * 只有一条指令 0x09(pushfalse)，于是 script_len = 5、main_start = 4。 */
static const unsigned char kPushFalseHcb[9] = {
    0x05, 0x00, 0x00, 0x00, /* script_len = 5 */
    0x09,                   /* pushfalse */
    0x04, 0x00, 0x00, 0x00  /* main_start = 4 */
};

static int write_file(const char *path, const unsigned char *data, size_t len)
{
    FILE *f = fopen(path, "wb");
    size_t n;

    if (f == NULL) {
        return 0;
    }
    n = fwrite(data, 1, len, f);
    fclose(f);
    return n == len;
}

static void test_null_value(void)
{
    FvpValue nul = fvp_make_null();
    FvpValue zero = fvp_make_int(0);
    FvpValue one = fvp_make_int(1);
    FvpValue fz = fvp_make_float(0.0);
    FvpValue f5 = fvp_make_float(5.0);
    FvpArena arena;
    FvpStr *s;
    int ok = 0;

    /* --- 1. 类型与判定 --- */
    CHECK(nul.type == (uint8_t)FVP_NULL, "null type=%u", (unsigned)nul.type);
    CHECK(fvp_value_is_null(&nul) == 1, "is_null(null)");
    CHECK(fvp_value_is_null(&zero) == 0, "is_null(int 0)");
    CHECK(fvp_value_is_null(&fz) == 0, "is_null(float 0)");
    CHECK(fvp_value_is_null(NULL) == 0, "is_null(NULL ptr)");
    CHECK(nul.i == 0 && nul.f == 0.0 && nul.str == NULL,
          "null payload zeroed");

    /* --- 2. truthiness：null 为假（与原版 type 0 一致） --- */
    CHECK(fvp_value_truth(&nul) == 0, "truth(null)");
    CHECK(fvp_value_truth(&zero) == 0, "truth(int 0)");
    CHECK(fvp_value_truth(&one) == 1, "truth(int 1)");

    /* --- 3. eq：与 int 0 等价（冻结基准的根据） --- */
    CHECK(fvp_value_eq(&nul, &zero) == 1, "null == int 0");
    CHECK(fvp_value_eq(&zero, &nul) == 1, "int 0 == null");
    CHECK(fvp_value_eq(&nul, &one) == 0, "null == int 1");
    CHECK(fvp_value_eq(&nul, &fz) == 1, "null == float 0.0");
    CHECK(fvp_value_eq(&nul, &f5) == 0, "null == float 5.0");
    CHECK(fvp_value_eq(&nul, &nul) == 1, "null == null");

    /* --- 4. cmp：同样按 0 参与比较 --- */
    CHECK(fvp_value_cmp(&nul, &zero, &ok) == 0 && ok == 1, "cmp(null,0)");
    CHECK(fvp_value_cmp(&nul, &one, &ok) == -1 && ok == 1, "cmp(null,1)");
    CHECK(fvp_value_cmp(&one, &nul, &ok) == 1 && ok == 1, "cmp(1,null)");

    /* --- 5. to_str：与 int 0 一致（脚本把值贴进文本时基准不变） --- */
    fvp_arena_init(&arena);
    s = fvp_value_to_str(&arena, &nul);
    CHECK(s != NULL && s->len == 1 && s->data[0] == '0',
          "to_str(null)='%s'", s != NULL ? s->data : "(null)");
    s = fvp_value_to_str(&arena, &zero);
    CHECK(s != NULL && s->len == 1 && s->data[0] == '0', "to_str(int 0)");
    fvp_arena_free(&arena);
}

static void test_lip_and_control(void)
{
    FvpVM vm;
    FvpValue nul = fvp_make_null();
    FvpValue id = fvp_make_int(7);
    FvpValue one = fvp_make_int(1);
    FvpValue zero = fvp_make_int(0);
    FvpValue args[8];
    FvpValue floatv = fvp_make_float(1.0);

    fvp_vm_init(&vm, NULL, 1);

    /* --- 6. LipSync(39)：只看第二参数是不是 null --- */
    CHECK(fvp_vm_lip_sync(&vm, &id, &nul) == 1, "lipsync(null) accepted");
    CHECK(vm.lip_sync_on == 0, "lipsync(null) -> on=0 (stop), got %d",
          vm.lip_sync_on);
    CHECK(vm.lip_sync_id == 7, "lipsync id");
    CHECK(fvp_vm_lip_sync(&vm, &id, &one) == 1, "lipsync(1) accepted");
    CHECK(vm.lip_sync_on == 1, "lipsync(1) -> on=1");
    CHECK(fvp_vm_lip_sync(&vm, &id, &zero) == 1, "lipsync(int 0) accepted");
    CHECK(vm.lip_sync_on == 1, "lipsync(int 0) -> on=1 (only null means stop)");
    CHECK(fvp_vm_lip_sync(&vm, &id, &floatv) == 1, "lipsync(float) accepted");
    CHECK(vm.lip_sync_on == 1, "lipsync(float) -> on=1");
    CHECK(fvp_vm_lip_sync(&vm, &id, NULL) == 1, "lipsync(NULL arg) accepted");
    CHECK(vm.lip_sync_on == 0, "lipsync(NULL arg) -> on=0");
    CHECK(fvp_vm_lip_sync(&vm, &zero, &one) == 0, "lipsync bad id 0");
    CHECK(fvp_vm_lip_sync(NULL, &id, &one) == 0, "lipsync NULL vm");

    /* --- 7. LipAnim(38)：arg2 null → 只停 --- */
    vm.lip_anim_arg[1] = -1; /* 脏值，确认"只停"分支不写它 */
    args[0] = id;
    args[1] = nul;
    CHECK(fvp_vm_lip_anim(&vm, args, 2) == 1, "lipanim(id,null) accepted");
    CHECK(vm.lip_anim_stop_calls == 1, "lipanim stop count=%llu",
          (unsigned long long)vm.lip_anim_stop_calls);
    CHECK(vm.lip_anim_calls == 1, "lipanim call count");
    CHECK(vm.lip_anim_id == 7, "lipanim stop keeps id");
    CHECK(vm.lip_anim_arg[1] == -1, "lipanim stop does not write mode");

    /* 正常启动：6 个必填参数 */
    args[0] = id;
    args[1] = fvp_make_int(2);
    args[2] = fvp_make_int(3);
    args[3] = fvp_make_int(100);
    args[4] = fvp_make_int(4);
    args[5] = fvp_make_int(200);
    CHECK(fvp_vm_lip_anim(&vm, args, 6) == 1, "lipanim normal accepted");
    CHECK(vm.lip_anim_calls == 2, "lipanim calls=%llu",
          (unsigned long long)vm.lip_anim_calls);
    CHECK(vm.lip_anim_stop_calls == 1, "lipanim stop stays 1");
    CHECK(vm.lip_anim_mode == 2, "lipanim mode=%d", vm.lip_anim_mode);
    CHECK(vm.lip_anim_arg[5] == 200, "lipanim arg6=%d", vm.lip_anim_arg[5]);

    /* 参数不合法：整条丢弃，且不影响已有计数 */
    args[0] = zero; /* arg1 必须 1..0xFFF */
    args[1] = nul;
    CHECK(fvp_vm_lip_anim(&vm, args, 6) == 0, "lipanim bad id rejected");
    CHECK(vm.lip_anim_stop_calls == 1, "rejected call does not count");
    args[0] = id;
    args[1] = fvp_make_int(9); /* arg2 必须 0..3 */
    CHECK(fvp_vm_lip_anim(&vm, args, 6) == 0, "lipanim bad mode rejected");

    /* --- 8. ControlMask(8)：参数是 null → engine+0x458 = 1 --- */
    vm.control_mask = 9u; /* 脏值 */
    fvp_vm_control_mask(&vm, &nul);
    CHECK(vm.control_mask == 1u, "control_mask(null)=%u",
          (unsigned)vm.control_mask);
    CHECK(vm.control_mask_type == (unsigned char)FVP_NULL, "control mask type");
    fvp_vm_control_mask(&vm, &zero);
    CHECK(vm.control_mask == 0u, "control_mask(int 0)=%u",
          (unsigned)vm.control_mask);
    fvp_vm_control_mask(&vm, &one);
    CHECK(vm.control_mask == 0u, "control_mask(int 1)=0");
    CHECK(vm.control_calls[0] == 3u, "control_calls=%llu",
          (unsigned long long)vm.control_calls[0]);

    /* --- 9. CursorShow：参数是 null → 0（隐藏），其它 → 1 --- */
    CHECK(fvp_vm_cursor_show(&vm, &nul) == 1, "cursorshow(null) accepted");
    CHECK(vm.cursor_show == 0u, "cursorshow(null)=0");
    CHECK(fvp_vm_cursor_show(&vm, &one) == 1, "cursorshow(1) accepted");
    CHECK(vm.cursor_show == 1u, "cursorshow(1)=1");
    CHECK(fvp_vm_cursor_show(&vm, &zero) == 1, "cursorshow(0) accepted");
    CHECK(vm.cursor_show == 1u, "cursorshow(int 0)=1 (non-null)");
    CHECK(fvp_vm_cursor_show(NULL, &one) == 0, "cursorshow NULL vm");

    fvp_vm_free(&vm);
}

static void test_pushfalse_opcode(void)
{
    const char *path = "bin/_nullvalue_pushfalse.hcb";
    FvpHcb hcb;
    FvpVM vm;
    char err[256];

    CHECK(write_file(path, kPushFalseHcb, sizeof(kPushFalseHcb)) == 1,
          "write minimal hcb");
    if (!fvp_hcb_load(&hcb, path, FVP_ENC_CP932, err, sizeof(err))) {
        CHECK(0, "load minimal hcb: %s", err);
        remove(path);
        return;
    }
    CHECK(hcb.ninstr == 1, "one instruction decoded (got %u)",
          (unsigned)hcb.ninstr);
    CHECK(hcb.main_start == 4u, "main_start=%u", (unsigned)hcb.main_start);

    fvp_vm_init(&vm, &hcb, 1);
    fvp_vm_start(&vm);
    (void)fvp_vm_run(&vm, 1);

    CHECK(vm.cur != NULL, "thread exists after one step");
    if (vm.cur != NULL) {
        CHECK(vm.cur->sp == 1, "sp=%d after pushfalse", (int)vm.cur->sp);
        if (vm.cur->sp == 1) {
            FvpValue top = vm.cur->stack[0];

            CHECK(top.type == (uint8_t)FVP_NULL,
                  "pushfalse pushes FVP_NULL (type=%u)", (unsigned)top.type);
            CHECK(fvp_value_is_null(&top) == 1, "pushed value is null");
            CHECK(fvp_value_eq(&top, &(FvpValue){ .type = (uint8_t)FVP_INT,
                                                  .i = 0, .f = 0.0,
                                                  .str = NULL }) == 1,
                  "pushed null == int 0");
        }
    }

    fvp_vm_free(&vm);
    fvp_hcb_free(&hcb);
    remove(path);
}

int main(void)
{
    test_null_value();
    test_lip_and_control();
    test_pushfalse_opcode();

    printf("test_nullvalue: %d passed, %d failed\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}