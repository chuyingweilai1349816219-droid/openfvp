/* FVP VM in C -- hzc1 / NVSG image decoder.
 *
 * Container (see fvp/docs/hzc1-format.md):
 *
 *   0x00  4  'hzc1'           magic
 *   0x04  u32 uncompressed_size   raw pixel bytes (zlib output length)
 *   0x08  u32 header_size         distance from 0x0C to the pixel data (0x20)
 *   0x0C  4  'NVSG'           magic
 *   0x10  u16 unknown (always 0x0100)
 *   0x12  u16 format: 0 BGR24, 1 BGRA32, 2 BGRA32 stacked frames,
 *                     3 Gray8, 4 1-bit mask (stored as one byte per pixel)
 *   0x14  u16 width
 *   0x16  u16 height          (per frame for format 2)
 *   0x18  u16 x                screen position in the 1920x1080 canvas
 *   0x1A  u16 y
 *   0x1C  u32 zero
 *   0x20  u32 frame_count     (format 2 only)
 *   0x24  u32 zero
 *   0x28  u32 zero
 *   0x2C  ... zlib stream
 *
 * Pixel rows are top-down and channels are stored B,G,R[,A].  Multi-frame
 * files stack the frames vertically: frame i lives at (0, i*height).
 *
 * The engine-internal output format is RGBA8888, top-down.  fmt 0/3/4 have
 * no alpha channel and are expanded with alpha = 255; fmt 3 replicates the
 * gray value to R,G,B; fmt 4 maps the stored 0/1 byte to 0/255 the same way
 * the Python reference does (numpy uint8 multiply, i.e. mod 256).
 */
#ifndef FVP_HZC1_H
#define FVP_HZC1_H

#include <stddef.h>
#include <stdint.h>

#define FVP_HZC1_HEADER_SIZE 0x2Cu
#define FVP_HZC1_MAX_FRAMES 65536u

typedef enum Hzc1Error {
    HZC1_OK = 0,
    HZC1_ERR_ARG = -1,       /* NULL / bad argument                     */
    HZC1_ERR_TRUNC = -2,     /* input shorter than the 44-byte header   */
    HZC1_ERR_MAGIC = -3,     /* missing 'hzc1' or 'NVSG' magic          */
    HZC1_ERR_FORMAT = -4,    /* unknown fmt, zero size, too many frames */
    HZC1_ERR_SIZE = -5,      /* size fields disagree                    */
    HZC1_ERR_INFLATE = -6,   /* zlib stream failed to decompress        */
    HZC1_ERR_INDEX = -7,     /* frame index out of range                */
    HZC1_ERR_MEMORY = -8     /* allocation failure                      */
} Hzc1Error;

typedef struct Hzc1Info {
    uint32_t uncompressed_size; /* header field 0x04                     */
    uint16_t unknown_0x10;      /* always 0x0100 in the shipped data     */
    uint16_t fmt;               /* 0..4                                  */
    uint16_t width;
    uint16_t height;            /* per frame                             */
    uint16_t x;                 /* game-screen position (metadata)       */
    uint16_t y;
    uint32_t frame_count_field; /* raw 0x20 field                        */
    uint32_t frames;            /* effective frame count (>= 1)          */
    uint32_t bpp;               /* bytes per pixel in the file: 1/3/4    */
    size_t raw_len;             /* width*height*bpp*frames bytes         */
    const uint8_t *payload;     /* zlib stream, points into caller buffer*/
    size_t payload_len;
} Hzc1Info;

/* Parse and validate the header.  buf stays owned by the caller and must
 * outlive the Hzc1Info (info->payload points into it).  Returns HZC1_OK. */
int hzc1_parse(const uint8_t *buf, size_t len, Hzc1Info *info);

/* Decode one frame to width*height*4 bytes of RGBA8888 (top-down).  The
 * output buffer must have room for width*height*4 bytes. */
int hzc1_decode_frame(const Hzc1Info *info, uint32_t index, uint8_t *rgba_out);

/* Decode all frames back to back (frames * width*height*4 bytes). */
int hzc1_decode_all(const Hzc1Info *info, uint8_t *rgba_out);

/* "BGR24", "BGRA32", "BGRA32-multiframe", "Gray8", "1bit-mask", "?" */
const char *hzc1_format_name(int fmt);
const char *hzc1_strerror(int code);

#endif /* FVP_HZC1_H */