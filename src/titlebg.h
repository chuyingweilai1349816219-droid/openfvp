/* FVP engine -- 标题背景的动画状态（缩放/旋转），参数来自脚本。
 *
 * 出处：`ref/title-bg-motion.json`（`scripts/extract-title-bg-motion.py`）+ 门禁
 * `tests/verify_title_bg_motion.py`。当前只实现"规则最干净"的那一个：
 *
 *   星空（hcb 0x6FD87）：背景槽 258 的 1920x1080 图，`MotionMoveS2(2000→1000,
 *   40000 ms, mode 3)`，没有 PrimSetOP（轴心默认 (0,0)，即左上锚），也没有
 *   MotionMoveR（不旋转）。
 *
 * 五彩斑斓（4 层 258..261、1300..4000→800..900、15/25/55/125°→0、25000 ms、
 * OP=(1200,675)）**还没实现**：它的源图是超尺寸（2400x1350 等），"OP 轴心缩放时
 * 可见哪一块"还需要再对一遍 exe 绘制路径（见 docs/title-menu.md §22/§23）。
 */
#ifndef FVP_TITLEBG_H
#define FVP_TITLEBG_H

#ifdef __cplusplus
extern "C" {
#endif

typedef enum FvpTitleBgGame {
    FVP_TITLEBG_GAME_IROSEKA = 0,
    FVP_TITLEBG_GAME_HOSHIMEMO = 1
} FvpTitleBgGame;

typedef struct FvpTitleBgState {
    int animated;    /* 0 = 本作还没实现背景动画（按 1:1 静态裁剪画） */
    int scale_x1000; /* per-mille */
    int scale_y1000;
    int angle_x10;   /* 0.1° */
    int done;
} FvpTitleBgState;

/* 游戏名（"iroseka"/"hoshimemo"）→ 枚举；未知/NULL 用五彩斑斓。 */
FvpTitleBgGame fvp_titlebg_game_from_name(const char *name);

/* 该作背景动画总时长（ms）；0 = 没有实现动画。 */
int fvp_titlebg_total_ms(FvpTitleBgGame game);

/* 某时刻的状态。elapsed_ms < 0 按 0 处理；超过总时长按终值。 */
void fvp_titlebg_state(FvpTitleBgGame game, int elapsed_ms,
                       FvpTitleBgState *out);

#ifdef __cplusplus
}
#endif

#endif /* FVP_TITLEBG_H */