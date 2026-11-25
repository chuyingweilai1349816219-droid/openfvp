/* 开场 FAVORITE logo 序列的脚本常量 —— 全部从 HCB 里读出来的事实。
 *
 * 出处：`ref/opening-scene.json`（`scripts/extract-opening-scene.py` 静态提取，
 * 五彩斑斓 hcb 0x58096..0x584CB、星空 hcb 0x6EFF1..0x6F415）。门禁
 * `tests/verify_opening_anim.py` 会重新提取并断言这里的数字与脚本一致。
 *
 * 两个作品**用的是同一段开场脚本**（槽位/时长/坐标完全一样），只有两处数据不同：
 *   1. 逻辑画布尺寸：五彩斑斓 1024×640，星空 800×600（脚本里的坐标是逻辑像素，
 *      helper 里用 float32 常量 1.875/1.6875 与 2.4/1.8 换成 1920×1080）；
 *   2. `logo_favo_view_p1..p3` 的 OP（缩放/旋转轴心）：五彩斑斓 (960,628)，
 *      星空 (960,610) —— 这两组值分别来自直接 `PrimSetOP` 与 helper 里的逻辑坐标。
 *
 * 脚本时间轴（无输入时；按 hcb 加载/等待顺序）：
 *
 *   t=0      白底(250) 立即 255；其余五层 alpha=0，开始淡入
 *   t=0..1200  logo_favo(252)  alpha 0→255、缩放 0.75→1.0
 *   t=0..1400  logo_favo_view(254) alpha 0→255、缩放 0.75→1.0、y 从 +84 滑到 0
 *              p1/p2/p3(255/256/257) alpha 0→255、缩放 10×→1×、旋转 360°→0°、
 *              从 (±375,337)/(0,-337) 收拢到原位（星空对应 ±480,360）
 *   t=1200     `MotionAlphaTest(252)`（hcb 0x5834A）等 252 淡入结束 → 开始淡出
 *   t=1200..3700  logo 层 alpha→0（2500 ms）
 *   t=1200..4700  白底(250) alpha→0（3500 ms）
 *   t=3700..3800  全部层 alpha→0 快速收尾（100 ms，hcb 0x58479..0x584C7）
 *   t=3800    函数返回，标题场景接着做它自己的逐元素进场（4000 ms，见 titleenter.h）
 *
 * 所以开场总长 **3800 ms**。旧实现（白底 300 / 淡入 1200 / 停留 2100 / 淡出 1200 /
 * 白场 600 = 5400 ms）是按抓帧时间线拟合的，已作废。
 */
#ifndef FVP_OPENINGANIM_H
#define FVP_OPENINGANIM_H

/* ---- 层与槽位（GraphLoad 的槽位号 = 图元号，hcb 0x580AA..0x581C4）---- */
#define FVP_OPEN_LAYER_COUNT 6
#define FVP_OPEN_SLOT_BG 250     /* logo_bg            1920x1080 纯白 */
#define FVP_OPEN_SLOT_FAVO 252   /* logo_favo          FAVORITE 字标 */
#define FVP_OPEN_SLOT_VIEW 254   /* logo_favo_view     VIEW 字标 */
#define FVP_OPEN_SLOT_P1 255     /* logo_favo_view_p1  46x36 碎片 */
#define FVP_OPEN_SLOT_P2 256     /* logo_favo_view_p2  46x37 碎片 */
#define FVP_OPEN_SLOT_P3 257     /* logo_favo_view_p3  46x36 碎片 */

/* ---- alpha：MotionAlpha(prim, 0, 255, ms)（hcb 0x5830E / 0x58344 / 0x58217…）---- */
#define FVP_OPEN_ALPHA_MS_FAVO 1200    /* 252 */
#define FVP_OPEN_ALPHA_MS_VIEW 1400    /* 254 与 255/256/257 */
/* 淡出：MotionAlpha(prim, keep, 0, ms) —— 250 是 3500，其余层 2500 */
#define FVP_OPEN_OUT_START_MS 1200
#define FVP_OPEN_BG_OUT_MS 3500
#define FVP_OPEN_LOGO_OUT_MS 2500
/* 收尾：MotionAlpha(全部层, keep, 0, 100)（hcb 0x58479..0x584C7） */
#define FVP_OPEN_CUT_START_MS 3700
#define FVP_OPEN_CUT_MS 100
#define FVP_OPEN_TOTAL_MS 3800

/* ---- 缩放：MotionMoveZ(252/254, 750, 1000, ms)（hcb 0x582FE / 0x58320）---- */
#define FVP_OPEN_SCALE_FROM_FAVO 750
#define FVP_OPEN_SCALE_TO_FAVO 1000
#define FVP_OPEN_SCALE_MS_FAVO 1200
#define FVP_OPEN_SCALE_MS_VIEW 1400

/* ---- p1..p3：MotionMoveS2(prim, 10000, 1000, 10000, 1000, 1400, 3)
 *      + MotionMoveR(prim, 3600, 0, 1400, 3)
 *      + MotionMove(prim, 逻辑坐标…, 1400, 3)（hcb 0x5822B..0x582EC）---- */
#define FVP_OPEN_VIEW_SCALE_FROM 10000
#define FVP_OPEN_VIEW_SCALE_TO 1000
#define FVP_OPEN_VIEW_ANGLE_FROM 3600 /* 0.1° 单位：360° */
#define FVP_OPEN_VIEW_ANGLE_TO 0
#define FVP_OPEN_VIEW_MS 1400

/* ---- 位移（脚本里的逻辑像素；HD 像素 = 逻辑 × 1920/逻辑宽、×1080/逻辑高）---- */
#define FVP_OPEN_MOVE_VIEW_LOGICAL 200 /* p1 (-200,+200) / p2 (+200,+200) / p3 (0,-200) */
#define FVP_OPEN_MOVE_VIEW_LOGICAL_Y 200
#define FVP_OPEN_MOVE_VIEW_MS 1400
#define FVP_OPEN_MOVE_LOGICAL_Y_254 50 /* logo_favo_view：y 从 +50 滑到 0 */

/* 四条 Motion* 的缓动 mode 都是 3（缓出，见 docs/animation-baseline.md §2.3） */
#define FVP_OPEN_EASE_MODE 3

/* ---- OP（缩放/旋转轴心，HD 像素）：由脚本的 PrimSetOP/helper 读出 ---- */
#define FVP_OPEN_OP_FAVO_X 960
#define FVP_OPEN_OP_FAVO_Y 540
#define FVP_OPEN_OP_VIEW_X 960
#define FVP_OPEN_OP_VIEW_Y_IROSEKA 628
#define FVP_OPEN_OP_VIEW_Y_HOSHIMEMO 610

/* ---- 每部作品的逻辑画布（脚本坐标 → HD 像素的换算依据）---- */
#define FVP_OPEN_LOGICAL_W_IROSEKA 1024
#define FVP_OPEN_LOGICAL_H_IROSEKA 640
#define FVP_OPEN_LOGICAL_W_HOSHIMEMO 800
#define FVP_OPEN_LOGICAL_H_HOSHIMEMO 600
#define FVP_OPEN_HD_W 1920
#define FVP_OPEN_HD_H 1080

#endif /* FVP_OPENINGANIM_H */