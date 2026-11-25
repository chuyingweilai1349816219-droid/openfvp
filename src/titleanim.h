/* 标题**进场动画**的脚本参数 —— 全部是从 iroseka.hcb 里读出来的常量，不是猜的。
 *
 * 出处（`ref/title-scene.json` 的 `games.iroseka.layer_motion`，由
 * `scripts/extract-title-scene.py` 静态提取；门禁 `tests/verify_title_anim.py`
 * 会重新读 hcb 并断言这里的数字与脚本一致）：
 *
 *   0x00058217  MotionAlpha (257, 0, 255, 1400)          logo 第 3 块 淡入
 *   0x00058243  MotionMoveS2(257, 10000, 1000, 10000, 1000, 1400, 3)
 *   0x0005826b / 0x00058297   256 同上
 *   0x000582c0 / 0x000582ec   255 同上
 *
 * 语义（`docs/vm-commands.md` §3、`docs/animation-baseline.md` §2.3）：
 *   * MotionAlpha(prim, from, to, dur) —— 透明度从 from 到 to，dur 毫秒；
 *   * MotionMoveS2(prim, sx_from, sx_to, sy_from, sy_to, dur, mode) —— 缩放，
 *     单位 per-mille（1000 = 1.0）；
 *   * mode 3 = 缓出（二次），曲线在 src/motion.c 的 fvp_motion_ease() 里。
 *
 * 所以标题进场的稳态是 **alpha=255、scale=1000(=1.0)** —— 这与第 131/133 轮
 * "原版截图里标题处于动画终点"的结论一致（那次结论现在只是验证）。
 *
 * 明确**没有**实现的（留给后续轮次，不要假装做了）：
 *   * 背景四层 258..261 的 MotionMoveS2(1300→900 等，7 ms) —— 第 131 轮已判定是
 *     "离开标题"时的缩小，不是进场；
 *   * 图元 252/254 的 MotionMoveZ(750→1000) + MotionAlpha(0→255)、
 *     250/253 的淡出链 —— 这些是白/黑遮罩层，与菜单绘制顺序绑在一起，需要先把
 *     标题改成"按图元绘制"才能忠实实现（见 docs/title-menu.md §19）。
 */
#ifndef FVP_TITLEANIM_H
#define FVP_TITLEANIM_H

/* logo 在标题画面里的 graph 槽（main_win32.c 的 TITLE_LOGO_SLOT） */
#define FVP_TITLEANIM_LOGO_SLOT 0x0FF1

/* MotionAlpha(257, 0, 255, 1400) */
#define FVP_TITLEANIM_ALPHA_FROM 0
#define FVP_TITLEANIM_ALPHA_TO 255

/* MotionMoveS2(257, 10000, 1000, 10000, 1000, 1400, 3) */
#define FVP_TITLEANIM_SCALE_FROM 10000
#define FVP_TITLEANIM_SCALE_TO 1000

/* 两条补间的时长与缓动模式 */
#define FVP_TITLEANIM_DUR_MS 1400
#define FVP_TITLEANIM_EASE_MODE 3

#endif /* FVP_TITLEANIM_H */