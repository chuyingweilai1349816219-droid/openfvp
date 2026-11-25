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
 *   TextFont(twIdx, fontId, fontId2)             → tw+0x590（u16）/ tw+0x593（u8）
 *   TextFormat(twIdx, a, b, c, d, e, f)          → tw+0x5A8/0x5AA/0x5AE/0x5AC/0x5B4/0x5B0
 *   TextFunction(twIdx, a, b, c)                 → tw+0x5A5（0..1）/ 0x5A4（0..2）/ 0x5A6（0..2）
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
#define FVP_TEXTATTR_FONT 0x40u
#define FVP_TEXTATTR_FORMAT 0x80u
#define FVP_TEXTATTR_FUNCTION 0x100u

/* TextShadowDist 的距离 0..12（静态分析 0x43CBB0：<0 当 0、>12 当 12 后转 0x4248B0）。
 * TextSpace 的两个分量各 -32..32（静态分析 0x43CE10：cmp $0xffffffe0 / cmp $0x20）。
 * TextSkip 的模式是 0..3（静态分析 0x43CDD0：cmp $0x3）。 */
#define FVP_TEXT_SHADOW_MIN 0
#define FVP_TEXT_SHADOW_MAX 12
#define FVP_TEXT_SPACE_MIN (-32)
#define FVP_TEXT_SPACE_MAX 32
#define FVP_TEXT_SKIP_MAX 3
/* TextFont 的下界是 -5（静态分析 0x43DAB0/0x43DACA：`cmp $0xfffffffb,%eax; jl`）；
 * 上界是**引擎字体列表长度 - 1**（`fontCount = (end-begin)/4 - 1`），VM 层看不到那份
 * 列表，所以这里用 FVP_TEXT_FONT_MAX 作保守上界，真正的上界判定留给渲染层。
 * 实测两部作品只用到 -1 / -2（见 docs/vm-commands.md §5）。 */
#define FVP_TEXT_FONT_MIN (-5)
#define FVP_TEXT_FONT_MAX 31
/* TextFormat(118) 六个参数的取值范围（静态分析 0x43CC10，逐字段独立判定）：
 *   arg1/arg2 → tw+0x5A8/+0x5AA : -32..32（和 TextSpace 是**同一对字段**）
 *   arg3      → tw+0x5AE         : 0..64
 *   arg4      → tw+0x5AC         : 0..64（并且会把 tw+0x5E8 抬到不小于 arg4）
 *   arg5      → tw+0x5B4         : -16..16
 *   arg6      → tw+0x5B0         : -16..16
 * TextFunction(119) 三个开关（静态分析 0x43CCF0）：arg1 0..1、arg2 0..2、arg3 0..2。 */
#define FVP_TEXT_FMT_PAD_MAX 64
#define FVP_TEXT_FMT_OFF_MIN (-16)
#define FVP_TEXT_FMT_OFF_MAX 16
#define FVP_TEXT_FUNC1_MAX 1
#define FVP_TEXT_FUNC2_MAX 2
#define FVP_TEXT_FUNC3_MAX 2

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
    int font_id;   /* TextFont 的第一个字体号（原版写 tw+0x590，u16） */
    int font_id2;  /* TextFont 的第二个字体号（原版写 tw+0x593，u8） */
    /* TextFormat(118)：tw+0x5AE / 0x5AC / 0x5B4 / 0x5B0 四个排版参数
     * （前两个参数写的就是 space_x / space_y，即 tw+0x5A8 / 0x5AA）。 */
    int fmt_pad_a;
    int fmt_pad_b;
    int fmt_off_a;
    int fmt_off_b;
    /* TextFormat(118) arg4 除了写 tw+0x5AC，还会把 tw+0x5E8 **抬到不小于 arg4**
     * （静态分析 0x43CC9B：`cmp %dx,0x5e8(%ecx)` → 只有更大才写）。
     * tw+0x5E8 在排版侧是"累计行宽"：文本方法 0x4247C4/0x4247DA 用 `add` 往它上面
     * 累加，换行判断 0x433006 拿它跟 tw+0x5DC 比；所以 arg4 的语义是**行宽下限**。 */
    int fmt_width_floor;
    /* TextFunction(119)：tw+0x5A5（0..1）/ 0x5A4（0..2）/ 0x5A6（0..2）三个开关。 */
    int func_a;
    int func_b;
    int func_c;
    unsigned int set_mask;
} FvpTextAttr;

/* 全部 32 个窗口重置成引擎默认值：白字、字号 16x12、字体号 -2/-2、
 * 速度 0（不等待）。这些默认值来自 exe 文本窗口构造函数 0x430EA0，
 * 基线见 ref/exe-text-defaults.json（门禁 tests/verify_exe_text_defaults.py）。 */
void fvp_textattr_reset(FvpTextAttr *attrs, int count);

/* setter 返回 1 表示已生效，0 表示参数被拒（窗口号越界 / NULL / 整条命令被忽略）。
 * 各命令的超范围处理**不一样**，按静态分析分别实现：
 *   TextColor / TextSize : 钳制（引擎里是逐字段 clamp）
 *   TextSpeed            : 超范围整条忽略（-1 合法）
 *   TextShadowDist       : <0 → 0，>12 → 12（钳制，不是忽略）
 *   TextSpace            : 每个分量独立判定，超范围的那个分量跳过、另一个仍生效
 *   TextSkip             : 超范围整条忽略
 *   TextFont             : 每个字体号独立判定（-5 以下跳过该分量，另一个仍生效）
 *   TextFormat / TextFunction : 同样是逐字段独立判定 */
int fvp_textattr_set_color(FvpTextAttr *attrs, int count, int idx, int r, int g,
                           int b);
int fvp_textattr_set_size(FvpTextAttr *attrs, int count, int idx, int w, int h);
int fvp_textattr_set_speed(FvpTextAttr *attrs, int count, int idx, int ms);
int fvp_textattr_set_shadow(FvpTextAttr *attrs, int count, int idx, int dist);
int fvp_textattr_set_space(FvpTextAttr *attrs, int count, int idx, int x, int y);
int fvp_textattr_set_skip(FvpTextAttr *attrs, int count, int idx, int mode);
int fvp_textattr_set_font(FvpTextAttr *attrs, int count, int idx, int id,
                          int id2);
int fvp_textattr_set_format(FvpTextAttr *attrs, int count, int idx, int a,
                            int b, int c, int d, int e, int f);
int fvp_textattr_set_function(FvpTextAttr *attrs, int count, int idx, int a,
                              int b, int c);

/* 被脚本设置过至少一个字段的窗口数（日志 / 验收用）。 */
int fvp_textattr_configured_count(const FvpTextAttr *attrs, int count);

#ifdef __cplusplus
}
#endif

#endif /* FVP_TEXTATTR_H */
