/* 全局 flag 位图测试（FlagGet/FlagSet 的存储层 + save.bin 播种）。
 *
 *   bin/test_flags.exe                 合成用例
 *   bin/test_flags.exe <save.bin>      额外检查真实存档里的 flag 位图
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../src/flags.h"
#include "../src/save.h"
#include "../src/save_vm.h"

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

static void selftest(void)
{
    unsigned char map[FVP_FLAG_BYTES];
    int i;

    memset(map, 0, sizeof(map));

    /* 1. 初始全 0。 */
    for (i = 0; i < FVP_FLAG_COUNT; ++i) {
        if (fvp_flag_get(map, i) != 0) {
            break;
        }
    }
    CHECK(i == FVP_FLAG_COUNT, "fresh map has a set bit at %d", i);
    CHECK(fvp_flag_count(map) == 0, "fresh count=%d", fvp_flag_count(map));

    /* 2. 置位 / 重复置位 / 计数。 */
    CHECK(fvp_flag_set(map, 0, 1) == 1, "set(0) should change the bit");
    CHECK(fvp_flag_get(map, 0) == 1, "get(0)=%d", fvp_flag_get(map, 0));
    CHECK(fvp_flag_count(map) == 1, "count=%d", fvp_flag_count(map));
    CHECK(fvp_flag_set(map, 0, 1) == 0, "set(0) twice must not report a change");
    CHECK(fvp_flag_count(map) == 1, "count after repeat=%d",
          fvp_flag_count(map));

    /* 3. 边界：最后一个合法索引、越界两个方向都安全。 */
    CHECK(fvp_flag_valid(0) && fvp_flag_valid(FVP_FLAG_COUNT - 1),
          "valid range");
    CHECK(!fvp_flag_valid(FVP_FLAG_COUNT) && !fvp_flag_valid(-1),
          "out-of-range must be invalid");
    CHECK(fvp_flag_set(map, FVP_FLAG_COUNT - 1, 1) == 1, "set(2047)");
    CHECK(fvp_flag_get(map, FVP_FLAG_COUNT - 1) == 1, "get(2047)");
    CHECK(fvp_flag_get(map, FVP_FLAG_COUNT) == 0, "get(2048) must be 0");
    CHECK(fvp_flag_set(map, FVP_FLAG_COUNT, 1) == 0, "set(2048) must be a no-op");
    CHECK(fvp_flag_set(map, -1, 1) == 0, "set(-1) must be a no-op");
    CHECK(fvp_flag_get(map, -1) == 0, "get(-1) must be 0");
    CHECK(fvp_flag_count(map) == 2, "count=%d", fvp_flag_count(map));

    /* 4. 位序：idx 8 落在第 1 字节的最低位（小端位序，与引擎 `or %esi,(%eax)` 一致）。 */
    CHECK(fvp_flag_set(map, 8, 1) == 1, "set(8)");
    CHECK((map[0] & 0x01) == 0x01, "map[0]=0x%02x after set(0)", map[0]);
    CHECK((map[1] & 0x01) == 0x01, "map[1]=0x%02x after set(8)", map[1]);
    CHECK((map[255] & 0x80) == 0x80, "map[255]=0x%02x after set(2047)",
          map[255]);

    /* 5. 清位。 */
    CHECK(fvp_flag_set(map, 0, 0) == 1, "clear(0) should change the bit");
    CHECK(fvp_flag_get(map, 0) == 0, "get(0) after clear=%d",
          fvp_flag_get(map, 0));
    CHECK(fvp_flag_count(map) == 2, "count after clear=%d (idx 8 and 2047 stay)",
          fvp_flag_count(map));

    /* 6. 计数：两字节全 1 = 16 位（先把整张图清零，别把 bit2047 算进来）。 */
    memset(map, 0, sizeof(map));
    memset(map, 0xFF, 2);
    CHECK(fvp_flag_count(map) == 16, "count(0xFFFF)=%d", fvp_flag_count(map));

    /* 7. NULL 安全。 */
    CHECK(fvp_flag_get(NULL, 0) == 0, "get(NULL) must be 0");
    CHECK(fvp_flag_set(NULL, 0, 1) == 0, "set(NULL) must be a no-op");
    CHECK(fvp_flag_count(NULL) == 0, "count(NULL) must be 0");
    CHECK(fvp_flag_count(map) == 16, "map untouched by NULL calls");
}

/* save.bin -> flags 位图 */
static void check_save_flags(const char *path)
{
    FvpSaveBin b;
    unsigned char flags[FVP_FLAG_BYTES];
    int got;

    memset(&b, 0, sizeof(b));
    CHECK(fvp_save_bin_load_file(&b, path), "cannot load %s", path);
    if (b.globals == NULL && b.readmap == NULL) {
        return;
    }

    memset(flags, 0xAA, sizeof(flags)); /* 哨兵 */
    got = fvp_save_flags_apply(&b, flags, sizeof(flags));
    CHECK(got == FVP_FLAG_BYTES, "flags_apply copied %d bytes", got);
    CHECK(memcmp(flags, b.flags, FVP_FLAG_BYTES) == 0,
          "flags bitmap differs from save.bin");
    CHECK(fvp_flag_count(flags) == 0,
          "expected 0 flags set in this title (docs §3.5), got %d",
          fvp_flag_count(flags));

    /* 容量不足 / NULL 都要拒绝。 */
    {
        unsigned char small[FVP_FLAG_BYTES - 1];

        memset(small, 0, sizeof(small));
        CHECK(fvp_save_flags_apply(&b, small, sizeof(small)) == 0,
              "short buffer must be rejected");
    }
    CHECK(fvp_save_flags_apply(&b, NULL, FVP_FLAG_BYTES) == 0,
          "NULL target must be rejected");
    CHECK(fvp_save_flags_apply(NULL, flags, FVP_FLAG_BYTES) == 0,
          "NULL savebin must be rejected");

    /* 合成：置几位置进 b.flags 再搬出来。 */
    b.flags[0] = 0x05; /* bit0 + bit2 */
    b.flags[255] = 0x80;
    got = fvp_save_flags_apply(&b, flags, sizeof(flags));
    CHECK(got == FVP_FLAG_BYTES, "synthetic flags_apply=%d", got);
    CHECK(fvp_flag_count(flags) == 3, "synthetic count=%d (%d,%d,%d)",
          fvp_flag_count(flags), fvp_flag_get(flags, 0), fvp_flag_get(flags, 2),
          fvp_flag_get(flags, 2047));

    fvp_save_bin_free(&b);
}

int main(int argc, char **argv)
{
    setvbuf(stdout, NULL, _IONBF, 0);

    selftest();
    if (argc >= 2) {
        int i;

        for (i = 1; i < argc; ++i) {
            check_save_flags(argv[i]);
        }
    }
    printf("\ntest_flags: %d passed, %d failed\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}