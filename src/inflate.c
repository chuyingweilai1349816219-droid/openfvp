/* FVP VM in C -- DEFLATE / zlib decompressor.
 *
 * Hand-written because the bundled w64devkit has no zlib.  Structure follows
 * RFC 1951 closely:
 *
 *   - a 64-bit LSB-first bit reader over the input,
 *   - a two-level canonical Huffman table (9-bit root + 6-bit sub-tables)
 *     for the fast path, and a bit-at-a-time canonical decoder near the end
 *     of the input (where fewer than 15 bits remain),
 *   - stored / fixed / dynamic blocks, all bounds checked,
 * - zlib wrapper (RFC 1950) with Adler-32 verification.
 *
 * Every read from the input and every write to the output is range checked:
 * corrupt data yields an error code, never a crash.  Verified with a 100k
 * iteration random-input fuzz run plus mutated/truncated real streams; see
 * fvp/docs/c-assets.md.
 */
#include "inflate.h"

#include <limits.h>
#include <stdlib.h>
#include <string.h>

#define MAXBITS 15
#define ROOTBITS 9
#define ROOT_SIZE (1u << ROOTBITS)
#define SUBBITS (MAXBITS - ROOTBITS)
#define SUB_SIZE (1u << SUBBITS)
#define MAX_SYMBOLS 288

/* One entry of a decoding table.  len == 0 marks an unused (invalid) slot. */
typedef struct {
    uint16_t sym; /* symbol value, or sub-table index when sub != 0 */
    uint8_t len;  /* total code length in bits */
    uint8_t sub;  /* 1: this root slot points at sub[sym * SUB_SIZE] */
} HEntry;

typedef struct {
    uint16_t count[MAXBITS + 1]; /* number of codes of each length */
    uint16_t symbol[MAX_SYMBOLS]; /* symbols in canonical order */
    HEntry root[ROOT_SIZE];
    int16_t sub_base[ROOT_SIZE]; /* -1 or index of the sub-table */
    HEntry *sub;                 /* sub-table pool, freed by huf_build */
    int nsub;
} Huf;

typedef struct {
    const uint8_t *p;
    const uint8_t *end;
    uint64_t buf;
    unsigned cnt;
} BitReader;

/* ------------------------------------------------------------------ */
/* bit reader                                                          */
/* ------------------------------------------------------------------ */

static int br_fill(BitReader *b, unsigned n)
{
    while (b->cnt < n) {
        if (b->p >= b->end) {
            return 0;
        }
        b->buf |= (uint64_t)(*b->p++) << b->cnt;
        b->cnt += 8;
    }
    return 1;
}

static uint32_t br_peek(const BitReader *b, unsigned n)
{
    if (n == 0) {
        return 0;
    }
    return (uint32_t)(b->buf & ((((uint64_t)1) << n) - 1u));
}

static void br_drop(BitReader *b, unsigned n)
{
    b->buf >>= n;
    b->cnt -= n;
}

/* Read n (<= 32) bits into *val.  Returns 0 on truncated input. */
static int br_read(BitReader *b, unsigned n, uint32_t *val)
{
    if (!br_fill(b, n)) {
        return 0;
    }
    *val = br_peek(b, n);
    br_drop(b, n);
    return 1;
}

/* Copy n bytes out of the bit reader.  The reader must be byte aligned
 * (br_drop(b, b->cnt & 7) beforehand), which the stored-block path ensures. */
static int br_copy(BitReader *b, uint8_t *dst, size_t n)
{
    while (n > 0 && b->cnt >= 8) {
        *dst++ = (uint8_t)(b->buf & 0xFFu);
        b->buf >>= 8;
        b->cnt -= 8;
        n--;
    }
    if (n > 0) {
        size_t avail = (size_t)(b->end - b->p);
        if (avail < n) {
            return 0;
        }
        memcpy(dst, b->p, n);
        b->p += n;
    }
    return 1;
}

/* ------------------------------------------------------------------ */
/* Huffman tables                                                      */
/* ------------------------------------------------------------------ */

