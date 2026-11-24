/* FVP engine -- 存档槽尾的字符串池解析（见 savetail.h）。 */
#include <string.h>

#include "fvp.h"
#include "savetail.h"

static unsigned int rd_u32(const unsigned char *p)
{
    return (unsigned int)p[0] | ((unsigned int)p[1] << 8) |
           ((unsigned int)p[2] << 16) | ((unsigned int)p[3] << 24);
}

static unsigned int rd_u16(const unsigned char *p)
{
    return (unsigned int)p[0] | ((unsigned int)p[1] << 8);
}

/* 像图名吗？实测形如 "BG063_000b" / "EV001_000a"：大写字母开头、含 '_'。 */
static int looks_like_graph(const char *s)
{
    int i;
    int has_us = 0;
    int letters = 0;

    if (s == NULL || s[0] < 'A' || s[0] > 'Z') {
        return 0;
    }
    for (i = 0; s[i] != '\0' && i < 32; ++i) {
        if (s[i] == '_') {
            has_us = 1;
        } else if (s[i] >= 'A' && s[i] <= 'Z') {
            letters += 1;
        } else if (!((s[i] >= '0' && s[i] <= '9') ||
                     (s[i] >= 'a' && s[i] <= 'z'))) {
            return 0;
        }
    }
    return has_us && letters >= 2;
}

/* 把 cp932 字节（含结尾 NUL）解成 UTF-8。返回是否成功。 */
static int decode_one(char *dst, size_t cap, FvpEncoding enc,
                      const unsigned char *src, size_t n)
{
    FvpArena arena;
    FvpStr *s;
    size_t take;

    if (dst == NULL || cap == 0 || src == NULL || n == 0) {
        return 0;
    }
    dst[0] = '\0';
    fvp_arena_init(&arena);
    s = fvp_decode(&arena, enc, src, n);
    if (s == NULL || s->len == 0) {
        fvp_arena_free(&arena);
        return 0;
    }
    take = s->len;
    if (take > cap - 1) {
        take = cap - 1;
    }
    memcpy(dst, s->data, take);
    dst[take] = '\0';
    fvp_arena_free(&arena);
    return 1;
}

int fvp_savetail_parse(FvpSaveTailInfo *out, const unsigned char *tail,
                       size_t len, FvpEncoding enc)
{
    size_t base = FVP_SAVETAIL_POOL_OFFSET;
    size_t off;
    unsigned int count;
    int i;

    if (out == NULL) {
        return 0;
    }
    memset(out, 0, sizeof(*out));
    out->graph_index = -1;
    if (tail == NULL || len < base + 8) {
        return 0;
    }
    if (rd_u32(tail + base) != FVP_SAVETAIL_TAG) {
        return 0;
    }
    count = rd_u32(tail + base + 4);
    if (count == 0 || count > FVP_SAVETAIL_MAX_STRINGS) {
        return 0;
    }
    off = base + 8;
    for (i = 0; i < (int)count; ++i) {
        unsigned int n;
        size_t pad = 0;

        if (off + 2 > len) {
            break;
        }
        n = rd_u16(tail + off);
        off += 2;
        if (n < 2 || n > FVP_SAVETAIL_STR_MAX || off + n > len) {
            break;
        }
        /* 条目之间允许 0 填充（实测有；精确规则未定，跳过去即可） */
        if (tail[off + n - 1] != 0) {
            break; /* 必须是 NUL 结尾的字符串 */
        }
        if (!decode_one(out->str[out->count], FVP_SAVETAIL_STR_MAX, enc,
                        tail + off, (size_t)n - 1u)) {
            break;
        }
        if (out->graph_index < 0 && looks_like_graph(out->str[out->count])) {
            out->graph_index = out->count;
        }
        out->count += 1;
        off += n;
        while (off < len && tail[off] == 0 && pad < 8) {
            off += 1;
            pad += 1;
        }
    }
    return out->count > 0;
}