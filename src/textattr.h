/* 文本窗口属性：TextColor / TextSize / TextSpeed 的存储与钳制。
 *
 * 语义来自静态分析（`docs/vm-commands.md` §5）：
 *
 *   TextColor(twIdx 0..31, R, G, B 0..255)   → 窗口颜色
 *   TextSize (twIdx, w 12..64, h 8..32)      → 字号（字节，写 tw+0x592/0x594）
 *   TextSpeed(twIdx, ms 0..300000)           → 每字毫秒（写 tw+0x5e0）
 *
 * 这一层只做「存起来 + 按引擎的范围钳制」；渲染层要不要用、怎么用是下一步的事
 * （当前 gfx 仍然用自己的字体图集，所以这里不会改变任何已验收的帧）。
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

/* TextShadowDist 的两个分量都是 -32..32（静态分析 0x43CE10：cmp $0xffffffe0 / cmp $0x20）。
 * TextSkip 的模式是 0..3（静态分析 0x43CDD0：cmp $0x3）。 */
#define FVP_TEXT_SHADOW_MIN (-32)
#define FVP_TEXT_SHADOW_MAX 32
#define FVP_TEXT_SKIP_MAX 3

typedef struct FvpTextAttr {
    int r;
    int g;
    int b;
    int w;       /* 字号宽（12..64） */
    int h;       /* 字号高（8..32） */
    int speed_ms; /* 每字毫秒（0..300000，-1 = 用引擎设置） */
    int shadow_x; /* TextShadowDist 的 x 分量（-32..32） */
    int shadow_y; /* TextShadowDist 的 y 分量（-32..32） */
    int skip_mode; /* TextSkip 的模式（0..3） */
    unsigned int set_mask;
} FvpTextAttr;

/* 全部 32 个窗口重置成引擎默认值（白字、12x12、速度 0 表示不等待）。 */
void fvp_textattr_reset(FvpTextAttr *attrs, int count);

/* 三个 setter 返回 1 表示已生效，0 表示参数被拒（窗口号越界 / NULL）。
 * 数值超出范围时按引擎的钳制区间夹紧，不报错 —— 与原版程序里的 cmp 分支一致。 */
int fvp_textattr_set_color(FvpTextAttr *attrs, int count, int idx, int r, int g,
                           int b);
int fvp_textattr_set_size(FvpTextAttr *attrs, int count, int idx, int w, int h);
int fvp_textattr_set_speed(FvpTextAttr *attrs, int count, int idx, int ms);
/* TextShadowDist(twIdx, dx, dy) / TextSkip(twIdx, mode)：见上面静态分析出处。 */
int fvp_textattr_set_shadow(FvpTextAttr *attrs, int count, int idx, int dx,
                            int dy);
int fvp_textattr_set_skip(FvpTextAttr *attrs, int count, int idx, int mode);

/* 被脚本设置过至少一个字段的窗口数（日志 / 验收用）。 */
int fvp_textattr_configured_count(const FvpTextAttr *attrs, int count);

#ifdef __cplusplus
}
#endif

#endif /* FVP_TEXTATTR_H */