static unsigned rev_bits(unsigned v, int n)
{
    unsigned r = 0;
    while (n-- > 0) {
        r = (r << 1) | (v & 1u);
        v >>= 1;
    }
    return r;
}

/* Build a decoding table for the code lengths in lens[0..n).  Returns 0 on
 * success, -1 when the code lengths are over-subscribed, -2 on malloc
 * failure.  Incomplete codes are accepted (the missing slots stay invalid
 * and decode as errors); RFC 1951 permits that for distance trees and real
 * encoders emit it. */
static int huf_build(Huf *h, const uint8_t *lens, int n)
{
    int i;
    int len;
    int left;
    int nlong = 0;
    int si = 0;
    unsigned first;

    if (n <= 0 || n > MAX_SYMBOLS) {
        return -1;
    }
    memset(h->count, 0, sizeof h->count);
    for (i = 0; i < n; i++) {
        if (lens[i] > MAXBITS) {
            return -1;
        }
        h->count[lens[i]]++;
    }
    h->count[0] = 0;

    /* Kraft inequality: reject over-subscribed code sets. */
    left = 1;
    for (len = 1; len <= MAXBITS; len++) {
        left <<= 1;
        left -= h->count[len];
        if (left < 0) {
            return -1;
        }
    }

    /* Symbols in canonical order. */
    {
        uint16_t offs[MAXBITS + 2];
        offs[1] = 0;
        for (len = 1; len < MAXBITS; len++) {
            offs[len + 1] = (uint16_t)(offs[len] + h->count[len]);
        }
        for (i = 0; i < n; i++) {
            if (lens[i] != 0) {
                h->symbol[offs[lens[i]]++] = (uint16_t)i;
            }
        }
    }

    for (i = 0; i < n; i++) {
        if (lens[i] > ROOTBITS) {
            nlong++;
        }
    }
    free(h->sub);
    h->sub = NULL;
    h->nsub = 0;
    if (nlong > 0) {
        size_t entries = (size_t)nlong * SUB_SIZE;
        h->sub = (HEntry *)calloc(entries, sizeof(HEntry));
        if (h->sub == NULL) {
            return -2;
        }
    }
    memset(h->root, 0, sizeof h->root);
    for (i = 0; i < (int)ROOT_SIZE; i++) {
        h->sub_base[i] = -1;
    }

    /* Assign canonical codes in length order and fill the tables. */
    first = 0;
    for (len = 1; len <= MAXBITS; len++) {
        unsigned k;
        for (k = 0; k < h->count[len]; k++) {
            unsigned code = first + k;
            unsigned rev = rev_bits(code, len);
            uint16_t sym = h->symbol[si++];
            if (len <= ROOTBITS) {
                unsigned step = 1u << len;
                unsigned j;
                for (j = rev; j < ROOT_SIZE; j += step) {
                    h->root[j].sym = sym;
                    h->root[j].len = (uint8_t)len;
                    h->root[j].sub = 0;
                }
            } else {
                unsigned prefix = rev & (ROOT_SIZE - 1u);
                unsigned rest = (unsigned)len - ROOTBITS;
                unsigned suffix = rev >> ROOTBITS;
                unsigned step = 1u << rest;
                unsigned j;
                int s = h->sub_base[prefix];
                if (s < 0) {
                    s = h->nsub++;
                    h->sub_base[prefix] = (int16_t)s;
                }
                for (j = suffix; j < SUB_SIZE; j += step) {
                    HEntry *e = &h->sub[(size_t)s * SUB_SIZE + j];
                    e->sym = sym;
                    e->len = (uint8_t)len;
                    e->sub = 0;
                }
                h->root[prefix].sym = (uint16_t)s;
                h->root[prefix].len = ROOTBITS;
                h->root[prefix].sub = 1;
            }
        }
        first = (first + h->count[len]) << 1;
    }
    return 0;
}

static void huf_free(Huf *h)
{
    free(h->sub);
    h->sub = NULL;
    h->nsub = 0;
}

/* Canonical bit-at-a-time decoder, used when fewer than MAXBITS bits remain
 * in the input (i.e. the last few symbols of a stream). */
