/* FVP engine -- 标题菜单（引擎侧 UI）的布局与命中判定。
 *
 * 为什么单独一个模块：原版的标题菜单**不是脚本画的**——2026-11-24 用原版抓帧确认，
 * `New Game / Continue / Album / Option / Exit` 在脚本执行之前就出现在屏幕上并等待输入
 * （见 docs/original-verification.md §4.1）。本引擎目前从脚本第 0 条指令跑起，所以
 * 缺的就是这块引擎侧 UI。这里先把**布局 + 命中**做成与渲染无关的纯逻辑，
 * 渲染/字体/背景图分别再接。
 *
 * 几何数字全部来自原版截图实测（1920x1080 客户区，out/orig-title/orig-title-005.png，
 * 用 work/measure_title.py 量的）：
 *
 *   | 项目       | 墨迹 y  | 高 | 墨迹 x    | 宽  | 中心        |
 *   | New Game   | 511..552| 42 | 1461..1769 | 309 | (1615, 531) |
 *   | Continue   | 611..652| 42 | 1514..1769 | 256 | (1641, 631) |
 *   | Album      | 712..753| 42 | 1580..1768 | 189 | (1674, 732) |
 *   | Option     | 811..852| 42 | 1575..1768 | 194 | (1671, 831) |
 *   | Exit       | 912..951| 40 | 1662..1768 | 107 | (1715, 931) |
 *
 * 由此得到三条硬指标：**右对齐到 x = 1769**、**行距 100 ± 1 px**、
 * **墨迹高 40..42 px（≈ 56 px 字号，衬线体）**。
 */
#ifndef FVP_TITLEMENU_H
#define FVP_TITLEMENU_H

#define FVP_TITLE_ITEMS 5

/* 菜单项 id（同时也是绘制顺序） */
enum FvpTitleItem {
    FVP_TITLE_NEW_GAME = 0,
    FVP_TITLE_CONTINUE = 1,
    FVP_TITLE_ALBUM = 2,
    FVP_TITLE_OPTION = 3,
    FVP_TITLE_EXIT = 4
};

/* 文本宽度测量回调：把 UTF-8 标签的像素宽度填进 *width。
 * 返回 1 成功；0 表示没有字体（调用方回落到等宽估计）。 */
typedef int (*FvpTitleMeasure)(void *ctx, const char *utf8, int font_px,
                               int *width);

typedef struct FvpTitleMenu {
    int client_w;   /* 客户区宽（原版 1920） */
    int client_h;   /* 客户区高（原版 1080） */
    int x_right;    /* 右对齐基准线：墨迹右边界（原版 1769） */
    int y_first;    /* 第 1 项墨迹上沿（原版 511） */
    int pitch;      /* 行距（原版 100） */
    int font_px;    /* 字号（由墨迹高 42 反推 56） */
    int hover;      /* 鼠标悬停项：-1 = 无 */
    int selected;   /* 键盘/最近一次选中项：-1 = 无 */
    int enabled[FVP_TITLE_ITEMS]; /* Continue 在没存档时通常是灰的 */

    /* ---- 图像模式（2026-11-24 发现：原版菜单项是**预渲染贴图**）----
     *
     * graph.bin 里的 `title_start`(340x80) 就是「New Game」整张图（衬线字 +
     * 白色外发光都烘焙在贴图里），用文字掩码在原始标题帧里能对上 92.2%，
     * 而且墨迹右边界正好 1769。所以原版根本没有"菜单字体"这回事。
     * 5 个贴图共用同一个 340x80 画布、文字在画布内右对齐：
     *   贴图左上角 = (1445, 492 + i*100)
     *   墨迹左/右 = 贴图 x + 15 / x + 324   → 1460 / 1769（与实测 1461/1769 一致）
     *   墨迹上沿   = 贴图 y + 19            → 511（实测 511）
     * use_images = 1 时 fvp_title_item_rect() 返回**贴图矩形**（340x80），
     * 由调用方把 graph 贴图按这个位置画出来；= 0 时退回文字模式。 */
    int use_images;
    int img_w;      /* 340 */
    int img_h;      /* 80  */
    int img_x;      /* 1445 */
    int img_y0;     /* 492 */
    int ink_x0_off; /* 15  —— 贴图内墨迹左边界偏移 */
    int ink_x1_off; /* 324 —— 贴图内墨迹右边界偏移 */
    int ink_y0_off; /* 19  —— 贴图内墨迹上沿偏移 */
    int img_ready[FVP_TITLE_ITEMS]; /* 已加载成功的贴图 */
} FvpTitleMenu;

/* 菜单标签（原版是衬线拉丁体，不是 font/ 里的 Sarasa；见文档 §5.1 待查） */
const char *fvp_title_label(int item);

/* ------------------------------------------------------------------ */
/* 每作品 profile（2026-11-24 实测：两部作品的标题菜单**布局不一样**）  */
/* ------------------------------------------------------------------ */

/* 对齐方式：iroseka 右对齐到 x=1769；hoshimemo 全部**居中**于 x=960。 */
typedef enum FvpTitleAlign {
    FVP_TITLE_ALIGN_RIGHT = 0,
    FVP_TITLE_ALIGN_CENTER = 1
} FvpTitleAlign;

#define FVP_TITLE_ASSET_MAX 24

