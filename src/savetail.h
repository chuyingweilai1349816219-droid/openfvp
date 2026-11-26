/* FVP engine -- 存档槽尾的"字符串池"（只读解析）。
 *
 * 结构实测（6 个真实槽，`docs/save-format.md` §2.5.1）：
 *     u32 标签 0x20
 *     u32 条数（0..8）
 *     逐条 u16 长度（含结尾 NUL）+ 字节；条目之间可能有 0 填充
 * 池里是"当时的图名 + 当前对话行"。
 *
 * **池的位置不是固定偏移**：它紧跟在序列化器的 E 段（128 槽字符串表）之后
 * （`ref/exe-save-serialize.json` 里第 6..10 段的顺序），而 E 段之前的
 * "每槽全局变量"长度由运行时的条数决定。6 份真实存档实测那条数 = **1471**
 * （1471 × 8 = 0x2DF8 字节），所以标签落在
 *     0x16610 + 1471×8 = 0x19408
 * 但这个 0x19408 只是**样本值**：换版本 / 换条数就会平移。
 * 因此这里的做法是**扫描定位**——在 [0x16610, 0x16610+0x4000) 里按 8 字节步进
 * 找"标签 0x20 + 条数 ≤ 8 + 字符串能解析"的唯一候选；找不到或有多个候选就
 * 整段放弃（宁可不显示，也不显示错）。
 *
 * 这是**只读**解析：它不写存档，也不改变"Continue 暂时无法恢复进度"的结论。
 */
#ifndef FVP_SAVETAIL_H
#define FVP_SAVETAIL_H

#include <stddef.h>

#include "encoding.h"

#define FVP_SAVETAIL_POOL_BASE 0x16610u /* 全局变量条数为 0 时标签的位置 */
#define FVP_SAVETAIL_SCAN_MAX 0x4000u   /* 扫描上限（每槽全局变量最多这么多字节） */
#define FVP_SAVETAIL_TAG 0x20u
#define FVP_SAVETAIL_MAX_STRINGS 8
#define FVP_SAVETAIL_STR_MAX 160

typedef struct FvpSaveTailInfo {
    int count;                 /* 解析出的字符串条数（0 = 没有/解析失败） */
    int graph_index;           /* 哪一条像图名（-1 = 没有） */
    size_t pool_offset;        /* 标签在槽尾里的偏移（诊断/门禁用） */
    char str[FVP_SAVETAIL_MAX_STRINGS][FVP_SAVETAIL_STR_MAX]; /* UTF-8 */
} FvpSaveTailInfo;

/* 扫描定位字符串池标签。返回 1 = 找到唯一候选，*off 被写入标签偏移；
 * 0 = 找不到 / 多个候选。 */
int fvp_savetail_find_pool(const unsigned char *tail, size_t len, size_t *off);

/* 解析槽尾里那段字符串池（内部先 find_pool）。返回 1 = 至少解出一条。 */
int fvp_savetail_parse(FvpSaveTailInfo *out, const unsigned char *tail,
                       size_t len, FvpEncoding enc);

#endif /* FVP_SAVETAIL_H */