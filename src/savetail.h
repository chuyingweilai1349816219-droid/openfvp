/* FVP engine -- 存档槽尾的"128 槽动态字符串表"（只读解析）。
 *
 * 结构（`docs/save-format.md` §2.4.5 / §2.5.1，ref/exe-save-serialize.json）：
 *     u32 线程下标 [engine+0x6A66D0]    6 份真实存档都是 0x20（= 当时没有活跃脚本线程）
 *     u32 条数     [engine+0x6A49E8]    非空字符串的条数（0..8）
 *     逐条 u16 长度（含结尾 NUL）+ 字节，条目之间可能有 ≤8 字节 0 填充
 *     其余 (128 − 条数) 个槽全为空（各 2 字节 0）
 * 表里的字符串就是"当时的图名 / 当前对话行"这类脚本全局字符串。
 *
 * **位置不是固定偏移**：它紧跟在 32 个协程上下文（32 × 0xB1D = 0x163A0）之后，
 * 而这段之前是"每槽全局变量"（运行时的条数 × 8 字节）。6 份真实存档实测
 * 条数 = **1549**（1549 × 8 = 0x3068，`docs/save-format.md` §3.3 的推导：
 * save.bin 写的是数组后半段 1471 条，脚本用到的最大全局下标是 3019，所以
 * 1549 = 3020 − 1471），于是线程下标落在
 *     0x163A0 + 1549×8 = 0x19408
 * 这个 0x19408 只是**样本值**：换版本 / 换条数就会平移。
 * 因此这里的做法是**扫描定位**——在 [0x163A0, 0x163A0+0x4000) 里按 8 字节步进
 * 找唯一候选（线程下标 = 0x20 + 条数 ≤ 8 + 字符串能解析 + 其余空槽全为 0）；
 * 找不到或有多个候选就整段放弃（宁可不显示，也不显示错）。
 *
 * 这是**只读**解析：它不写存档，也不改变"Continue 暂时无法恢复进度"的结论。
 */
#ifndef FVP_SAVETAIL_H
#define FVP_SAVETAIL_H

#include <stddef.h>

#include "encoding.h"

/* 全局变量条数为 0 时“线程下标 u32”所在偏移；字符串本身从 +8 开始。 */
#define FVP_SAVETAIL_POOL_BASE 0x163A0u
#define FVP_SAVETAIL_SCAN_MAX 0x4000u /* 每槽全局变量最多这么多字节 */
#define FVP_SAVETAIL_THREAD_NONE 0x20u /* 线程下标 0x20 = 存档时没有活跃脚本线程 */
#define FVP_SAVETAIL_SLOT_COUNT 128u   /* 动态字符串表的槽数 */
#define FVP_SAVETAIL_MAX_STRINGS 8
#define FVP_SAVETAIL_STR_MAX 160

typedef struct FvpSaveTailInfo {
    int count;                 /* 非空字符串条数（0 = 表是空的，结构仍成立） */
    int graph_index;           /* 哪一条像图名（-1 = 没有） */
    unsigned int thread_index; /* 序列化时的脚本线程下标（样本里恒为 0x20） */
    size_t pool_offset;        /* 线程下标 u32 在槽尾里的偏移（诊断/门禁用） */
    char str[FVP_SAVETAIL_MAX_STRINGS][FVP_SAVETAIL_STR_MAX]; /* UTF-8 */
} FvpSaveTailInfo;

/* 扫描定位动态字符串表头部的线程下标 u32。返回 1 = 找到唯一候选，*off 被写入
 * 该 u32 的偏移；0 = 找不到 / 多个候选。 */
int fvp_savetail_find_pool(const unsigned char *tail, size_t len, size_t *off);

/* 解析槽尾里那张动态字符串表（内部先 find_pool）。
 * 返回 1 = 结构成立（可能一条字符串都没有，此时 count=0、graph_index=-1）。 */
int fvp_savetail_parse(FvpSaveTailInfo *out, const unsigned char *tail,
                       size_t len, FvpEncoding enc);

#endif /* FVP_SAVETAIL_H */