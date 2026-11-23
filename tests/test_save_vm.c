/* save.bin → VM 全局变量桥测试。
 *
 *   bin/test_save_vm.exe                      合成数据自检
 *   bin/test_save_vm.exe <save.bin>           真实文件：记录类型统计 + 地面真相
 *
 * 地面真相来自 docs/save-format.md §3.3：A 版样本 1471 条记录中
 * type 0 = 480、type 1 = 846、type 2 = 145，且 type 2 的值开头是
 *   5, 5, 1, 0, 0, 180, 255, 128, 100, 90, 70, 70, 65, 100, …
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../src/fvp.h"
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

/* 记录：{u8 type, 3B pad, u32 value} */
static void put_record(unsigned char *p, unsigned char type, unsigned int value)
{
    memset(p, 0, 8);
    p[0] = type;
    p[4] = (unsigned char)(value & 0xFFu);
    p[5] = (unsigned char)((value >> 8) & 0xFFu);
    p[6] = (unsigned char)((value >> 16) & 0xFFu);
    p[7] = (unsigned char)((value >> 24) & 0xFFu);
}

static unsigned char *read_file(const char *path, size_t *len_out)
{
    FILE *f;
    long n;
    unsigned char *buf;

    *len_out = 0;
    f = fopen(path, "rb");
    if (f == NULL) {
        return NULL;
    }
    if (fseek(f, 0, SEEK_END) != 0) {
        fclose(f);
        return NULL;
    }
    n = ftell(f);
    if (n <= 0) {
        fclose(f);
        return NULL;
    }
    rewind(f);
    buf = (unsigned char *)malloc((size_t)n);
    if (buf == NULL) {
        fclose(f);
        return NULL;
    }
    if (fread(buf, 1, (size_t)n, f) != (size_t)n) {
        free(buf);
        fclose(f);
        return NULL;
    }
    fclose(f);
    *len_out = (size_t)n;
    return buf;
}

/* ------------------------------------------------------------- 自检 */

static void selftest(void)
{
    FvpSaveBin b;
    FvpValue globals[8];
    FvpSaveGlobalsStats st;
    int written;
    int i;

    /* 4 条记录的合成 save.bin：type2/int、type1/未知、type0/未设、type2/负数。 */
    memset(&b, 0, sizeof(b));
    b.nglobals = 4;
    b.globals = (unsigned char *)calloc(4, 8);
    b.readmap = (unsigned char *)calloc(FVP_SAVE_READMAP_SIZE, 1);
    b.font_len = 0;
    b.font = (unsigned char *)calloc(1, 1);
    CHECK(b.globals != NULL && b.readmap != NULL, "oom");
    if (b.globals == NULL || b.readmap == NULL) {
        fvp_save_bin_free(&b);
        return;
    }
    put_record(b.globals + 0, 2, 7);
    put_record(b.globals + 8, 1, 9);
    put_record(b.globals + 16, 0, 0);
    put_record(b.globals + 24, 2, 0xFFFFFFF0u); /* int32 -16 */

    for (i = 0; i < 8; ++i) {
        globals[i] = fvp_make_int(0x5A5A); /* 哨兵：未被覆盖的值应保持 */
    }

    memset(&st, 0xFF, sizeof(st));
    written = fvp_save_globals_apply(&b, globals, 8, &st);
    CHECK(written == 2, "written=%d", written);
    CHECK(st.total == 4, "total=%d", st.total);
    CHECK(st.ints == 2, "ints=%d", st.ints);
    CHECK(st.unknown == 1, "unknown=%d", st.unknown);
    CHECK(st.unset == 1, "unset=%d", st.unset);
    CHECK(st.skipped == 0, "skipped=%d", st.skipped);
    CHECK(globals[0].type == (uint8_t)FVP_INT && globals[0].i == 7,
          "globals[0]=%lld", (long long)globals[0].i);
    CHECK(globals[1].type == (uint8_t)FVP_INT && globals[1].i == 0x5A5A,
          "type1 record must be skipped");
    CHECK(globals[2].i == 0x5A5A, "type0 record must be skipped");
    CHECK(globals[3].type == (uint8_t)FVP_INT && globals[3].i == -16,
          "globals[3]=%lld (u32 -> int32)", (long long)globals[3].i);
    CHECK(globals[4].i == 0x5A5A, "globals[4] untouched");

    /* 容量不足时：只统计、不越界。 */
    for (i = 0; i < 8; ++i) {
        globals[i] = fvp_make_int(0x5A5A);
    }
    written = fvp_save_globals_apply(&b, globals, 2, &st);
    CHECK(written == 1, "capacity-limited written=%d", written);
    CHECK(st.ints == 2 && st.skipped == 1, "ints=%d skipped=%d", st.ints,
          st.skipped);
    CHECK(globals[1].i == 0x5A5A, "index 1 is type1 -> untouched");
    CHECK(globals[2].i == 0x5A5A, "index 2 out of capacity");

    /* 只统计模式（globals == NULL）。 */
    written = fvp_save_globals_apply(&b, NULL, 0, &st);
    CHECK(written == 0 && st.ints == 2 && st.total == 4, "stats-only mode");

    /* NULL 文件。 */
    memset(&st, 0xFF, sizeof(st));
    CHECK(fvp_save_globals_apply(NULL, globals, 8, &st) == 0, "NULL bin");
    CHECK(st.total == 0 && st.ints == 0, "NULL bin stats");

    fvp_save_bin_free(&b);
}

