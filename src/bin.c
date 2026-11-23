/* FVP VM in C -- BIN resource archive reader.  See bin.h. */
#include "bin.h"

#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "gen_cp932.h"

#define FVP_NTRAIL 192
#define FVP_TRAIL_LO 0x40
#define FVP_CP_INVALID 0xFFFFu
#define FVP_BIN_MAX_COUNT 0x1000000u     /* 16M entries: sanity bound */
#define FVP_BIN_MAX_NAMETAB 0x10000000u  /* 256 MiB name table       */

struct BinArchive {
    FILE *fp;
    uint32_t count;
    uint32_t name_len;
    uint64_t names_start; /* 8 + 12 * count                        */
    uint64_t data_start;  /* names_start + name_len                */
    struct BinEntry {
        uint32_t name_off;
        uint32_t data_off;
        uint32_t size;
    } *ents;
    uint8_t *name_tab; /* name table, name_len bytes */
    char **utf8;       /* lazily decoded names, count entries */
    uint64_t file_size;
};

static char g_bin_error[256];

static void set_error(const char *msg)
{
    size_t n = strlen(msg);
    if (n >= sizeof g_bin_error) {
        n = sizeof g_bin_error - 1;
    }
    memcpy(g_bin_error, msg, n);
    g_bin_error[n] = '\0';
}

const char *bin_last_error(void)
{
    return g_bin_error[0] != '\0' ? g_bin_error : "no error";
}

const char *bin_strerror(int code)
{
    switch (code) {
    case BIN_OK:
        return "ok";
    case BIN_ERR_ARG:
        return "invalid argument or index out of range";
    case BIN_ERR_OPEN:
        return "cannot open package";
    case BIN_ERR_IO:
        return "read/seek error";
    case BIN_ERR_CORRUPT:
        return "corrupt package index";
    case BIN_ERR_SMALL:
        return "caller buffer too small";
    case BIN_ERR_MEMORY:
        return "out of memory";
    default:
        return "unknown error";
    }
}

/* ---- portable 64-bit stdio helpers ---------------------------------- */

static int seek_to(FILE *fp, uint64_t off)
{
#if defined(_WIN32)
    return _fseeki64(fp, (long long)off, SEEK_SET) == 0;
#else
    if (off > (uint64_t)LONG_MAX) {
        return 0;
    }
    return fseek(fp, (long)off, SEEK_SET) == 0;
#endif
}

static int seek_end(FILE *fp)
{
#if defined(_WIN32)
    return _fseeki64(fp, 0, SEEK_END) == 0;
#else
    return fseek(fp, 0, SEEK_END) == 0;
#endif
}

static int64_t tell_off(FILE *fp)
{
#if defined(_WIN32)
    return (int64_t)_ftelli64(fp);
#else
    return (int64_t)ftell(fp);
#endif
}

static uint32_t rd32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) |
           ((uint32_t)p[3] << 24);
}

/* ---- Shift-JIS (CP932) -> UTF-8 ------------------------------------- */