static int huf_decode_slow(const Huf *h, BitReader *b)
{
    int len;
    int code = 0;
    int first = 0;
    int index = 0;

    for (len = 1; len <= MAXBITS; len++) {
        int count;
        if (!br_fill(b, 1)) {
            return -2; /* truncated */
        }
        code |= (int)br_peek(b, 1);
        br_drop(b, 1);
        count = (int)h->count[len];
        if (code - first < count) {
            return (int)h->symbol[index + (code - first)];
        }
        index += count;
        first += count;
        first <<= 1;
        code <<= 1;
    }
    return -1; /* invalid code */
}

/* Decode one symbol.  Returns >= 0 on success, -1 invalid code, -2 truncated. */
static int huf_decode(const Huf *h, BitReader *b)
{
    HEntry e;

    if (!br_fill(b, MAXBITS)) {
        return huf_decode_slow(h, b);
    }
    e = h->root[br_peek(b, ROOTBITS)];
    if (e.len == 0) {
        return -1;
    }
    if (!e.sub) {
        br_drop(b, e.len);
        return (int)e.sym;
    }
    {
        unsigned sub = (br_peek(b, MAXBITS) >> ROOTBITS) & (SUB_SIZE - 1u);
        HEntry s = h->sub[(size_t)e.sym * SUB_SIZE + sub];
        if (s.len == 0) {
            return -1;
        }
        br_drop(b, s.len);
        return (int)s.sym;
    }
}

/* ------------------------------------------------------------------ */
/* DEFLATE block decoding                                              */
/* ------------------------------------------------------------------ */

static const uint16_t k_len_base[29] = {
    3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 17, 19, 23, 27, 31,
    35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258
};
static const uint8_t k_len_extra[29] = {
    0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2,
    3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0
};
static const uint16_t k_dist_base[30] = {
    1, 2, 3, 4, 5, 7, 9, 13, 17, 25, 33, 49, 65, 97, 129, 193,
    257, 385, 513, 769, 1025, 1537, 2049, 3073, 4097, 6145, 8193, 12289,
    16385, 24577
};
static const uint8_t k_dist_extra[30] = {
    0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6,
    7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13
};

/* Decode literal/length/distance symbols until end-of-block. */
static int inflate_codes(BitReader *b, const Huf *lit, const Huf *dist,
                         uint8_t **outp, uint8_t *outend, uint8_t *outstart)
{
    uint8_t *out = *outp;

    for (;;) {
        int sym = huf_decode(lit, b);
        if (sym < 0) {
            return sym == -2 ? FVP_INFLATE_ERR_TRUNC : FVP_INFLATE_ERR_DATA;
        }
        if (sym < 256) {
            if (out >= outend) {
                return FVP_INFLATE_ERR_BUF;
            }
            *out++ = (uint8_t)sym;
        } else if (sym == 256) {
            break;
        } else {
            unsigned li = (unsigned)(sym - 257);
            unsigned length;
            unsigned dsym;
            unsigned distance;
            uint32_t extra;
            if (li >= 29) {
                return FVP_INFLATE_ERR_DATA;
            }
            length = k_len_base[li];
            if (k_len_extra[li] != 0) {
                if (!br_read(b, k_len_extra[li], &extra)) {
                    return FVP_INFLATE_ERR_TRUNC;
                }
                length += extra;
            }
            dsym = (unsigned)huf_decode(dist, b);
            if ((int)dsym < 0) {
                return (int)dsym == -2 ? FVP_INFLATE_ERR_TRUNC
                                       : FVP_INFLATE_ERR_DATA;
            }
            if (dsym >= 30) {
                return FVP_INFLATE_ERR_DATA;
            }
            distance = k_dist_base[dsym];
            if (k_dist_extra[dsym] != 0) {
                if (!br_read(b, k_dist_extra[dsym], &extra)) {
                    return FVP_INFLATE_ERR_TRUNC;
                }
                distance += extra;
            }
            if ((size_t)distance > (size_t)(out - outstart)) {
                return FVP_INFLATE_ERR_DATA; /* reference before output start */
            }
            if ((size_t)length > (size_t)(outend - out)) {
                return FVP_INFLATE_ERR_BUF;
            }
            if (distance >= length) {
                memcpy(out, out - distance, length);
                out += length;
            } else {
                const uint8_t *q = out - distance;
                unsigned n = length;
                while (n-- > 0) {
                    *out++ = *q++;
                }
            }
        }
    }
    *outp = out;
    return FVP_INFLATE_OK;
}

