/* 台词历史记录单元测试：HistorySet(26) / HistoryGet(25) 的存储层。
 *
 * 语义与静态分析出处见 src/history.h 顶部（handler 0x43A5E0 / 0x43A490，
 * 取字段 0x438090 / 0x4380C0 / 0x4380F0）。这里只测存储层，不经过 VM：
 *   - 100 条环形、count 上限
 *   - "开新的一条"推进 pos、清空该槽
 *   - 字符串整串替换、value 只收 >= 0
 *   - 环下标 slot = pos - idx（负数 +100）
 *   - 越界返回"无"，未设置的字符串返回空串、未设置的 value 返回"无"
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../src/history.h"
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

#define SET_TEXT(h, kind, lit) \
    fvp_history_set_text(&(h), (kind), (lit), strlen(lit))

/* VM 级：HistorySet(26)/HistoryGet(25) 的**参数判定**（src/vm.c 的
 * fvp_vm_history_set/get）。静态分析依据：0x43A5E4 用 `test dl,dl` 判 type 0，
 * 即只有 Variant **type 0（null）** 才是"开新的一条"，int 0 不算；
 * HistoryGet 同样只认 null 与 int 两类 arg0。 */
static void test_vm_history(void)
{
    FvpVM vm;
    FvpValue args[2];
    FvpValue out;
    FvpStr *s_name;
    FvpStr *s_line;
    int i;

    fvp_vm_init(&vm, NULL, 1);
    s_name = fvp_arena_str(&vm.arena, "name", 4);
    s_line = fvp_arena_str(&vm.arena, "line", 4);

    /* null → 开新的一条（init count=1 → 这里 count=2，pos 先自增到 1：
     * 0x43A5FC 的顺序是先 ++ 再写，所以新记录落在槽 1，槽 0 = 上一行） */
    args[0] = fvp_make_null();
    args[1] = fvp_make_int(0);
    CHECK(fvp_vm_history_set(&vm, args, 2) == 1, "vm set(null) accepted");
    CHECK(vm.history.count == 2 && vm.history.pos == 1,
          "vm set(null): count=%u pos=%u", vm.history.count, vm.history.pos);
    CHECK(vm.history.entry[1].value == -1 &&
              vm.history.entry[1].text0 == NULL,
          "vm set(null) clears the new slot");

    /* int 0 不等于 null：不能开新条，只写 text0 */
    args[0] = fvp_make_int(0);
    args[1] = fvp_make_str(s_name);
    CHECK(fvp_vm_history_set(&vm, args, 2) == 1, "vm set(0,str) accepted");
    CHECK(vm.history.count == 2, "vm set(0,str) does not bump count");
    CHECK(vm.history.entry[1].text0 != NULL &&
              strcmp(vm.history.entry[1].text0, "name") == 0,
          "vm set(0,str) -> text0");

    args[0] = fvp_make_int(1);
    args[1] = fvp_make_str(s_line);
    CHECK(fvp_vm_history_set(&vm, args, 2) == 1, "vm set(1,str) accepted");
    CHECK(vm.history.entry[1].text1 != NULL &&
              strcmp(vm.history.entry[1].text1, "line") == 0,
          "vm set(1,str) -> text1");

    args[0] = fvp_make_int(2);
    args[1] = fvp_make_int(12345);
    CHECK(fvp_vm_history_set(&vm, args, 2) == 1, "vm set(2,int) accepted");
    CHECK(vm.history.entry[1].value == 12345, "vm set(2,int) -> value");

    /* 类型/范围不符：整条忽略（返回 0，状态不变） */
    args[0] = fvp_make_int(2);
    args[1] = fvp_make_int(-1);
    CHECK(fvp_vm_history_set(&vm, args, 2) == 0, "vm set(2,-1) rejected");
    CHECK(vm.history.entry[1].value == 12345, "value unchanged after reject");
    args[1] = fvp_make_str(s_line);
    CHECK(fvp_vm_history_set(&vm, args, 2) == 0, "vm set(2,str) rejected");
    args[0] = fvp_make_int(0);
    args[1] = fvp_make_int(0); /* kind 0 要字符串，int 被忽略 */
    CHECK(fvp_vm_history_set(&vm, args, 2) == 0, "vm set(0,int) rejected");
    args[0] = fvp_make_int(3);
    args[1] = fvp_make_str(s_line);
    CHECK(fvp_vm_history_set(&vm, args, 2) == 0, "vm set(kind 3) rejected");
    args[0] = fvp_make_float(0.0);
    CHECK(fvp_vm_history_set(&vm, args, 2) == 0, "vm set(float 0.0) rejected");

    /* HistoryGet：null → count；kind 0/1 → 字符串；kind 2 → 整型 */
    args[0] = fvp_make_null();
    CHECK(fvp_vm_history_get(&vm, args, 2, &out) == 1, "vm get(null) returns");
    CHECK(out.type == (uint8_t)FVP_INT && out.i == 2,
          "vm get(null) -> count 2 (got type %u i %lld)",
          (unsigned)out.type, (long long)out.i);

    args[0] = fvp_make_int(0);
    args[1] = fvp_make_int(0);
    CHECK(fvp_vm_history_get(&vm, args, 2, &out) == 1, "vm get(0,0) returns");
    CHECK(out.type == (uint8_t)FVP_STR && out.str != NULL &&
              out.str->len == 4 && memcmp(out.str->data, "name", 4) == 0,
          "vm get(0,0) -> text0");

    args[0] = fvp_make_int(1);
    CHECK(fvp_vm_history_get(&vm, args, 2, &out) == 1, "vm get(1,0) returns");
    CHECK(out.type == (uint8_t)FVP_STR && out.str != NULL &&
              memcmp(out.str->data, "line", 4) == 0, "vm get(1,0) -> text1");

    args[0] = fvp_make_int(2);
    CHECK(fvp_vm_history_get(&vm, args, 2, &out) == 1, "vm get(2,0) returns");
    CHECK(out.type == (uint8_t)FVP_INT && out.i == 12345, "vm get(2,0) -> value");

    /* idx 越界 / kind 越界 / 类型不符 → 原版不写返回值 */
    args[1] = fvp_make_int(1);
    CHECK(fvp_vm_history_get(&vm, args, 2, &out) == 0, "vm get(2,1) no return");
    args[0] = fvp_make_int(3);
    args[1] = fvp_make_int(0);
    CHECK(fvp_vm_history_get(&vm, args, 2, &out) == 0, "vm get(3,0) no return");
    args[0] = fvp_make_int(0);
    args[1] = fvp_make_int(-1);
    CHECK(fvp_vm_history_get(&vm, args, 2, &out) == 0, "vm get(0,-1) no return");
    args[1] = fvp_make_str(s_name);
    CHECK(fvp_vm_history_get(&vm, args, 2, &out) == 0, "vm get(0,str) no return");

    /* 再开一条：idx 0 变成"刚开的那条"，字段未设置 → 空串（不是 null） */
    args[0] = fvp_make_null();
    CHECK(fvp_vm_history_set(&vm, args, 2) == 1, "vm set(null) again");
    CHECK(vm.history.count == 3 && vm.history.pos == 2,
          "vm second begin: count=%u pos=%u", vm.history.count, vm.history.pos);
    args[0] = fvp_make_int(1);
    args[1] = fvp_make_int(0);
    CHECK(fvp_vm_history_get(&vm, args, 2, &out) == 1, "vm get(1,0) after begin");
    CHECK(out.type == (uint8_t)FVP_STR && out.str != NULL && out.str->len == 0,
          "vm get(1,0) after begin -> empty string, not null");

    /* count 上限 100：101 次 begin 之后 count 仍是 100、pos 回绕 */
    for (i = 0; i < 101; ++i) {
        args[0] = fvp_make_null();
        (void)fvp_vm_history_set(&vm, args, 2);
    }
    CHECK(vm.history.count == (uint32_t)FVP_HISTORY_SLOTS,
          "vm count capped at 100 (got %u)", vm.history.count);
    CHECK(vm.history.pos < (uint32_t)FVP_HISTORY_SLOTS,
          "vm pos stays inside the ring (got %u)", vm.history.pos);

    /* 计数器：set/get 都记录到了 */
    CHECK(vm.history.sets == 9 + 1 + 101, "vm sets counter=%llu",
          (unsigned long long)vm.history.sets);
    CHECK(vm.history.gets == 9, "vm gets counter=%llu",
          (unsigned long long)vm.history.gets);

    fvp_vm_free(&vm);
}

