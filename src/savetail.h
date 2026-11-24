/* FVP engine -- 存档槽尾部的"字符串池"（只读解析）。
 *
 * 结论来自对 6 个真实槽的实测（docs/save-format.md §2.5.1）：
 * 槽尾 +0x19408 起是 u32 标签 0x20、u32 个数、随后是若干 u16 长度前缀的
 * **cp932 字符串**（当前对话行；有的槽里还有当时的背景图名，例如
 * "BG063_000b"）。条目之间夹着 0 填充，实测存在，但精确对齐规则还没钉死，
 * 所以解析器按"跳过 0 填充直到下一个合理条目"来做，并在失败时整段放弃
 * （宁可不显示，也不显示错）。
 *
 * 这是**只读**解析：它不写存档，也不改变"Continue 暂时无法恢复进度"的结论。
 */
#ifndef FVP_SAVETAIL_H
#define FVP_SAVETAIL_H

#include <stddef.h>

#include "encoding.h"

#define FVP_SAVETAIL_POOL_OFFSET 0x19408u
#define FVP_SAVETAIL_TAG 0x20u
#define FVP_SAVETAIL_MAX_STRINGS 8
#define FVP_SAVETAIL_STR_MAX 160

typedef struct FvpSaveTailInfo {
    int count;                 /* 解析出的字符串条数（0 = 没有/解析失败） */
    int graph_index;           /* 哪一条像图名（-1 = 没有） */
    char str[FVP_SAVETAIL_MAX_STRINGS][FVP_SAVETAIL_STR_MAX]; /* UTF-8 */
} FvpSaveTailInfo;

/* 解析槽尾里那段字符串池。返回 1 = 至少解出一条，0 = 不像该结构。 */
int fvp_savetail_parse(FvpSaveTailInfo *out, const unsigned char *tail,
                       size_t len, FvpEncoding enc);

#endif /* FVP_SAVETAIL_H */