static int inflate_stored(BitReader *b, uint8_t **outp, uint8_t *outend)
{
    unsigned len;
    unsigned nlen;

    br_drop(b, b->cnt & 7u); /* skip to the next byte boundary */
    if (!br_read(b, 16, &len) || !br_read(b, 16, &nlen)) {
        return FVP_INFLATE_ERR_TRUNC;
    }
    if ((len ^ 0xFFFFu) != nlen) {
        return FVP_INFLATE_ERR_DATA;
    }
    if ((size_t)len > (size_t)(outend - *outp)) {
        return FVP_INFLATE_ERR_BUF;
    }
    if (!br_copy(b, *outp, len)) {
        return FVP_INFLATE_ERR_TRUNC;
    }
    *outp += len;
    return FVP_INFLATE_OK;
}

static int inflate_fixed(BitReader *b, uint8_t **outp, uint8_t *outend,
                         uint8_t *outstart, Huf *lit, Huf *dist)
{
    uint8_t lens[288];
    int i;
    int rc;

    for (i = 0; i < 144; i++) lens[i] = 8;
    for (; i < 256; i++) lens[i] = 9;
    for (; i < 280; i++) lens[i] = 7;
    for (; i < 288; i++) lens[i] = 8;
    rc = huf_build(lit, lens, 288);
    if (rc != 0) {
        return rc == -2 ? FVP_INFLATE_ERR_MEMORY : FVP_INFLATE_ERR_DATA;
    }
    for (i = 0; i < 32; i++) lens[i] = 5;
    rc = huf_build(dist, lens, 32);
    if (rc != 0) {
        huf_free(lit);
        return rc == -2 ? FVP_INFLATE_ERR_MEMORY : FVP_INFLATE_ERR_DATA;
    }
    rc = inflate_codes(b, lit, dist, outp, outend, outstart);
    huf_free(lit);
    huf_free(dist);
    return rc;
}

static int inflate_dynamic(BitReader *b, uint8_t **outp, uint8_t *outend,
                           uint8_t *outstart, Huf *lit, Huf *dist)
{
    static const uint8_t k_order[19] = {
        16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15
    };
    uint8_t cl_lens[19];
    uint8_t lens[288 + 32];
    Huf cl;
    unsigned hlit;
    unsigned hdist;
    unsigned hclen;
    unsigned i;
    int rc = FVP_INFLATE_ERR_DATA;

    memset(&cl, 0, sizeof cl);
    memset(cl_lens, 0, sizeof cl_lens);
    memset(lens, 0, sizeof lens);

    if (!br_read(b, 5, &hlit) || !br_read(b, 5, &hdist) ||
        !br_read(b, 4, &hclen)) {
        return FVP_INFLATE_ERR_TRUNC;
    }
    hlit += 257;
    hdist += 1;
    hclen += 4;
    if (hlit > 286 || hdist > 30) {
        return FVP_INFLATE_ERR_DATA;
    }
    for (i = 0; i < hclen; i++) {
        uint32_t v;
        if (!br_read(b, 3, &v)) {
            return FVP_INFLATE_ERR_TRUNC;
        }
        cl_lens[k_order[i]] = (uint8_t)v;
    }
    if (huf_build(&cl, cl_lens, 19) != 0) {
        return FVP_INFLATE_ERR_DATA;
    }

    i = 0;
    while (i < hlit + hdist) {
        int sym = huf_decode(&cl, b);
        if (sym < 0) {
            rc = sym == -2 ? FVP_INFLATE_ERR_TRUNC : FVP_INFLATE_ERR_DATA;
            goto done;
        }
        if (sym < 16) {
            lens[i++] = (uint8_t)sym;
        } else {
            uint32_t rep;
            uint8_t val = 0;
            if (sym == 16) {
                if (i == 0) {
                    rc = FVP_INFLATE_ERR_DATA;
                    goto done;
                }
                val = lens[i - 1];
                if (!br_read(b, 2, &rep)) {
                    rc = FVP_INFLATE_ERR_TRUNC;
                    goto done;
                }
                rep += 3;
            } else if (sym == 17) {
                if (!br_read(b, 3, &rep)) {
                    rc = FVP_INFLATE_ERR_TRUNC;
                    goto done;
                }
                rep += 3;
            } else if (sym == 18) {
                if (!br_read(b, 7, &rep)) {
                    rc = FVP_INFLATE_ERR_TRUNC;
                    goto done;
                }
                rep += 11;
            } else {
                rc = FVP_INFLATE_ERR_DATA;
                goto done;
            }
            if (rep > hlit + hdist - i) {
                rc = FVP_INFLATE_ERR_DATA;
                goto done;
            }
            while (rep-- > 0) {
                lens[i++] = val;
            }
        }
    }

    /* The literal/length tree must contain an end-of-block symbol. */
    if (lens[256] == 0) {
        rc = FVP_INFLATE_ERR_DATA;
        goto done;
    }
    if (huf_build(lit, lens, (int)hlit) != 0) {
        rc = FVP_INFLATE_ERR_DATA;
        goto done;
    }
    {
        int brc = huf_build(dist, lens + hlit, (int)hdist);
        if (brc != 0) {
            huf_free(lit);
            rc = brc == -2 ? FVP_INFLATE_ERR_MEMORY : FVP_INFLATE_ERR_DATA;
            goto done;
        }
    }
    rc = inflate_codes(b, lit, dist, outp, outend, outstart);
    huf_free(lit);
    huf_free(dist);
    return rc;

done:
    huf_free(&cl);
    return rc;
}

