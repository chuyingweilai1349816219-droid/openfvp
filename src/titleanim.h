/* 标题画面进场动画的脚本参数 —— 从 iroseka.hcb 里读出来的常量。
 *
 * 出处：`ref/title-scene.json`（`scripts/extract-title-scene.py` 静态提取）+ 对
 * hcb 加载顺序/控制流的复核（pc 见注释）。门禁 `tests/verify_title_anim.py` 会
 * 重新读基线并断言这里的数字与脚本一致。
 *
 * 槽位归属（hcb 0x580AA 起，按 GraphLoad 顺序）：
 *
 *   250 `logo_bg`             ┐
 *   252 `logo_favo`           │ 开场「FAVORITE」logo —— 参数在 src/openinganim.h，
 *   254 `logo_favo_view`      │ 本文件不再保存（见 docs/opening.md）
 *   255..257 `logo_favo_view_p1..p3` ┘
 *
 *   258..261 `title_bg1_A..D` ┐ 标题背景四层（加载于 0x5853F..0x5858A）
 *   265 `title_logo1`         │ 标题画面 logo（加载于 0x58B72，local 2）
 *   278 `title_copyright`     │ 版权行（加载于 0x58BB3，local 3）
 *   1501.. / 1508.. / …        │ 菜单项（加载于 0x58BC8 起）
 *   277 `title_trial`         ┘ 体验版标记（0x58B9E）
 *
 * 标题进场（hcb 0x5933E..0x594C0）：
 *   * 图元 262/263/264/265 与菜单项都是 `MotionAlpha(prim, from, 255, <栈局部>)`：
 *     263/264/265 的时长是 local 5，262 是 local 7；菜单项也是 local 5
 *     （0x59349 / 0x59361 / 0x59370 / 0x59394 / 0x59454…）。
 *   * 背景四层同时做 `MotionMoveS2(1300→900 / 1600→800 / 2500→800 / 4000→800,
 *     local7 = 25000 ms)`（hcb 0x59143..0x59188）—— **25 秒的极慢缩放**，
 *     另外还有 `MotionMoveR(258..261, 150/250/550/1250 → 0, 25000 ms)` 的慢旋转
 *     （0x5918B..0x591D3）。这两条**尚未实现**（需要标题背景改成按图元缩放/旋转绘制）。
 *
 * ⚠️ 已知待复核（下一轮）：标题对象淡入的**时长**也是栈局部变量，而脚本会按
 * `global236` 分支重设：第一次进场走 0x592C7 分支 → local5 = 2000、local7 = 3000；
 * 菜单项在 0x593B7 分支 → local5 = 800。本文件现在冻结的 500 ms 来自
 * 0x59120 的 local5，那一处其实只被背景动画用（同一函数的标题淡入在此之前已被
 * 0x592C7 覆盖），所以 **500 ms 是待复核值**；下一轮按"逐元素时长 + 0x59335 的
 * 1000 ms 间隔 + 菜单 800 ms"重做标题进场。见 docs/opening.md §5。
 */
#ifndef FVP_TITLEANIM_H
#define FVP_TITLEANIM_H

/* ---- 标题画面进场（当前实现：单一 alpha 淡入，时长待复核）---- */
/* MotionAlpha(262/263/264/265 与菜单项, 0, 255, <local5/local7>)
 * （hcb 0x59349..0x595C1）。500 ms 见文件头的"已知待复核"。 */
#define FVP_TITLEANIM_FADE_FROM 0
#define FVP_TITLEANIM_FADE_TO 255
#define FVP_TITLEANIM_FADE_MS 500

/* MotionMoveS2(258..261, 1300/1600/2500/4000 → 900/800/800/800, 25000, 3)
 * （hcb 0x59143..0x59188）。**尚未实现**：需要先把标题背景改成"按图元缩放绘制"，
 * 而且 25 s 的慢缩放在 3 s 的门禁截图里几乎不可见。 */
#define FVP_TITLEANIM_BG_SCALE_FROM 1300
#define FVP_TITLEANIM_BG_SCALE_TO 900
#define FVP_TITLEANIM_BG_MS 25000

#endif /* FVP_TITLEANIM_H */