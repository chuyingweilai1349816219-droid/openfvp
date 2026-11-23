/* c_assets -- acceptance / inspection tool for the FVP asset layer.
 *
 * Commands (see fvp/docs/c-assets.md):
 *
 *   list <package>                     entry count + index/name/size/offset
 *   find <package> <utf8-name>         index of a name, or -1
 *   findall <package> <names.txt>      one index per input line
 *   dump <package> <index> <out>       raw entry bytes
 *   hdr <hzc1-file>                    decoded header fields (one line)
 *   rgba <hzc1-file> <out> [frame|all] RGBA8888 pixel dump
 *   ppm <hzc1-file> <out.ppm> [frame]  RGB PPM export
 *   inflate <in> <out> <expect> [off]  zlib/deflate decompress from offset
 *   rawinflate <in> <out> <expect> [off]  force bare deflate
 *   fuzz <iterations> <seed> [seedlist]  random-input robustness run
 *   bench <hzc1-file> <iters>          decode timing (inflate + RGBA)
 *   bench-inflate <in> <expect> <iters> [off]  raw inflate throughput
 *
 * Exit code 0 = success, 1 = failure, 2 = usage error.
 * Built by tests/verify_assets.ps1; no external dependency besides libc.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "bin.h"
#include "hzc1.h"
#include "inflate.h"

/* ------------------------------------------------------------------ */
/* small helpers                                                       */
/* ------------------------------------------------------------------ */

static void die(const char *msg)
{
    fprintf(stderr, "c_assets: %s\n", msg);
}

static uint8_t *read_file(const char *path, size_t *len_out)
{
    FILE *fp;
    long n;
    uint8_t *buf;

    if (len_out != NULL) {
        *len_out = 0;
    }
    fp = fopen(path, "rb");
    if (fp == NULL) {
        fprintf(stderr, "c_assets: cannot open %s\n", path);
        return NULL;
    }
    if (fseek(fp, 0, SEEK_END) != 0) {
        fclose(fp);
        return NULL;
    }
    n = ftell(fp);
    if (n < 0) {
        fclose(fp);
        return NULL;
    }
    if (fseek(fp, 0, SEEK_SET) != 0) {
        fclose(fp);
        return NULL;
    }
    buf = (uint8_t *)malloc((size_t)n + 1u);
    if (buf == NULL) {
        fclose(fp);
        return NULL;
    }
    if (n > 0 && fread(buf, 1, (size_t)n, fp) != (size_t)n) {
        free(buf);
        fclose(fp);
        return NULL;
    }
    fclose(fp);
    buf[n] = 0;
    if (len_out != NULL) {
        *len_out = (size_t)n;
    }
    return buf;
}

static int write_file(const char *path, const void *data, size_t len)
{
    FILE *fp = fopen(path, "wb");
    if (fp == NULL) {
        fprintf(stderr, "c_assets: cannot create %s\n", path);
        return 0;
    }
    if (len != 0 && fwrite(data, 1, len, fp) != len) {
        fclose(fp);
        return 0;
    }
    return fclose(fp) == 0;
}

static double now_ms(void)
{
    return (double)clock() * 1000.0 / (double)CLOCKS_PER_SEC;
}

/* ------------------------------------------------------------------ */
/* BIN commands                                                        */
/* ------------------------------------------------------------------ */

static int cmd_list(const char *path)
{
    BinArchive *b = bin_open(path);
    uint32_t i;
    if (b == NULL) {
        fprintf(stderr, "c_assets: bin_open failed: %s\n", bin_last_error());
        return 1;
    }
    printf("count %u\n", bin_count(b));
    printf("nametab %u\n", bin_name_table_len(b));
    printf("names_start %llu\n", (unsigned long long)bin_names_start(b));
    printf("data_start %llu\n", (unsigned long long)bin_data_start(b));
    for (i = 0; i < bin_count(b); i++) {
        const char *name = bin_name(b, i);
        /* Name last: it is the only field that may contain tabs. */
        printf("%u\t%u\t%llu\t%s\n", i, bin_size(b, i),
               (unsigned long long)bin_offset(b, i), name != NULL ? name : "?");
    }
    bin_close(b);
    return 0;
}

