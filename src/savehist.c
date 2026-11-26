/* FVP engine -- 存档槽尾「台词历史环」的解析与回灌（见 savehist.h）。 */
#include "savehist.h"

#include <stdlib.h>
#include <string.h>

#include "fvp.h"

/* 一条记录在文件里的游标：{len0, off0, len1, off1, value}（-1 = 没写字节）。 */
typedef struct RawRec {
    unsigned int len0;
    size_t off0;
    unsigned int len1;
    size_t off1;
    int32_t value;
} RawRec;

static unsigned int rd_u32(const unsigned char *p)
{
    return (unsigned int)p[0] | ((unsigned int)p[1] << 8) |
           ((unsigned int)p[2] << 16) | ((unsigned int)p[3] << 24);
}

static int32_t rd_i32(const unsigned char *p)
{
    return (int32_t)rd_u32(p);
}

static int field_looks_textual(const unsigned char *p, unsigned int n);

/* 文本字段的字节是否"像文字"：不允许控制字符（记录是长度前缀、没有 NUL）。 */
static int text_bytes_ok(const unsigned char *p, size_t n)
{
    size_t i;

    for (i = 0; i < n; ++i) {
        if (p[i] < 0x20) {
            return 0;
        }
    }
    return 1;
}

/* 从 start 起按写档顺序解析 100 条记录（不做语义校验）。 */
static int parse_raw(const unsigned char *tail, size_t len, size_t start,
                     RawRec raw[FVP_SAVEHIST_SLOTS])
{
    size_t cur = start;
    int i;

    for (i = 0; i < (int)FVP_SAVEHIST_SLOTS; ++i) {
        unsigned int n;

        if (cur + 4 > len) {
            return 0;
        }
        n = rd_u32(tail + cur);
        cur += 4;
        if (n > FVP_SAVEHIST_FIELD_MAX || cur + n > len) {
            return 0;
        }
        raw[i].len0 = n;
        raw[i].off0 = cur;
        cur += n;

        if (cur + 4 > len) {
            return 0;
        }
        n = rd_u32(tail + cur);
        cur += 4;
        if (n > FVP_SAVEHIST_FIELD_MAX || cur + n > len) {
            return 0;
        }
        raw[i].len1 = n;
        raw[i].off1 = cur;
        cur += n;

        if (cur + 4 > len) {
            return 0;
        }
        raw[i].value = rd_i32(tail + cur);
        cur += 4;
    }
    return 1;
}

/* 结构判据：count/pos 合理、第 count 条之后是空记录、文本像文字、
 * （父串非空时）最新一条 text1 等于当前台词。 */
static int structure_ok(const unsigned char *tail, size_t len, size_t start,
                        const unsigned char *cur_line, size_t cur_len,
                        const RawRec raw[FVP_SAVEHIST_SLOTS])
{
    unsigned int count;
    unsigned int pos;
    int i;

    if (start < 8) {
        return 0;
    }
    count = rd_u32(tail + start - 8);
    pos = rd_u32(tail + start - 4);
    if (count < 1 || count >= (unsigned int)FVP_SAVEHIST_SLOTS) {
        return 0;
    }
    if (pos >= (unsigned int)FVP_SAVEHIST_SLOTS) {
        return 0;
    }
    /* 未回绕时 pos == count-1。**count == 100（环写满）的候选一律不收**：
     * 那种情况下 pos 可以任意、100 条都有历史值，光凭"100 条能解析完"区分不出
     * 真环和数据里偶然对齐的假环（实测 s485 就有 3 个 count=100 的假候选）。
     * 我们手上的 6 份存档 count 都 ≤ 11，所以这条限制暂时不影响任何样本；
     * 真遇到写满的存档时这里会如实返回"没找到"，而不是乱回灌。 */
    if (pos != count - 1u) {
        return 0;
    }
    for (i = 0; i < (int)FVP_SAVEHIST_SLOTS; ++i) {
        if ((unsigned int)i < count) {
            if (!field_looks_textual(tail + raw[i].off0, raw[i].len0) ||
                !field_looks_textual(tail + raw[i].off1, raw[i].len1)) {
                return 0;
            }
        } else if (raw[i].len0 != 0 || raw[i].len1 != 0 ||
                   raw[i].value != -1) {
            return 0; /* 未写的槽必须是 {0, 0, -1} */
        }
    }
    if (cur_len != 0) {
        const RawRec *r = &raw[pos];

        if (r->len1 != cur_len ||
            memcmp(tail + r->off1, cur_line, cur_len) != 0) {
            return 0;
        }
    }
    (void)len;
    return 1;
}

void fvp_savehist_init(FvpSaveHist *h)
{
    if (h != NULL) {
        memset(h, 0, sizeof(*h));
    }
}

