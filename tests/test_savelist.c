/* 读档列表（savelist）的单元测试。
 *
 *   bin/test_savelist.exe
 *
 * 覆盖：合成一个真实格式的槽文件 → 扫描成一条列表记录 → 时间戳/章节名/当前行
 * 解码正确 → 行文本格式 → 命中判定 → 上限截断 → 坏文件/缺文件/NULL 安全。
 * 需要文件是因为 fvp_savelist_add_slot_file() 就是按"槽文件路径"读的；临时文件
 * 写在 bin/ 下，测试结束删掉。
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../src/save.h"
#include "../src/savelist.h"

static int g_fail = 0;
static int g_pass = 0;

#define CHECK(cond, ...)                                                      \
    do {                                                                      \
        if (cond) {                                                           \
            ++g_pass;                                                         \
        } else {                                                              \
            ++g_fail;                                                         \
            printf("FAIL %s:%d: ", __FILE__, __LINE__);                       \
            printf(__VA_ARGS__);                                              \
            printf("\n");                                                     \
        }                                                                     \
    } while (0)

static unsigned char *dup_bytes(const char *s, size_t n)
{
    unsigned char *p = (unsigned char *)malloc(n);

    if (p != NULL) {
        memcpy(p, s, n);
    }
    return p;
}

/* 合成一个槽文件：时间戳 2026/11/24 20:36、章节「プロローグ」= cp932、
 * 当前行「……お願い」、2x2 缩略图、5 字节尾段。 */
static int write_slot(const char *path, int year, int month, int day, int hour,
                      int minute)
{
    FvpSaveSlot s;
    unsigned char *buf;
    size_t need;
    size_t wrote;
    FILE *f;
    int i;

    memset(&s, 0, sizeof(s));
    s.year = (unsigned short)year;
    s.month = (unsigned char)month;
    s.day = (unsigned char)day;
    s.hour = (unsigned char)hour;
    s.minute = (unsigned char)minute;
    s.str[0] = dup_bytes("\x83\x76\x83\x8d\x83\x8d\x81\x5b\x83\x4f", 10);
    s.len[0] = 10;
    s.str[2] = dup_bytes("\x81\x63\x81\x63\x81\x63\x82\xa8\x8a\xe8\x82\xa2", 12);
    s.len[2] = 12;
    s.thumb_w = 2;
    s.thumb_h = 2;
    s.thumb = (unsigned char *)malloc(16);
    for (i = 0; i < 16; ++i) {
        s.thumb[i] = (unsigned char)(i & 0xFF);
    }
    for (i = 3; i < 16; i += 4) {
        s.thumb[i] = 0xFF;
    }
    s.tail = dup_bytes("\x01\x02\x03\x04\x01", 5);
    s.tail_len = 5;

    need = fvp_save_slot_size(&s);
    buf = (unsigned char *)malloc(need);
    wrote = fvp_save_slot_store(&s, buf, need);
    if (wrote != need) {
        free(buf);
        return 0;
    }
    f = fopen(path, "wb");
    if (f == NULL) {
        free(buf);
        return 0;
    }
    if (fwrite(buf, 1, need, f) != need) {
        fclose(f);
        free(buf);
        return 0;
    }
    fclose(f);
    free(buf);
    return 1;
}