static int cmd_find(const char *path, const char *name)
{
    BinArchive *b = bin_open(path);
    int idx;
    if (b == NULL) {
        fprintf(stderr, "c_assets: bin_open failed: %s\n", bin_last_error());
        return 1;
    }
    idx = bin_find(b, name);
    printf("%d\n", idx);
    bin_close(b);
    return idx >= 0 ? 0 : 1;
}

static int cmd_findall(const char *path, const char *listpath)
{
    BinArchive *b = bin_open(path);
    FILE *fp;
    char line[4096];
    if (b == NULL) {
        fprintf(stderr, "c_assets: bin_open failed: %s\n", bin_last_error());
        return 1;
    }
    fp = fopen(listpath, "rb");
    if (fp == NULL) {
        fprintf(stderr, "c_assets: cannot open %s\n", listpath);
        bin_close(b);
        return 1;
    }
    while (fgets(line, sizeof line, fp) != NULL) {
        size_t n = strlen(line);
        int idx;
        while (n > 0 && (line[n - 1] == '\n' || line[n - 1] == '\r')) {
            line[--n] = '\0';
        }
        if (n == 0) {
            continue;
        }
        idx = bin_find(b, line);
        printf("%d\t%s\n", idx, line);
    }
    fclose(fp);
    bin_close(b);
    return 0;
}

static int cmd_dump(const char *path, long index, const char *out)
{
    BinArchive *b = bin_open(path);
    void *data;
    size_t len = 0;
    int ok;
    if (b == NULL) {
        fprintf(stderr, "c_assets: bin_open failed: %s\n", bin_last_error());
        return 1;
    }
    if (index < 0 || (unsigned long)index >= bin_count(b)) {
        fprintf(stderr, "c_assets: index %ld out of range (count %u)\n", index,
                bin_count(b));
        bin_close(b);
        return 1;
    }
    data = bin_read_alloc(b, (uint32_t)index, &len);
    if (data == NULL) {
        fprintf(stderr, "c_assets: bin_read_alloc failed\n");
        bin_close(b);
        return 1;
    }
    ok = write_file(out, data, len);
    free(data);
    bin_close(b);
    return ok ? 0 : 1;
}

/* ------------------------------------------------------------------ */
/* HZC1 commands                                                       */
/* ------------------------------------------------------------------ */

static int load_hzc1(const char *path, uint8_t **buf_out, size_t *len_out,
                     Hzc1Info *info)
{
    uint8_t *buf = read_file(path, len_out);
    int rc;
    if (buf == NULL) {
        return -1;
    }
    rc = hzc1_parse(buf, *len_out, info);
    if (rc != HZC1_OK) {
        fprintf(stderr, "c_assets: %s: %s\n", path, hzc1_strerror(rc));
        free(buf);
        return -1;
    }
    *buf_out = buf;
    return 0;
}

/* Parse "all" / "-1" / a decimal frame index.  Returns -1 for "all". */
static int parse_frame_arg(const char *s)
{
    if (s == NULL || strcmp(s, "all") == 0 || strcmp(s, "-1") == 0) {
        return -1;
    }
    return atoi(s);
}

static int cmd_hdr(const char *path)
{
    uint8_t *buf = NULL;
    size_t len = 0;
    Hzc1Info info;
    if (load_hzc1(path, &buf, &len, &info) != 0) {
        return 1;
    }
    printf("fmt=%u format=%s w=%u h=%u frames=%u bpp=%u usize=%u x=%u y=%u "
           "rawlen=%llu\n",
           (unsigned)info.fmt, hzc1_format_name(info.fmt),
           (unsigned)info.width, (unsigned)info.height,
           (unsigned)info.frames, (unsigned)info.bpp,
           (unsigned)info.uncompressed_size, (unsigned)info.x,
           (unsigned)info.y, (unsigned long long)info.raw_len);
    free(buf);
    return 0;
}