int bin_sjis_to_utf8(const uint8_t *src, size_t len, char *dst, size_t dstlen)
{
    size_t i = 0;
    size_t o = 0;

    if (src == NULL || dst == NULL) {
        return -1;
    }
    while (i < len) {
        uint32_t cp;
        uint32_t c = fvp_cp932_single[src[i]];
        if (c != FVP_CP_INVALID) {
            cp = c;
            i += 1;
        } else {
            uint8_t row = fvp_cp932_lead_row[src[i]];
            int paired = 0;
            if (row != 0xFF && i + 1 < len && src[i + 1] >= FVP_TRAIL_LO) {
                uint32_t p = fvp_cp932_rows[(size_t)row * FVP_NTRAIL +
                                            (size_t)(src[i + 1] - FVP_TRAIL_LO)];
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
        if (dstlen - o < 4) {
            return -1; /* room for up to 3 UTF-8 bytes plus the NUL */
        }
        if (cp < 0x80u) {
            dst[o++] = (char)cp;
        } else if (cp < 0x800u) {
            dst[o++] = (char)(0xC0u | (cp >> 6));
            dst[o++] = (char)(0x80u | (cp & 0x3Fu));
        } else {
            dst[o++] = (char)(0xE0u | (cp >> 12));
            dst[o++] = (char)(0x80u | ((cp >> 6) & 0x3Fu));
            dst[o++] = (char)(0x80u | (cp & 0x3Fu));
        }
    }
    dst[o] = '\0';
    return (int)o;
}

/* ---- open / close ---------------------------------------------------- */

void bin_close(BinArchive *b)
{
    uint32_t i;
    if (b == NULL) {
        return;
    }
    if (b->utf8 != NULL) {
        for (i = 0; i < b->count; i++) {
            free(b->utf8[i]);
        }
        free(b->utf8);
    }
    if (b->fp != NULL) {
        fclose(b->fp);
    }
    free(b->ents);
    free(b->name_tab);
    free(b);
}

BinArchive *bin_open(const char *path)
{
    uint8_t hdr[8];
    BinArchive *b;
    uint64_t fsize;
    uint64_t i;

    g_bin_error[0] = '\0';
    if (path == NULL) {
        set_error("bin_open: path is NULL");
        return NULL;
    }
    b = (BinArchive *)calloc(1, sizeof *b);
    if (b == NULL) {
        set_error("bin_open: out of memory");
        return NULL;
    }
    b->fp = fopen(path, "rb");
    if (b->fp == NULL) {
        set_error("bin_open: cannot open file");
        free(b);
        return NULL;
    }
    if (fread(hdr, 1, sizeof hdr, b->fp) != sizeof hdr) {
        set_error("bin_open: file shorter than the 8-byte header");
        bin_close(b);
        return NULL;
    }
    b->count = rd32(hdr);
    b->name_len = rd32(hdr + 4);
    if (b->count > FVP_BIN_MAX_COUNT) {
        set_error("bin_open: implausible entry count");
        bin_close(b);
        return NULL;
    }
    if (b->name_len > FVP_BIN_MAX_NAMETAB) {
        set_error("bin_open: implausible name table length");
        bin_close(b);
        return NULL;
    }
    if (!seek_end(b->fp)) {
        set_error("bin_open: cannot seek to end of file");
        bin_close(b);
        return NULL;
    }
    {
        int64_t end = tell_off(b->fp);
        if (end < 0) {
            set_error("bin_open: cannot determine file size");
            bin_close(b);
            return NULL;
        }
        fsize = (uint64_t)end;
    }
    b->file_size = fsize;
    b->names_start = 8u + 12ull * (uint64_t)b->count;
    b->data_start = b->names_start + (uint64_t)b->name_len;
    if (b->names_start > fsize) {
        set_error("bin_open: entry table past end of file");
        bin_close(b);
        return NULL;
    }
    if (b->data_start > fsize) {
        set_error("bin_open: name table past end of file");
        bin_close(b);
        return NULL;
    }

    if (b->count != 0) {
        b->ents = (struct BinEntry *)calloc(b->count, sizeof *b->ents);
        b->utf8 = (char **)calloc(b->count, sizeof *b->utf8);
        if (b->ents == NULL || b->utf8 == NULL) {
            set_error("bin_open: out of memory for the index");
            bin_close(b);
            return NULL;
        }
        if (!seek_to(b->fp, 8)) {
            set_error("bin_open: cannot seek to the entry table");
            bin_close(b);
            return NULL;
        }
        for (i = 0; i < (uint64_t)b->count; i++) {
            uint8_t raw[12];
            if (fread(raw, 1, sizeof raw, b->fp) != sizeof raw) {
                set_error("bin_open: truncated entry table");
                bin_close(b);
                return NULL;
            }
            b->ents[i].name_off = rd32(raw);
            b->ents[i].data_off = rd32(raw + 4);
            b->ents[i].size = rd32(raw + 8);
        }
    }
    if (b->name_len != 0) {
        b->name_tab = (uint8_t *)malloc(b->name_len);
        if (b->name_tab == NULL) {
            set_error("bin_open: out of memory for the name table");
            bin_close(b);
            return NULL;
        }
        if (!seek_to(b->fp, b->names_start)) {
            set_error("bin_open: cannot seek to the name table");
            bin_close(b);
            return NULL;
        }
        if (fread(b->name_tab, 1, b->name_len, b->fp) != b->name_len) {
            set_error("bin_open: truncated name table");
            bin_close(b);
            return NULL;
        }
    }

    /* Validate every entry against the file size and the name table. */
    for (i = 0; i < (uint64_t)b->count; i++) {
        const struct BinEntry *e = &b->ents[i];
        if (b->name_len == 0 || e->name_off >= b->name_len) {
            set_error("bin_open: entry name offset outside the name table");
            bin_close(b);
            return NULL;
        }
        if ((uint64_t)e->data_off < 8u ||
            (uint64_t)e->data_off + (uint64_t)e->size > fsize) {
            set_error("bin_open: entry data range outside the file");
            bin_close(b);
            return NULL;
        }
    }
    return b;
}

/* ---- accessors ------------------------------------------------------- */

uint32_t bin_count(const BinArchive *b)
{
    return b != NULL ? b->count : 0;
}

uint32_t bin_name_table_len(const BinArchive *b)
{
    return b != NULL ? b->name_len : 0;
}

uint64_t bin_names_start(const BinArchive *b)
{
    return b != NULL ? b->names_start : 0;
}

uint64_t bin_data_start(const BinArchive *b)
{
    return b != NULL ? b->data_start : 0;
}

const uint8_t *bin_name_raw(const BinArchive *b, uint32_t index,
                            size_t *len_out)
{
    const uint8_t *start;
    const uint8_t *nul;
    size_t maxlen;

    if (len_out != NULL) {
        *len_out = 0;
    }
    if (b == NULL || index >= b->count || b->name_tab == NULL) {
        return NULL;
    }
    start = b->name_tab + b->ents[index].name_off;
    maxlen = (size_t)(b->name_len - b->ents[index].name_off);
    nul = (const uint8_t *)memchr(start, 0, maxlen);
    if (len_out != NULL) {
        *len_out = nul != NULL ? (size_t)(nul - start) : maxlen;
    }
    return start;
}

const char *bin_name(const BinArchive *b, uint32_t index)
{
    const uint8_t *raw;
    size_t raw_len;
    char *s;
    size_t cap;
    int n;

    if (b == NULL || index >= b->count) {
        return NULL;
    }
    if (b->utf8[index] != NULL) {
        return b->utf8[index];
    }
    raw = bin_name_raw(b, index, &raw_len);
    if (raw == NULL) {
        return NULL;
    }
    cap = raw_len * 3u + 4u;
    s = (char *)malloc(cap);
    if (s == NULL) {
        return NULL;
    }
    n = bin_sjis_to_utf8(raw, raw_len, s, cap);
    if (n < 0) {
        free(s);
        return NULL;
    }
    b->utf8[index] = s;
    return s;
}

uint32_t bin_size(const BinArchive *b, uint32_t index)
{
    if (b == NULL || index >= b->count) {
        return 0;
    }
    return b->ents[index].size;
}

uint64_t bin_offset(const BinArchive *b, uint32_t index)
{
    if (b == NULL || index >= b->count) {
        return 0;
    }
    return b->ents[index].data_off;
}

/* ---- data access ----------------------------------------------------- */

int bin_read(const BinArchive *b, uint32_t index, void *buf, size_t buflen)
{
    uint32_t size;

    if (b == NULL || buf == NULL || index >= b->count) {
        return BIN_ERR_ARG;
    }
    size = b->ents[index].size;
    if (buflen < (size_t)size) {
        return BIN_ERR_SMALL;
    }
    if (!seek_to(b->fp, b->ents[index].data_off)) {
        return BIN_ERR_IO;
    }
    if (size != 0 && fread(buf, 1, (size_t)size, b->fp) != (size_t)size) {
        return BIN_ERR_IO;
    }
    return BIN_OK;
}

void *bin_read_alloc(const BinArchive *b, uint32_t index, size_t *size_out)
{
    uint32_t size;
    uint8_t *buf;

    if (size_out != NULL) {
        *size_out = 0;
    }
    if (b == NULL || index >= b->count) {
        return NULL;
    }
    size = b->ents[index].size;
    buf = (uint8_t *)malloc((size_t)size + 1u);
    if (buf == NULL) {
        return NULL;
    }
    if (bin_read(b, index, buf, size) != BIN_OK) {
        free(buf);
        return NULL;
    }
    buf[size] = 0;
    if (size_out != NULL) {
        *size_out = (size_t)size;
    }
    return buf;
}

int bin_find(const BinArchive *b, const char *name)
{
    uint32_t i;

    if (b == NULL || name == NULL) {
        return -1;
    }
    for (i = 0; i < b->count; i++) {
        const char *n = bin_name(b, i);
        if (n != NULL && strcmp(n, name) == 0) {
            return (int)i;
        }
    }
    return -1;
}