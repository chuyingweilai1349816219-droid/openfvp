/* 槽尾字符串池解析的单测（src/savetail.c）。
 *
 * 重点：池的位置**不是固定偏移**——它跟在"每槽全局变量"（条数 × 8 字节）之后，
 * 序列化器里第 6 段的顺序已冻结（ref/exe-save-serialize.json）。6 份真实存档的
 * 全局变量条数都是 1471，所以标签落在 0x16610 + 1471×8 = 0x19408；换条数就会平移。
 *
 * 覆盖：
 *   - 合成槽尾：全局变量 0 条 / 1471 条 / 100 条时都能定位到正确的标签偏移
 *   - 畸形：标签缺失、条数 > 8、字符串没有 NUL、两个候选（歧义）→ 放弃
 *   - 真实存档：s905 / s481 能解出图名与台词；系统存档 count=0 但结构仍在
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../src/fvp.h"
#include "../src/save.h"
#include "../src/savetail.h"

static int g_pass = 0;
static int g_fail = 0;

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

#define TAIL_SIZE 0x40000u

static void put_u32(unsigned char *p, unsigned int v)
{
    p[0] = (unsigned char)(v & 0xFF);
    p[1] = (unsigned char)((v >> 8) & 0xFF);
    p[2] = (unsigned char)((v >> 16) & 0xFF);
    p[3] = (unsigned char)((v >> 24) & 0xFF);
}

/* 在 0x16610 + globals 处放一个标签 + 两条字符串。返回标签偏移。 */
static size_t put_pool(unsigned char *tail, unsigned int globals_bytes,
                       const char *graph, const char *line)
{
    size_t off = FVP_SAVETAIL_POOL_BASE + globals_bytes;
    size_t cur = off + 8;
    unsigned int n;

    put_u32(tail + off, FVP_SAVETAIL_TAG);
    put_u32(tail + off + 4, 2);
    n = (unsigned int)strlen(graph) + 1u;
    tail[cur] = (unsigned char)(n & 0xFF);
    tail[cur + 1] = (unsigned char)(n >> 8);
    cur += 2;
    memcpy(tail + cur, graph, n);
    cur += n;
    cur += 4; /* 条目之间实测有 0 填充 */
    n = (unsigned int)strlen(line) + 1u;
    tail[cur] = (unsigned char)(n & 0xFF);
    tail[cur + 1] = (unsigned char)(n >> 8);
    cur += 2;
    memcpy(tail + cur, line, n);
    return off;
}

static void test_synthetic(void)
{
    unsigned char *tail = (unsigned char *)calloc(1, TAIL_SIZE);
    FvpSaveTailInfo info;
    size_t off = 0;
    size_t want;

    if (tail == NULL) {
        CHECK(0, "calloc failed");
        return;
    }

    /* --- 1. 0 条全局变量：标签在 0x16610 --- */
    want = put_pool(tail, 0, "BG063_000b", "hello line");
    CHECK(fvp_savetail_find_pool(tail, TAIL_SIZE, &off) == 1,
          "find: 0 globals");
    CHECK(off == want && off == FVP_SAVETAIL_POOL_BASE,
          "find: offset %#lx (want %#lx)", (unsigned long)off,
          (unsigned long)want);
    CHECK(fvp_savetail_parse(&info, tail, TAIL_SIZE, FVP_ENC_CP932) == 1,
          "parse: 0 globals");
    CHECK(info.count == 2, "parse: count=%d", info.count);
    CHECK(info.graph_index == 0, "parse: graph index");
    CHECK(info.pool_offset == want, "parse: pool_offset");

    /* --- 2. 1471 条全局变量：标签在 0x19408（6 份真实存档的实测值） --- */
    memset(tail, 0, TAIL_SIZE);
    want = put_pool(tail, 1471u * 8u, "BG063_000b", "hello line");
    CHECK(want == 0x19408u, "synthetic: 1471×8 -> %#lx", (unsigned long)want);
    CHECK(fvp_savetail_find_pool(tail, TAIL_SIZE, &off) == 1 &&
              off == 0x19408u, "find: 1471 globals -> %#lx",
          (unsigned long)off);

    /* --- 3. 100 条全局变量：标签随机平移 --- */
    memset(tail, 0, TAIL_SIZE);
    want = put_pool(tail, 100u * 8u, "EV001_000a", "line two");
    CHECK(fvp_savetail_find_pool(tail, TAIL_SIZE, &off) == 1 && off == want,
          "find: 100 globals -> %#lx (want %#lx)", (unsigned long)off,
          (unsigned long)want);
    CHECK(fvp_savetail_parse(&info, tail, TAIL_SIZE, FVP_ENC_CP932) == 1 &&
              info.count == 2, "parse: 100 globals");

    /* --- 4. 畸形：标签缺失 --- */
    memset(tail, 0, TAIL_SIZE);
    CHECK(fvp_savetail_find_pool(tail, TAIL_SIZE, &off) == 0,
          "find: no tag -> 0");

    /* --- 5. 畸形：条数 > 8 --- */
    memset(tail, 0, TAIL_SIZE);
    put_u32(tail + FVP_SAVETAIL_POOL_BASE, FVP_SAVETAIL_TAG);
    put_u32(tail + FVP_SAVETAIL_POOL_BASE + 4, 9);
    CHECK(fvp_savetail_find_pool(tail, TAIL_SIZE, &off) == 0,
          "find: count 9 -> 0");

    /* --- 6. 畸形：字符串没有 NUL 结尾 --- */
    memset(tail, 0, TAIL_SIZE);
    put_u32(tail + FVP_SAVETAIL_POOL_BASE, FVP_SAVETAIL_TAG);
    put_u32(tail + FVP_SAVETAIL_POOL_BASE + 4, 1);
    tail[FVP_SAVETAIL_POOL_BASE + 8] = 3;
    tail[FVP_SAVETAIL_POOL_BASE + 9] = 0;
    tail[FVP_SAVETAIL_POOL_BASE + 10] = 'a';
    tail[FVP_SAVETAIL_POOL_BASE + 11] = 'b';
    tail[FVP_SAVETAIL_POOL_BASE + 12] = 'c'; /* 结尾不是 0 */
    CHECK(fvp_savetail_find_pool(tail, TAIL_SIZE, &off) == 0,
          "find: string without NUL -> 0");

    /* --- 7. 畸形：两个候选 → 歧义，放弃 --- */
    memset(tail, 0, TAIL_SIZE);
    put_pool(tail, 0, "BG063_000b", "line");
    put_pool(tail, 0x800, "BG064_000b", "line2");
    CHECK(fvp_savetail_find_pool(tail, TAIL_SIZE, &off) == 0,
          "find: two pools -> 0");

    free(tail);
}

