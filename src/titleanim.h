/* 标题画面进场动画的脚本参数 —— 从两部作品的 hcb 里读出来的常量。
 *
 * 出处：`ref/title-scene.json`（`scripts/extract-title-scene.py`）、
 * `ref/title-enter.json`（`scripts/extract-title-enter.py` 解析进场块：
 * 五彩斑斓 0x5933E..0x59650、星空 0x6FEDE..0x701E2），以及 exe handler 静态分析。
 * 门禁 `tests/verify_title_enter.py` / `tests/verify_title_anim.py` 会重新提取并
 * 断言这里的数字与脚本一致。
 *
 * 槽位归属（五彩斑斓 hcb 0x580AA 起；星空另一套槽位，见 ref/title-enter.json）：
 *
 *   250/252/254..257          开场 FAVORITE logo —— 参数在 src/openinganim.h
 *   258..261 `title_bg1_A..D` 标题背景四层（加载于 0x5853F..0x5858A）
 *   262..264                  标题装饰层（五彩斑斓）；本站**尚未绘制**
 *   265 / 278                 标题 logo / 版权行（0x58B72 / 0x58BB3）
 *   1501..                    菜单项（0x58BC8 起）
 *   277 `title_trial`         体验版标记（0x58B9E，正式版不画）
 *
 * 标题进场（正常路径；`global236` 是"跳过动画"开关）：
 *
 *   | | 五彩斑斓 | 星空 |
 *   |---|---|---|
 *   | 装饰层 | 264 @1000/2000；263 @2000/2000 + 纵向 −47；262 @2000/3000 | 259/260 @1000/2000 + 纵向 ±50 |
 *   | logo | **@2000，2000 ms** | **@1000，2000 ms**（与装饰层同时） |
 *   | 菜单 | **@2400**，第一项 900 ms、其余 820 ms、+40 ms 步进，纵向 **+23 → 0** | **@1400**，同时长，纵向 **+50 → 0** |
 *   | 版权行 | **@2560，820 ms**，横向 **+30 → 0** | **@1560，820 ms**，横向 **+76 → 0** |
 *   | 总时长 | **4000 ms** | **3000 ms** |
 *
 * 位移换算用各自的 float32 逻辑→HD 因子（五彩斑斓 1.875/1.6875、星空 2.4/1.8）：
 * 14×1.6875=23、16×1.875=30、28×1.8=50、32×2.4=76。
 * 旧实现是"统一的 500 ms 淡入"（那个 500 来自 0x59120 的 local5，只被背景动画
 * 用到），已作废。
 *
 * 标题背景（`MotionMoveS2` + `MotionMoveR`，都 25000 ms）**尚未实现**：
 * 需要先把标题背景改成按图元 OP 轴心缩放/旋转绘制。
 */
#ifndef FVP_TITLEANIM_H
#define FVP_TITLEANIM_H

/* ---- 标题进场：两作共用的时长/步进（见 src/titleenter.h）---- */
#define FVP_TITLEENTER_LOGO_MS 2000
#define FVP_TITLEENTER_MENU_FIRST_MS 900
#define FVP_TITLEENTER_MENU_MS 820
#define FVP_TITLEENTER_MENU_STEP_MS 40
#define FVP_TITLEENTER_MENU_COUNT 5
#define FVP_TITLEENTER_COPY_MS 820

/* ---- 五彩斑斓的世界 HD（hcb 0x5933E..0x59650）---- */
#define FVP_TITLEENTER_IROSEKA_LOGO_START_MS 2000
#define FVP_TITLEENTER_IROSEKA_MENU_START_MS 2400
#define FVP_TITLEENTER_IROSEKA_COPY_START_MS 2560
#define FVP_TITLEENTER_IROSEKA_TOTAL_MS 4000
#define FVP_TITLEENTER_IROSEKA_MENU_DY 23
#define FVP_TITLEENTER_IROSEKA_COPY_DX 30

/* ---- 星空的记忆 HD（hcb 0x6FEDE..0x701E2；只等一次 1000 ms）---- */
#define FVP_TITLEENTER_HOSHIMEMO_LOGO_START_MS 1000
#define FVP_TITLEENTER_HOSHIMEMO_MENU_START_MS 1400
#define FVP_TITLEENTER_HOSHIMEMO_COPY_START_MS 1560
#define FVP_TITLEENTER_HOSHIMEMO_TOTAL_MS 3000
#define FVP_TITLEENTER_HOSHIMEMO_MENU_DY 50
#define FVP_TITLEENTER_HOSHIMEMO_COPY_DX 76

/* 装饰层（本站尚未绘制；参数冻结给以后实现用）：
 * 五彩斑斓 264 →255/2000 从 1000 ms 起；263 →255/2000 + 纵向 −47 px；262 →255/3000。
 * 星空 259/260 →255/2000 从 1000 ms 起 + 纵向 ±50 px。 */
#define FVP_TITLEENTER_FRAME_START_MS 1000
#define FVP_TITLEENTER_FRAME_MS 2000
#define FVP_TITLEENTER_FRAME2_MS 3000
#define FVP_TITLEENTER_SLIDE_263 47

/* ---- 标题背景（参数冻结在 ref/title-bg-motion.json）----
 * 星空那条（2000→1000 / 40000 ms、左上锚、无旋转）**已实现**：
 * 外壳 --title-bg-anim → src/titlebg.c → gfx_draw_image_zoomed（带缓存）。
 * 五彩斑斓那条（4 层超尺寸图 + OP 轴心 + 旋转）**尚未实现**。
 * 五彩斑斓（hcb 0x59143..0x591D3）四层 258..261：
 *   MotionMoveS2(1300/1600/2500/4000 → 900/800/800/800, 25000, 3)
 *   + MotionMoveR(150/250/550/1250 → 0, 25000, 3)   ← 15°/25°/55°/125° → 0°
 *   + PrimSetOP(prim, 1200, 675)（设计画布 2400×1350 中心）
 * 星空（hcb 0x6FD87）：背景只有槽 258（1920×1080，无 OP），
 *   MotionMoveS2(2000 → 1000, 40000, 3)，**没有 MotionMoveR（不旋转）**。
 * 图像包（HZC1）只存像素 + w/h/x/y/fmt，不含旋转信息 —— 旋转是脚本参数。 */
#define FVP_TITLEANIM_BG_SCALE_FROM 1300
#define FVP_TITLEANIM_BG_SCALE_TO 900
#define FVP_TITLEANIM_BG_MS 25000
#define FVP_TITLEANIM_BG_ANGLE_FROM 150 /* 0.1° = 15° */
#define FVP_TITLEANIM_BG_ANGLE_TO 0
#define FVP_TITLEANIM_BG_OP_X 1200
#define FVP_TITLEANIM_BG_OP_Y 675
#define FVP_TITLEANIM_HOSHIMEMO_BG_SCALE_FROM 2000
#define FVP_TITLEANIM_HOSHIMEMO_BG_SCALE_TO 1000
#define FVP_TITLEANIM_HOSHIMEMO_BG_MS 40000

#endif /* FVP_TITLEANIM_H */