/* FVP VM in C -- DEFLATE / zlib decompressor (no external dependency).
 *
 * The bundled w64devkit ships no zlib, so this is a self-contained C99
 * implementation of RFC 1951 (DEFLATE) and RFC 1950 (zlib wrapper).  It is
 * byte-for-byte compatible with Python's zlib.decompress() on valid input
 * and returns an error code (never crashes) on corrupt input.  See
 * fvp/docs/c-assets.md for the verification numbers.
 */
#ifndef FVP_INFLATE_H
#define FVP_INFLATE_H

#include <stddef.h>
#include <stdint.h>

/* Error codes.  fvp_inflate() returns the number of decompressed bytes
 * (>= 0) on success, or one of these negative codes. */
enum {
    FVP_INFLATE_OK           = 0,
    FVP_INFLATE_ERR_ARG      = -1,  /* NULL pointer / invalid argument        */
    FVP_INFLATE_ERR_HEADER   = -2,  /* bad zlib header / preset dictionary    */
    FVP_INFLATE_ERR_TRUNC    = -3,  /* input ends in the middle of the stream */
    FVP_INFLATE_ERR_DATA     = -4,  /* corrupt Huffman data / distance / type */
    FVP_INFLATE_ERR_BUF      = -5,  /* output does not fit in dst             */
    FVP_INFLATE_ERR_CHECKSUM = -6,  /* zlib Adler-32 mismatch                 */
    FVP_INFLATE_ERR_MEMORY   = -7   /* out of memory while building a tree    */
};

/* Decompress a zlib-wrapped (RFC 1950) or bare DEFLATE (RFC 1951) stream.
 * The wrapper is auto-detected from the first two bytes (a valid zlib header
 * is CM=8 and (CMF*256+FLG) % 31 == 0).  A bare stream whose first two bytes
 * happen to look like a zlib header is misdetected; use fvp_inflate_raw()
 * when the caller knows the stream is raw.
 *
 * dstlen is the capacity of dst.  Producing more than dstlen bytes is an
 * error rather than a truncation.  Returns the produced size on success. */
int fvp_inflate(const uint8_t *src, size_t srclen, uint8_t *dst, size_t dstlen);

/* Same, but requiring/forbidding the zlib wrapper. */
int fvp_inflate_zlib(const uint8_t *src, size_t srclen, uint8_t *dst, size_t dstlen);
int fvp_inflate_raw(const uint8_t *src, size_t srclen, uint8_t *dst, size_t dstlen);

/* Human-readable description of an error code ("ok" for FVP_INFLATE_OK). */
const char *fvp_inflate_strerror(int code);

/* Adler-32 (RFC 1950).  Exposed so tools can cross-check the wrapper. */
uint32_t fvp_adler32(uint32_t adler, const void *data, size_t len);

#endif /* FVP_INFLATE_H */