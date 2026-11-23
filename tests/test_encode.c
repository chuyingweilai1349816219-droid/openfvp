/* UTF-8 <-> CP932/GBK 编码方向测试（写存档需要编码，不是解码）。 */
#include <stdio.h>
#include <string.h>

#include "../src/encoding.h"
#include "../src/fvp.h"

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

/* 编码后逐字节比对，再解码回 UTF-8 验证往返。 */
static void expect(const char *name, FvpEncoding enc, const char *utf8,
                   const unsigned char *want, size_t want_len)
{
    FvpArena arena;
    FvpStr *enc_str;
    FvpStr *back;

    fvp_arena_init(&arena);
    enc_str = fvp_encode(&arena, enc, utf8, strlen(utf8));
    CHECK(enc_str->len == want_len, "%s: len %u != %zu", name, enc_str->len,
          want_len);
    if (enc_str->len == want_len) {
        CHECK(memcmp(enc_str->data, want, want_len) == 0, "%s: bytes differ",
              name);
    }
    back = fvp_decode(&arena, enc, (const unsigned char *)enc_str->data,
                      enc_str->len);
    CHECK(back->len == strlen(utf8) && memcmp(back->data, utf8, back->len) == 0,
          "%s: round-trip '%s' -> '%s'", name, utf8, back->data);
    fvp_arena_free(&arena);
}

int main(void)
{
    /* 1. CP932：存档槽里真实出现过的章节名（プロローグ = 83 76 83 8D 83 8D 81 5B 83 4F）。 */
    expect("cp932 prologue", FVP_ENC_CP932, "プロローグ",
           (const unsigned char *)"\x83\x76\x83\x8d\x83\x8d\x81\x5b\x83\x4f",
           10);

    /* 2. CP932：ASCII 原样。 */
    expect("cp932 ascii", FVP_ENC_CP932, "ABC xyz", (const unsigned char *)"ABC xyz",
           7);

    /* 3. GBK：汉化版存档里的当前对话行（谁这样说道。）。 */
    expect("gbk line", FVP_ENC_GBK, "谁这样说道。",
           (const unsigned char *)"\xcb\xad\xd5\xe2\xd1\xf9\xcb\xb5\xb5\xc0\xa1\xa3",
           12);

    /* 4. GBK：全角空格（A1A1）。 */
    expect("gbk ideographic space", FVP_ENC_GBK, "\u3000",
           (const unsigned char *)"\xa1\xa1", 2);

    /* 5. 表里没有的码点 -> '?'，不能崩。 */
    {
        FvpArena arena;
        FvpStr *s;

        fvp_arena_init(&arena);
        s = fvp_encode(&arena, FVP_ENC_CP932, "\xf0\x9f\x99\x82", 4); /* 😂 */
        CHECK(s->len == 1 && s->data[0] == '?', "unmappable -> '?' (len=%u)",
              s->len);
        fvp_arena_free(&arena);
    }

    /* 6. 空串。 */
    {
        FvpArena arena;
        FvpStr *s;

        fvp_arena_init(&arena);
        s = fvp_encode(&arena, FVP_ENC_CP932, "", 0);
        CHECK(s->len == 0 && s->data[0] == '\0', "empty string");
        fvp_arena_free(&arena);
    }

    printf("\ntest_encode: %d passed, %d failed\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}