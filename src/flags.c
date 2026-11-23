/* FVP 全局 flag 位图实现。见 flags.h。 */
#include "flags.h"

int fvp_flag_valid(int idx)
{
    return idx >= 0 && idx < FVP_FLAG_COUNT;
}

int fvp_flag_get(const unsigned char *map, int idx)
{
    unsigned int byte;

    if (map == NULL || !fvp_flag_valid(idx)) {
        return 0;
    }
    byte = (unsigned int)idx >> 3;
    return (map[byte] >> ((unsigned int)idx & 7u)) & 1;
}

int fvp_flag_set(unsigned char *map, int idx, int on)
{
    unsigned int byte;
    unsigned char mask;
    unsigned char before;

    if (map == NULL || !fvp_flag_valid(idx)) {
        return 0;
    }
    byte = (unsigned int)idx >> 3;
    mask = (unsigned char)(1u << ((unsigned int)idx & 7u));
    before = map[byte];
    if (on) {
        map[byte] = (unsigned char)(before | mask);
    } else {
        map[byte] = (unsigned char)(before & (unsigned char)~mask);
    }
    return map[byte] != before;
}

int fvp_flag_count(const unsigned char *map)
{
    int i;
    int n = 0;

    if (map == NULL) {
        return 0;
    }
    for (i = 0; i < FVP_FLAG_BYTES; ++i) {
        unsigned int v = map[i];

        while (v != 0u) {
            v &= v - 1u; /* 清掉最低位的 1 */
            n += 1;
        }
    }
    return n;
}