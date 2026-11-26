/* 槽尾「台词历史环」解析 + 回灌的单测（src/savehist.c）。
 *
 * 覆盖：
 *   - 合成槽尾里的定位/解析（count/pos/文本/value 全对）
 *   - 回灌进 FvpHistory（count/pos/字符串/值），以及"覆盖掉旧内容"
 *   - 拒绝：count=0 / count>100 / count==100（回绕判据不足）/ pos 不等于
 *     count-1 / 字段超长 / 控制字节 / 尾部截断 / 两个候选（歧义）
 *   - 真实存档（缺文件时 SKIP）：s905 -> tail+0x20633、count=11/pos=10、
 *     最新一条是中文台词；s481 -> count=1/pos=0
 *
 * 静态分析依据见 src/savehist.h 顶部与 ref/exe-history.json。
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../src/fvp.h"
#include "../src/history.h"
#include "../src/savehist.h"
#include "../src/save.h"

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

#define TAIL_SIZE 0x8000u

/* 往 tail 的 off 处写一条记录：{len0, text0, len1, text1, value}。 */
static size_t put_rec(unsigned char *tail, size_t off, const char *t0,
                      const char *t1, int32_t value)
{
    size_t n0 = t0 != NULL ? strlen(t0) : 0;
    size_t n1 = t1 != NULL ? strlen(t1) : 0;

    tail[off + 0] = (unsigned char)(n0 & 0xFFu);
    tail[off + 1] = (unsigned char)((n0 >> 8) & 0xFFu);
    tail[off + 2] = 0;
    tail[off + 3] = 0;
    off += 4;
    if (n0 != 0) {
        memcpy(tail + off, t0, n0);
    }
    off += n0;
    tail[off + 0] = (unsigned char)(n1 & 0xFFu);
    tail[off + 1] = (unsigned char)((n1 >> 8) & 0xFFu);
    tail[off + 2] = 0;
    tail[off + 3] = 0;
    off += 4;
    if (n1 != 0) {
        memcpy(tail + off, t1, n1);
    }
    off += n1;
    tail[off + 0] = (unsigned char)(value & 0xFF);
    tail[off + 1] = (unsigned char)((value >> 8) & 0xFF);
    tail[off + 2] = (unsigned char)((value >> 16) & 0xFF);
    tail[off + 3] = (unsigned char)((value >> 24) & 0xFF);
    return off + 4;
}

/* 在 tail 的 start 处写一个 100 条的环：前面 8 字节是 count/pos。 */
static int build_ring(unsigned char *tail, size_t size, size_t start,
                      unsigned int count, unsigned int pos)
{
    size_t off = start;
    unsigned int i;

    if (start < 8 || start + 8 > size) {
        return 0;
    }
    tail[start - 8] = (unsigned char)(count & 0xFF);
    tail[start - 7] = (unsigned char)((count >> 8) & 0xFF);
    tail[start - 6] = (unsigned char)((count >> 16) & 0xFF);
    tail[start - 5] = (unsigned char)((count >> 24) & 0xFF);
    tail[start - 4] = (unsigned char)(pos & 0xFF);
    tail[start - 3] = (unsigned char)((pos >> 8) & 0xFF);
    tail[start - 2] = (unsigned char)((pos >> 16) & 0xFF);
    tail[start - 1] = (unsigned char)((pos >> 24) & 0xFF);
    for (i = 0; i < (unsigned int)FVP_SAVEHIST_SLOTS; ++i) {
        char buf[32];

        if (i < count) {
            snprintf(buf, sizeof buf, " line %u ", i);
            off = put_rec(tail, off, "", buf, -1);
        } else {
            off = put_rec(tail, off, NULL, NULL, -1);
        }
        if (off >= size) {
            return 0;
        }
    }
    return 1;
}

