/* FVP engine -- 标题背景的动画状态（缩放/旋转），参数来自脚本。
 *
 * 出处：`ref/title-bg-motion.json`（`scripts/extract-title-bg-motion.py`）、
 * `ref/exe-rs-transform.json`（path A/B 变换）与门禁
 * `tests/verify_title_bg_motion.py`。
 *
 * 两作的背景动画模型不同：
 *
 *   星空（hcb 0x6FD87）：背景槽 258 的 1920x1080 图，`MotionMoveS2(2000→1000,
 *   40000 ms, mode 3)`；没有 PrimSetOP/PrimSetRS ⇒ path B、锚点 = 图头 (0,0)
 *   ⇒ 左上锚缩放、不旋转。已实现（gfx_draw_image_zoomed）。
 *
 *   五彩斑斓（hcb 0x5912F..0x591D3）四层 258..261，各自：
 *     A(258) 缩放 1300→900、旋转 150→0（7.5°→0）；**唯一设了 PrimSetRS 的层**
 *            ⇒ path A，锚点 = 画面中心；
 *     B(259) 1600→800、250→0；C(260) 2500→800、550→0；D(261) 4000→800、1250→0
 *            ⇒ path B，锚点 = OP=(1200,675)；
 *     全部 25000 ms、mode 3；OP 四层都是 (1200,675)。
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
    int animated;    /* 0 = 本作没有逐层背景动画（按静态裁剪画） */
    int scale_x1000; /* per-mille（星空单层用；五彩请用 layer_state） */
    int scale_y1000;
    int angle_units; /* 1 单位 = 0.05°（7200 = 360°） */
    int done;
} FvpTitleBgState;

/* 逐层定义（五彩用；星空只有一层，走上面的单层接口）。 */
#define FVP_TITLEBG_MAX_LAYERS 4

typedef struct FvpTitleBgLayerDef {
    const char *name;   /* 资产名（日志/文档用） */
    int slot_tag;       /* 0 = 背景槽；1..3 = 叠加层槽 TITLE_OVL_SLOT+i-1 */
    int img_x;          /* 素材头位置（原图像素） */
    int img_y;
    int op_x;           /* OP（原图像素） */
    int op_y;
    int anchor_center;  /* 1 = path A（锚点 = 画面中心），0 = path B（锚点 = OP） */
    int scale_from;     /* per-mille */
    int scale_to;
    int angle_from;     /* 0.05° 单位 */
    int angle_to;
} FvpTitleBgLayerDef;

typedef struct FvpTitleBgLayerState {
    int scale_x1000;
    int scale_y1000;
    int angle_units;
} FvpTitleBgLayerState;

/* 游戏名（"iroseka"/"hoshimemo"）→ 枚举；未知/NULL 用五彩斑斓。 */
FvpTitleBgGame fvp_titlebg_game_from_name(const char *name);

/* 该作背景动画总时长（ms）；0 = 没有实现动画。 */
int fvp_titlebg_total_ms(FvpTitleBgGame game);

/* 单层接口（星空用）：某时刻的状态。elapsed_ms < 0 按 0 处理；超过总时长按终值。 */
void fvp_titlebg_state(FvpTitleBgGame game, int elapsed_ms,
                       FvpTitleBgState *out);

/* 逐层接口（五彩用）：层数 / 定义 / 某时刻状态；越界返回 0/NULL。 */
int fvp_titlebg_layer_count(FvpTitleBgGame game);
const FvpTitleBgLayerDef *fvp_titlebg_layer_def(FvpTitleBgGame game, int idx);
int fvp_titlebg_layer_state(FvpTitleBgGame game, int idx, int elapsed_ms,
                            FvpTitleBgLayerState *out);
/* 该作是否走逐层动画（目前只有五彩）。 */
int fvp_titlebg_uses_layers(FvpTitleBgGame game);

/* 资产名（"title_bg1_A".."title_bg1_D"）→ slot_tag：0 = 背景槽，1..3 = 叠加槽
 * TITLE_OVL_SLOT+tag-1。**这个编号必须和脚本的 GraphLoad 槽号一致**
 * （五彩 hcb：258=A、259=B、260=C、261=D，见 ref/title-scene.json 的 loads），
 * 否则动画层会拿到隔壁层的图（2026-11-26 修：静态路径按显示顺序装叠加层，
 * 动画按 slot_tag 取图，两套编号不一致 ⇒ 风车被当成 D 层按 4 倍缩放+62° 画）。
 * 返回 -1 = 名字不认识。 */
int fvp_titlebg_slot_tag_for_name(const char *name);

#ifdef __cplusplus
}
#endif

#endif /* FVP_TITLEBG_H */