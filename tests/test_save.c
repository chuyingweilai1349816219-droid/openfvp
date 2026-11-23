/* 存档层测试：无参数跑自检（合成数据）；带文件参数跑真实样本回环。
 *
 *   bin/test_save.exe
 *   bin/test_save.exe --savebin <save.bin> <sNNN.bin ...>
 *
 * 真实样本的验收标准：**读入再写出必须与原文件逐字节相同**（尾段原样透传），
 * 否则说明解析边界错了。
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../src/save.h"

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

static unsigned char *read_file(const char *path, size_t *len_out)
{
    FILE *f;
    long n;
    unsigned char *buf;

    *len_out = 0;
    f = fopen(path, "rb");
    if (f == NULL) {
        return NULL;
    }
    if (fseek(f, 0, SEEK_END) != 0) {
        fclose(f);
        return NULL;
    }
    n = ftell(f);
    if (n <= 0) {
        fclose(f);
        return NULL;
    }
    rewind(f);
    buf = (unsigned char *)malloc((size_t)n);
    if (buf == NULL) {
        fclose(f);
        return NULL;
    }
    if (fread(buf, 1, (size_t)n, f) != (size_t)n) {
        free(buf);
        fclose(f);
        return NULL;
    }
    fclose(f);
    *len_out = (size_t)n;
    return buf;
}

static unsigned char *dup_bytes(const char *s, size_t n)
{
    unsigned char *p = (unsigned char *)malloc(n);

    if (p != NULL) {
        memcpy(p, s, n);
    }
    return p;
}

/* ------------------------------------------------------------- 自检 */

