/* save.bin ←→ VM 全局变量桥。
 *
 * `save/save.bin` 开头是 1471 条 8 字节记录（`docs/save-format.md` §3.3）：
 *
 *     +0  u8   type   0 = 未设，1 = 未解，2 = int（已确证）
 *     +1..3    pad
 *     +4  u32  value
 *
 * 这里只做**已确证**的方向：把 type==2 的记录写进 VM 的 globals 数组。
 * type==1 的 846 条语义未知（§4「未解」第 5 条），一律跳过并在统计里报数，
 * 免得把脏值灌进脚本变量。
 *
 * 注意：引擎的 type 编号与 VM 的 `FvpType`（INT=0/FLOAT=1/STR=2）是两套不同的
 * 枚举，不要混用——存档里的 2 表示 int，而 VM 里 int 是 0。
 */
#ifndef FVP_SAVE_VM_H
#define FVP_SAVE_VM_H

#include "fvp.h"
#include "save.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct FvpSaveGlobalsStats {
    int total;   /* 文件里的记录条数 */
    int ints;    /* type==2，已写进 VM */
    int unknown; /* type==1，语义未解，跳过 */
    int unset;   /* type==0，未设，跳过 */
    int skipped; /* 下标超出 VM globals 容量，跳过 */
} FvpSaveGlobalsStats;

/* 把 b 的全局变量写进 globals[0..n-1]。返回实际写入条数（= stats.ints）。
 * globals 为 NULL 或 n <= 0 时只做统计、不写入，可用于先看文件里有什么。 */
int fvp_save_globals_apply(const FvpSaveBin *b, FvpValue *globals, int n,
                           FvpSaveGlobalsStats *stats);

/* 把 save.bin 的 256 字节 flag 位图拷进 flags（容量 cap 字节）。
 * 返回拷贝的字节数（0 表示参数不对）。置位数通过 fvp_flag_count() 另取。 */
int fvp_save_flags_apply(const FvpSaveBin *b, unsigned char *flags, size_t cap);

#ifdef __cplusplus
}
#endif

#endif /* FVP_SAVE_VM_H */