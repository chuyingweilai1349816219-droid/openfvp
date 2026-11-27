/* FVP VM in C -- hzc1 / NVSG image decoder.  See hzc1.h and
 * fvp/docs/hzc1-format.md. */
#include "hzc1.h"

#include <stdlib.h>
#include <string.h>

#include "inflate.h"

static uint16_t rd16(const uint8_t *p)
{
    return (uint16_t)((unsigned)p[0] | ((unsigned)p[1] << 8));
}

static uint32_t rd32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) |
           ((uint32_t)p[3] << 24);
}

static uint32_t fmt_bpp(uint16_t fmt)
{
    switch (fmt) {
    case 0:
        return 3;
    case 1:
        return 4;
    case 2:
        return 4;
    case 3:
        return 1;
    case 4:
        return 1;
    default:
        return 0;
    }
}

const char *hzc1_format_name(int fmt)
{
    switch (fmt) {
    case 0:
        return "BGR24";
    case 1:
        return "BGRA32";
    case 2:
        return "BGRA32-multiframe";
    case 3:
        return "Gray8";
    case 4:
        return "1bit-mask";
    default:
        return "?";
    }
}

const char *hzc1_strerror(int code)
{
    switch (code) {
    case HZC1_OK:
        return "ok";
    case HZC1_ERR_ARG:
        return "invalid argument";
    case HZC1_ERR_TRUNC:
        return "file shorter than the 44-byte header";
    case HZC1_ERR_MAGIC:
        return "bad hzc1/NVSG magic";
    case HZC1_ERR_FORMAT:
        return "unsupported pixel format / zero dimension / too many frames";
    case HZC1_ERR_SIZE:
        return "uncompressed size does not match w*h*bpp*frames";
    case HZC1_ERR_INFLATE:
        return "zlib decompression failed";
    case HZC1_ERR_INDEX:
        return "frame index out of range";
    case HZC1_ERR_MEMORY:
        return "out of memory";
    default:
        return "unknown error";
    }
}

int hzc1_parse(const uint8_t *buf, size_t len, Hzc1Info *info)
{
    uint64_t raw_len;

    if (buf == NULL || info == NULL) {
        return HZC1_ERR_ARG;
    }
    memset(info, 0, sizeof *info);
    if (len < FVP_HZC1_HEADER_SIZE) {
        return HZC1_ERR_TRUNC;
    }
    if (memcmp(buf, "hzc1", 4) != 0 || memcmp(buf + 0x0C, "NVSG", 4) != 0) {
        return HZC1_ERR_MAGIC;
    }
    info->uncompressed_size = rd32(buf + 0x04);
    info->unknown_0x10 = rd16(buf + 0x10);
    info->fmt = rd16(buf + 0x12);
    info->width = rd16(buf + 0x14);
    info->height = rd16(buf + 0x16);
    info->x = rd16(buf + 0x18);
    info->y = rd16(buf + 0x1A);
    info->frame_count_field = rd32(buf + 0x20);

    info->bpp = fmt_bpp(info->fmt);
    if (info->bpp == 0) {
        return HZC1_ERR_FORMAT;
    }
    if (info->width == 0 || info->height == 0) {
        return HZC1_ERR_FORMAT;
    }
    if (info->fmt == 2 && info->frame_count_field != 0) {
        info->frames = info->frame_count_field;
    } else {
        info->frames = 1;
    }
    if (info->frames == 0 || info->frames > FVP_HZC1_MAX_FRAMES) {
        return HZC1_ERR_FORMAT;
    }
    raw_len = (uint64_t)info->width * info->height * info->bpp * info->frames;
    if (raw_len > (uint64_t)SIZE_MAX || raw_len > 0x7FFFFFFFu) {
        return HZC1_ERR_SIZE; /* > 2 GiB: not representable in this API */
    }
    if ((uint32_t)raw_len != info->uncompressed_size) {
        return HZC1_ERR_SIZE;
    }
    info->raw_len = (size_t)raw_len;
    info->payload = buf + FVP_HZC1_HEADER_SIZE;
    info->payload_len = len - FVP_HZC1_HEADER_SIZE;
    return HZC1_OK;
}