/* --------------------------------------------------------- 真实文件 */

static int run_file(const char *path)
{
    unsigned char *buf;
    size_t len = 0;
    FvpSaveBin b;
    FvpValue *globals;
    FvpSaveGlobalsStats st;
    int written;
    int i;
    /* docs/save-format.md §3.3：type 2 的值开头。 */
    static const int expect[] = {5, 5, 1, 0, 0, 180, 255, 128, 100, 90, 70, 70,
                                 65, 100};
    const int nexpect = (int)(sizeof(expect) / sizeof(expect[0]));
    int seen = 0;
    int mismatch = 0;

    buf = read_file(path, &len);
    CHECK(buf != NULL, "cannot read %s", path);
    if (buf == NULL) {
        return 0;
    }
    memset(&b, 0, sizeof(b));
    CHECK(fvp_save_bin_load(&b, buf, len), "save.bin parse failed");
    if (b.globals == NULL) {
        free(buf);
        return 0;
    }
    CHECK(b.nglobals == 1471, "nglobals=%u (expected 1471)", b.nglobals);

    globals = (FvpValue *)calloc(FVP_GLOBAL_COUNT, sizeof(FvpValue));
    CHECK(globals != NULL, "oom");
    if (globals == NULL) {
        fvp_save_bin_free(&b);
        free(buf);
        return 0;
    }
    for (i = 0; i < FVP_GLOBAL_COUNT; ++i) {
        globals[i] = fvp_make_int(-1);
    }

    written = fvp_save_globals_apply(&b, globals, FVP_GLOBAL_COUNT, &st);
    printf("save.bin: records=%d int=%d unknown=%d unset=%d -> applied %d\n",
           st.total, st.ints, st.unknown, st.unset, written);
    CHECK(st.total == 1471, "total=%d", st.total);
    CHECK(st.ints == 145, "ints=%d (docs say 145)", st.ints);
    CHECK(st.unknown == 846, "unknown=%d (docs say 846)", st.unknown);
    CHECK(st.unset == 480, "unset=%d (docs say 480)", st.unset);
    CHECK(written == st.ints, "written=%d != ints=%d", written, st.ints);
    CHECK(st.ints + st.unknown + st.unset == st.total, "type counts do not add up");

    /* 按文件顺序取前 N 个 type2 的值，与文档地面真相比对。 */
    for (i = 0; i < (int)b.nglobals && seen < nexpect; ++i) {
        const unsigned char *rec = b.globals + (size_t)i * 8u;

        if (rec[0] != 2) {
            continue;
        }
        if (globals[i].i != (int64_t)expect[seen]) {
            ++mismatch;
            printf("  mismatch at record %d: got %lld want %d\n", i,
                   (long long)globals[i].i, expect[seen]);
        }
        ++seen;
    }
    CHECK(seen == nexpect, "only %d of %d type-2 records seen", seen, nexpect);
    CHECK(mismatch == 0, "%d value mismatches vs docs §3.3", mismatch);

    /* type1 / type0 的记录必须保持哨兵值（-1），不能被灌进来。 */
    {
        int touched = 0;

        for (i = 0; i < (int)b.nglobals; ++i) {
            if (b.globals[(size_t)i * 8u] == 2) {
                continue;
            }
            if (globals[i].i != -1) {
                ++touched;
            }
        }
        CHECK(touched == 0, "%d non-int records were written into the VM",
              touched);
    }

    free(globals);
    fvp_save_bin_free(&b);
    free(buf);
    return 1;
}

int main(int argc, char **argv)
{
    setvbuf(stdout, NULL, _IONBF, 0);

    if (argc < 2) {
        selftest();
    } else {
        int i;

        for (i = 1; i < argc; ++i) {
            run_file(argv[i]);
        }
    }
    printf("\ntest_save_vm: %d passed, %d failed\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}