static int cmd_rgba(const char *path, const char *out, const char *frame_arg)
{
    uint8_t *buf = NULL;
    size_t len = 0;
    Hzc1Info info;
    uint8_t *rgba;
    size_t frame_pixels;
    int frame;
    int rc;
    int ok;

    if (load_hzc1(path, &buf, &len, &info) != 0) {
        return 1;
    }
    frame = parse_frame_arg(frame_arg);
    frame_pixels = (size_t)info.width * info.height * 4u;
    if (frame < 0) {
        rgba = (uint8_t *)malloc(frame_pixels * info.frames);
        if (rgba == NULL) {
            fprintf(stderr, "c_assets: out of memory\n");
            free(buf);
            return 1;
        }
        rc = hzc1_decode_all(&info, rgba);
        if (rc != HZC1_OK) {
            fprintf(stderr, "c_assets: %s: %s\n", path, hzc1_strerror(rc));
            free(rgba);
            free(buf);
            return 1;
        }
        ok = write_file(out, rgba, frame_pixels * info.frames);
        free(rgba);
    } else {
        if ((uint32_t)frame >= info.frames) {
            fprintf(stderr, "c_assets: frame %d out of range (%u frames)\n",
                    frame, (unsigned)info.frames);
            free(buf);
            return 1;
        }
        rgba = (uint8_t *)malloc(frame_pixels);
        if (rgba == NULL) {
            free(buf);
            return 1;
        }
        rc = hzc1_decode_frame(&info, (uint32_t)frame, rgba);
        if (rc != HZC1_OK) {
            fprintf(stderr, "c_assets: %s: %s\n", path, hzc1_strerror(rc));
            free(rgba);
            free(buf);
            return 1;
        }
        ok = write_file(out, rgba, frame_pixels);
        free(rgba);
    }
    free(buf);
    return ok ? 0 : 1;
}

static int cmd_ppm(const char *path, const char *out, const char *frame_arg)
{
    uint8_t *buf = NULL;
    size_t len = 0;
    Hzc1Info info;
    uint8_t *rgba;
    uint8_t *rgb;
    size_t pixels;
    size_t i;
    int frame;
    int rc;
    int ok;
    char header[64];

    if (load_hzc1(path, &buf, &len, &info) != 0) {
        return 1;
    }
    frame = parse_frame_arg(frame_arg);
    if (frame < 0) {
        frame = 0;
    }
    if ((uint32_t)frame >= info.frames) {
        fprintf(stderr, "c_assets: frame %d out of range (%u frames)\n", frame,
                (unsigned)info.frames);
        free(buf);
        return 1;
    }
    pixels = (size_t)info.width * info.height;
    rgba = (uint8_t *)malloc(pixels * 4u);
    rgb = (uint8_t *)malloc(pixels * 3u);
    if (rgba == NULL || rgb == NULL) {
        free(rgba);
        free(rgb);
        free(buf);
        return 1;
    }
    rc = hzc1_decode_frame(&info, (uint32_t)frame, rgba);
    if (rc != HZC1_OK) {
        fprintf(stderr, "c_assets: %s: %s\n", path, hzc1_strerror(rc));
        free(rgba);
        free(rgb);
        free(buf);
        return 1;
    }
    for (i = 0; i < pixels; i++) {
        rgb[i * 3 + 0] = rgba[i * 4 + 0];
        rgb[i * 3 + 1] = rgba[i * 4 + 1];
        rgb[i * 3 + 2] = rgba[i * 4 + 2];
    }
    sprintf(header, "P6\n%u %u\n255\n", (unsigned)info.width,
            (unsigned)info.height);
    {
        FILE *fp = fopen(out, "wb");
        ok = 0;
        if (fp != NULL) {
            size_t hlen = strlen(header);
            ok = fwrite(header, 1, hlen, fp) == hlen &&
                 fwrite(rgb, 1, pixels * 3u, fp) == pixels * 3u;
            if (fclose(fp) != 0) {
                ok = 0;
            }
        }
    }
    free(rgba);
    free(rgb);
    free(buf);
    return ok ? 0 : 1;
}

/* ------------------------------------------------------------------ */
/* inflate commands                                                    */
/* ------------------------------------------------------------------ */

static int inflate_file(const char *in, const char *out, long expect, long off,
                        int force_raw)
{
    uint8_t *src;
    uint8_t *dst;
    size_t srclen = 0;
    int rc;
    int ok;

    src = read_file(in, &srclen);
    if (src == NULL) {
        return 1;
    }
    if (off < 0 || (size_t)off > srclen) {
        fprintf(stderr, "c_assets: offset %ld outside input\n", off);
        free(src);
        return 1;
    }
    if (expect < 0) {
        fprintf(stderr, "c_assets: negative expected size\n");
        free(src);
        return 1;
    }
    dst = (uint8_t *)malloc((size_t)expect + 1u);
    if (dst == NULL) {
        free(src);
        return 1;
    }
    if (force_raw) {
        rc = fvp_inflate_raw(src + off, srclen - (size_t)off, dst,
                             (size_t)expect);
    } else {
        rc = fvp_inflate(src + off, srclen - (size_t)off, dst, (size_t)expect);
    }
    free(src);
    if (rc < 0) {
        fprintf(stderr, "c_assets: inflate failed: %s (%d)\n",
                fvp_inflate_strerror(rc), rc);
        free(dst);
        return 1;
    }
    if ((long)rc != expect) {
        fprintf(stderr, "c_assets: inflate produced %d bytes, expected %ld\n",
                rc, expect);
        free(dst);
        return 1;
    }
    ok = write_file(out, dst, (size_t)rc);
    free(dst);
    return ok ? 0 : 1;
}

