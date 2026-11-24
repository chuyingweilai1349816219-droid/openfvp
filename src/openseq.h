/* FVP engine -- 开场序列（白底 logo 淡入 → 停留 → 淡出 → 标题从白色淡入）。
 *
 * 实测（2026-11-25）：`logo_bg`（graph.bin 744）是一张**纯白**的 1920x1080 图，
 * `logo_favo`（745，868x133 @ 526,407）是带 alpha 的 FAVORITE logo。所以原版开场是
 * **白底** + logo 淡入淡出，然后标题画面从白色里淡入（不是黑底）。
 * 顺序依据 docs/original-verification.md §6 的抓帧时间线；时长与缓动是近似值。
 *
 * 模块只算"时间 → 相位/alpha"，不碰 gfx，方便单测。
 */
#ifndef FVP_OPENSEQ_H
#define FVP_OPENSEQ_H

typedef enum FvpOpenPhase {
    FVP_OPEN_WHITE_HOLD = 0, /* 纯白、无 logo */
    FVP_OPEN_LOGO_IN,        /* logo alpha 0 -> 255 */
    FVP_OPEN_HOLD,           /* 保持 255 */
    FVP_OPEN_LOGO_OUT,       /* alpha 255 -> 0 */
    FVP_OPEN_TITLE_FADE,     /* 标题画出来，白色遮罩 255 -> 0 */
    FVP_OPEN_DONE            /* 结束 */
} FvpOpenPhase;

typedef struct FvpOpenSeq {
    int elapsed_ms;
} FvpOpenSeq;

#define FVP_OPEN_WHITE_MS 300
#define FVP_OPEN_IN_MS 1200
#define FVP_OPEN_HOLD_MS 2100
#define FVP_OPEN_OUT_MS 1200
#define FVP_OPEN_FADE_MS 600
#define FVP_OPEN_TOTAL_MS                                                    \
    (FVP_OPEN_WHITE_MS + FVP_OPEN_IN_MS + FVP_OPEN_HOLD_MS + FVP_OPEN_OUT_MS + \
     FVP_OPEN_FADE_MS)

void fvp_openseq_init(FvpOpenSeq *s);
void fvp_openseq_advance(FvpOpenSeq *s, int ms);
int fvp_openseq_elapsed(const FvpOpenSeq *s);
FvpOpenPhase fvp_openseq_phase(const FvpOpenSeq *s);
/* logo 的 alpha（0..255），非 logo 相位为 0。 */
int fvp_openseq_logo_alpha(const FvpOpenSeq *s);
/* 标题上的白色遮罩 alpha（0..255）：只在 TITLE_FADE 相位非零，从 255 掉到 0。 */
int fvp_openseq_veil_alpha(const FvpOpenSeq *s);
int fvp_openseq_done(const FvpOpenSeq *s);

#endif /* FVP_OPENSEQ_H */