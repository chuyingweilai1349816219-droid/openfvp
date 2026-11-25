/* FVP engine -- 开场 FAVORITE logo 序列的时间轴（见 openinganim.h）。
 *
 * 只算"毫秒 → 每层 alpha/缩放/旋转/位移/轴心"，不碰 gfx，方便单测；
 * 外壳（win32/main_win32.c）按返回的状态逐层画。
 *
 * 实现完全按脚本：淡入是线性的（MotionAlpha 的 handler 0x43A8B0 没有 mode 1..5，
 * 第 5 个实参是 0/1 标志），缩放/旋转/位移是 mode 3 缓出（handler 0x43AF70 /
 * 0x43AE50 / 0x43ABD0）。淡出从 252 淡入结束（1200 ms）开始，由脚本的
 * `MotionAlphaTest(252)` 触发；最后一组 100 ms alpha→0 快速收尾。
 */
#ifndef FVP_OPENSEQ_H
#define FVP_OPENSEQ_H

#include "openinganim.h"

#ifdef __cplusplus
extern "C" {
#endif

/* 脚本开场函数属于哪部作品：决定逻辑画布与 p1..p3 的 OP。 */
typedef enum FvpOpenGame {
    FVP_OPEN_GAME_IROSEKA = 0,
    FVP_OPEN_GAME_HOSHIMEMO = 1
} FvpOpenGame;

/* 相位：0..1200 淡入 / 1200..3700 淡出 / 3700..3800 快速收尾 / 之后结束。 */
typedef enum FvpOpenPhase {
    FVP_OPEN_IN = 0,
    FVP_OPEN_OUT,
    FVP_OPEN_CUT,
    FVP_OPEN_DONE
} FvpOpenPhase;

typedef struct FvpOpenSeq {
    int elapsed_ms;
    FvpOpenGame game;
} FvpOpenSeq;

/* 某一层当前要画成什么样（全是整数字段，便于门禁断言）。 */
typedef struct FvpOpenLayerState {
    int alpha;       /* 0..255 */
    int scale_x1000; /* per-mille，1000 = 原尺寸 */
    int scale_y1000;
    int angle_units; /* 角度单位：1 = 0.05°（7200 = 360°），0..3599 */
    int dx;          /* 相对素材头位置（图头 x/y）的 HD 像素偏移 */
    int dy;
    int op_x;        /* 缩放/旋转轴心（HD 像素） */
    int op_y;
} FvpOpenLayerState;

/* 游戏名（"iroseka"/"hoshimemo"，来自标题 profile）→ 枚举；未知/ NULL 用五彩斑斓。 */
FvpOpenGame fvp_openseq_game_from_name(const char *name);
const char *fvp_openseq_game_name(FvpOpenGame game);

void fvp_openseq_init(FvpOpenSeq *s, FvpOpenGame game);
void fvp_openseq_advance(FvpOpenSeq *s, int ms);
/* 直接设绝对时间（外壳用墙钟推进，避免首帧加载素材造成 dt 跳变）。 */
void fvp_openseq_set_elapsed(FvpOpenSeq *s, int ms);
int fvp_openseq_elapsed(const FvpOpenSeq *s);
FvpOpenPhase fvp_openseq_phase(const FvpOpenSeq *s);
int fvp_openseq_done(const FvpOpenSeq *s);

/* 层表（顺序 = 脚本 GraphLoad 顺序，也是绘制顺序）。 */
int fvp_openseq_layer_count(void);
const char *fvp_openseq_layer_name(int index);
int fvp_openseq_layer_slot(int index);
/* 取第 index 层的当前状态；越界/NULL 返回 0。 */
int fvp_openseq_layer_state(const FvpOpenSeq *s, int index,
                            FvpOpenLayerState *out);
/* 按槽位号查层下标（找不到返回 -1）。 */
int fvp_openseq_layer_index(int slot);

#ifdef __cplusplus
}
#endif

#endif /* FVP_OPENSEQ_H */