/* ------------------------------------------------------------------ */
/* fuzz                                                                */
/* ------------------------------------------------------------------ */

/* A few embedded valid streams so the fuzzer reaches deep decode paths
 * even without a corpus (zlib level 9 / level 1 / level 6, a bare deflate
 * stream and a stored block). */
static const uint8_t fuzz_zlib_a[] = {
    0x78, 0xda, 0x0b, 0xc9, 0x48, 0x55, 0x28, 0x2c, 0xcd, 0x4c, 0xce, 0x56,
    0x48, 0x2a, 0xca, 0x2f, 0xcf, 0x53, 0x48, 0xcb, 0xaf, 0x50, 0xc8, 0x2a,
    0xcd, 0x2d, 0x28, 0x56, 0xc8, 0x2f, 0x4b, 0x2d, 0x52, 0x28, 0x01, 0x4a,
    0xe7, 0x24, 0x56, 0x55, 0x2a, 0xa4, 0xe4, 0xa7, 0xeb, 0x29, 0x84, 0x8c,
    0x2a, 0x1e, 0x55, 0x3c, 0xaa, 0x78, 0x54, 0xf1, 0xa8, 0xe2, 0x51, 0xc5,
    0xc3, 0x4b, 0x31, 0x00, 0x88, 0x0a, 0x86, 0x37,
};
static const uint8_t fuzz_zlib_b[] = {
    0x78, 0x01, 0x73, 0x0b, 0x0b, 0x70, 0x1b, 0x45, 0xa3, 0x21, 0x30, 0x1a,
    0x02, 0x03, 0x17, 0x02, 0x00, 0xc3, 0xae, 0x14, 0xa0,
};
static const uint8_t fuzz_zlib_c[] = {
    0x78, 0x9c, 0x63, 0x60, 0x64, 0x62, 0x66, 0x61, 0x65, 0x63, 0xe7, 0xe0,
    0xe4, 0xe2, 0xe6, 0xe1, 0xe5, 0xe3, 0x17, 0x10, 0x14, 0x12, 0x16, 0x11,
    0x15, 0x13, 0x97, 0x90, 0x94, 0x92, 0x96, 0x91, 0x95, 0x93, 0x57, 0x50,
    0x54, 0x52, 0x56, 0x51, 0x55, 0x53, 0xd7, 0xd0, 0xd4, 0xd2, 0xd6, 0xd1,
    0xd5, 0xd3, 0x37, 0x30, 0x34, 0x32, 0x36, 0x31, 0x35, 0x33, 0xb7, 0xb0,
    0xb4, 0xb2, 0xb6, 0xb1, 0xb5, 0xb3, 0x77, 0x70, 0x74, 0x72, 0x76, 0x71,
    0x75, 0x73, 0xf7, 0xf0, 0xf4, 0xf2, 0xf6, 0xf1, 0xf5, 0xf3, 0x0f, 0x08,
    0x0c, 0x0a, 0x0e, 0x09, 0x0d, 0x0b, 0x8f, 0x88, 0x8c, 0x8a, 0x8e, 0x89,
    0x8d, 0x8b, 0x4f, 0x48, 0x4c, 0x4a, 0x4e, 0x49, 0x4d, 0x4b, 0xcf, 0xc8,
    0xcc, 0xca, 0xce, 0xc9, 0xcd, 0xcb, 0x2f, 0x28, 0x2c, 0x2a, 0x2e, 0x29,
    0x2d, 0x2b, 0xaf, 0xa8, 0xac, 0xaa, 0xae, 0xa9, 0xad, 0xab, 0x6f, 0x68,
    0x6c, 0x6a, 0x6e, 0x69, 0x6d, 0x6b, 0xef, 0xe8, 0xec, 0xea, 0xee, 0xe9,
    0xed, 0xeb, 0x9f, 0x30, 0x71, 0xd2, 0xe4, 0x29, 0x53, 0xa7, 0x4d, 0x9f,
    0x31, 0x73, 0xd6, 0xec, 0x39, 0x73, 0xe7, 0xcd, 0x5f, 0xb0, 0x70, 0xd1,
    0xe2, 0x25, 0x4b, 0x97, 0x2d, 0x5f, 0xb1, 0x72, 0xd5, 0xea, 0x35, 0x6b,
    0xd7, 0xad, 0xdf, 0xb0, 0x71, 0xd3, 0xe6, 0x2d, 0x5b, 0xb7, 0x6d, 0xdf,
    0xb1, 0x73, 0xd7, 0xee, 0x3d, 0x7b, 0xf7, 0xed, 0x3f, 0x70, 0xf0, 0xd0,
    0xe1, 0x23, 0x47, 0x8f, 0x1d, 0x3f, 0x71, 0xf2, 0xd4, 0xe9, 0x33, 0x67,
    0xcf, 0x9d, 0xbf, 0x70, 0xf1, 0xd2, 0xe5, 0x2b, 0x57, 0xaf, 0x5d, 0xbf,
    0x71, 0xf3, 0xd6, 0xed, 0x3b, 0x77, 0xef, 0xdd, 0x7f, 0xf0, 0xf0, 0xd1,
    0xe3, 0x27, 0x4f, 0x9f, 0x3d, 0x7f, 0xf1, 0xf2, 0xd5, 0xeb, 0x37, 0x6f,
    0xdf, 0xbd, 0xff, 0xf0, 0xf1, 0xd3, 0xe7, 0x2f, 0x5f, 0xbf, 0x7d, 0xff,
    0xf1, 0xf3, 0xd7, 0xef, 0x3f, 0x7f, 0xff, 0xfd, 0x67, 0x18, 0xf5, 0xff,
    0xa8, 0xff, 0x47, 0xfd, 0x3f, 0xea, 0xff, 0x51, 0xff, 0x8f, 0xfa, 0x7f,
    0x04, 0xfa, 0x1f, 0x00, 0x41, 0x38, 0xfc, 0x2e,
};
static const uint8_t fuzz_raw_a[] = {
    0x2b, 0x4a, 0x2c, 0x57, 0x48, 0x49, 0x4d, 0xcb, 0x49, 0x2c, 0x49, 0x55,
    0x28, 0x48, 0xac, 0xcc, 0xc9, 0x4f, 0x4c, 0x51, 0x30, 0x34, 0x32, 0x36,
    0x31, 0x35, 0x33, 0xb7, 0xb0, 0x34, 0x50, 0x28, 0x1a, 0x95, 0x1e, 0x95,
    0x1e, 0x95, 0x1e, 0x95, 0x1e, 0x95, 0x1e, 0x95, 0x1e, 0x95, 0xc6, 0x2e,
    0x0d, 0x00,
};
static const uint8_t fuzz_stored[] = {
    0x78, 0x01, 0x01, 0x02, 0x00, 0xfd, 0xff, 0x61, 0x62, 0x01, 0x26, 0x00,
    0xc4,
};

