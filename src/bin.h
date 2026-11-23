/* FVP VM in C -- BIN resource archive reader.
 *
 * Package layout (little endian, see fvp/docs/00-engine.md):
 *
 *   +0x00  u32  file_count
 *   +0x04  u32  name_table_len
 *   +0x08  count * { u32 name_off, u32 data_off, u32 size }   (12 B/entry)
 *          name table: Shift-JIS strings, NUL terminated; name_off is
 *                      relative to the start of the name table
 *          data area:  data_off and size are absolute file offsets
 *
 * Only the index and the name table are kept in memory; entry payloads are
 * read on demand with a 64-bit file offset, because the shipped packages
 * reach ~2 GB.  Names are decoded from Shift-JIS (CP932) to UTF-8 lazily
 * and cached.  The decoder uses the generated CP932 tables in
 * src/gen_cp932.h (the same tables as src/encoding.c, so the C VM and the
 * asset layer produce identical names).
 */
#ifndef FVP_BIN_H
#define FVP_BIN_H

#include <stddef.h>
#include <stdint.h>

typedef enum BinError {
    BIN_OK = 0,
    BIN_ERR_ARG = -1,    /* NULL / index out of range                  */
    BIN_ERR_OPEN = -2,   /* cannot open the file                       */
    BIN_ERR_IO = -3,     /* read/seek failed                           */
    BIN_ERR_CORRUPT = -4,/* header / entry / name table out of bounds  */
    BIN_ERR_SMALL = -5,  /* caller buffer smaller than the entry       */
    BIN_ERR_MEMORY = -6  /* allocation failure                         */
} BinError;

typedef struct BinArchive BinArchive;

/* Open a package and parse its index.  Returns NULL on failure; the reason
 * is available from bin_last_error(). */
BinArchive *bin_open(const char *path);
void bin_close(BinArchive *b);

uint32_t bin_count(const BinArchive *b);
uint32_t bin_name_table_len(const BinArchive *b);
/* Offset of the name table: 8 + 12 * count. */
uint64_t bin_names_start(const BinArchive *b);
/* Offset just past the name table, i.e. where the data area begins. */
uint64_t bin_data_start(const BinArchive *b);

/* Entry helpers.  All return 0 / NULL when the index is out of range. */
const char *bin_name(const BinArchive *b, uint32_t index); /* UTF-8, cached */
const uint8_t *bin_name_raw(const BinArchive *b, uint32_t index,
                            size_t *len_out); /* Shift-JIS bytes, no NUL */
uint32_t bin_size(const BinArchive *b, uint32_t index);
uint64_t bin_offset(const BinArchive *b, uint32_t index);

/* Read one entry into buf.  buflen must be >= bin_size(index); the whole
 * entry is read (no partial reads). */
int bin_read(const BinArchive *b, uint32_t index, void *buf, size_t buflen);

/* Read one entry into a freshly malloc'd buffer (NUL terminated as a
 * convenience, entry bytes may of course contain NULs).  *size_out receives
 * the exact byte count.  Free with free(). */
void *bin_read_alloc(const BinArchive *b, uint32_t index, size_t *size_out);

/* Index of the entry whose UTF-8 name equals name, or -1. */
int bin_find(const BinArchive *b, const char *name);

/* Decode Shift-JIS (CP932) bytes to a NUL-terminated UTF-8 string.  Invalid
 * sequences become U+FFFD (one byte consumed), matching the streaming rule
 * of fvp_decode() in src/encoding.c and Python's decode('cp932', 'replace').
 * Returns the UTF-8 byte length, or -1 when dst is too small. */
int bin_sjis_to_utf8(const uint8_t *src, size_t len, char *dst, size_t dstlen);

const char *bin_last_error(void);
const char *bin_strerror(int code);

#endif /* FVP_BIN_H */