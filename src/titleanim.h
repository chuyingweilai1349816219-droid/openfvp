/* 标题画面进场动画的脚本参数 —— 从 iroseka.hcb 里读出来的常量。
 *
 * 出处：`ref/title-scene.json`（`scripts/extract-title-scene.py` 静态提取）、
 * `ref/title-enter.json`（`scripts/extract-title-enter.py` 解析进场块 0x5933E..0x5961A），
 * 以及 exe handler 静态分析。门禁 `tests/verify_title_enter.py` /
 * `tests/verify_title_anim.py` 会重新提取并断言这里的数字与脚本一致。
 *
 * 槽位归属（hcb 0x580AA 起，按 GraphLoad 顺序）：
 *
 *   250 `logo_bg`             ┐
 *   252 `logo_favo`           │ 开场「FAVORITE」logo —— 参数在 src/openinganim.h，
 *   254 `logo_favo_view`      │ 本文件与进场动画无关（见 docs/opening.md）
 *   255..257 `logo_favo_view_p1..p3` ┘
 *
 *   258..261 `title_bg1_A..D` ┐ 标题背景四层（加载于 0x5853F..0x5858A）
 *   262..264                  │ 标题装饰层 —— 本引擎**尚未绘制**（见下）
 *   265 `title_logo1`         │ 标题画面 logo（0x58B72）
 *   278 `title_copyright`     │ 版权行（0x58BB3）
 *   1501.. / 1508.. / …        │ 菜单项（0x58BC8 起）
 *   277 `title_trial`         ┘ 体验版标记（0x58B9E，正式版不画）
 *
 * 标题进场（hcb 0x5933E..0x5961A，`global236` 是"跳过动画"开关；下面是正常路径）：
 *
 *   等待 1000 ms
 *   图元 264 透明度 →255（2000 ms）                      ← 装饰层，本站未画
 *   等待 1000 ms
 *   图元 265 = **标题 logo**：0→255 / 2000 ms            @0x59361
 *   图元 263：0→255 / 2000 ms + 纵向 -47 px → 0（原 -28 逻辑像素）@0x59370/0x59373
 *   图元 262：0→255 / 3000 ms                            ← 装饰层，本站未画
 *   菜单项：等待 400 ms 后每项间隔 40 ms：
 *     1501 0→255 / **900 ms** + 纵向 +23 → 0             @0x59454/0x59457
 *     1508/1515/1522/1529 0→255 / **820 ms** + 纵向 +23 → 0 @0x594A6/0x594F8/0x5954A/0x595C1
 *     （1522/1529 还有 `global3` 分支的 ±81 px 特例，本站未实现，见 docs/title-menu.md §21）
 *   图元 278 = **版权行**：0→255 / 820 ms + 横向 +30 → 0   @0x59609/0x5961A
 *
 * 所以总时长 4000 ms（logo 最后结束）。旧实现是"统一的 500 ms 淡入"，
 * 那个 500 来自 0x59120 的 local5 —— 其实只被背景动画用到，**已作废**。
 *
 * 标题背景（`MotionMoveS2` + `MotionMoveR`，都 25000 ms）**尚未实现**：
 * 需要先把标题背景改成按图元 OP 轴心缩放/旋转绘制。
 */
#ifndef FVP_TITLEANIM_H
#define FVP_TITLEANIM_H

/* ---- 标题进场（逐元素 alpha + 位移，见 src/titleenter.h）---- */
#define FVP_TITLEENTER_TOTAL_MS 4000

#define FVP_TITLEENTER_LOGO_START_MS 2000
#define FVP_TITLEENTER_LOGO_MS 2000

/* 菜单：第一项 900 ms、其余 820 ms、相邻项间隔 40 ms，整段从 2400 ms 起 */
#define FVP_TITLEENTER_MENU_START_MS 2400
#define FVP_TITLEENTER_MENU_FIRST_MS 900
#define FVP_TITLEENTER_MENU_MS 820
#define FVP_TITLEENTER_MENU_STEP_MS 40
#define FVP_TITLEENTER_MENU_COUNT 5
#define FVP_TITLEENTER_MENU_DY 23 /* 原 14 逻辑像素 ×1.6875 = 23（向下 23 px → 0） */

/* 版权行：最后一项菜单排完后开始，横向 +30 px → 0（原 16 逻辑像素 ×1.875） */
#define FVP_TITLEENTER_COPY_START_MS 2560
#define FVP_TITLEENTER_COPY_MS 820
#define FVP_TITLEENTER_COPY_DX 30

/* 装饰层 262/263/264（本站尚未绘制；参数冻结下来给以后实现用）：
 * 264 →255/2000 从 1000 ms 起；263 →255/2000 + 纵向 -47 px；262 →255/3000。 */
#define FVP_TITLEENTER_FRAME_START_MS 1000
#define FVP_TITLEENTER_FRAME_MS 2000
#define FVP_TITLEENTER_FRAME2_MS 3000
#define FVP_TITLEENTER_SLIDE_263 47 /* 原 -28 逻辑像素 ×1.6875 = -47 */

/* ---- 标题背景（hcb 0x59143..0x591D3，尚未实现）----
 * MotionMoveS2(258..261, 1300/1600/2500/4000 → 900/800/800/800, 25000, 3)
 * + MotionMoveR(258..261, 150/250/550/1250 → 0, 25000, 3)
 * —— 即 25 秒的极慢缩放 **和** 慢旋转。 */
#define FVP_TITLEANIM_BG_SCALE_FROM 1300
#define FVP_TITLEANIM_BG_SCALE_TO 900
#define FVP_TITLEANIM_BG_MS 25000

#endif /* FVP_TITLEANIM_H */