#define FUZZ_SEED_MAX 16
#define FUZZ_SRC_MAX 8192
#define FUZZ_DST_MAX (1u << 20)
#define FUZZ_GUARD 64

typedef struct {
    const uint8_t *data[FUZZ_SEED_MAX];
    size_t len[FUZZ_SEED_MAX];
    int count;
} SeedSet;

static void seeds_add(SeedSet *s, const uint8_t *data, size_t len)
{
    if (s->count < FUZZ_SEED_MAX && len > 0) {
        s->data[s->count] = data;
        s->len[s->count] = len;
        s->count++;
    }
}

static uint64_t g_rng;

static uint32_t rnd32(void)
{
    /* xorshift64* */
    g_rng ^= g_rng >> 12;
    g_rng ^= g_rng << 25;
    g_rng ^= g_rng >> 27;
    return (uint32_t)((g_rng * 2685821657736338717ull) >> 32);
}

static int cmd_fuzz(unsigned long iters, unsigned long seed,
                    const char *seedlist)
{
    SeedSet seeds;
    uint8_t *src;
    uint8_t *dst;
    unsigned long it;
    unsigned long n_ok = 0;
    unsigned long n_err = 0;
    unsigned long n_bad = 0;
    uint8_t *file_seeds[FUZZ_SEED_MAX];
    int n_file_seeds = 0;
    int i;

    memset(&seeds, 0, sizeof seeds);
    seeds_add(&seeds, fuzz_zlib_a, sizeof fuzz_zlib_a);
    seeds_add(&seeds, fuzz_zlib_b, sizeof fuzz_zlib_b);
    seeds_add(&seeds, fuzz_zlib_c, sizeof fuzz_zlib_c);
    seeds_add(&seeds, fuzz_raw_a, sizeof fuzz_raw_a);
    seeds_add(&seeds, fuzz_stored, sizeof fuzz_stored);

    /* Optional: a text file listing real hzc1 files whose payloads are used
     * as mutation seeds (this exercises the production data path). */
    if (seedlist != NULL) {
        FILE *fp = fopen(seedlist, "rb");
        if (fp != NULL) {
            char line[4096];
            while (fgets(line, sizeof line, fp) != NULL) {
                size_t n = strlen(line);
                size_t flen = 0;
                uint8_t *fb;
                while (n > 0 && (line[n - 1] == '\n' || line[n - 1] == '\r')) {
                    line[--n] = '\0';
                }
                if (n == 0 || n_file_seeds >= 8) {
                    continue;
                }
                fb = read_file(line, &flen);
                if (fb == NULL) {
                    continue;
                }
                if (flen > 44) {
                    file_seeds[n_file_seeds] = fb;
                    seeds_add(&seeds, fb + 44, flen - 44);
                    n_file_seeds++;
                } else {
                    free(fb);
                }
            }
            fclose(fp);
        }
    }

    src = (uint8_t *)malloc(FUZZ_SRC_MAX + 2u * FUZZ_GUARD);
    dst = (uint8_t *)malloc(FUZZ_DST_MAX + 2u * FUZZ_GUARD);
    if (src == NULL || dst == NULL) {
        die("fuzz: out of memory");
        free(src);
        free(dst);
        for (i = 0; i < n_file_seeds; i++) {
            free(file_seeds[i]);
        }
        return 1;
    }
    g_rng = seed != 0 ? seed : 0x9E3779B97F4A7C15ull;

    for (it = 0; it < iters; it++) {
        uint8_t *sp = src + FUZZ_GUARD;
        uint8_t *dp = dst + FUZZ_GUARD;
        size_t srclen;
        size_t dstlen;
        uint32_t mode = rnd32() % 4u;
        int rc;

        srclen = rnd32() % (FUZZ_SRC_MAX + 1u);
        if (mode == 0 || seeds.count == 0) {
            size_t k;
            for (k = 0; k < srclen; k++) {
                sp[k] = (uint8_t)(rnd32() & 0xFFu);
            }
        } else {
            const uint8_t *base;
            size_t baselen;
            int si = (int)(rnd32() % (uint32_t)seeds.count);
            size_t k;
            int muts;
            base = seeds.data[si];
            baselen = seeds.len[si];
            if (baselen > FUZZ_SRC_MAX) {
                baselen = FUZZ_SRC_MAX;
            }
            if (mode == 3) {
                /* truncate a valid stream */
                srclen = rnd32() % (baselen + 1u);
            } else {
                srclen = baselen;
            }
            memcpy(sp, base, srclen);
            if (mode == 1) {
                /* bit/byte mutations */
                muts = 1 + (int)(rnd32() % 12u);
                while (muts-- > 0) {
                    if (srclen == 0) {
                        break;
                    }
                    sp[rnd32() % (uint32_t)srclen] ^=
                        (uint8_t)(1u << (rnd32() & 7u));
                }
            } else if (mode == 2) {
                /* append random trailing garbage */
                size_t extra = rnd32() % 64u;
                if (srclen + extra > FUZZ_SRC_MAX) {
                    extra = FUZZ_SRC_MAX - srclen;
                }
                for (k = 0; k < extra; k++) {
                    sp[srclen + k] = (uint8_t)(rnd32() & 0xFFu);
                }
                srclen += extra;
            }
        }
        if (srclen > FUZZ_SRC_MAX) {
            srclen = FUZZ_SRC_MAX;
        }
        dstlen = rnd32() % (FUZZ_DST_MAX + 1u);
        if ((rnd32() & 3u) == 0u) {
            dstlen = dstlen % 256u; /* small buffers: exercise ERR_BUF */
        }

        memset(src, 0xA5, FUZZ_GUARD);
        memset(src + FUZZ_GUARD + srclen, 0xA5, FUZZ_GUARD);
        memset(dst, 0x5A, FUZZ_DST_MAX + 2u * FUZZ_GUARD);

        rc = fvp_inflate(sp, srclen, dp, dstlen);
        if (rc >= 0) {
            if ((size_t)rc > dstlen) {
                n_bad++;
            }
            n_ok++;
        } else {
            n_err++;
        }
        {
            size_t k;
            for (k = 0; k < FUZZ_GUARD; k++) {
                if (dst[k] != 0x5A || dst[FUZZ_GUARD + dstlen + k] != 0x5A ||
                    src[k] != 0xA5 || src[FUZZ_GUARD + srclen + k] != 0xA5) {
                    n_bad++;
                    break;
                }
            }
        }
    }

    printf("fuzz iterations=%lu ok=%lu err=%lu guard_violations=%lu "
           "seeds=%d file_seeds=%d\n",
           iters, n_ok, n_err, n_bad, seeds.count, n_file_seeds);
    free(src);
    free(dst);
    for (i = 0; i < n_file_seeds; i++) {
        free(file_seeds[i]);
    }
    return n_bad == 0 ? 0 : 1;
}