void fvp_savehist_free(FvpSaveHist *h)
{
    int i;

    if (h == NULL) {
        return;
    }
    for (i = 0; i < (int)FVP_SAVEHIST_SLOTS; ++i) {
        free(h->rec[i].text0);
        free(h->rec[i].text1);
    }
    memset(h, 0, sizeof(*h));
}

/* 解出来的文本"像不像人话"：汉字/假名/全角标点/ASCII 加分，半角片假名
 * （CP932 把 GBK 双字节当半角片假名读时的典型产物）和 U+FFFD 扣分。
 * 槽里的台词是 GBK、而汉化版的脚本仍是 CP932，所以不能只看脚本编码。 */
static int score_utf8(const char *s, size_t n)
{
    size_t i = 0;
    int score = 0;

    while (i < n) {
        unsigned char c = (unsigned char)s[i];
        unsigned int cp;
        size_t skip;
        size_t k;
        int bad = 0;

        if (c < 0x80) {
            score += 1;
            i += 1;
            continue;
        }
        if (c >= 0xC2 && c < 0xE0) {
            skip = 2;
            cp = (unsigned int)(c & 0x1Fu);
        } else if (c >= 0xE0 && c < 0xF0) {
            skip = 3;
            cp = (unsigned int)(c & 0x0Fu);
        } else if (c >= 0xF0 && c < 0xF8) {
            skip = 4;
            cp = (unsigned int)(c & 0x07u);
        } else {
            score -= 2;
            i += 1;
            continue;
        }
        if (i + skip > n) {
            break;
        }
        for (k = 1; k < skip; ++k) {
            unsigned char cc = (unsigned char)s[i + k];

            if ((cc & 0xC0u) != 0x80u) {
                bad = 1;
                break;
            }
            cp = (cp << 6) | (unsigned int)(cc & 0x3Fu);
        }
        if (bad) {
            score -= 2;
            i += 1;
            continue;
        }
        i += skip;
        if (cp >= 0x4E00u && cp <= 0x9FFFu) {
            score += 2; /* CJK 汉字 */
        } else if ((cp >= 0x3040u && cp <= 0x30FFu) ||
                   (cp >= 0x3000u && cp <= 0x303Fu) ||
                   (cp >= 0xFF01u && cp <= 0xFF60u)) {
            score += 1; /* 假名 / 全角标点 */
        } else if (cp >= 0xFF61u && cp <= 0xFF9Fu) {
            score -= 3; /* 半角片假名：几乎只会在编码读错时出现 */
        } else if (cp == 0xFFFDu) {
            score -= 2;
        }
    }
    return score;
}

static char *decode_with(const unsigned char *p, unsigned int n,
                         FvpEncoding enc, int *score_out, size_t *len_out)
{
    FvpArena arena;
    FvpStr *s;
    char *out;
    size_t take;

    *score_out = -1000000;
    *len_out = 0;
    fvp_arena_init(&arena);
    s = fvp_decode(&arena, enc, p, (size_t)n);
    if (s == NULL) {
        fvp_arena_free(&arena);
        return NULL;
    }
    take = s->len;
    if (take > (size_t)FVP_SAVEHIST_TEXT_MAX - 1u) {
        take = (size_t)FVP_SAVEHIST_TEXT_MAX - 1u;
    }
    out = (char *)malloc(take + 1u);
    if (out != NULL) {
        memcpy(out, s->data, take);
        out[take] = '\0';
        *score_out = score_utf8(out, take);
        *len_out = take;
    }
    fvp_arena_free(&arena);
    return out;
}

/* 字段能不能按 GBK / CP932 之一解成"像人话"的串（空字段算通过）。
 * 这是定位阶段的判据：真实文本两种编码里总有一种读得通，随机指针数据不会。 */
static int field_looks_textual(const unsigned char *p, unsigned int n)
{
    char *a;
    char *b;
    int sa = -1000000;
    int sb = -1000000;
    size_t la = 0;
    size_t lb = 0;
    int ok;

    if (n == 0) {
        return 1;
    }
    if (!text_bytes_ok(p, n)) {
        return 0;
    }
    a = decode_with(p, n, FVP_ENC_GBK, &sa, &la);
    b = decode_with(p, n, FVP_ENC_CP932, &sb, &lb);
    ok = ((a != NULL && sa > 0) || (b != NULL && sb > 0));
    free(a);
    free(b);
    (void)la;
    (void)lb;
    return ok;
}

/* 把字段解成 UTF-8：GBK / CP932 都试一次，按"像不像人话"选；
 * 打平时用 enc（调用者给的游戏编码）当提示。 */
