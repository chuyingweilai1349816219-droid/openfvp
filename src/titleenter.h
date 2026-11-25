/* FVP engine -- 标题进场的逐元素时间轴（见 titleanim.h / ref/title-enter.json）。
 *
 * 脚本（iroseka hcb 0x5933E..0x5961A）不是一个统一淡入，而是**逐元素错开**：
 *
 *   t=2000  logo(265)      alpha 0→255 / 2000 ms
 *   t=2400  菜单 1501       alpha 0→255 / 900 ms，纵向 +23 px → 0
 *   t=2440  菜单 1508       alpha 0→255 / 820 ms，纵向 +23 px → 0
 *   t=2480  菜单 1515       同上
 *   t=2520  菜单 1522       同上（脚本另有 global3 分支的 -81 px 特例，未实现）
 *   t=2560  菜单 1529       同上
 *   t=2560  版权行(278)     alpha 0→255 / 820 ms，横向 +30 px → 0
 *   t=4000  logo 结束 = 整段结束
 *
 * 本模块只算"毫秒 → 每个部件 alpha / 像素偏移"，不碰 gfx，方便单测；
 * 外壳默认把 alpha 全部当 255、偏移 0（绘制结果与静态画面逐字节相同）。
 */
#ifndef FVP_TITLEENTER_H
#define FVP_TITLEENTER_H

#include "titleanim.h"

#ifdef __cplusplus
extern "C" {
#endif

/* 部件编号：0 = logo，1 = 版权行，2..2+N-1 = 菜单项 */
#define FVP_TITLEENTER_PART_LOGO 0
#define FVP_TITLEENTER_PART_COPY 1
#define FVP_TITLEENTER_PART_MENU0 2
#define FVP_TITLEENTER_PART_COUNT (2 + FVP_TITLEENTER_MENU_COUNT)

typedef struct FvpTitleEnter {
    int elapsed_ms;
} FvpTitleEnter;

void fvp_titleenter_init(FvpTitleEnter *s);
void fvp_titleenter_set_elapsed(FvpTitleEnter *s, int ms);
int fvp_titleenter_elapsed(const FvpTitleEnter *s);
int fvp_titleenter_done(const FvpTitleEnter *s);

/* 部件 part 的 alpha（0..255）；非法 part 返回 0。 */
int fvp_titleenter_alpha(const FvpTitleEnter *s, int part);
/* 部件 part 的像素偏移（dx,dy）；非法 part 置 0。 */
void fvp_titleenter_offset(const FvpTitleEnter *s, int part, int *dx, int *dy);

#ifdef __cplusplus
}
#endif

#endif /* FVP_TITLEENTER_H */