/* ------------------------------------------------------------------ */
/* benchmarks                                                          */
/* ------------------------------------------------------------------ */

static int cmd_bench(const char *path, unsigned long iters)
{
    uint8_t *buf = NULL;
    size_t len = 0;
    Hzc1Info info;
    uint8_t *rgba;
    size_t out_len;
    unsigned long i;
    double t0;
    double dt;

    if (iters == 0) {
        iters = 1;
    }
    if (load_hzc1(path, &buf, &len, &info) != 0) {
        return 1;
    }
    out_len = (size_t)info.width * info.height * 4u * info.frames;
    rgba = (uint8_t *)malloc(out_len);
    if (rgba == NULL) {
        free(buf);
        return 1;
    }
    t0 = now_ms();
    for (i = 0; i < iters; i++) {
        int rc = hzc1_decode_all(&info, rgba);
        if (rc != HZC1_OK) {
            fprintf(stderr, "c_assets: decode failed: %s\n", hzc1_strerror(rc));
            free(rgba);
            free(buf);
            return 1;
        }
    }
    dt = now_ms() - t0;
    printf("bench decode file=%s fmt=%u w=%u h=%u frames=%u iters=%lu "
           "total_ms=%.3f per_iter_ms=%.3f raw_bytes=%llu mib_per_s=%.1f\n",
           path, (unsigned)info.fmt, (unsigned)info.width,
           (unsigned)info.height, (unsigned)info.frames, iters, dt,
           dt / (double)iters, (unsigned long long)info.raw_len,
           dt > 0.0 ? ((double)info.raw_len / (1024.0 * 1024.0)) /
                          (dt / 1000.0 / (double)iters)
                    : 0.0);
    free(rgba);
    free(buf);
    return 0;
}