/* Convert one already-inflated frame to RGBA8888. */
static void convert_frame(const Hzc1Info *info, const uint8_t *raw,
                          uint32_t index, uint8_t *out)
{
    size_t pixels = (size_t)info->width * info->height;
    size_t i;
    const uint8_t *src =
        raw + (size_t)index * (size_t)info->height * info->width * info->bpp;

    switch (info->fmt) {
    case 0: /* BGR24 */
        for (i = 0; i < pixels; i++) {
            out[0] = src[2];
            out[1] = src[1];
            out[2] = src[0];
            out[3] = 255;
            src += 3;
            out += 4;
        }
        break;
    case 1: /* BGRA32 */
    case 2: /* BGRA32, frames stacked vertically */
        for (i = 0; i < pixels; i++) {
            out[0] = src[2];
            out[1] = src[1];
            out[2] = src[0];
            out[3] = src[3];
            src += 4;
            out += 4;
        }
        break;
    case 3: /* Gray8 */
        for (i = 0; i < pixels; i++) {
            uint8_t v = *src++;
            out[0] = v;
            out[1] = v;
            out[2] = v;
            out[3] = 255;
            out += 4;
        }
        break;
    case 4: /* 1-bit mask stored as one byte per pixel（0 = 无、1 = 字形） */
        for (i = 0; i < pixels; i++) {
            /* 掩码不是"灰阶图"：值 0 是**没有 ink**，值 1 是字形。
             * 早先按 (m*255) 压成灰阶并写 alpha=255，于是整块 156×156 的
             * 黑底被当成不透明贴到画面上（用户报的"正文出现黑底水滴/汗/怒"）。
             * 现在 0 → alpha 0（透），1 → 不透明白（字形）。12 个 fmt-4 素材
             * 全是 gaiji（`extract/graph/0705..0716_gaiji_*`），没有别的用途。 */
            uint8_t ink = *src++;

            out[0] = ink ? 255 : 0;
            out[1] = ink ? 255 : 0;
            out[2] = ink ? 255 : 0;
            out[3] = ink ? 255 : 0;
            out += 4;
        }
        break;
    default:
        break; /* hzc1_parse rejects unknown formats */
    }
}

/* Inflate the whole pixel block; caller frees. */
static int inflate_pixels(const Hzc1Info *info, uint8_t **raw_out)
{
    uint8_t *raw;
    int rc;

    raw = (uint8_t *)malloc(info->raw_len != 0 ? info->raw_len : 1u);
    if (raw == NULL) {
        return HZC1_ERR_MEMORY;
    }
    rc = fvp_inflate(info->payload, info->payload_len, raw, info->raw_len);
    if (rc < 0) {
        free(raw);
        return HZC1_ERR_INFLATE;
    }
    if ((size_t)rc != info->raw_len) {
        free(raw);
        return HZC1_ERR_SIZE;
    }
    *raw_out = raw;
    return HZC1_OK;
}

int hzc1_decode_frame(const Hzc1Info *info, uint32_t index, uint8_t *rgba_out)
{
    uint8_t *raw = NULL;
    int rc;

    if (info == NULL || rgba_out == NULL) {
        return HZC1_ERR_ARG;
    }
    if (index >= info->frames) {
        return HZC1_ERR_INDEX;
    }
    rc = inflate_pixels(info, &raw);
    if (rc != HZC1_OK) {
        return rc;
    }
    convert_frame(info, raw, index, rgba_out);
    free(raw);
    return HZC1_OK;
}

int hzc1_decode_all(const Hzc1Info *info, uint8_t *rgba_out)
{
    uint8_t *raw = NULL;
    uint32_t i;
    int rc;

    if (info == NULL || rgba_out == NULL) {
        return HZC1_ERR_ARG;
    }
    rc = inflate_pixels(info, &raw);
    if (rc != HZC1_OK) {
        return rc;
    }
    for (i = 0; i < info->frames; i++) {
        convert_frame(info, raw, i,
                      rgba_out + (size_t)i * info->width * info->height * 4u);
    }
    free(raw);
    return HZC1_OK;
}