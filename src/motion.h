/* 动画补间（MotionMove / MotionAlpha / MotionMoveS2 …）的核心数学。
 *
 * 这些命令在脚本里的形态（`docs/vm-commands.md` §3）：
 *
 *   MotionMove(primId, x, y, z, ?, timeMs 1..300000, mode 1..5, flag)
 *   MotionAlpha(primId, from?, to?, timeMs, mode?, flag)
 *   MotionMoveS2(primId, scaleX, scaleY, ?, ?, timeMs, mode, flag)
 *
 * 目前实现：时间轴（开始 / 持续 / 结束）+ **mode 1..5 的缓动曲线**
 * （`fvp_motion_ease()`；语义来自静态分析，证据级别见
 * docs/animation-baseline.md §2.3）。
 *
 * 本模块不依赖 gfx / VM：入口是「毫秒时间戳 + 补间表」，出口是「当前值 / 是否还在跑」，
 * 所以可以直接单测，也方便以后接到别的时间源上。
 */
#ifndef FVP_MOTION_H
#define FVP_MOTION_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* 补间种类（对应三条 Motion* 命令）。 */
typedef enum FvpTweenKind {
    FVP_TWEEN_MOVE_XY = 0, /* 位置 x / y */
    FVP_TWEEN_ALPHA = 1,   /* 透明度 */
    FVP_TWEEN_SCALE = 2    /* 缩放 */
} FvpTweenKind;

typedef struct FvpTween {
    int id;          /* 自增句柄，0 表示空槽 */
    int prim_id;     /* 目标图元 */
    int kind;        /* FvpTweenKind */
    double from_x;
    double to_x;
    double from_y;
    double to_y;
    double start_ms;
    double dur_ms;
    int mode;        /* 引擎的 1..5 缓动曲线，见 fvp_motion_ease() */
    int flag;
} FvpTween;

typedef struct FvpMotion {
    FvpTween *e;
    size_t n;
    size_t cap;
    int next_id;
    double now_ms;
} FvpMotion;

void fvp_motion_init(FvpMotion *m);
void fvp_motion_free(FvpMotion *m);

/* 清空补间但保留时间轴（读档 / 切场景时用）。 */
void fvp_motion_clear(FvpMotion *m);

/* 推进时间轴。dt_ms <= 0 时只重算状态、不前进。 */
void fvp_motion_advance(FvpMotion *m, double dt_ms);

/* 加一条补间，返回句柄（>0）；参数非法（dur < 0 / NULL）返回 0。
 * dur_ms == 0 表示瞬间到位。 */
int fvp_motion_add(FvpMotion *m, int prim_id, int kind, double from_x,
                   double from_y, double to_x, double to_y, double dur_ms,
                   int mode, int flag);

/* 缓动曲线：把线性进度 p(0..1) 映射成插值系数（0..1）。
 *
 * `mode` 的语义来自静态分析（handler 0x43ABD0 把 mode 存进补间条目 +4，
 * 推进函数 0x422C10 用 `jmp *0x423154(,%eax,4)` 按 mode-1 分派到 5 个例程）：
 *   1 = 线性、2 = 缓入（二次）、3 = 缓出（二次）、
 *   4 = 缓入-缓出（以 d/2 分段）、5 = 先缓出后缓入（同样分段）
 * mode 0 / 表外值按线性。细节与证据级别见 docs/animation-baseline.md §2.3。 */
double fvp_motion_ease(int mode, double p);

/* 取某条补间的当前值：还在跑或刚好跑完都算有效（跑完时给终值）。
 * 返回 1 = 有这条补间，0 = 没有。 */
int fvp_motion_value(const FvpMotion *m, int id, double *x, double *y);

/* 已经跑完、可以从表里回收的条数（调用 fvp_motion_reap 清掉）。 */
size_t fvp_motion_done_count(const FvpMotion *m);
void fvp_motion_reap(FvpMotion *m);

/* 还在跑的条数。 */
size_t fvp_motion_active_count(const FvpMotion *m);

/* 某个图元（kind < 0 = 任意类型）是否还有没跑完的补间。MotionMoveTest(50) 用它。 */
int fvp_motion_test(const FvpMotion *m, int prim_id, int kind);

/* Motion*Stop(primId)：撤掉某个图元的某一类补间（正在跑的与已跑完的都撤）。
 * kind < 0 表示"所有种类"。返回撤掉的条数。
 * 对应原版程序里的 4 条命令：
 *   MotionAlphaStop(43)  → FVP_TWEEN_ALPHA
 *   MotionMoveS2Stop(55) → FVP_TWEEN_SCALE
 *   MotionMoveZStop(58)  → Z 补间（本引擎没有 Z 补间，永远是空操作）
 *   MotionAnimStop(46)   → 帧动画（同上）
 * 语义与出处见 docs/vm-commands.md §5。 */
int fvp_motion_cancel(FvpMotion *m, int prim_id, int kind);

#ifdef __cplusplus
}
#endif

#endif /* FVP_MOTION_H */