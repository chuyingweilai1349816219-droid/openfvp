/* FVP 引擎侧游戏内菜单（右键菜单的等价物）。
 *
 * 原版的游戏内菜单是**exe 自己画的**（脚本里只有 `MenuMessSkip` 与
 * `TitleMenu` 两条相关命令，没有"画菜单"的指令），所以这里和标题菜单一样，
 * 属于外壳的引擎侧 UI：纯逻辑（布局/命中/导航/激活）放在本文件里，可单测；
 * 具体怎么画由平台外壳决定。
 *
 * 坐标约定与 titlemenu 一致：墨迹上沿 y，文字右对齐/居中对齐由调用方给。
 */
#ifndef FVP_GAMEMENU_H
#define FVP_GAMEMENU_H

#ifdef __cplusplus
extern "C" {
#endif

#define FVP_GAMEMENU_MAX 8
#define FVP_GAMEMENU_LABEL_MAX 64

typedef struct FvpGameMenuItem {
    char label[FVP_GAMEMENU_LABEL_MAX];
    int enabled;
} FvpGameMenuItem;

typedef struct FvpGameMenu {
    int active;
    int count;
    int hover;
    int selected;
    int x;       /* 菜单左边界（居中布局时由 fvp_gamemenu_layout 算出） */
    int y;       /* 第一项墨迹上沿 */
    int pitch;   /* 行距 */
    int width;   /* 命中区域宽度 */
    int item_h;  /* 命中区域高度 */
    FvpGameMenuItem items[FVP_GAMEMENU_MAX];
} FvpGameMenu;

/* 初始化：拷贝标签，默认把所有项设为可用、hover/selected = -1。 */
void fvp_gamemenu_init(FvpGameMenu *m, const char *const *labels, int count,
                       int y, int pitch);

/* 按"居中到 client_w"重新算 x 与 width（width = 最长标签宽度 + 内边距）。 */
void fvp_gamemenu_layout(FvpGameMenu *m, int client_w, int text_width,
                         int pad);

/* 打开/关闭。打开时 hover = 0（第一项），selected 保持。 */
void fvp_gamemenu_open(FvpGameMenu *m);
void fvp_gamemenu_close(FvpGameMenu *m);

/* 命中测试：返回项下标，未命中返回 -1；只认可用项。 */
int fvp_gamemenu_hit(const FvpGameMenu *m, int mx, int my);

/* 鼠标移动更新 hover（不走边界时保持原状）。 */
void fvp_gamemenu_move(FvpGameMenu *m, int mx, int my);

/* 键盘导航：dir = -1 上、+1 下；跳过不可用项。返回新的 hover。 */
int fvp_gamemenu_nav(FvpGameMenu *m, int dir);

/* 激活当前 hover（或指定坐标）：返回下标并记 selected，未命中返回 -1。 */
int fvp_gamemenu_activate(FvpGameMenu *m);
int fvp_gamemenu_click(FvpGameMenu *m, int mx, int my);

/* 第 i 项的墨迹上沿（供绘制）。 */
int fvp_gamemenu_item_y(const FvpGameMenu *m, int i);

#ifdef __cplusplus
}
#endif

#endif /* FVP_GAMEMENU_H */