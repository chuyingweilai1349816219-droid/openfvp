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
#include "../src/savetail.h"

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

/* 合成一个"槽尾里带字符串池"的槽（图名 BG063_000b + 对话行）。 */
static int write_slot_with_graph(const char *path, int year, int month, int day,
                                 int hour, int minute)
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
    s.str[0] = dup_bytes("\x83v\x83\x8d\x83\x8d\x81[\x83O", 10);
    s.len[0] = 10;
    s.thumb_w = 2;
    s.thumb_h = 2;
    s.thumb = (unsigned char *)malloc(16);
    for (i = 0; i < 16; ++i) {
        s.thumb[i] = (unsigned char)(i & 0xFF);
    }
    for (i = 3; i < 16; i += 4) {
        s.thumb[i] = 0xFF;
    }
    /* 槽尾：0x19440 字节，0x19408 处是字符串池 */
    s.tail_len = 0x19440;
    s.tail = (unsigned char *)calloc(1, s.tail_len);
    s.tail[0x19408] = 0x20;
    s.tail[0x1940c] = 1;
    s.tail[0x19410] = 11;
    memcpy(s.tail + 0x19412, "BG063_000b\0", 11);

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

/* str[2] 用 GBK 字节写（汉化文本），str[0] 用 CP932（层数据）——同一个槽里
 * 两条字符串编码不同，调用方只传 FVP_ENC_CP932，靠"看着不对就换编码"纠正。 */