static int cmd_bench_inflate(const char *in, long expect, unsigned long iters,
                             long off)
{
    uint8_t *src;
    uint8_t *dst;
    size_t srclen = 0;
    unsigned long i;
    double t0;
    double dt;
    int rc;

    if (iters == 0) {
        iters = 1;
    }
    if (expect < 0) {
        return 1;
    }
    src = read_file(in, &srclen);
    if (src == NULL) {
        return 1;
    }
    if (off < 0 || (size_t)off > srclen) {
        free(src);
        return 1;
    }
    dst = (uint8_t *)malloc((size_t)expect + 1u);
    if (dst == NULL) {
        free(src);
        return 1;
    }
    t0 = now_ms();
    for (i = 0; i < iters; i++) {
        rc = fvp_inflate(src + off, srclen - (size_t)off, dst, (size_t)expect);
        if (rc != (int)expect) {
            fprintf(stderr, "c_assets: inflate failed: %s (%d)\n",
                    fvp_inflate_strerror(rc), rc);
            free(dst);
            free(src);
            return 1;
        }
    }
    dt = now_ms() - t0;
    printf("bench inflate file=%s compressed=%llu expanded=%ld iters=%lu "
           "total_ms=%.3f per_iter_ms=%.3f mib_per_s=%.1f\n",
           in, (unsigned long long)(srclen - (size_t)off), expect, iters, dt,
           dt / (double)iters,
           dt > 0.0 ? ((double)expect / (1024.0 * 1024.0)) /
                          (dt / 1000.0 / (double)iters)
                    : 0.0);
    free(dst);
    free(src);
    return 0;
}