/* The actual RFC 1951 decoder.  When with_zlib is set the 4-byte Adler-32
 * trailer is read and verified after the final block. */
static int inflate_core(const uint8_t *src, size_t srclen, uint8_t *dst,
                        size_t dstlen, int with_zlib, size_t *produced)
{
    BitReader b;
    Huf lit;
    Huf dist;
    uint8_t *out = dst;
    uint8_t *outend = dst + dstlen;
    int final = 0;
    int rc = FVP_INFLATE_OK;

    memset(&lit, 0, sizeof lit);
    memset(&dist, 0, sizeof dist);
    b.p = src;
    b.end = src + srclen;
    b.buf = 0;
    b.cnt = 0;

    do {
        uint32_t type;
        uint32_t bfinal;
        if (!br_read(&b, 1, &bfinal) || !br_read(&b, 2, &type)) {
            rc = FVP_INFLATE_ERR_TRUNC;
            goto done;
        }
        final = (int)bfinal;
        if (type == 0) {
            rc = inflate_stored(&b, &out, outend);
        } else if (type == 1) {
            rc = inflate_fixed(&b, &out, outend, dst, &lit, &dist);
        } else if (type == 2) {
            rc = inflate_dynamic(&b, &out, outend, dst, &lit, &dist);
        } else {
            rc = FVP_INFLATE_ERR_DATA;
        }
        if (rc != FVP_INFLATE_OK) {
            goto done;
        }
    } while (!final);

    if (with_zlib) {
        uint8_t ck[4];
        uint32_t want;
        uint32_t got;
        br_drop(&b, b.cnt & 7u); /* skip to the next byte boundary */
        if (!br_copy(&b, ck, 4)) {
            rc = FVP_INFLATE_ERR_TRUNC;
            goto done;
        }
        want = ((uint32_t)ck[0] << 24) | ((uint32_t)ck[1] << 16) |
               ((uint32_t)ck[2] << 8) | (uint32_t)ck[3];
        got = fvp_adler32(1u, dst, (size_t)(out - dst));
        if (want != got) {
            rc = FVP_INFLATE_ERR_CHECKSUM;
            goto done;
        }
    }
    *produced = (size_t)(out - dst);

done:
    huf_free(&lit);
    huf_free(&dist);
    return rc;
}