int main(void)
{
    FvpHistory h;
    int32_t v = 0;
    int i;

    /* --- 1. 初始化：count = 1（当前这一行）、pos = 0；
     * 所有 value = -1、字符串 = NULL。
     * 依据 0x442C4A `movl $0x1,(%esi)`（容器构造）+ 存档实测（s481 未开新条
     * 的存档 count=1/pos=0；s905 开过 10 次 count=11/pos=10）。 --- */
    fvp_history_init(&h);
    CHECK(h.count == 1 && h.pos == 0, "init: count=1 pos=0");
    CHECK(h.sets == 0 && h.gets == 0, "init: counters 0");
    CHECK(h.entry[0].value == -1 && h.entry[0].text0 == NULL &&
              h.entry[0].text1 == NULL,
          "init: entry 0 cleared");
    CHECK(h.entry[FVP_HISTORY_SLOTS - 1].value == -1,
          "init: every slot value = -1");
    CHECK(fvp_history_filled(&h) == 0, "init: nothing filled");

    /* --- 2. 写入：字符串整串替换、value 只收 >= 0 --- */
    CHECK(SET_TEXT(h, 1, "first line") == 1, "set text1");
    CHECK(strcmp(h.entry[0].text1, "first line") == 0, "text1 stored");
    CHECK(h.entry[0].text0 == NULL, "text0 untouched");
    CHECK(fvp_history_set_value(&h, 7) == 1, "set value 7");
    CHECK(h.entry[0].value == 7, "value stored");
    CHECK(SET_TEXT(h, 1, "second") == 1, "replace text1");
    CHECK(strcmp(h.entry[0].text1, "second") == 0, "text1 replaced (not concat)");
    CHECK(fvp_history_set_value(&h, -1) == 0, "negative value rejected");
    CHECK(h.entry[0].value == 7, "value unchanged after reject");
    CHECK(SET_TEXT(h, 2, "x") == 0, "kind 2 is not a text kind");
    CHECK(SET_TEXT(h, -1, "x") == 0, "kind -1 rejected");

    /* --- 3. 空串 / NULL 也写进去（原版不分空与非空） --- */
    CHECK(SET_TEXT(h, 0, "") == 1, "empty string accepted");
    CHECK(h.entry[0].text0 != NULL && h.entry[0].text0[0] == '\0',
          "empty string stored as \"\"");
    CHECK(fvp_history_set_text(&h, 0, NULL, 0) == 1, "NULL string accepted");
    CHECK(h.entry[0].text0[0] == '\0', "NULL string becomes empty");

    /* --- 4. 还没开新条时：只有 idx 0（当前行，槽 0）可读，idx 1 越界 --- */
    CHECK(fvp_history_slot(&h, 0) == 0, "slot: idx 0 with count 1 -> 0");
    CHECK(fvp_history_slot(&h, 1) == -1, "slot: idx 1 with count 1 -> -1");
    CHECK(fvp_history_text(&h, 1, 1) == NULL, "text: out of range -> NULL");
    CHECK(fvp_history_value(&h, 1, &v) == 0, "value: out of range -> 0");

    /* --- 5. "开新的一条"：count++、pos++、清空该槽 --- */
    CHECK(fvp_history_begin(&h) == 1, "begin 1");
    CHECK(h.count == 2 && h.pos == 1, "begin 1: count=2 pos=1");
    CHECK(h.entry[1].text0 == NULL && h.entry[1].text1 == NULL &&
              h.entry[1].value == -1,
          "begin 1: new slot cleared");
    CHECK(fvp_history_slot(&h, 0) == 1, "slot: idx 0 = pos");
    CHECK(fvp_history_slot(&h, 1) == 0, "slot: idx 1 = previous");
    CHECK(fvp_history_slot(&h, 2) == -1, "slot: idx 2 beyond count -> -1");
    CHECK(fvp_history_filled(&h) == 1, "filled: only the old slot has data");

    CHECK(SET_TEXT(h, 1, "line A") == 1, "write line A");
    CHECK(fvp_history_begin(&h) == 1, "begin 2");
    CHECK(SET_TEXT(h, 1, "line B") == 1, "write line B");
    CHECK(h.count == 3 && h.pos == 2, "begin 2: count=3 pos=2");
    /* idx 0 = 最近一条（pos），idx 1 = 前一条 */
    CHECK(strcmp(fvp_history_text(&h, 1, 0), "line B") == 0, "idx 0 = newest");
    CHECK(strcmp(fvp_history_text(&h, 1, 1), "line A") == 0, "idx 1 = previous");

    /* --- 6. 历史条的 value：-1 表示未设置 --- */
    CHECK(fvp_history_value(&h, 0, &v) == 0, "newest value unset -> 0");
    CHECK(fvp_history_set_value(&h, 42) == 1, "set value 42 on newest");
    CHECK(fvp_history_value(&h, 0, &v) == 1 && v == 42, "value 42 read back");
    h.entry[h.pos].value = -1; /* 手工置回 -1（模拟原版清空语义） */
    CHECK(fvp_history_value(&h, 0, &v) == 0, "value -1 reads as unset");

    /* --- 7. 环回绕：第 100 条把 pos 推回 0，count 停在上限 --- */
    fvp_history_free(&h);
    fvp_history_init(&h);
    for (i = 0; i < FVP_HISTORY_SLOTS; ++i) {
        char buf[32];

        (void)fvp_history_begin(&h);
        snprintf(buf, sizeof buf, "line %d", i);
        CHECK(SET_TEXT(h, 1, buf) == 1, "write slot %d", i);
    }
    CHECK(h.count == (uint32_t)FVP_HISTORY_SLOTS, "count reached the cap (100)");
    CHECK(h.pos == 0, "pos wrapped back to 0");
    CHECK(fvp_history_filled(&h) == FVP_HISTORY_SLOTS, "all 100 slots filled");
    CHECK(strcmp(fvp_history_text(&h, 1, 0), "line 99") == 0,
          "idx 0 is the last written entry");
    CHECK(strcmp(fvp_history_text(&h, 1, 99), "line 0") == 0,
          "idx 99 is the oldest entry");
    CHECK(fvp_history_slot(&h, 99) == 1, "slot 99 wraps to 1");
    CHECK(fvp_history_slot(&h, 100) == -1, "idx 100 out of range");
    CHECK(fvp_history_slot(&h, -1) == -1, "negative idx out of range");

    /* 写满后继续 begin：count 不再增长，pos 继续环走 */
    CHECK(fvp_history_begin(&h) == 1, "begin after cap");
    CHECK(h.count == (uint32_t)FVP_HISTORY_SLOTS, "count stays at cap");
    CHECK(h.pos == 1, "pos advanced after wrap");
    CHECK(h.entry[1].text1 == NULL, "wrapped slot cleared");

    /* --- 8. 截断：超过 FVP_HISTORY_TEXT_MAX 的串被截断，不越界 --- */
    fvp_history_free(&h);
    fvp_history_init(&h);
    {
        size_t big = (size_t)FVP_HISTORY_TEXT_MAX + 64u;
        char *buf = (char *)malloc(big + 1u);

        if (buf != NULL) {
            size_t k;

            for (k = 0; k < big; ++k) {
                buf[k] = 'a';
            }
            buf[big] = '\0';
            CHECK(fvp_history_set_text(&h, 1, buf, big) == 1, "huge string ok");
            CHECK(strlen(h.entry[0].text1) == (size_t)FVP_HISTORY_TEXT_MAX,
                  "huge string truncated to FVP_HISTORY_TEXT_MAX");
            free(buf);
        }
    }

    /* --- 9. free 可重复调用；free 之后结构是"零"，再 init 才回到 -1 --- */
    fvp_history_free(&h);
    fvp_history_free(&h);
    CHECK(h.count == 0 && h.pos == 0, "free zeroes count/pos");
    CHECK(h.entry[0].text0 == NULL && h.entry[0].text1 == NULL,
          "free releases the strings");
    fvp_history_init(&h);
    CHECK(h.entry[0].value == -1, "init after free restores value = -1");

    /* --- 10. NULL / 越界安全 --- */
    CHECK(fvp_history_begin(NULL) == 0, "begin(NULL)");
    CHECK(fvp_history_set_text(NULL, 1, "x", 1) == 0, "set_text(NULL)");
    CHECK(fvp_history_set_value(NULL, 1) == 0, "set_value(NULL)");
    CHECK(fvp_history_slot(NULL, 0) == -1, "slot(NULL)");
    CHECK(fvp_history_text(NULL, 1, 0) == NULL, "text(NULL)");
    CHECK(fvp_history_value(NULL, 0, &v) == 0, "value(NULL)");
    CHECK(fvp_history_filled(NULL) == 0, "filled(NULL)");
    fvp_history_free(NULL);
    CHECK(1, "free(NULL) does not crash");

    test_vm_history();

    printf("test_history: %d passed, %d failed\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}