/* 一项的实测几何（客户区 1920x1080 坐标）+ 它用的贴图素材名。 */
typedef struct FvpTitleItemDef {
    const char *label;                    /* 菜单文字（英文原样） */
    char asset[FVP_TITLE_ASSET_MAX];      /* graph.bin 里的贴图名 */
    char asset_glow[FVP_TITLE_ASSET_MAX]; /* 发光层素材名（没有就空串） */
    int tex_w, tex_h;                     /* 贴图尺寸 */
    int ink_x0, ink_x1, ink_y0, ink_y1;   /* 实测墨迹包围盒 */
} FvpTitleItemDef;

/* 一部作品的标题菜单 profile。所有数字都来自原版截图实测。 */
typedef struct FvpTitleProfile {
    const char *game;      /* 识别用的关键字（小写子串） */
    FvpTitleAlign align;
    int client_w, client_h;
    int pitch;             /* 行距 */
    int ink_top0;          /* 第 1 项墨迹上沿 */
    int right_x;           /* 右对齐基准（右对齐模式） */
    int center_x;          /* 居中基准（居中模式） */
    FvpTitleItemDef items[FVP_TITLE_ITEMS];
    /* 标题画面背景素材名（整屏 1920x1080）。空串 = 还没指认出来（iroseka 的
     * 白天背景仍未找到，见 docs/title-menu.md §4.1）。 */
    char bg_asset[FVP_TITLE_ASSET_MAX];
} FvpTitleProfile;

extern const FvpTitleProfile fvp_title_profile_iroseka;
extern const FvpTitleProfile fvp_title_profile_hoshimemo;

/* 按游戏目录/脚本名里的关键字找 profile（大小写不敏感）；找不到返回 NULL。 */
const FvpTitleProfile *fvp_title_profile_find(const char *hint);

/* 把 profile 的几何灌进菜单状态（会重设 hover/selected）。 */
void fvp_title_apply_profile(FvpTitleMenu *m, const FvpTitleProfile *p);

/* 第 item 项按 profile 算出来的墨迹矩形（客户区坐标）。 */
int fvp_title_profile_ink_rect(const FvpTitleProfile *p, int item, int *x0,
                               int *y0, int *x1, int *y1);

/* 第 item 项贴图的左上角：让贴图内墨迹正好落在实测位置上。 */
int fvp_title_profile_image_pos(const FvpTitleProfile *p, int item, int *x,
                                int *y);

/* 用 profile 的实测几何做命中判定（不需要字体/测宽）：
 * 墨迹矩形左右各放宽 24 px、纵向补足到整行 pitch。返回 0..4 / -1。 */
int fvp_title_profile_hit(const FvpTitleProfile *p, int mx, int my);

/* 用实测几何初始化：x_right=1769、y_first=511（墨迹上沿）、pitch=100、
 * font_px=56、client 1920x1080，全部项可用、hover/selected = -1。
 * 图像模式的常数（340x80 @ (1445,492+i*100)）也一并填好，use_images 默认 0。 */
void fvp_title_init(FvpTitleMenu *m);

/* 切到图像模式（原版做法）：之后 fvp_title_item_rect() 返回贴图矩形。 */
void fvp_title_set_images(FvpTitleMenu *m, int on);

/* 第 i 项贴图的左上角（图像模式）。返回 1 成功。 */
int fvp_title_image_pos(const FvpTitleMenu *m, int item, int *x, int *y);

/* 第 item 项的相对墨迹矩形（客户区坐标，含行高带）。
 * 有 measure 时按实测宽度右对齐；没有（或 measure 返回 0）时用
 * `font_px/2 * 字符数` 的等宽估计。返回 1 成功，0 = 参数非法。 */
int fvp_title_item_rect(const FvpTitleMenu *m, int item, FvpTitleMeasure measure,
                        void *ctx, int *x0, int *y0, int *x1, int *y1);

/* 命中判定：返回 0..FVP_TITLE_ITEMS-1，未命中返回 -1。
 * 纵向按"行高带"判定（y_first + item*pitch 起、高 font_px），横向按该项墨迹
 * 矩形并**向左右各放宽 24 px**（原版鼠标悬停的可点区域比墨迹宽）。 */
int fvp_title_hit(const FvpTitleMenu *m, int mx, int my,
                  FvpTitleMeasure measure, void *ctx);

/* 鼠标移动：更新 hover（未命中置 -1），返回新的 hover。 */
int fvp_title_move(FvpTitleMenu *m, int mx, int my, FvpTitleMeasure measure,
                   void *ctx);

/* 键盘导航：把 hover 移动 delta 项（+1 下 / -1 上），跳过不可用项并环绕；
 * hover 为 -1 时从 0（delta>0）或最后一项（delta<0）开始。返回新的 hover。 */
int fvp_title_nav(FvpTitleMenu *m, int delta);

/* 激活当前 hover（没有 hover 则选第 0 项可用项）。返回 0..4，失败 -1。 */
int fvp_title_activate(FvpTitleMenu *m);

/* 点击：命中且可用时返回该项 id 并把 hover 记进 selected；否则返回 -1。 */
int fvp_title_click(FvpTitleMenu *m, int mx, int my, FvpTitleMeasure measure,
                    void *ctx);

#endif /* FVP_TITLEMENU_H */