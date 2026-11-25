/* 标题画面/开场动画的脚本参数 —— 全部从 iroseka.hcb 里读出来的常量。
 *
 * 出处：`ref/title-scene.json`（`scripts/extract-title-scene.py` 静态提取）+ 本轮
 * 对槽位加载顺序的复核（hcb pc 见注释）。门禁 `tests/verify_title_anim.py` 会重新
 * 读基线并断言这里的数字与脚本一致。
 *
 * ⚠️ 2026-11-25 修正：之前把「0→255 / 1400 ms + 缩放 10000→1000」当成**标题 logo**
 * 的进场动画，是错的。按加载顺序复核后，槽位归属是：
 *
 *   250 `logo_bg`             ┐
 *   252 `logo_favo`           │ 开场「FAVORITE」logo（--opening 用的那套）
 *   254 `logo_favo_view`      │ 加载于 hcb 0x580AA..0x581C4
 *   255 `logo_favo_view_p1`   │
 *   256 `logo_favo_view_p2`   │
 *   257 `logo_favo_view_p3`   ┘
 *
 *   258..261 `title_bg1_A..D` ┐ 标题背景四层（加载于 0x5853F..0x5858A）
 *   265 `title_logo1`         │ 标题画面 logo（加载于 0x58B72，local 2）
 *   278 `title_copyright`     │ 版权行（加载于 0x58BB3，local 3）
 *   1501.. / 1508.. / …        │ 菜单项（加载于 0x58BC8 起）
 *   277 `title_trial`         ┘ 体验版标记（0x58B9E）
 *
 * 所以：
 *   * **标题画面进场 = 淡入**：图元 262/263/264/265 与菜单项都是
 *     `MotionAlpha(prim, 0, 255, 500)`（hcb 0x59349 / 0x59361 / 0x59394 / 0x59454…），
 *     即 **alpha 0→255、时长 500 ms**；背景四层同时做
 *     `MotionMoveS2(1300→900 / 1600→800 / 2500→800 / 4000→800, 25000 ms)`（0x59143…）
 *     —— **25 秒的极慢缩放**，肉眼几乎只看到淡入。
 *   * **缩放 10 倍 → 1 倍（1400 ms）是开场 FAVORITE logo 的 `logo_favo_view_p1/p2/p3`**
 *     （hcb 0x58217..0x582EC），不是标题 logo。
 *   * 开场 logo：`logo_bg`(250) alpha 0→255、1200 ms（0x58386）、
 *     `logo_favo`(252) alpha 0→255、1400 ms（0x5830E）、
 *     `logo_favo_view`(254) alpha 0→255、1400 ms（0x58344）。
 */
#ifndef FVP_TITLEANIM_H
#define FVP_TITLEANIM_H

/* ---- 标题画面进场（我们实现了这一条）---- */
/* MotionAlpha(262/263/264/265, 0, 255, 500)，菜单项同参数（hcb 0x59349..0x595C1） */
#define FVP_TITLEANIM_FADE_FROM 0
#define FVP_TITLEANIM_FADE_TO 255
#define FVP_TITLEANIM_FADE_MS 500

/* MotionMoveS2(258..261, 1300/1600/2500/4000 → 900/800/800/800, 25000, 3)
 * （hcb 0x59143..0x59188）。**尚未实现**：需要先把标题背景改成"按图元缩放绘制"，
 * 而且 25 s 的慢缩放在 3 s 的门禁截图里几乎不可见。 */
#define FVP_TITLEANIM_BG_SCALE_FROM 1300
#define FVP_TITLEANIM_BG_SCALE_TO 900
#define FVP_TITLEANIM_BG_MS 25000

/* ---- 开场 FAVORITE logo（由 --opening 的 openseq 负责）---- */
/* MotionAlpha(250, 0, 255, 1200) / MotionAlpha(252, 0, 255, 1400)
 * / MotionAlpha(254, 0, 255, 1400)（hcb 0x58386 / 0x5830E / 0x58344） */
#define FVP_OPENANIM_LOGO_BG_MS 1200
#define FVP_OPENANIM_LOGO_FAVO_MS 1400
#define FVP_OPENANIM_LOGO_VIEW_MS 1400
/* MotionMoveS2(255/256/257, 10000, 1000, 10000, 1000, 1400, 3)（hcb 0x58243…）
 * —— 这三个是 logo_favo_view_p1/p2/p3：**我们还没画这三层**，所以这条缩放
 * 目前只是一个已冻结的常数，不是"标题动画"。 */
#define FVP_OPENANIM_VIEW_SCALE_FROM 10000
#define FVP_OPENANIM_VIEW_SCALE_TO 1000
#define FVP_OPENANIM_VIEW_MS 1400

#endif /* FVP_TITLEANIM_H */