/* ------------------------------------------------------------------ */
/* public API                                                          */
/* ------------------------------------------------------------------ */

uint32_t fvp_adler32(uint32_t adler, const void *data, size_t len)
{
    const uint8_t *p = (const uint8_t *)data;
    uint32_t s1 = adler & 0xFFFFu;
    uint32_t s2 = (adler >> 16) & 0xFFFFu;

    while (len > 0) {
        size_t chunk = len < 5552u ? len : 5552u;
        size_t n = chunk;
        len -= chunk;
        while (n-- > 0) {
            s1 += *p++;
            s2 += s1;
        }
        s1 %= 65521u;
        s2 %= 65521u;
    }
    return (s2 << 16) | s1;
}

const char *fvp_inflate_strerror(int code)
{
    switch (code) {
    case FVP_INFLATE_OK:
        return "ok";
    case FVP_INFLATE_ERR_ARG:
        return "invalid argument";
    case FVP_INFLATE_ERR_HEADER:
        return "bad zlib header / preset dictionary unsupported";
    case FVP_INFLATE_ERR_TRUNC:
        return "truncated stream";
    case FVP_INFLATE_ERR_DATA:
        return "corrupt deflate data";
    case FVP_INFLATE_ERR_BUF:
        return "output buffer too small";
    case FVP_INFLATE_ERR_CHECKSUM:
        return "adler32 checksum mismatch";
    case FVP_INFLATE_ERR_MEMORY:
        return "out of memory";
    default:
        return "unknown error";
    }
}

int fvp_inflate_raw(const uint8_t *src, size_t srclen, uint8_t *dst,
                    size_t dstlen)
{
    size_t produced = 0;
    int rc;
    if (src == NULL || dst == NULL) {
        return FVP_INFLATE_ERR_ARG;
    }
    rc = inflate_core(src, srclen, dst, dstlen, 0, &produced);
    if (rc != FVP_INFLATE_OK) {
        return rc;
    }
    if (produced > (size_t)INT_MAX) {
        return FVP_INFLATE_ERR_BUF; /* not representable as an int */
    }
    return (int)produced;
}

int fvp_inflate_zlib(const uint8_t *src, size_t srclen, uint8_t *dst,
                     size_t dstlen)
{
    size_t produced = 0;
    int rc;
    if (src == NULL || dst == NULL) {
        return FVP_INFLATE_ERR_ARG;
    }
    if (srclen < 2) {
        return FVP_INFLATE_ERR_TRUNC;
    }
    if ((src[0] & 0x0Fu) != 8u) {
        return FVP_INFLATE_ERR_HEADER; /* CM must be deflate */
    }
    if ((((unsigned)src[0] << 8) | (unsigned)src[1]) % 31u != 0u) {
        return FVP_INFLATE_ERR_HEADER; /* FCHECK */
    }
    if ((src[0] >> 4) > 7u) {
        return FVP_INFLATE_ERR_HEADER; /* CINFO: window > 32 KiB */
    }
    if ((src[1] & 0x20u) != 0u) {
        return FVP_INFLATE_ERR_HEADER; /* FDICT: preset dictionary required */
    }
    rc = inflate_core(src + 2, srclen - 2, dst, dstlen, 1, &produced);
    if (rc != FVP_INFLATE_OK) {
        return rc;
    }
    if (produced > (size_t)INT_MAX) {
        return FVP_INFLATE_ERR_BUF;
    }
    return (int)produced;
}

int fvp_inflate(const uint8_t *src, size_t srclen, uint8_t *dst, size_t dstlen)
{
    if (src == NULL || dst == NULL) {
        return FVP_INFLATE_ERR_ARG;
    }
    /* A valid zlib header: CM = 8 (deflate), (CMF * 256 + FLG) % 31 == 0. */
    if (srclen >= 2 && (src[0] & 0x0Fu) == 8u &&
        ((((unsigned)src[0] << 8) | (unsigned)src[1]) % 31u) == 0u) {
        return fvp_inflate_zlib(src, srclen, dst, dstlen);
    }
    return fvp_inflate_raw(src, srclen, dst, dstlen);
}