/* FVP engine -- 存档槽尾的 128 槽动态字符串表解析（见 savetail.h）。
 *
 * 位置不写死：表头跟在"每槽全局变量"（条数 × 8 字节）与 32 个协程上下文
 * （0x163A0 字节）之后，6 份真实存档实测条数 = 1549（0x3068），于是线程下标
 * u32 落在 0x163A0 + 0x3068 = 0x19408；换条数就平移。这里扫描
 * [0x163A0, 0x163A0+0x4000) 按 8 字节步进找唯一候选。
 */
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

/* 把 cp932 字节（不含结尾 NUL）解成 UTF-8。返回是否成功。 */
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

/* 解析 off（= 线程下标 u32）处的表头；成功返回 1。空表（count=0）也算成立。 */
static int parse_pool_at(FvpSaveTailInfo *out, const unsigned char *tail,
                         size_t len, size_t off, FvpEncoding enc)
{
    unsigned int count;
    unsigned int i;
    size_t cur;
    size_t rest;

    if (off + 8 > len || rd_u32(tail + off) != FVP_SAVETAIL_THREAD_NONE) {
        return 0;
    }
    count = rd_u32(tail + off + 4);
    if (count > FVP_SAVETAIL_MAX_STRINGS) {
        return 0;
    }
    out->count = 0;
    out->graph_index = -1;
    out->thread_index = rd_u32(tail + off);
    out->pool_offset = off;
    cur = off + 8;
    for (i = 0; i < count; ++i) {
        unsigned int n;
        size_t pad = 0;

        while (cur < len && tail[cur] == 0 && pad < 8) {
            cur += 1;
            pad += 1;
        }
        if (cur + 2 > len) {
            return 0;
        }
        n = rd_u16(tail + cur);
        cur += 2;
        if (n < 2 || n > FVP_SAVETAIL_STR_MAX || cur + n > len) {
            return 0;
        }
        if (tail[cur + n - 1] != 0) {
            return 0; /* 必须是 NUL 结尾的字符串 */
        }
        if (!decode_one(out->str[out->count], FVP_SAVETAIL_STR_MAX, enc,
                        tail + cur, (size_t)n - 1u)) {
            return 0;
        }
        if (out->graph_index < 0 && looks_like_graph(out->str[out->count])) {
            out->graph_index = out->count;
        }
        out->count += 1;
        cur += n;
    }
    /* 其余空槽：每个 2 字节 0。这条约束把“全零区里的假候选”排除掉。 */
    rest = 2u * (size_t)(FVP_SAVETAIL_SLOT_COUNT - count);
    if (cur + rest > len) {
        return 0;
    }
    for (i = 0; i < rest; ++i) {
        if (tail[cur + i] != 0) {
            return 0;
        }
    }
    return 1;
}

int fvp_savetail_globals(FvpSaveGlobal *out, int max,
                         const unsigned char *tail, size_t len)
{
    size_t off = 0;
    size_t i;
    long count;

    if (out == NULL || tail == NULL || max <= 0) {
        return 0;
    }
    if (!fvp_savetail_find_pool(tail, len, &off)) {
        return 0;
    }
    if (off < (size_t)FVP_SAVETAIL_POOL_BASE) {
        return 0;
    }
    count = (long)((off - (size_t)FVP_SAVETAIL_POOL_BASE) / 8u);
    if (count <= 0 || count > (long)max ||
        (size_t)count * 8u > len) {
        return 0;
    }
    for (i = 0; i < (size_t)count; ++i) {
        const unsigned char *cell = tail + i * 8u;

        out[i].type = (int)cell[0];
        out[i].i = (int32_t)rd_u32(cell + 4);
    }
    return (int)count;
}

int fvp_savetail_find_pool(const unsigned char *tail, size_t len, size_t *off)
{
    size_t base;
    size_t found = 0;
    size_t hit = 0;
    FvpSaveTailInfo tmp;

    if (tail == NULL || off == NULL) {
        return 0;
    }
    for (base = FVP_SAVETAIL_POOL_BASE;
         base + 8 <= len &&
         base < FVP_SAVETAIL_POOL_BASE + FVP_SAVETAIL_SCAN_MAX;
         base += 8) {
        if (rd_u32(tail + base) != FVP_SAVETAIL_THREAD_NONE) {
            continue;
        }
        if (rd_u32(tail + base + 4) > FVP_SAVETAIL_MAX_STRINGS) {
            continue;
        }
        /* 结构能走完：条数、长度前缀、NUL 结尾、其余空槽全零。
         * 文本解码先用 CP932，解不开再换 GBK（汉化版把台词写成 GBK）。 */
        if (!parse_pool_at(&tmp, tail, len, base, FVP_ENC_CP932) &&
            !parse_pool_at(&tmp, tail, len, base, FVP_ENC_GBK)) {
            continue;
        }
        if (found > 0) {
            return 0; /* 多个候选 → 放弃（不猜） */
        }
        found += 1;
        hit = base;
    }
    if (found == 0) {
        return 0;
    }
    *off = hit;
    return 1;
}

int fvp_savetail_parse(FvpSaveTailInfo *out, const unsigned char *tail,
                       size_t len, FvpEncoding enc)
{
    size_t off = 0;
    FvpSaveTailInfo tmp;

    if (out == NULL || tail == NULL) {
        return 0;
    }
    memset(out, 0, sizeof(*out));
    out->graph_index = -1;
    if (!fvp_savetail_find_pool(tail, len, &off)) {
        return 0;
    }
    if (parse_pool_at(&tmp, tail, len, off, enc)) {
        *out = tmp;
        return 1;
    }
    if (parse_pool_at(&tmp, tail, len, off, FVP_ENC_GBK)) {
        *out = tmp;
        return 1;
    }
    return 0;
}