static void selftest(void)
{
    FvpSaveSlot s;
    FvpSaveSlot t;
    unsigned char *buf;
    size_t need;
    size_t wrote;
    int i;

    /* 1. 合成一个槽：时间戳 + 三条字符串（含 CP932/GBK 混合字节）+ 2x2 缩略图。 */
    memset(&s, 0, sizeof(s));
    s.year = 2023;
    s.month = 3;
    s.day = 19;
    s.wday = 0;
    s.hour = 23;
    s.minute = 43;
    s.str[0] = dup_bytes("\x83\x76\x83\x8d\x83\x8d\x81\x5b\x83\x4f", 10); /* プロローグ */
    s.len[0] = 10;
    s.str[2] = dup_bytes("\xa1\xa1\xa1\xad\xa1\xad\xa1\xa3", 8); /* 　……。 */
    s.len[2] = 8;
    s.thumb_w = 2;
    s.thumb_h = 2;
    s.thumb = (unsigned char *)malloc(16);
    for (i = 0; i < 16; ++i) {
        s.thumb[i] = (unsigned char)(i & 0xFF);
    }
    for (i = 3; i < 16; i += 4) {
        s.thumb[i] = 0xFF;
    }
    s.tail = (unsigned char *)malloc(5);
    memcpy(s.tail, "\x01\x02\x03\x04\x01", 5);
    s.tail_len = 5;

    need = fvp_save_slot_size(&s);
    CHECK(need == 7u + 2u + 10u + 2u + 0u + 2u + 8u + 16u + 5u, "size=%zu", need);
    buf = (unsigned char *)malloc(need);
    wrote = fvp_save_slot_store(&s, buf, need);
    CHECK(wrote == need, "store wrote=%zu need=%zu", wrote, need);

    /* 2. 写出的字节必须能被自己读回来，且字段一致。 */
    memset(&t, 0, sizeof(t));
    CHECK(fvp_save_slot_load(&t, buf, need, 2, 2), "reload failed");
    CHECK(t.year == 2023 && t.month == 3 && t.day == 19 && t.wday == 0 &&
              t.hour == 23 && t.minute == 43,
          "timestamp mismatch");
    CHECK(t.len[0] == 10 && memcmp(t.str[0], s.str[0], 10) == 0, "str0 mismatch");
    CHECK(t.len[1] == 0 && t.str[1] == NULL, "str1 should be empty");
    CHECK(t.len[2] == 8 && memcmp(t.str[2], s.str[2], 8) == 0, "str2 mismatch");
    CHECK(t.thumb_w == 2 && t.thumb_h == 2 && t.thumb != NULL, "thumb size");
    CHECK(memcmp(t.thumb, s.thumb, 16) == 0, "thumb bytes");
    CHECK(t.tail_len == 5 && memcmp(t.tail, s.tail, 5) == 0, "tail bytes");

    /* 3. 再写出一次，必须与第一次逐字节相同（幂等）。 */
    {
        unsigned char *buf2 = (unsigned char *)malloc(need);

        CHECK(buf2 != NULL, "oom");
        if (buf2 != NULL) {
            CHECK(fvp_save_slot_store(&t, buf2, need) == need, "restore size");
            CHECK(memcmp(buf, buf2, need) == 0, "round-trip not idempotent");
            free(buf2);
        }
    }

    /* 3b. 写盘回环：序列化 → 写文件 → 读回 → 逐字节一致（存档落盘路径）。 */
    {
        size_t wr = fvp_save_slot_write_file(&s, "out/test_slot_tmp.bin");

        CHECK(wr == need, "write_file wrote %zu, need %zu", wr, need);
        if (wr == need) {
            size_t rlen = 0;
            unsigned char *rb = read_file("out/test_slot_tmp.bin", &rlen);

            CHECK(rb != NULL, "cannot read back out/test_slot_tmp.bin");
            if (rb != NULL) {
                CHECK(rlen == need, "readback size %zu != %zu", rlen, need);
                CHECK(memcmp(rb, buf, need) == 0, "readback bytes differ");
                free(rb);
            }
            remove("out/test_slot_tmp.bin");
        }
        CHECK(fvp_save_slot_write_file(NULL, "out/x.bin") == 0,
              "write_file(NULL slot) must fail");
        CHECK(fvp_save_slot_write_file(&s, NULL) == 0,
              "write_file(NULL path) must fail");
    }

    /* 4. 修改时间戳与 str2，尾段必须原样保留。 */
    t.year = 2026;
    t.minute = 7;
    free(t.str[2]);
    t.str[2] = dup_bytes("hello", 5);
    t.len[2] = 5;
    CHECK(t.str[2] != NULL, "oom");
    {
        size_t need2 = fvp_save_slot_size(&t);
        unsigned char *buf3 = (unsigned char *)malloc(need2);
        FvpSaveSlot u;

        CHECK(buf3 != NULL, "oom");
        if (buf3 != NULL) {
            CHECK(fvp_save_slot_store(&t, buf3, need2) == need2, "store2");
            memset(&u, 0, sizeof(u));
            CHECK(fvp_save_slot_load(&u, buf3, need2, 2, 2), "reload2");
            CHECK(u.year == 2026 && u.minute == 7, "new timestamp");
            CHECK(u.len[2] == 5 && memcmp(u.str[2], "hello", 5) == 0, "new str2");
            CHECK(u.tail_len == 5 && memcmp(u.tail, "\x01\x02\x03\x04\x01", 5) == 0,
                  "tail lost after edit");
            fvp_save_slot_free(&u);
            free(buf3);
        }
    }

    /* 5. 畸形输入必须被拒（不崩、不越界）。 */
    CHECK(!fvp_save_slot_load(&t, buf, 3, 2, 2), "3-byte buffer should fail");
    CHECK(!fvp_save_slot_load(&t, buf, 7, 2, 2), "header-only should fail");
    CHECK(!fvp_save_slot_load(&t, buf, 9, 2, 2), "truncated string len");
    /* 截到缩略图中途：应当失败（尾段可短，缩略图不可短）。 */
    CHECK(!fvp_save_slot_load(&t, buf, 46, 2, 2), "truncated thumb");
    CHECK(!fvp_save_slot_load(&t, NULL, 0, 2, 2), "NULL buffer");
    /* 长度前缀指向缓冲之外：把 str0 长度改成 0xFFFF。 */
    {
        unsigned char *bad = (unsigned char *)malloc(need);

        CHECK(bad != NULL, "oom");
        if (bad != NULL) {
            memcpy(bad, buf, need);
            bad[7] = 0xFF;
            bad[8] = 0xFF;
            CHECK(!fvp_save_slot_load(&t, bad, need, 2, 2), "bogus strlen accepted");
            free(bad);
        }
    }

    /* 6. alpha 推断：32x18 全 alpha=0xFF 的缩略图应被认出来。 */
    {
        int gw = 0;
        int gh = 0;
        const size_t pix = 32u * 18u;
        const size_t off = 7u + 2u + 10u + 2u + 2u + 8u; /* 头部 + 三条字符串 */
        unsigned char *gbuf = (unsigned char *)malloc(off + pix * 4u + 8u);
        size_t gi;

        CHECK(gbuf != NULL, "oom");
        if (gbuf != NULL) {
            memset(gbuf, 0, off + pix * 4u + 8u);
            for (gi = 0; gi < pix; ++gi) {
                gbuf[off + gi * 4u + 3u] = 0xFF;
            }
            CHECK(fvp_save_thumb_size_guess(gbuf, off + pix * 4u, off, &gw, &gh),
                  "guess failed");
            /* 启发式只能定出像素总数，矩形分解不唯一（576 = 36x16 = 32x18 =
             * 24x24），所以只要求面积对、且落在 SaveThumbSize 的钳制范围里。 */
            CHECK((size_t)gw * (size_t)gh == pix, "guess area %dx%d", gw, gh);
            CHECK(gw >= 20 && gw <= 200 && gh >= 15 && gh <= 150,
                  "guess out of range %dx%d", gw, gh);
            free(gbuf);
        }
    }

    /* 7. 空槽：三条字符串全空 + 无尾段也要能来回。 */
    {
        FvpSaveSlot e;
        unsigned char ebuf[64];
        size_t n;

        memset(&e, 0, sizeof(e));
        e.year = 2024;
        n = fvp_save_slot_size(&e);
        CHECK(n == 7u + 6u, "empty slot size=%zu", n);
        CHECK(fvp_save_slot_store(&e, ebuf, sizeof(ebuf)) == n, "empty store");
        CHECK(!fvp_save_slot_load(&t, ebuf, n, 0, 0),
              "no thumb and nothing to guess -> should fail");
    }

    /* 8. save.bin 合成回环。 */
    {
        FvpSaveBin b;
        FvpSaveBin c;
        unsigned char *sbuf;
        size_t sneed;

        memset(&b, 0, sizeof(b));
        b.nglobals = 4;
        b.globals = (unsigned char *)calloc(4, 8);
        b.readmap = (unsigned char *)calloc(FVP_SAVE_READMAP_SIZE, 1);
        CHECK(b.globals != NULL && b.readmap != NULL, "oom");
        b.globals[0] = 2;
        b.globals[4] = 5; /* 第 0 条 value = 5 */
        b.hdr[12] = (unsigned char)(200 & 0xFF);
        b.hdr[16] = (unsigned char)(113 & 0xFF);
        b.readmap[0] = 0x01;
        b.flags[7] = 0x80;
        b.tail_u32 = 9;
        b.font = dup_bytes("\x83\x81\x83\x43\x83\x8a\x83\x49", 8); /* メイリオ */
        b.font_len = 8;
        CHECK(b.font != NULL && b.globals != NULL && b.readmap != NULL, "oom");

        sneed = fvp_save_bin_size(&b);
        CHECK(sneed == 4u * 8u + 20u + FVP_SAVE_READMAP_SIZE + 256u + 4u + 2u + 9u,
              "savebin size=%zu", sneed);
        sbuf = (unsigned char *)malloc(sneed);
        CHECK(sbuf != NULL, "oom");
        if (sbuf != NULL) {
            int loaded;

            CHECK(fvp_save_bin_store(&b, sbuf, sneed) == sneed, "savebin store");
            memset(&c, 0, sizeof(c));
            loaded = fvp_save_bin_load(&c, sbuf, sneed);
            CHECK(loaded, "savebin load");
            if (loaded) {
                CHECK(c.nglobals == 4, "nglobals=%u", c.nglobals);
                CHECK(c.globals[0] == 2 && c.globals[4] == 5, "globals bytes");
                CHECK(c.hdr[12] == 200 && c.hdr[16] == 113, "hdr thumb size");
                CHECK(c.readmap[0] == 0x01, "readmap");
                CHECK(c.flags[7] == 0x80, "flags");
                CHECK(c.tail_u32 == 9, "tail_u32");
                CHECK(c.font_len == 8 && memcmp(c.font, b.font, 8) == 0, "font");
                {
                    unsigned char *sbuf2 = (unsigned char *)malloc(sneed);

                    CHECK(sbuf2 != NULL, "oom");
                    if (sbuf2 != NULL) {
                        CHECK(fvp_save_bin_store(&c, sbuf2, sneed) == sneed,
                              "restore");
                        CHECK(memcmp(sbuf, sbuf2, sneed) == 0,
                              "savebin idempotent");
                        free(sbuf2);
                    }
                }
            }
            fvp_save_bin_free(&c);
            free(sbuf);
        }
        fvp_save_bin_free(&b);
    }

    fvp_save_slot_free(&t);
    fvp_save_slot_free(&s);
    free(buf);
}

