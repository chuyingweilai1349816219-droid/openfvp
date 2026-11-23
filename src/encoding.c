/* FVP VM in C -- text decoding (CP932 / GBK -> UTF-8).
 *
 * The tables are generated from Python's own cp932/gbk codecs by
 * fvp/tests/gen_tables.py, which validates the decoder against Python for all
 * 1- and 2-byte sequences, random longer sequences and every string in the
 * target script.  The streaming rule (invalid pair -> U+FFFD, consume one byte)
 * matches CPython's 'replace' error handler.
 */
#include "encoding.h"

#include <string.h>

#include "gen_cp932.h"
#include "gen_gbk.h"

#define FVP_NTRAIL 192
#define FVP_TRAIL_LO 0x40
#define FVP_CP_INVALID 0xFFFF

typedef struct FvpCodec {
    const uint16_t *single;   /* 256 entries */
    const uint8_t *lead_row;  /* 256 entries, 0xFF = not a lead byte */
    const uint16_t *rows;     /* lead_count * FVP_NTRAIL entries */
} FvpCodec;

static const FvpCodec fvp_codecs[2] = {
    { fvp_cp932_single, fvp_cp932_lead_row, fvp_cp932_rows },
    { fvp_gbk_single, fvp_gbk_lead_row, fvp_gbk_rows }
};

int fvp_encoding_from_name(const char *name, FvpEncoding *out)
{
    if (name == NULL) {
        return 0;
    }
    if (strcmp(name, "cp932") == 0 || strcmp(name, "sjis") == 0 ||
        strcmp(name, "shift-jis") == 0 || strcmp(name, "shift_jis") == 0) {
        *out = FVP_ENC_CP932;
        return 1;
    }
    if (strcmp(name, "gbk") == 0 || strcmp(name, "gb2312") == 0 ||
        strcmp(name, "cp936") == 0) {
        *out = FVP_ENC_GBK;
        return 1;
    }
    return 0;
}

const char *fvp_encoding_name(FvpEncoding enc)
{
    return enc == FVP_ENC_GBK ? "gbk" : "cp932";
}

static unsigned char *fvp_utf8_put(unsigned char *p, uint32_t cp)
{
    if (cp < 0x80u) {
        *p++ = (unsigned char)cp;
    } else if (cp < 0x800u) {
        *p++ = (unsigned char)(0xC0u | (cp >> 6));
        *p++ = (unsigned char)(0x80u | (cp & 0x3Fu));
    } else {
        *p++ = (unsigned char)(0xE0u | (cp >> 12));
        *p++ = (unsigned char)(0x80u | ((cp >> 6) & 0x3Fu));
        *p++ = (unsigned char)(0x80u | (cp & 0x3Fu));
    }
    return p;
}

/* Walk the input once, emitting every decoded code point.  Returns the number
 * of code points; when out is non-NULL, *out_bytes receives the UTF-8 length. */
static size_t fvp_decode_cps(const FvpCodec *codec, const unsigned char *data,
                             size_t len, unsigned char *out, size_t *out_bytes)
{
    size_t i = 0;
    size_t n = 0;
    while (i < len) {
        uint32_t cp;
        uint32_t c = codec->single[data[i]];
        if (c != FVP_CP_INVALID) {
            cp = c;
            i += 1;
        } else {
            uint8_t row = codec->lead_row[data[i]];
            int paired = 0;
            if (row != 0xFF && i + 1 < len && data[i + 1] >= FVP_TRAIL_LO) {
                uint32_t p = codec->rows[(size_t)row * FVP_NTRAIL +
                                         (size_t)(data[i + 1] - FVP_TRAIL_LO)];
                if (p != FVP_CP_INVALID) {
                    cp = p;
                    i += 2;
                    paired = 1;
                }
            }
            if (!paired) {
                cp = 0xFFFDu;
                i += 1;
            }
        }
        if (out != NULL) {
            unsigned char *next = fvp_utf8_put(out, cp);
            *out_bytes += (size_t)(next - out);
            out = next;
        }
        n += 1;
    }
    return n;
}

