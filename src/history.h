/* 台词历史记录（回想 / backlog）：HistorySet(26) / HistoryGet(25) 的存储层。
 *
 * 语义逐条来自静态分析，不是猜的（iroseka_HD.exe，2026-11 重实现）：
 *
 *   引擎对象在 `engine + 0x6a49c0`，布局三个 dword：
 *     +0x00  u32  count   已记录条数，上限 100
 *     +0x04  u32  pos     当前写入槽位，0..99 循环
 *     +0x08  ptr  base    100 条 × 12 字节的记录数组
 *   每条记录 12 字节：
 *     +0x00  char* text0  HistorySet(0, str) 写入 → **说话人名字**（实测
 *                           例："　　 悠馬　　 " / "　女の子　"）
 *     +0x04  char* text1  HistorySet(1, str) 写入 → **台词正文**
 *     +0x08  i32  value   HistorySet(2, int) 写入 → **脚本给的行标识**
 *                           （实测例：iroseka 2000130、hoshimemo 600010）；
 *                           清空时置 -1
 *
 *   实测（900k 步，两部作品）HistorySet 的四种调用形态：
 *     (0, str) 人名 / (1, str) 台词 / (2, int) 行 id / (0|1, int) 无操作
 *   —— 脚本也会用 `pushtrue` 发 (0, int)、(1, int)，原版按类型不符整条忽略。
 *   **两部作品都从不发 HistorySet(null)**，所以 count 恒 0、pos 恒 0，历史里
 *   永远只有"当前这一条"（槽 0）。这不是移植缺陷：唯一能让 count 增长的路径
 *   就是这条 null 调用，原版程序里没有第二个写 count 的地方（存档反序列化除外，
 *   见 0x43F264）。HistoryGet 在两部作品里动态调用次数都是 0。
 *
 *   HistorySet  handler 0x43A5E0：
 *     arg0.type == 0（null）→ count = min(count+1,100)；pos = (pos+1) % 100；
 *                              base[pos] = {NULL, NULL, -1}      （"开新的一条"）
 *     arg0 = 0 → base[pos].text0 = strdup(arg1)  仅当 arg1 是字符串
 *     arg0 = 1 → base[pos].text1 = strdup(arg1)  仅当 arg1 是字符串
 *     arg0 = 2 → base[pos].value = arg1          仅当 arg1 是整型且 >= 0
 *     其它 kind（<0 / >2）整条命令被忽略。arg0 不是整型也不是 null 也忽略。
 *
 *   HistoryGet  handler 0x43A490：
 *     arg0.type == 0 → 返回 count（整型）
 *     arg0 = 0/1/2, arg1 = idx（整型、0 <= idx < count）否则返回 null
 *     槽位映射：slot = pos - idx；slot < 0 则 +100      （idx 0 = 最近一条）
 *     kind 0/1 → 返回该字段的字符串（字段为 NULL 时返回**空串**，不是 null，
 *                见 0x43A518–0x43A540：先建空字符串句柄，取到指针才追加）
 *     kind 2   → 该字段为 -1 时返回 null，否则返回整型
 *
 * 环下标/上限/清空/映射都抽到这里，VM 只负责搬操作数（见 src/syscall.c），
 * 这样能像 timer.c 一样单测。字符串用 malloc 拷贝，fvp_history_free 释放。
 */
#ifndef FVP_HISTORY_H
#define FVP_HISTORY_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define FVP_HISTORY_SLOTS 100    /* 原版程序里的 0x64 */
#define FVP_HISTORY_FIELDS 2     /* text0 / text1 */
#define FVP_HISTORY_TEXT_MAX 4096 /* 单条文本上限（超出截断，原版无上限） */

typedef struct FvpHistoryEntry {
    char *text0;   /* 记录 +0x00 */
    char *text1;   /* 记录 +0x04 */
    int32_t value; /* 记录 +0x08，-1 = 未设置 */
} FvpHistoryEntry;

typedef struct FvpHistory {
    FvpHistoryEntry entry[FVP_HISTORY_SLOTS];
    uint32_t count; /* engine+0x6a49c0 */
    uint32_t pos;   /* engine+0x6a49c4 */
    uint64_t sets;  /* 诊断：HistorySet 调用次数 */
    uint64_t gets;  /* 诊断：HistoryGet 调用次数 */
} FvpHistory;

/* 全部清零：count = pos = 0，所有记录的 value = -1、字符串 = NULL。 */
void fvp_history_init(FvpHistory *h);

/* 释放所有字符串并把结构清零（可重复调用）。 */
void fvp_history_free(FvpHistory *h);

/* HistorySet(null)：开一条新记录。返回 1 成功，0 表示 h == NULL。 */
int fvp_history_begin(FvpHistory *h);

/* HistorySet(0|1, str)：写当前槽的 text0 / text1（整串替换）。
 * kind 只接受 0/1；s == NULL 或 len == 0 时写入空串。返回 1 成功。 */
int fvp_history_set_text(FvpHistory *h, int kind, const char *s, size_t len);

/* HistorySet(2, n)：写当前槽的 value，只接受 n >= 0。返回 1 成功。 */
int fvp_history_set_value(FvpHistory *h, int n);

/* 环下标映射：idx 越界（<0 或 >= count）返回 -1；否则返回 0..99。 */
int fvp_history_slot(const FvpHistory *h, int idx);

/* HistoryGet(0|1, idx)：返回该字段的字符串（未设置时返回空串 ""），
 * 槽位越界返回 NULL。 */
const char *fvp_history_text(const FvpHistory *h, int kind, int idx);

/* HistoryGet(2, idx)：成功写 *out 并返回 1；槽位越界或该值为 -1 返回 0。 */
int fvp_history_value(const FvpHistory *h, int idx, int32_t *out);

/* 非空记录条数（诊断 / 测试用；与 count 不同，后者是"曾经开过多少条"）。 */
int fvp_history_filled(const FvpHistory *h);

#ifdef __cplusplus
}
#endif

#endif /* FVP_HISTORY_H */