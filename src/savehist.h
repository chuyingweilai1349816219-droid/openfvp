/* FVP engine -- 存档槽尾「台词历史环」的只读解析与回灌。
 *
 * 依据（全部来自静态分析，基线 `ref/exe-history.json`，门禁
 * `tests/verify_exe_history.py`）：
 *
 *   槽尾第 20 段（序列化器 `0x438120`，紧跟 0x400 调色板之后）是历史容器：
 *       u32 count
 *       u32 pos
 *       100 × { u32 strlen(text0), text0?, u32 strlen(text1), text1?, i32 value }
 *   长度 0 时不写字节；`value = -1` 表示"未设置"。读档侧 `0x43F25A` 起先读
 *   count/pos 写回 `engine+0x6A49C0/+0x6A49C4`，再 100 次调 `0x437EE0` 逐条读
 *   回（先 free 旧串、再 malloc len+1）。容器构造时 count = 1、pos = 0。
 *
 * **为什么是"扫描定位"而不是"固定偏移"**：写档段顺序已经确证（`docs/save-format.md`
 * §2.4.5），但第 11 段起各子序列化器的字节数还没逆出来，所以 `tail+0x16608`
 * 之后的绝对偏移算不出来。这里用**结构判据**定位：前面 8 字节像 (count,pos)、
 * 100 条能解析完、第 count 条之后必须是空记录 `{0,0,-1}`、文本可解码、且
 * （父串非空时）最新一条的 text1 == 槽头第 3 条字符串。判据在 6 份真实存档上
 * 唯一命中（s481 `tail+0x20622`、s905 `tail+0x20633`、s485 `+0x328ce` 等）。
 * 候选不唯一时**整段放弃**（宁可不恢复，也不恢复错）。
 *
 * 这是**只读**解析 + 灌进 `FvpHistory`，不写存档。
 */
#ifndef FVP_SAVEHIST_H
#define FVP_SAVEHIST_H

#include <stddef.h>
#include <stdint.h>

#include "encoding.h"
#include "history.h"

#ifdef __cplusplus
extern "C" {
#endif

#define FVP_SAVEHIST_SLOTS FVP_HISTORY_SLOTS     /* 100（0x43A602 的 0x64） */
#define FVP_SAVEHIST_TEXT_MAX 4096               /* 单字段 UTF-8 上限 */
#define FVP_SAVEHIST_FIELD_MAX 0x800u            /* 单字段序列化长度上限 */
#define FVP_SAVEHIST_SCAN_MIN 0x1000u            /* 槽尾前 4 KB 不必扫 */
#define FVP_SAVEHIST_SCAN_MARGIN 1400u           /* 100 条空记录至少这么多字节 */

typedef struct FvpSaveHistRec {
    char *text0;   /* UTF-8（malloc；NULL = 未设置） */
    char *text1;
    int32_t value; /* -1 = 未设置 */
} FvpSaveHistRec;

typedef struct FvpSaveHist {
    unsigned int count;
    unsigned int pos;
    size_t offset; /* 环在 tail 内的偏移（诊断用；原版读档不需要它） */
    FvpSaveHistRec rec[FVP_SAVEHIST_SLOTS];
} FvpSaveHist;

/* 清零（不释放，用于栈上声明后立刻调用）。 */
void fvp_savehist_init(FvpSaveHist *h);

/* 释放所有字符串并清零（可重复调用）。 */
void fvp_savehist_free(FvpSaveHist *h);

/* 在槽尾里定位历史环并解析。
 *
 *   tail/len     槽尾字节（`FvpSaveSlot.tail`）
 *   cur_line     槽头第 3 条字符串的**原始字节**（可以是 NULL/0 长度：
 *                系统存档里它常常是空串，此时不做"最新一条 == 当前台词"校验）
 *   enc          这些字符串的编码（汉化版是 GBK，原版是 CP932）
 *
 * 返回 1 = 找到唯一候选并解析成功；0 = 没找到 / 多个候选 / 结构不合法。 */
int fvp_savehist_locate(FvpSaveHist *h, const unsigned char *tail, size_t len,
                        const unsigned char *cur_line, size_t cur_len,
                        FvpEncoding enc);

/* 把解析结果灌进 VM 的历史容器（对应 exe 读档路径 0x43F25A 起的回填）。
 * target 里原有的字符串会被释放。返回 1 = 成功，0 = 参数非法。 */
int fvp_savehist_apply(const FvpSaveHist *h, FvpHistory *target);

#ifdef __cplusplus
}
#endif

#endif /* FVP_SAVEHIST_H */