static void test_synthetic(void)
{
    unsigned char *tail = (unsigned char *)calloc(1, TAIL_SIZE);
    FvpSaveHist sh;
    FvpHistory h;
    const size_t start = 0x2000;
    size_t off;

    if (tail == NULL) {
        CHECK(0, "calloc failed");
        return;
    }
    fvp_savehist_init(&sh);
    fvp_history_init(&h);

    /* --- 1. 正常环：count=3 pos=2，文本是 " line 0/1/2 " --- */
    CHECK(build_ring(tail, TAIL_SIZE, start, 3, 2) == 1, "build ring");
    CHECK(fvp_savehist_locate(&sh, tail, TAIL_SIZE, NULL, 0, FVP_ENC_GBK) == 1,
          "locate: normal ring found");
    CHECK(sh.count == 3 && sh.pos == 2, "locate: count=%u pos=%u",
          sh.count, sh.pos);
    CHECK(sh.offset == start, "locate: offset=%lu", (unsigned long)sh.offset);
    CHECK(sh.rec[0].text1 != NULL &&
              strcmp(sh.rec[0].text1, " line 0 ") == 0,
          "locate: rec0 text1=%s", sh.rec[0].text1);
    CHECK(sh.rec[2].text1 != NULL &&
              strcmp(sh.rec[2].text1, " line 2 ") == 0,
          "locate: rec2 text1");
    CHECK(sh.rec[2].value == -1, "locate: value = -1");
    CHECK(sh.rec[3].text1 == NULL && sh.rec[3].value == -1,
          "locate: slot past count is empty");

    /* 父串匹配：给对 -> 仍然找得到；给错 -> 找不到 */
    fvp_savehist_init(&sh);
    CHECK(fvp_savehist_locate(&sh, tail, TAIL_SIZE,
                              (const unsigned char *)" line 2 ", 8,
                              FVP_ENC_GBK) == 1,
          "locate: matching current line");
    fvp_savehist_init(&sh);
    CHECK(fvp_savehist_locate(&sh, tail, TAIL_SIZE,
                              (const unsigned char *)" other", 5,
                              FVP_ENC_GBK) == 0,
          "locate: mismatching current line -> rejected");

    /* --- 2. 回灌进 FvpHistory --- */
    fvp_savehist_init(&sh);
    CHECK(fvp_savehist_locate(&sh, tail, TAIL_SIZE, NULL, 0, FVP_ENC_GBK) == 1,
          "locate again");
    CHECK(fvp_savehist_apply(&sh, &h) == 1, "apply ok");
    CHECK(h.count == 3 && h.pos == 2, "apply: count/pos");
    CHECK(h.entry[1].text1 != NULL &&
              strcmp(h.entry[1].text1, " line 1 ") == 0, "apply: text");
    CHECK(fvp_history_filled(&h) == 3, "apply: filled=%d",
          fvp_history_filled(&h));
    CHECK(strcmp(fvp_history_text(&h, 1, 0), " line 2 ") == 0,
          "apply: HistoryGet(1, 0) = newest");
    CHECK(fvp_history_slot(&h, 0) == 2 && fvp_history_slot(&h, 2) == 0,
          "apply: slot mapping");
    /* 再 apply 一次：旧的被替换，不泄漏/不叠加 */
    CHECK(fvp_savehist_apply(&sh, &h) == 1, "apply twice");
    CHECK(h.count == 3 && fvp_history_filled(&h) == 3, "apply twice: state");
    fvp_savehist_free(&sh);

    /* --- 3. 拒绝掉的形态 --- */
    off = start;
    (void)off;
    {
        unsigned char *bad = (unsigned char *)calloc(1, TAIL_SIZE);

        if (bad != NULL) {
            FvpSaveHist b;

            CHECK(build_ring(bad, TAIL_SIZE, start, 3, 2) == 1, "bad ring seed");

            /* count=0 */
            memset(bad + start - 8, 0, 8);
            fvp_savehist_init(&b);
            CHECK(fvp_savehist_locate(&b, bad, TAIL_SIZE, NULL, 0,
                                      FVP_ENC_GBK) == 0,
                  "reject: count = 0");
            fvp_savehist_free(&b);

            /* count > 100 */
            CHECK(build_ring(bad, TAIL_SIZE, start, 3, 2) == 1, "reseed");
            bad[start - 8] = 200;
            fvp_savehist_init(&b);
            CHECK(fvp_savehist_locate(&b, bad, TAIL_SIZE, NULL, 0,
                                      FVP_ENC_GBK) == 0,
                  "reject: count > 100");
            fvp_savehist_free(&b);

            /* count == 100（回绕）：判据不足，明确拒收 */
            CHECK(build_ring(bad, TAIL_SIZE, start, 100, 2) == 1, "reseed 100");
            fvp_savehist_init(&b);
            CHECK(fvp_savehist_locate(&b, bad, TAIL_SIZE, NULL, 0,
                                      FVP_ENC_GBK) == 0,
                  "reject: count == 100 (ambiguous)");
            fvp_savehist_free(&b);

            /* pos != count-1 */
            CHECK(build_ring(bad, TAIL_SIZE, start, 5, 1) == 1, "reseed pos");
            fvp_savehist_init(&b);
            CHECK(fvp_savehist_locate(&b, bad, TAIL_SIZE, NULL, 0,
                                      FVP_ENC_GBK) == 0,
                  "reject: pos != count-1");
            fvp_savehist_free(&b);

            /* 未写槽里塞了 value != -1 */
            CHECK(build_ring(bad, TAIL_SIZE, start, 2, 1) == 1, "reseed dirty");
            bad[start + 2 * 12 + 8] = 7; /* 第 2 条（未写）的 value 低字节 */
            fvp_savehist_init(&b);
            CHECK(fvp_savehist_locate(&b, bad, TAIL_SIZE, NULL, 0,
                                      FVP_ENC_GBK) == 0,
                  "reject: unwritten slot has value != -1");
            fvp_savehist_free(&b);

            /* 字段超长（第一个字段长度写成 0x900） */
            CHECK(build_ring(bad, TAIL_SIZE, start, 1, 0) == 1, "reseed long");
            bad[start + 0] = 0x00;
            bad[start + 1] = 0x09;
            fvp_savehist_init(&b);
            CHECK(fvp_savehist_locate(&b, bad, TAIL_SIZE, NULL, 0,
                                      FVP_ENC_GBK) == 0,
                  "reject: field longer than 0x800");
            fvp_savehist_free(&b);

            /* 控制字节 */
            CHECK(build_ring(bad, TAIL_SIZE, start, 1, 0) == 1, "reseed ctrl");
            bad[start + 4] = 0x01;
            fvp_savehist_init(&b);
            CHECK(fvp_savehist_locate(&b, bad, TAIL_SIZE, NULL, 0,
                                      FVP_ENC_GBK) == 0,
                  "reject: control byte in text");
            fvp_savehist_free(&b);

            /* 两个候选（歧义）→ 拒收 */
            CHECK(build_ring(bad, TAIL_SIZE, start, 3, 2) == 1, "reseed two a");
            CHECK(build_ring(bad, TAIL_SIZE, 0x5000, 3, 2) == 1,
                  "reseed two b");
            fvp_savehist_init(&b);
            CHECK(fvp_savehist_locate(&b, bad, TAIL_SIZE, NULL, 0,
                                      FVP_ENC_GBK) == 0,
                  "reject: two candidates (ambiguous)");
            fvp_savehist_free(&b);

            /* 截断：len 太短 */
            CHECK(build_ring(bad, TAIL_SIZE, start, 3, 2) == 1, "reseed trunc");
            fvp_savehist_init(&b);
            CHECK(fvp_savehist_locate(&b, bad, start + 100, NULL, 0,
                                      FVP_ENC_GBK) == 0,
                  "reject: truncated tail");
            fvp_savehist_free(&b);

            free(bad);
        }
    }

    /* --- 4. apply 的参数校验 --- */
    {
        FvpSaveHist empty;

        fvp_savehist_init(&empty);
        CHECK(fvp_savehist_apply(&empty, &h) == 0, "apply: count=0 rejected");
        CHECK(fvp_savehist_apply(NULL, &h) == 0, "apply: NULL hist");
        CHECK(fvp_savehist_apply(&sh, NULL) == 0, "apply: NULL target");
    }

    fvp_savehist_free(&sh);
    fvp_history_free(&h);
    free(tail);
}

