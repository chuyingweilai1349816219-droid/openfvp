/* 文本窗口属性：TextColor / TextSize / TextSpeed / TextShadowDist / TextSpace /
 * TextSkip 的存储与范围判定。
 *
 * 语义全部来自静态分析（`docs/vm-commands.md` §5，handler 地址逐条标注）：
 *
 *   TextColor(twIdx 0..31, R, G, B 0..255)       → tw+0x590/0x591/0x592? 颜色
 *   TextSize (twIdx, w 12..64, h 8..32)          → 字号（写 tw+0x592/0x594）
 *   TextSpeed(twIdx, ms -1..300000)              → 每字毫秒（写 tw+0x5e0）
 *   TextShadowDist(twIdx, dist 0..12)            → **阴影距离**，写 tw+0x597（0x4248B0）
 *   TextSpace(twIdx, x -32..32, y -32..32)       → 字间距/行间距，写 tw+0x5A8 / tw+0x5AA
 *   TextSkip(twIdx, mode 0..3)                   → 写 tw+0x5D7
 *
 * ⚠️ 2026-11-24 修正：以前把 `0x43CE10` 当成 TextShadowDist，其实它是 **TextSpace**；
 * TextShadowDist 的 handler 是 `0x43CBB0`（单个 0..12 距离，转 0x4248B0 只写一个字节）。
 * 详见 `docs/render-model.md` §7.1。
 *
 * 这一层只做「存起来 + 按引擎的范围判定」；渲染层要不要用、怎么用是下一步的事
 * （当前 gfx 只记录、不画阴影/间距，所以这里不会改变任何已验收的帧）。
 */
#ifndef FVP_TEXTATTR_H
#define FVP_TEXTATTR_H

#ifdef __cplusplus
extern "C" {
#endif

#define FVP_TEXT_WINDOWS 32

#define FVP_TEXT_COLOR_MAX 255
#define FVP_TEXT_W_MIN 12
#define FVP_TEXT_W_MAX 64
#define FVP_TEXT_H_MIN 8
#define FVP_TEXT_H_MAX 32
#define FVP_TEXT_SPEED_MAX 300000

/* 哪一个字段被脚本设过 —— 没设过的保持引擎默认值。 */
#define FVP_TEXTATTR_COLOR 0x01u
#define FVP_TEXTATTR_SIZE 0x02u
#define FVP_TEXTATTR_SPEED 0x04u
#define FVP_TEXTATTR_SHADOW 0x08u
#define FVP_TEXTATTR_SKIP 0x10u
#define FVP_TEXTATTR_SPACE 0x20u

/* TextShadowDist 的距离 0..12（静态分析 0x43CBB0：<0 当 0、>12 当 12 后转 0x4248B0）。
 * TextSpace 的两个分量各 -32..32（静态分析 0x43CE10：cmp $0xffffffe0 / cmp $0x20）。
 * TextSkip 的模式是 0..3（静态分析 0x43CDD0：cmp $0x3）。 */
#define FVP_TEXT_SHADOW_MIN 0
#define FVP_TEXT_SHADOW_MAX 12
#define FVP_TEXT_SPACE_MIN (-32)
#define FVP_TEXT_SPACE_MAX 32
#define FVP_TEXT_SKIP_MAX 3

typedef struct FvpTextAttr {
    int r;
    int g;
    int b;
    int w;         /* 字号宽（12..64） */
    int h;         /* 字号高（8..32） */
    int speed_ms;  /* 每字毫秒（-1 = 用引擎设置，0 = 不等待） */
    int shadow_dist; /* TextShadowDist 的阴影距离（0..12） */
    int space_x;   /* TextSpace 的第一个分量（-32..32） */
    int space_y;   /* TextSpace 的第二个分量（-32..32） */
    int skip_mode; /* TextSkip 的模式（0..3） */
    unsigned int set_mask;
} FvpTextAttr;

/* 全部 32 个窗口重置成引擎默认值（白字、12x12、速度 0 表示不等待）。 */
void fvp_textattr_reset(FvpTextAttr *attrs, int count);

/* setter 返回 1 表示已生效，0 表示参数被拒（窗口号越界 / NULL / 整条命令被忽略）。
 * 各命令的超范围处理**不一样**，按静态分析分别实现：
 *   TextColor / TextSize : 钳制（引擎里是逐字段 clamp）
 *   TextSpeed            : 超范围整条忽略（-1 合法）
 *   TextShadowDist       : <0 → 0，>12 → 12（钳制，不是忽略）
 *   TextSpace            : 每个分量独立判定，超范围的那个分量跳过、另一个仍生效
 *   TextSkip             : 超范围整条忽略 */
int fvp_textattr_set_color(FvpTextAttr *attrs, int count, int idx, int r, int g,
                           int b);
int fvp_textattr_set_size(FvpTextAttr *attrs, int count, int idx, int w, int h);
int fvp_textattr_set_speed(FvpTextAttr *attrs, int count, int idx, int ms);
int fvp_textattr_set_shadow(FvpTextAttr *attrs, int count, int idx, int dist);
int fvp_textattr_set_space(FvpTextAttr *attrs, int count, int idx, int x, int y);
int fvp_textattr_set_skip(FvpTextAttr *attrs, int count, int idx, int mode);

/* 被脚本设置过至少一个字段的窗口数（日志 / 验收用）。 */
int fvp_textattr_configured_count(const FvpTextAttr *attrs, int count);

#ifdef __cplusplus
}
#endif

#endif /* FVP_TEXTATTR_H */