FvpStr *fvp_decode(FvpArena *arena, FvpEncoding enc,
                   const unsigned char *data, size_t len)
{
    const FvpCodec *codec = &fvp_codecs[enc == FVP_ENC_GBK ? 1 : 0];
    size_t bytes = 0;
    size_t ncp = fvp_decode_cps(codec, data, len, NULL, NULL);
    FvpStr *s;
    if (ncp == 0) {
        return fvp_arena_str(arena, "", 0);
    }
    s = (FvpStr *)fvp_arena_alloc(arena, sizeof(FvpStr) + ncp * 3u + 1u);
    (void)fvp_decode_cps(codec, data, len, (unsigned char *)s->data, &bytes);
    s->len = (uint32_t)bytes;
    s->data[bytes] = '\0';
    return s;
}

/* 从 UTF-8 里取出一个码点，返回消耗的字节数（非法序列按 1 字节、码点 U+FFFD）。 */
static size_t fvp_utf8_get(const unsigned char *p, size_t len, uint32_t *cp)
{
    unsigned char c = p[0];

    if (c < 0x80u) {
        *cp = c;
        return 1;
    }
    if ((c & 0xE0u) == 0xC0u && len >= 2u) {
        *cp = ((uint32_t)(c & 0x1Fu) << 6) | (uint32_t)(p[1] & 0x3Fu);
        return 2;
    }
    if ((c & 0xF0u) == 0xE0u && len >= 3u) {
        *cp = ((uint32_t)(c & 0x0Fu) << 12) | ((uint32_t)(p[1] & 0x3Fu) << 6) |
              (uint32_t)(p[2] & 0x3Fu);
        return 3;
    }
    if ((c & 0xF8u) == 0xF0u && len >= 4u) {
        *cp = ((uint32_t)(c & 0x07u) << 18) | ((uint32_t)(p[1] & 0x3Fu) << 12) |
              ((uint32_t)(p[2] & 0x3Fu) << 6) | (uint32_t)(p[3] & 0x3Fu);
        return 4;
    }
    *cp = 0xFFFDu;
    return 1;
}

/* 码点 -> 该编码的字节序列。返回 1 或 2（写入 out）；0 = 表里没有。 */
static int fvp_codec_encode_cp(const FvpCodec *codec, uint32_t cp,
                               unsigned char *out)
{
    int b;

    if (cp > 0xFFFFu) {
        return 0;
    }
    for (b = 0; b < 256; ++b) {
        if (codec->single[b] == (uint16_t)cp) {
            out[0] = (unsigned char)b;
            return 1;
        }
    }
    for (b = 0; b < 256; ++b) {
        uint8_t row = codec->lead_row[b];
        int t;

        if (row == 0xFFu) {
            continue;
        }
        for (t = 0; t < FVP_NTRAIL; ++t) {
            if (codec->rows[(size_t)row * FVP_NTRAIL + (size_t)t] ==
                (uint16_t)cp) {
                out[0] = (unsigned char)b;
                out[1] = (unsigned char)(FVP_TRAIL_LO + t);
                return 2;
            }
        }
    }
    return 0;
}

FvpStr *fvp_encode(FvpArena *arena, FvpEncoding enc, const char *utf8,
                   size_t len)
{
    const FvpCodec *codec = &fvp_codecs[enc == FVP_ENC_GBK ? 1 : 0];
    const unsigned char *p = (const unsigned char *)utf8;
    size_t i = 0;
    size_t cap = len * 2u + 1u; /* 一个 UTF-8 字符最多编成 2 字节 */
    FvpStr *s = (FvpStr *)fvp_arena_alloc(arena, sizeof(FvpStr) + cap);
    size_t n = 0;

    while (i < len) {
        uint32_t cp;
        unsigned char bytes[2];
        size_t used = fvp_utf8_get(p + i, len - i, &cp);
        int nb = fvp_codec_encode_cp(codec, cp, bytes);

        i += used;
        if (nb == 0) {
            s->data[n++] = '?';
        } else {
            s->data[n++] = (char)bytes[0];
            if (nb == 2) {
                s->data[n++] = (char)bytes[1];
            }
        }
    }
    s->len = (uint32_t)n;
    s->data[n] = '\0';
    return s;
}