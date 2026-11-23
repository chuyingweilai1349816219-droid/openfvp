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