static char *decode_field(const unsigned char *p, unsigned int n,
                          FvpEncoding hint)
{
    char *a; /* GBK */
    char *b; /* CP932 */
    int sa;
    int sb;
    size_t la;
    size_t lb;

    if (n == 0) {
        return NULL;
    }
    a = decode_with(p, n, FVP_ENC_GBK, &sa, &la);
    b = decode_with(p, n, FVP_ENC_CP932, &sb, &lb);
    if (a == NULL && b == NULL) {
        return NULL;
    }
    if (a == NULL) {
        return b;
    }
    if (b == NULL) {
        return a;
    }
    if (sa > sb || (sa == sb && hint == FVP_ENC_GBK)) {
        free(b);
        (void)la;
        (void)lb;
        return a;
    }
    free(a);
    (void)la;
    (void)lb;
    return b;
}

int fvp_savehist_locate(FvpSaveHist *h, const unsigned char *tail, size_t len,
                        const unsigned char *cur_line, size_t cur_len,
                        FvpEncoding enc)
{
    RawRec raw[FVP_SAVEHIST_SLOTS];
    size_t start;
    size_t found = 0;
    RawRec hit[FVP_SAVEHIST_SLOTS];
    unsigned int count = 0;
    unsigned int pos = 0;
    unsigned int found_count = 0;
    unsigned int found_pos = 0;
    int i;

    if (h == NULL) {
        return 0;
    }
    fvp_savehist_free(h);
    if (tail == NULL || len < FVP_SAVEHIST_SCAN_MIN + 8u +
                                FVP_SAVEHIST_SCAN_MARGIN) {
        return 0;
    }

    for (start = FVP_SAVEHIST_SCAN_MIN;
         start + FVP_SAVEHIST_SCAN_MARGIN <= len; ++start) {
        if (start < 8) {
            continue;
        }
        /* 先做最便宜的过滤：前面 8 字节像 (count,pos) 吗？
         * 零区（count=0）和绝大多数偏移在这一步就被筛掉。 */
        count = rd_u32(tail + start - 8);
        pos = rd_u32(tail + start - 4);
        if (count < 1 || count > (unsigned int)FVP_SAVEHIST_SLOTS ||
            pos >= (unsigned int)FVP_SAVEHIST_SLOTS) {
            continue;
        }
        if (count < (unsigned int)FVP_SAVEHIST_SLOTS && pos != count - 1u) {
            continue;
        }
        if (!parse_raw(tail, len, start, raw)) {
            continue;
        }
        if (!structure_ok(tail, len, start, cur_line, cur_len, raw)) {
            continue;
        }
        if (found > 0) {
            return 0; /* 多个候选 → 放弃（不猜） */
        }
        found += 1;
        memcpy(hit, raw, sizeof(hit));
        h->offset = start;
        found_count = count;
        found_pos = pos;
    }
    if (found == 0) {
        return 0;
    }

    h->count = found_count;
    h->pos = found_pos;
    for (i = 0; i < (int)FVP_SAVEHIST_SLOTS; ++i) {
        if ((unsigned int)i >= found_count) {
            h->rec[i].value = -1;
            continue;
        }
        h->rec[i].value = hit[i].value;
        h->rec[i].text0 = decode_field(tail + hit[i].off0, hit[i].len0, enc);
        h->rec[i].text1 = decode_field(tail + hit[i].off1, hit[i].len1, enc);
        if ((hit[i].len0 != 0 && h->rec[i].text0 == NULL) ||
            (hit[i].len1 != 0 && h->rec[i].text1 == NULL)) {
            fvp_savehist_free(h);
            return 0; /* 解码失败 → 整段放弃 */
        }
    }
    return 1;
}

int fvp_savehist_apply(const FvpSaveHist *h, FvpHistory *target)
{
    int i;

    if (h == NULL || target == NULL) {
        return 0;
    }
    if (h->count < 1 || h->count > (unsigned int)FVP_SAVEHIST_SLOTS ||
        h->pos >= (unsigned int)FVP_SAVEHIST_SLOTS) {
        return 0;
    }
    for (i = 0; i < (int)FVP_HISTORY_SLOTS; ++i) {
        free(target->entry[i].text0);
        free(target->entry[i].text1);
        target->entry[i].text0 = NULL;
        target->entry[i].text1 = NULL;
        target->entry[i].value = -1;
    }
    for (i = 0; i < (int)h->count; ++i) {
        const FvpSaveHistRec *r = &h->rec[i];

        if (r->text0 != NULL) {
            target->entry[i].text0 = fvp_xstrdup(r->text0);
            if (target->entry[i].text0 == NULL) {
                return 0;
            }
        }
        if (r->text1 != NULL) {
            target->entry[i].text1 = fvp_xstrdup(r->text1);
            if (target->entry[i].text1 == NULL) {
                return 0;
            }
        }
        target->entry[i].value = r->value;
    }
    target->count = h->count;
    target->pos = h->pos;
    return 1;
}