static int write_slot_with_gbk_line(const char *path, int year, int month,
                                    int day, int hour, int minute)
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
    s.str[0] = dup_bytes("\x83v\x83\x8d\x83\x8d\x81[\x83O", 10);
    s.len[0] = 10;
    /* "　谁这样说道。" 的 GBK 字节 */
    s.str[2] = dup_bytes("\xa1\xa1\xcb\xad\xd5\xe2\xd1\xf9\xcb\xb5\xb5\xc0\xa1\xa3", 14);
    s.len[2] = 14;
    s.thumb_w = 2;
    s.thumb_h = 2;
    s.thumb = (unsigned char *)malloc(16);
    for (i = 0; i < 16; ++i) {
        s.thumb[i] = (unsigned char)(i & 0xFF);
    }
    for (i = 3; i < 16; i += 4) {
        s.thumb[i] = 0xFF;
    }
    s.tail_len = 0x19440;
    s.tail = (unsigned char *)calloc(1, s.tail_len);
    s.tail[0x19408] = 0x20;
    s.tail[0x1940c] = 1;
    s.tail[0x19410] = 11;
    memcpy(s.tail + 0x19412, "BG063_000b\0", 11);

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

    /* --- 8. 槽尾字符串池：解析图名 + 对话行（savetail） --- */
    {
        unsigned char *t = (unsigned char *)calloc(1, 0x19440);
        FvpSaveTailInfo info;
        unsigned char *pool;
        size_t off;

        /* tag 0x20 @0x19408, count 2 @0x1940c */
        t[0x19408] = 0x20;
        t[0x1940c] = 2;
        off = 0x19410;
        /*** entry 1: "BG063_000b" + NUL (len 11) ***/
        t[off] = 11;
        memcpy(t + off + 2, "BG063_000b\0", 11);
        off += 2 + 11;
        off += 2; /* 实测条目间有 0 填充 */
        /*** entry 2: cp932 "　……。" + NUL (len 9) ***/
        t[off] = 9;
        memcpy(t + off + 2, "\xa1\xa1\xa1\xad\xa1\xad\xa1\xa3\0", 9);
        (void)pool;

        memset(&info, 0, sizeof(info));
        CHECK(fvp_savetail_parse(&info, t, 0x19440, FVP_ENC_CP932) == 1,
              "savetail parses synthetic pool");
        CHECK(info.count == 2, "savetail count=%d", info.count);
        CHECK(info.graph_index == 0, "savetail graph_index=%d",
              info.graph_index);
        CHECK(strcmp(info.str[0], "BG063_000b") == 0, "graph name '%s'",
              info.str[0]);
        CHECK(strcmp(info.str[1],
                     "\xef\xbd\xa1\xef\xbd\xa1\xef\xbd\xa1\xef\xbd\xad"
                     "\xef\xbd\xa1\xef\xbd\xad\xef\xbd\xa1\xef\xbd\xa3") == 0,
              "dialogue line decoded to UTF-8: '%s'", info.str[1]);

        /* 坏 tag */
        t[0x19408] = 0x21;
        CHECK(fvp_savetail_parse(&info, t, 0x19440, FVP_ENC_CP932) == 0,
              "wrong tag rejected");
        t[0x19408] = 0x20;
        /* count 0 */
        t[0x1940c] = 0;
        CHECK(fvp_savetail_parse(&info, t, 0x19440, FVP_ENC_CP932) == 0,
              "count 0 rejected");
        t[0x1940c] = 2;
        /* count 太大 */
        t[0x1940d] = 1;
        CHECK(fvp_savetail_parse(&info, t, 0x19440, FVP_ENC_CP932) == 0,
              "count > max rejected");
        t[0x1940d] = 0;
        /* tail 太短 */
        CHECK(fvp_savetail_parse(&info, t, 0x19408, FVP_ENC_CP932) == 0,
              "short tail rejected");
        /* NULL 安全 */
        CHECK(fvp_savetail_parse(NULL, t, 0x19440, FVP_ENC_CP932) == 0,
              "NULL out rejected");
        CHECK(fvp_savetail_parse(&info, NULL, 0x19440, FVP_ENC_CP932) == 0,
              "NULL tail rejected");
        free(t);
    }

    /* --- 8.5 编码启发式：CP932 解 GBK 文本会被纠正成真正的汉字 --- */
    {
        const char *p2 = "bin/_savelist_gbk.bin";
        FvpSaveList l2;
        int cjk = 0;
        int kana = 0;
        const unsigned char *q;

        if (write_slot_with_gbk_line(p2, 2023, 3, 19, 23, 44)) {
            fvp_savelist_init(&l2);
            CHECK(fvp_savelist_add_slot_file(&l2, p2, 905,
                                             FVP_ENC_CP932) == 1,
                  "gbk-line slot accepted");
            /* 取第一个非 ASCII 码点：正确解码时应是 CJK（>= 0x4E00），
             * 而不是半角片假名（0xFF61..0xFF9F）。 */
            q = (const unsigned char *)l2.items[0].line;
            while (*q != 0) {
                if ((q[0] & 0xF0) == 0xE0 && (q[1] & 0xC0) == 0x80 &&
                    (q[2] & 0xC0) == 0x80) {
                    unsigned int cp = ((unsigned int)(q[0] & 0x0F) << 12) |
                                      ((unsigned int)(q[1] & 0x3F) << 6) |
                                      (unsigned int)(q[2] & 0x3F);

                    if (cp >= 0x4E00 && cp <= 0x9FFF) {
                        cjk += 1;
                    }
                    if (cp >= 0xFF61 && cp <= 0xFF9F) {
                        kana += 1;
                    }
                    q += 3;
                    continue;
                }
                q += 1;
            }
            CHECK(cjk >= 3 && kana == 0,
                  "gbk line decodes to CJK ideographs (cjk=%d kana=%d, "
                  "line='%s')", cjk, kana, l2.items[0].line);
        } else {
            CHECK(0, "could not write the gbk-line slot");
        }
    }

    /* --- 9. 存档记录里带上图名，行文本要显示出来 --- */
    {
        FvpSaveList l;

        fvp_savelist_init(&l);
        fvp_savelist_set_thumb_size(&l, 2, 2);
        CHECK(write_slot_with_graph(path, 2023, 3, 19, 23, 43) == 1,
              "write slot with pool");
        CHECK(fvp_savelist_add_slot_file(&l, path, 905, FVP_ENC_CP932) == 1,
              "scan slot with pool");
        CHECK(strcmp(l.items[0].graph, "BG063_000b") == 0,
              "entry graph '%s'", l.items[0].graph);
        CHECK(fvp_savelist_row_text(&l, 0, row, sizeof(row)) == row,
              "row text with graph");
        CHECK(strstr(row, "BG063_000b") != NULL, "row shows graph: '%s'", row);
        CHECK(strstr(row, "2023/03/19 23:43") != NULL,
              "row still shows timestamp: '%s'", row);
    }

    remove(path);
    printf("test_savelist: %d passed, %d failed\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}