/* 真实存档（缺文件就跳过）。 */
static void test_real_slots(void)
{
    static const struct {
        const wchar_t *path;
        unsigned int count;
        unsigned int pos;
        const char *newest;
    } cases[] = {
        { L"C:\\Users\\Mechrevo\\Downloads\\五彩斑斓的世界HD\\save\\s905.bin",
          11, 10, "谁这样说道。" },
        { L"C:\\Users\\Mechrevo\\Downloads\\五彩斑斓的世界HD\\save\\s481.bin",
          1, 0, "……。" },
    };
    int i;

    for (i = 0; i < (int)(sizeof(cases) / sizeof(cases[0])); ++i) {
        FILE *f = _wfopen(cases[i].path, L"rb");
        unsigned char *buf;
        long n;
        FvpSaveSlot sd;
        FvpSaveHist sh;
        FvpHistory h;

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
        fvp_savehist_init(&sh);
        fvp_history_init(&h);
        CHECK(fvp_save_slot_load(&sd, buf, (size_t)n, 0, 0) == 1,
              "slot %d: parsed", i);
        CHECK(fvp_savehist_locate(&sh, sd.tail, sd.tail_len, sd.str[2],
                                  sd.len[2], FVP_ENC_CP932) == 1,
              "slot %d: history ring found", i);
        CHECK(sh.count == cases[i].count && sh.pos == cases[i].pos,
              "slot %d: count=%u pos=%u（期望 %u/%u）", i, sh.count, sh.pos,
              cases[i].count, cases[i].pos);
        CHECK(sh.rec[sh.pos].text1 != NULL &&
                  strstr(sh.rec[sh.pos].text1, cases[i].newest) != NULL,
              "slot %d: newest=%s（期望含 %s）", i,
              sh.rec[sh.pos].text1 != NULL ? sh.rec[sh.pos].text1 : "(null)",
              cases[i].newest);
        CHECK(fvp_savehist_apply(&sh, &h) == 1, "slot %d: apply", i);
        CHECK(h.count == cases[i].count, "slot %d: history count", i);
        fvp_savehist_free(&sh);
        fvp_history_free(&h);
        fvp_save_slot_free(&sd);
        free(buf);
    }
}

int main(void)
{
    test_synthetic();
    test_real_slots();
    printf("test_savehist: %d passed, %d failed\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}