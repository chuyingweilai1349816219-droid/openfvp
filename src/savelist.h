/* FVP engine -- 标题菜单里的"读取存档"列表（只读）。
 *
 * 原版点 Continue 会列出存档槽（时间 + 章节名 + 当前行 + 缩略图）。本模块只做
 * **列表这一层**：把 `<game_dir>/save/sNNN.bin` 逐个解析成一条记录，提供排版、
 * 行文本和命中判定；真正的"恢复引擎快照"还没做（docs/save-format.md §2.5：
 * 槽尾 500–597 KB 的引擎状态快照字段语义未解），所以它**不启动脚本**。
 *
 * 目录枚举交给平台层（Win32 用 FindFirstFileW），本文件保持可移植：只吃
 * "一个槽文件路径"，不碰目录。
 */
#ifndef FVP_SAVELIST_H
#define FVP_SAVELIST_H

#include <stddef.h>

#include "encoding.h"

#define FVP_SAVELIST_MAX 32
#define FVP_SAVELIST_CHAPTER_MAX 96
#define FVP_SAVELIST_LINE_MAX 160

typedef struct FvpSaveEntry {
    int slot;                /* 槽号（文件名里的 NNN） */
    unsigned short year;     /* 7 字节时间戳 */
    unsigned char month;
    unsigned char day;
    unsigned char hour;
    unsigned char minute;
    char chapter[FVP_SAVELIST_CHAPTER_MAX]; /* UTF-8，空串 = 没有 */
    char graph[24];          /* 槽尾字符串池里的图名（如 BG063_000b），空串 = 没有 */
    char line[FVP_SAVELIST_LINE_MAX];       /* UTF-8，空串 = 没有 */
    int has_thumb;
    size_t file_bytes;
} FvpSaveEntry;

typedef struct FvpSaveList {
    FvpSaveEntry items[FVP_SAVELIST_MAX];
    int count;
    int hover;
    int selected;
    /* 排版（引擎侧 UI 的 1920x1080 客户坐标） */
    int x;
    int y;
    int row_h;
    int width;
    /* 缩略图尺寸（来自 save.bin 的 20 字节头；0 = 让解析器自己猜） */
    int thumb_w;
    int thumb_h;
} FvpSaveList;

/* 清空 + 默认排版：x=120, y=150, row_h=64, width=1680。 */
void fvp_savelist_init(FvpSaveList *l);

/* 设定缩略图尺寸（默认 200x113，与原作 save.bin 头一致；0,0 = 让解析器猜）。 */
void fvp_savelist_set_thumb_size(FvpSaveList *l, int w, int h);

/* 读一个槽文件并追加一条记录。成功返回 1；文件读不了/格式不符返回 0。
 * chapter/line 按 enc 解码成 UTF-8（越界截断）。
 * 平台层如果自己用宽字符路径读文件（Windows 下 CJK 路径只能这么读），
 * 可以直接调 fvp_savelist_add_slot_bytes()。 */
int fvp_savelist_add_slot_file(FvpSaveList *l, const char *path, int slot,
                               FvpEncoding enc);

/* 用已读进内存的槽文件字节追加一条记录（同上，只是不做 I/O）。 */
int fvp_savelist_add_slot_bytes(FvpSaveList *l, const unsigned char *buf,
                                size_t len, int slot, FvpEncoding enc);

/* 第 i 行显示文本："478  2026/11/24 20:36  チャプター名"。
 * 返回 buf（失败/越界返回空串）。 */
char *fvp_savelist_row_text(const FvpSaveList *l, int i, char *buf,
                            size_t cap);

/* 命中判定：行带 y + i*row_h，高 row_h，x 在 [x, x+width]。
 * 返回 0..count-1，未命中 -1。 */
int fvp_savelist_hit(const FvpSaveList *l, int mx, int my);

#endif /* FVP_SAVELIST_H */