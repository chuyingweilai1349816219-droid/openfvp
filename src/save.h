/* FVP 存档层：save/sNNN.bin（存档槽）与 save/save.bin（全局设置）。
 *
 * 只实现格式**已确证**的部分（见 docs/save-format.md）：
 *
 *   槽文件 = 7B 本地时间戳 + 3 条 u16 长度前缀字符串 + w*h*4 的裸 BGRA 缩略图
 *            + 引擎状态快照（本层不解释，按不透明字节原样保留）
 *
 *   save.bin = N×8B 全局变量 + 20B 设置头 + 1MB 已读文本位图
 *              + 256B Flag 位图 + u32 + 字体名
 *
 * 设计原则：**读进来什么字节，写回去就什么字节**。凡是语义未解的段落
 * 一律不解释、不重排，只在长度可推导的前提下原样透传，这样重写存档不会
 * 把尚未搞懂的状态写坏。
 *
 * 字符串按原始字节保存（CP932 / GBK / UTF-8 都可能），不在这里转码：
 * 槽里的章节名是 CP932，而汉化版的当前对话行是 GBK，转码会破坏原字节。
 */
#ifndef FVP_SAVE_H
#define FVP_SAVE_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* 槽文件头长度：u16 年 + 月 + 日 + 星期 + 时 + 分（没有秒）。 */
#define FVP_SAVE_SLOT_HEADER 7
#define FVP_SAVE_STR_COUNT 3
#define FVP_SAVE_THUMB_W_DEFAULT 200
#define FVP_SAVE_THUMB_H_DEFAULT 113

typedef struct FvpSaveSlot {
    /* 7 字节时间戳。 */
    unsigned short year;
    unsigned char month;
    unsigned char day;
    unsigned char wday; /* 0 = 周日 */
    unsigned char hour;
    unsigned char minute;

    /* 3 条长度前缀字符串：0 = 章节名，1 = 未使用，2 = 当前对话行。
     * str[i] 指向 len[i] 字节，不以 NUL 结尾；len 为 0 时 str 为 NULL。 */
    unsigned char *str[FVP_SAVE_STR_COUNT];
    unsigned int len[FVP_SAVE_STR_COUNT];

    /* 缩略图：裸 BGRA，尺寸来自 save.bin 的 20 字节头（本作 200x113）。 */
    int thumb_w;
    int thumb_h;
    unsigned char *thumb; /* thumb_w * thumb_h * 4，可为 NULL（尺寸未知时） */

    /* 缩略图之后的引擎状态快照，原样透传。 */
    unsigned char *tail;
    size_t tail_len;
} FvpSaveSlot;

/* 槽文件路径：引擎里格式串是 **"%s/save/s%03d.bin"**（iroseka_HD.exe 的
 * 0x45da64，见 docs/save-format.md §1/§2.5）：第一个参数是游戏目录，第二个是
 * 槽号 0..999。本函数**照抄**这个拼接方式（不做斜杠去重），返回 1 = 成功，
 * 0 = 参数非法或缓冲区不够。 */
int fvp_save_slot_path(char *buf, size_t cap, const char *game_dir, int slot);

/* 解析一个槽文件缓冲。
 *
 *   thumb_w / thumb_h  已知的缩略图尺寸；传 0 表示未知，此时按「alpha 恒为
 *                      0xFF 的像素连续长度」推断（docs/save-format.md §2.3）。
 *
 * 返回 1 成功、0 失败（格式不符或内存不足）。失败时 s 被清空。 */
int fvp_save_slot_load(FvpSaveSlot *s, const unsigned char *buf, size_t len,
                       int thumb_w, int thumb_h);

/* 序列化所需的字节数（含尾段原样长度）。 */
size_t fvp_save_slot_size(const FvpSaveSlot *s);

/* 序列化到 out（容量 cap）。返回写入字节数，0 表示失败。 */
size_t fvp_save_slot_store(const FvpSaveSlot *s, unsigned char *out, size_t cap);

/* 按 alpha 连续长度推断缩略图尺寸。返回 1 成功、0 失败。 */
int fvp_save_thumb_size_guess(const unsigned char *buf, size_t len,
                              size_t offset, int *w, int *h);

void fvp_save_slot_free(FvpSaveSlot *s);

/* ---------------------------------------------------------------- save.bin */

typedef struct FvpSaveBin {
    unsigned int nglobals;   /* 全局变量条数（本作 1471） */
    unsigned char *globals;  /* nglobals * 8 字节，{u8 type, 3B pad, u32 value} */
    unsigned char hdr[20];   /* 设置头，+12/+16 是缩略图宽高 */
    unsigned char *readmap;  /* 1 MB 已读文本位图 */
    unsigned char flags[256]; /* FlagGet/FlagSet 位图 */
    unsigned int tail_u32;   /* 字体名之前的 u32（语义未解） */
    unsigned char *font;     /* 字体名原始字节（CP932，如 メイリオ） */
    unsigned int font_len;   /* 不含 NUL */
} FvpSaveBin;

/* 已知的位图长度。 */
#define FVP_SAVE_READMAP_SIZE 0x100000u

/* 从文件尾反推布局解析 save.bin（不依赖任何计数器，见 §3.1）。
 * 返回 1 成功、0 失败。 */
int fvp_save_bin_load(FvpSaveBin *b, const unsigned char *buf, size_t len);

size_t fvp_save_bin_size(const FvpSaveBin *b);
size_t fvp_save_bin_store(const FvpSaveBin *b, unsigned char *out, size_t cap);

void fvp_save_bin_free(FvpSaveBin *b);

/* ------------------------------------------------- 文件级便利函数 */

/* 读文件并解析 save.bin。成功返回 1；失败返回 0（b 已清空）。
 * 与 fvp_save_bin_load() 的区别：这个自己 fopen/fread，调用者不用管缓冲。 */
int fvp_save_bin_load_file(FvpSaveBin *b, const char *path);

/* 在一个游戏目录下找全局存档，返回 malloc 出来的路径（调用者 free），
 * 找不到返回 NULL。查找顺序：<game>/save/save.bin、<game>/save.bin。
 * 原版引擎的写死路径是 `%s/save/save.bin`（docs/save-format.md §1），
 * 第二个位置只是给解包过 / 便携版的目录兜底。 */
char *fvp_save_find_file(const char *game_dir);

/* 把槽序列化后写盘（UTF-8 路径）。成功返回写入字节数，0 = 失败。
 * 注意：它写的是调用者给的槽内容，**不会**替你去编码那 3 条字符串。 */
size_t fvp_save_slot_write_file(const FvpSaveSlot *s, const char *path);

#ifdef __cplusplus
}
#endif

#endif /* FVP_SAVE_H */