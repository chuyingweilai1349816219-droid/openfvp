/* FVP VM in C -- text decoding (CP932 / GBK -> UTF-8). */
#ifndef FVP_ENCODING_H
#define FVP_ENCODING_H

#include "fvp.h"

typedef enum FvpEncoding {
    FVP_ENC_CP932 = 0,
    FVP_ENC_GBK = 1
} FvpEncoding;

/* Accepts the encodings the Python reference understands (cp932, gbk). */
int fvp_encoding_from_name(const char *name, FvpEncoding *out);
const char *fvp_encoding_name(FvpEncoding enc);

/* Decode raw bytes with Python's decode(enc, 'replace') semantics. */
FvpStr *fvp_decode(FvpArena *arena, FvpEncoding enc,
                   const unsigned char *data, size_t len);

/* 反方向：UTF-8 -> CP932 / GBK 原始字节（写存档需要，槽里的字符串是原编码）。
 * 表里没有的码点写成 '?'（与 Python 的 encode(enc, 'replace') 一致）。
 * 返回的 FvpStr 的 len 是字节数，data 后仍带 NUL（arena 约定）。 */
FvpStr *fvp_encode(FvpArena *arena, FvpEncoding enc, const char *utf8,
                   size_t len);

#endif /* FVP_ENCODING_H */