int main(void)
{
    const char *path = "bin/_savelist_slot.bin";
    FvpSaveList list;
    char row[256];
    int i;

    /* --- 1. 初始化：空列表 + 默认排版 --- */
    fvp_savelist_init(&list);
    CHECK(list.count == 0, "empty after init");
    CHECK(list.x == 120 && list.y == 150 && list.row_h == 64 &&
              list.width == 1680,
          "default layout %d,%d row_h=%d w=%d", list.x, list.y, list.row_h,
          list.width);
    CHECK(list.thumb_w == 200 && list.thumb_h == 113,
          "default thumb size %dx%d", list.thumb_w, list.thumb_h);
    CHECK(fvp_savelist_hit(&list, 200, 160) == -1, "empty list never hits");

    /* --- 2. 扫描一个槽：时间戳 / 章节 / 当前行 / 缩略图 --- */
    fvp_savelist_set_thumb_size(&list, 2, 2); /* 合成槽用 2x2 缩略图 */
    CHECK(write_slot(path, 2026, 11, 24, 20, 36) == 1, "write synthetic slot");
    CHECK(fvp_savelist_add_slot_file(&list, path, 478, FVP_ENC_CP932) == 1,
          "scan slot 478");
    CHECK(list.count == 1, "count=%d", list.count);
    CHECK(list.items[0].slot == 478, "slot=%d", list.items[0].slot);
    CHECK(list.items[0].year == 2026 && list.items[0].month == 11 &&
              list.items[0].day == 24 && list.items[0].hour == 20 &&
              list.items[0].minute == 36,
          "timestamp %u/%u/%u %u:%u", (unsigned)list.items[0].year,
          (unsigned)list.items[0].month, (unsigned)list.items[0].day,
          (unsigned)list.items[0].hour, (unsigned)list.items[0].minute);
    /* cp932 プロローグ → UTF-8 */
    CHECK(strstr(list.items[0].chapter, "\xe3\x83\x97\xe3\x83\xad\xe3\x83\xad"
                                         "\xe3\x83\xbc\xe3\x82\xb0") != NULL,
          "chapter decoded to UTF-8: '%s'", list.items[0].chapter);
    CHECK(list.items[0].line[0] != '\0', "current line captured: '%s'",
          list.items[0].line);
    CHECK(list.items[0].has_thumb == 1, "thumbnail detected");

    /* --- 3. 行文本格式 --- */
    CHECK(fvp_savelist_row_text(&list, 0, row, sizeof(row)) == row,
          "row_text returns buf");
    CHECK(strstr(row, "No.478") != NULL, "row has slot number: '%s'", row);
    CHECK(strstr(row, "2026/11/24 20:36") != NULL, "row has timestamp: '%s'",
          row);
    CHECK(strstr(row, "\xe3\x83\x97\xe3\x83\xad") != NULL,
          "row has chapter: '%s'", row);
    CHECK(fvp_savelist_row_text(&list, 5, row, sizeof(row)) == row &&
              row[0] == '\0',
          "row_text out of range is empty");

    /* --- 4. 命中判定：第 i 行带 y + i*row_h，高 row_h --- */
    CHECK(fvp_savelist_hit(&list, 200, 150) == 0, "hit first row top");
    CHECK(fvp_savelist_hit(&list, 200, 213) == 0, "hit first row bottom");
    CHECK(fvp_savelist_hit(&list, 200, 214) == -1, "miss below first row");
    CHECK(fvp_savelist_hit(&list, 100, 160) == -1, "miss left of list");
    CHECK(fvp_savelist_hit(&list, 1810, 160) == -1, "miss right of list");

    /* --- 5. 多行：第二行命中落在 1 --- */
    CHECK(write_slot(path, 2025, 1, 2, 3, 4) == 1, "write second slot");
    CHECK(fvp_savelist_add_slot_file(&list, path, 12, FVP_ENC_CP932) == 1,
          "scan slot 12");
    CHECK(list.count == 2, "count=%d", list.count);
    CHECK(fvp_savelist_hit(&list, 200, 150 + 64) == 1, "hit second row");
    CHECK(fvp_savelist_hit(&list, 200, 150 + 64 + 63) == 1,
          "hit second row bottom");

    /* --- 6. 上限：最多 FVP_SAVELIST_MAX 条 --- */
    for (i = list.count; i < FVP_SAVELIST_MAX + 4; ++i) {
        CHECK(write_slot(path, 2020, 1, 1, 0, 0) == 1, "write loop slot");
        (void)fvp_savelist_add_slot_file(&list, path, i, FVP_ENC_CP932);
    }
    CHECK(list.count == FVP_SAVELIST_MAX, "capped at %d (got %d)",
          FVP_SAVELIST_MAX, list.count);

    /* --- 7. 坏文件 / 缺文件 / NULL --- */
    CHECK(fvp_savelist_add_slot_file(&list, "bin/_savelist_missing.bin", 1,
                                     FVP_ENC_CP932) == 0,
          "missing file rejected");
    CHECK(fvp_savelist_add_slot_file(NULL, path, 1, FVP_ENC_CP932) == 0,
          "NULL list rejected");
    fvp_savelist_init(NULL); /* 不该崩 */
    CHECK(fvp_savelist_hit(NULL, 0, 0) == -1, "NULL list hit");
    fvp_savelist_row_text(NULL, 0, row, sizeof(row));
    CHECK(row[0] == '\0', "NULL list row_text empty");

    remove(path);
    printf("test_savelist: %d passed, %d failed\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}