/* ------------------------------------------------------- 真实样本回环 */

static int run_files(const char *savebin_path, char **slots, int nslots)
{
    FvpSaveBin b;
    unsigned char *bbuf = NULL;
    size_t blen = 0;
    int thumb_w = 0;
    int thumb_h = 0;
    int i;

    memset(&b, 0, sizeof(b));
    if (savebin_path != NULL) {
        bbuf = read_file(savebin_path, &blen);
        CHECK(bbuf != NULL, "cannot read %s", savebin_path);
        if (bbuf != NULL) {
            CHECK(fvp_save_bin_load(&b, bbuf, blen), "save.bin parse failed");
            thumb_w = (int)((unsigned int)b.hdr[12] |
                            ((unsigned int)b.hdr[13] << 8) |
                            ((unsigned int)b.hdr[14] << 16) |
                            ((unsigned int)b.hdr[15] << 24));
            thumb_h = (int)((unsigned int)b.hdr[16] |
                            ((unsigned int)b.hdr[17] << 8) |
                            ((unsigned int)b.hdr[18] << 16) |
                            ((unsigned int)b.hdr[19] << 24));
            printf("save.bin   %zu bytes, %u globals, thumb %dx%d, font %u bytes\n",
                   blen, b.nglobals, thumb_w, thumb_h, b.font_len);
        }
    }

    /* 文件级便利函数：目录查找 + 直接读文件（桌面外壳启动时用这两个）。 */
    if (savebin_path != NULL) {
        char game_dir[1024];
        char *found;
        const char *cut = NULL;
        const char *p;

        for (p = savebin_path; *p != '\0'; ++p) {
            if (*p == '/' || *p == '\\') {
                cut = p;
            }
        }
        if (cut != NULL) {
            size_t n = (size_t)(cut - savebin_path);

            if (n + 1u < sizeof(game_dir)) {
                memcpy(game_dir, savebin_path, n);
                game_dir[n] = '\0';
                /* 丢掉末尾的 save/ 这一段 */
                cut = NULL;
                for (p = game_dir; *p != '\0'; ++p) {
                    if (*p == '/' || *p == '\\') {
                        cut = p;
                    }
                }
                if (cut != NULL) {
                    game_dir[(size_t)(cut - game_dir)] = '\0';
                }
                found = fvp_save_find_file(game_dir);
                CHECK(found != NULL, "find_file(%s) = NULL", game_dir);
                if (found != NULL) {
                    FvpSaveBin c;

                    CHECK(fvp_save_bin_load_file(&c, found),
                          "load_file(%s) failed", found);
                    CHECK(c.nglobals == b.nglobals,
                          "load_file globals %u != %u", c.nglobals,
                          b.nglobals);
                    fvp_save_bin_free(&c);
                    free(found);
                }
            }
        }
        CHECK(fvp_save_find_file("Z:\\definitely\\missing\\dir") == NULL,
              "find_file on a missing dir should be NULL");
        CHECK(!fvp_save_bin_load_file(&b, "Z:\\missing\\save.bin"),
              "load_file on a missing path should fail");
    }

    for (i = 0; i < nslots; ++i) {
        unsigned char *buf;
        unsigned char *out;
        size_t len = 0;
        size_t need;
        FvpSaveSlot s;

        buf = read_file(slots[i], &len);
        if (buf == NULL) {
            CHECK(0, "cannot read %s", slots[i]);
            continue;
        }
        memset(&s, 0, sizeof(s));
        if (!fvp_save_slot_load(&s, buf, len, thumb_w, thumb_h)) {
            CHECK(0, "%s: parse failed", slots[i]);
            free(buf);
            continue;
        }
        need = fvp_save_slot_size(&s);
        out = (unsigned char *)malloc(need);
        if (out == NULL) {
            CHECK(0, "%s: oom", slots[i]);
            fvp_save_slot_free(&s);
            free(buf);
            continue;
        }
        CHECK(fvp_save_slot_store(&s, out, need) == need, "%s: store size",
              slots[i]);
        CHECK(need == len, "%s: size %zu != %zu", slots[i], need, len);
        CHECK(memcmp(buf, out, len) == 0, "%s: bytes differ", slots[i]);
        /* 边界合理性：字段一旦读错位，下面的断言几乎必然崩。
         * 这也是「把 save.bin 误当槽文件」的自动拦截（它的年字段会是 2）。 */
        CHECK(s.year >= 1990 && s.year <= 2200 && s.month >= 1 && s.month <= 12 &&
                  s.day >= 1 && s.day <= 31 && s.hour < 24 && s.minute < 60,
              "%s: implausible timestamp %04u-%02u-%02u %02u:%02u", slots[i],
              s.year, s.month, s.day, s.hour, s.minute);
        CHECK(s.thumb_w > 0 && s.thumb_h > 0 && s.thumb != NULL,
              "%s: no thumbnail", slots[i]);
        CHECK(s.tail_len > 0, "%s: empty engine-state tail (bad boundary)",
              slots[i]);
        if (thumb_w > 0 && thumb_h > 0) {
            CHECK(s.thumb_w == thumb_w && s.thumb_h == thumb_h,
                  "%s: thumb %dx%d != save.bin %dx%d", slots[i], s.thumb_w,
                  s.thumb_h, thumb_w, thumb_h);
        }
        {
            const char *base = slots[i];
            const char *p;

            for (p = slots[i]; *p != '\0'; ++p) {
                if (*p == '/' || *p == '\\') {
                    base = p + 1;
                }
            }
            printf("%-12s %7zu B  %04u-%02u-%02u %02u:%02u  thumb %dx%d  str "
                   "%u/%u/%u  tail %zu\n",
                   base, len, s.year, s.month, s.day, s.hour, s.minute,
                   s.thumb_w, s.thumb_h, s.len[0], s.len[1], s.len[2],
                   s.tail_len);
        }
        free(out);
        fvp_save_slot_free(&s);
        free(buf);
    }

    /* 「保守写档」实测：拿第一个真实槽当模板，只改已确证字段（时间戳 / 字符串 /
     * 缩略图），写完读回验证 —— 其余字节（可变长中段 + 常量尾）必须与模板**逐字节相同**。 */
    if (nslots > 0) {
        const char *tmpl = slots[0];
        const char *out_path = "out/test_save_template.bin";
        unsigned char *buf;
        size_t len = 0;
        FvpSaveSlot s;
        size_t delta;

        buf = read_file(tmpl, &len);
        CHECK(buf != NULL, "template: cannot read %s", tmpl);
        if (buf != NULL) {
            memset(&s, 0, sizeof(s));
            CHECK(fvp_save_slot_load(&s, buf, len, thumb_w, thumb_h),
                  "template: parse failed");
            if (s.tail != NULL) {
                unsigned char *old_tail = s.tail;
                size_t old_tail_len = s.tail_len;
                size_t new_len;
                unsigned char *fresh;

                /* 只动已确证字段。 */
                s.year = 2026;
                s.month = 1;
                s.day = 2;
                s.wday = 5;
                s.hour = 3;
                s.minute = 4;
                free(s.str[0]);
                s.str[0] = dup_bytes("\x83\x65\x83\x58\x83\x67", 6); /* テスト */
                s.len[0] = 6;
                if (s.thumb != NULL) {
                    size_t n = (size_t)s.thumb_w * (size_t)s.thumb_h * 4u;
                    size_t k;

                    for (k = 0; k < n; k += 4u) {
                        s.thumb[k] = (unsigned char)(k & 0xFFu);
                        s.thumb[k + 1] = 0x10;
                        s.thumb[k + 2] = 0x20;
                        s.thumb[k + 3] = 0xFF;
                    }
                }

                /* 写盘 → 读回。 */
                new_len = fvp_save_slot_write_file(&s, out_path);
                CHECK(new_len > 0, "template: write_file failed");
                fresh = new_len > 0 ? read_file(out_path, &new_len) : NULL;
                CHECK(fresh != NULL, "template: cannot read back %s", out_path);
                if (fresh != NULL) {
                    FvpSaveSlot r;

                    memset(&r, 0, sizeof(r));
                    CHECK(fvp_save_slot_load(&r, fresh, new_len, thumb_w,
                                             thumb_h),
                          "template: reload failed");
                    CHECK(r.year == 2026 && r.month == 1 && r.day == 2 &&
                              r.wday == 5 && r.hour == 3 && r.minute == 4,
                          "template: timestamp not applied");
                    CHECK(r.len[0] == 6 && r.str[0] != NULL &&
                              memcmp(r.str[0], "\x83\x65\x83\x58\x83\x67",
                                     6) == 0,
                          "template: str0 not applied");
                    CHECK(r.tail_len == old_tail_len &&
                              memcmp(r.tail, old_tail, old_tail_len) == 0,
                          "template: engine-state tail changed "
                          "(%zu vs %zu bytes)",
                          r.tail_len, old_tail_len);
                    if (r.thumb != NULL) {
                        CHECK(r.thumb[0] == 0x00 && r.thumb[1] == 0x10 &&
                                  r.thumb[2] == 0x20 && r.thumb[3] == 0xFF,
                              "template: thumbnail not applied");
                    }
                    CHECK(memcmp(r.thumb, s.thumb,
                                 (size_t)r.thumb_w * (size_t)r.thumb_h * 4u) ==
                              0,
                          "template: thumbnail round-trip differs");
                    fvp_save_slot_free(&r);
                    free(fresh);
                }
                (void)delta;
                fvp_save_slot_free(&s);
            } else {
                CHECK(0, "template: tail missing");
                fvp_save_slot_free(&s);
            }
            free(buf);
        }
        remove(out_path);
    }

    fvp_save_bin_free(&b);
    free(bbuf);
    return g_fail == 0;
}

int main(int argc, char **argv)
{
    const char *savebin = NULL;
    char **slots;
    int i;
    int nslots = 0;

    setvbuf(stdout, NULL, _IONBF, 0); /* 崩溃时也要看到进度 */

    slots = (char **)calloc((size_t)argc + 1u, sizeof(char *));
    if (slots == NULL) {
        return 2;
    }
    for (i = 1; i < argc; ++i) {
        if (strcmp(argv[i], "--savebin") == 0 && i + 1 < argc) {
            savebin = argv[++i];
        } else {
            slots[nslots++] = argv[i];
        }
    }

    if (nslots == 0 && savebin == NULL) {
        selftest();
    } else {
        run_files(savebin, slots, nslots);
    }
    free(slots);

    printf("\ntest_save: %d passed, %d failed\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}