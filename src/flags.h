/* FVP 全局 flag 位图（`FlagGet` / `FlagSet`）。
 *
 * 引擎侧位置：`engine+0x6a48c0`，长度 `0x100` = **2048 bit**，索引 0..0x7FF
 * （见 `docs/vm-commands.md` §6-6 与 `docs/save-format.md` §3.5）。
 * `save/save.bin` 里那 256 字节就是这张位图的原始拷贝，所以本模块不依赖 VM，
 * 只操作一段调用者给的字节数组，方便存档层直接搬进搬出。
 */
#ifndef FVP_FLAGS_H
#define FVP_FLAGS_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define FVP_FLAG_COUNT 2048
#define FVP_FLAG_BYTES 256

/* 索引是否落在 0..0x7FF。 */
int fvp_flag_valid(int idx);

/* 读一位；越界返回 0（与引擎 `cmp $0x7ff` 的钳制一致）。 */
int fvp_flag_get(const unsigned char *map, int idx);

/* 置/清一位。返回 1 表示状态确实变了，0 表示越界或值未变。 */
int fvp_flag_set(unsigned char *map, int idx, int on);

/* 置位的 bit 数（日志 / 验收用）。 */
int fvp_flag_count(const unsigned char *map);

#ifdef __cplusplus
}
#endif

#endif /* FVP_FLAGS_H */