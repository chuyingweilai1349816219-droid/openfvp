/* FVP engine -- 标题进场的逐元素时间轴（见 titleanim.h / ref/title-enter.json）。
 *
 * 两个作品的进场**结构相同**（等待 → 装饰层 → logo → 菜单错开 → 版权行），
 * 但进门时刻/位移不同（脚本事实，hcb pc 见 ref/title-enter.json）：
 *
 *   五彩斑斓（0x5933E..0x59650）：
 *     t=1000  装饰层 264（+2000）；t=2000 logo(265) alpha 0→255 / 2000 ms
 *     t=2400  菜单 1501 / 900 ms；+40 ms 一项、各 820 ms；纵向 +23 px → 0
 *     t=2560  版权行 278 / 820 ms；横向 +30 px → 0；总计 4000 ms
 *
 *   星空（0x6FEDE..0x701E2）：
 *     t=1000  装饰层 259/260 与 logo(265/276) alpha 0→255 / 2000 ms（只等一次）
 *     t=1400  菜单 266 / 900 ms；+40 ms 一项、各 820 ms；纵向 +50 px → 0
 *     t=1560  版权行 278 / 820 ms；横向 +76 px → 0；总计 3000 ms
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

typedef enum FvpTitleEnterGame {
    FVP_TITLEENTER_GAME_IROSEKA = 0,
    FVP_TITLEENTER_GAME_HOSHIMEMO = 1
} FvpTitleEnterGame;

typedef struct FvpTitleEnter {
    int elapsed_ms;
    FvpTitleEnterGame game;
} FvpTitleEnter;

/* 游戏名（"iroseka"/"hoshimemo"，来自标题 profile）→ 枚举；未知/NULL 用五彩斑斓。 */
FvpTitleEnterGame fvp_titleenter_game_from_name(const char *name);
const char *fvp_titleenter_game_name(FvpTitleEnterGame game);

void fvp_titleenter_init(FvpTitleEnter *s, FvpTitleEnterGame game);
void fvp_titleenter_set_elapsed(FvpTitleEnter *s, int ms);
int fvp_titleenter_elapsed(const FvpTitleEnter *s);
int fvp_titleenter_done(const FvpTitleEnter *s);
int fvp_titleenter_total_ms(const FvpTitleEnter *s);

/* 部件 part 的 alpha（0..255）；非法 part 返回 0。 */
int fvp_titleenter_alpha(const FvpTitleEnter *s, int part);
/* 部件 part 的像素偏移（dx,dy）；非法 part 置 0。 */
void fvp_titleenter_offset(const FvpTitleEnter *s, int part, int *dx, int *dy);

#ifdef __cplusplus
}
#endif

#endif /* FVP_TITLEENTER_H */