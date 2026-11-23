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

#endif /* FVP_ENCODING_H */