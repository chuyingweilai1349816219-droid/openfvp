/* save.bin → VM 全局变量桥。见 save_vm.h 的说明。 */
#include "save_vm.h"

#include <string.h>

static unsigned int rd_u32(const unsigned char *p)
{
    return (unsigned int)p[0] | ((unsigned int)p[1] << 8) |
           ((unsigned int)p[2] << 16) | ((unsigned int)p[3] << 24);
}

int fvp_save_globals_apply(const FvpSaveBin *b, FvpValue *globals, int n,
                           FvpSaveGlobalsStats *stats)
{
    FvpSaveGlobalsStats local;
    unsigned int i;
    int written = 0;

    memset(&local, 0, sizeof(local));
    if (b == NULL) {
        if (stats != NULL) {
            *stats = local;
        }
        return 0;
    }

    local.total = (int)b->nglobals;
    for (i = 0; i < b->nglobals; ++i) {
        const unsigned char *rec = b->globals + (size_t)i * 8u;
        unsigned char type = rec[0];

        switch (type) {
        case 2: { /* 已确证的 int */
            int32_t v = (int32_t)rd_u32(rec + 4);

            local.ints += 1;
            if (globals != NULL && (int)i < n) {
                globals[i] = fvp_make_int((int64_t)v);
                written += 1;
            } else if (globals != NULL) {
                local.skipped += 1;
            }
            break;
        }
        case 1:
            local.unknown += 1;
            break;
        default: /* 0 = 未设；其它未知类型也按未设处理并只报数 */
            local.unset += 1;
            break;
        }
    }

    if (stats != NULL) {
        *stats = local;
    }
    return written;
}