static void test_real_slots(void)
{
    static const struct {
        const wchar_t *path;
        int count;          /* 期望条数 */
        const char *graph;  /* 期望图名子串（NULL = 不检查） */
    } cases[] = {
        { L"C:\\Users\\Mechrevo\\Downloads\\五彩斑斓的世界HD\\save\\s905.bin",
          2, "BG063_000b" },
        { L"C:\\Users\\Mechrevo\\Downloads\\五彩斑斓的世界HD\\save\\s481.bin",
          1, NULL },
        { L"C:\\Users\\Mechrevo\\Downloads\\五彩斑斓的世界HD\\save\\s800.bin",
          0, NULL },
    };
    int i;

    for (i = 0; i < (int)(sizeof(cases) / sizeof(cases[0])); ++i) {
        FILE *f = _wfopen(cases[i].path, L"rb");
        unsigned char *buf;
        long n;
        FvpSaveSlot sd;
        FvpSaveTailInfo info;
        size_t off = 0;

        if (f == NULL) {
            printf("  SKIP: slot %d (file missing)\n", i);
            continue;
        }
        fseek(f, 0, SEEK_END);
        n = ftell(f);
        fseek(f, 0, SEEK_SET);
        buf = (unsigned char *)malloc((size_t)n);
        if (buf == NULL || fread(buf, 1, (size_t)n, f) != (size_t)n) {
            free(buf);
            fclose(f);
            CHECK(0, "read slot %d", i);
            continue;
        }
        fclose(f);
        memset(&sd, 0, sizeof sd);
        CHECK(fvp_save_slot_load(&sd, buf, (size_t)n, 0, 0) == 1,
              "slot %d: parsed", i);
        CHECK(fvp_savetail_find_pool(sd.tail, sd.tail_len, &off) == 1,
              "slot %d: pool found", i);
        CHECK(off == 0x19408u, "slot %d: pool offset %#lx（1471 条全局变量）",
              i, (unsigned long)off);
        if (cases[i].count > 0) {
            CHECK(fvp_savetail_parse(&info, sd.tail, sd.tail_len,
                                     FVP_ENC_CP932) == 1,
                  "slot %d: parsed pool", i);
            CHECK(info.count == cases[i].count, "slot %d: count=%d（期望 %d）",
                  i, info.count, cases[i].count);
            if (cases[i].graph != NULL) {
                int hit = 0;
                int k;

                for (k = 0; k < info.count; ++k) {
                    if (strstr(info.str[k], cases[i].graph) != NULL) {
                        hit = 1;
                    }
                }
                CHECK(hit, "slot %d: 图名含 %s", i, cases[i].graph);
            }
        } else {
            /* 系统存档：标签在、条数为 0 → parse 返回 0（没有可显示的内容） */
            CHECK(fvp_savetail_parse(&info, sd.tail, sd.tail_len,
                                     FVP_ENC_CP932) == 0,
                  "slot %d: empty pool -> parse returns 0", i);
        }
        fvp_save_slot_free(&sd);
        free(buf);
    }
}

int main(void)
{
    test_synthetic();
    test_real_slots();
    printf("test_savetail: %d passed, %d failed\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}