/* ------------------------------------------------------------------ */
/* main                                                                */
/* ------------------------------------------------------------------ */

static int usage(void)
{
    fprintf(stderr,
            "usage: c_assets <command> ...\n"
            "  list <package>\n"
            "  find <package> <utf8-name>\n"
            "  findall <package> <names.txt>\n"
            "  dump <package> <index> <out>\n"
            "  hdr <hzc1-file>\n"
            "  rgba <hzc1-file> <out> [frame|all]\n"
            "  png <hzc1-file> <out.ppm> [frame]     (alias: ppm)\n"
            "  ppm <hzc1-file> <out.ppm> [frame]\n"
            "  inflate <in> <out> <expect> [offset]\n"
            "  rawinflate <in> <out> <expect> [offset]\n"
            "  fuzz <iterations> <seed> [seedlist]\n"
            "  bench <hzc1-file> <iters>\n"
            "  bench-inflate <in> <expect> <iters> [offset]\n");
    return 2;
}

int main(int argc, char **argv)
{
    const char *cmd;
    if (argc < 2) {
        return usage();
    }
    cmd = argv[1];
    if (strcmp(cmd, "list") == 0 && argc == 3) {
        return cmd_list(argv[2]);
    }
    if (strcmp(cmd, "find") == 0 && argc == 4) {
        return cmd_find(argv[2], argv[3]);
    }
    if (strcmp(cmd, "findall") == 0 && argc == 4) {
        return cmd_findall(argv[2], argv[3]);
    }
    if (strcmp(cmd, "dump") == 0 && argc == 5) {
        return cmd_dump(argv[2], atol(argv[3]), argv[4]);
    }
    if (strcmp(cmd, "hdr") == 0 && argc == 3) {
        return cmd_hdr(argv[2]);
    }
    if (strcmp(cmd, "rgba") == 0 && (argc == 4 || argc == 5)) {
        return cmd_rgba(argv[2], argv[3], argc == 5 ? argv[4] : NULL);
    }
    if (strcmp(cmd, "ppm") == 0 && (argc == 4 || argc == 5)) {
        return cmd_ppm(argv[2], argv[3], argc == 5 ? argv[4] : NULL);
    }
    /* `png` is the name used in the task description; it writes a PPM
     * (P6) because no PNG encoder is bundled. */
    if (strcmp(cmd, "png") == 0 && (argc == 4 || argc == 5)) {
        return cmd_ppm(argv[2], argv[3], argc == 5 ? argv[4] : NULL);
    }
    if (strcmp(cmd, "inflate") == 0 && (argc == 5 || argc == 6)) {
        return inflate_file(argv[2], argv[3], atol(argv[4]),
                            argc == 6 ? atol(argv[5]) : 0, 0);
    }
    if (strcmp(cmd, "rawinflate") == 0 && (argc == 5 || argc == 6)) {
        return inflate_file(argv[2], argv[3], atol(argv[4]),
                            argc == 6 ? atol(argv[5]) : 0, 1);
    }
    if (strcmp(cmd, "fuzz") == 0 && (argc == 4 || argc == 5)) {
        return cmd_fuzz(strtoul(argv[2], NULL, 10), strtoul(argv[3], NULL, 10),
                        argc == 5 ? argv[4] : NULL);
    }
    if (strcmp(cmd, "bench") == 0 && argc == 4) {
        return cmd_bench(argv[2], strtoul(argv[3], NULL, 10));
    }
    if (strcmp(cmd, "bench-inflate") == 0 && (argc == 5 || argc == 6)) {
        return cmd_bench_inflate(argv[2], atol(argv[3]),
                                 strtoul(argv[4], NULL, 10),
                                 argc == 6 ? atol(argv[5]) : 0);
    }
    return usage();
}