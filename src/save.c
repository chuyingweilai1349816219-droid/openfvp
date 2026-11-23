/* FVP 存档层实现。见 save.h 顶部的设计原则。 */
#include "save.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <windows.h>
#endif

/* ------------------------------------------------------------ 小工具 */

static unsigned int rd_u16(const unsigned char *p)
{
    return (unsigned int)p[0] | ((unsigned int)p[1] << 8);
}

static unsigned int rd_u32(const unsigned char *p)
{
    return (unsigned int)p[0] | ((unsigned int)p[1] << 8) |
           ((unsigned int)p[2] << 16) | ((unsigned int)p[3] << 24);
}

static void wr_u16(unsigned char *p, unsigned int v)
{
    p[0] = (unsigned char)(v & 0xFFu);
    p[1] = (unsigned char)((v >> 8) & 0xFFu);
}

static void wr_u32(unsigned char *p, unsigned int v)
{
    p[0] = (unsigned char)(v & 0xFFu);
    p[1] = (unsigned char)((v >> 8) & 0xFFu);
    p[2] = (unsigned char)((v >> 16) & 0xFFu);
    p[3] = (unsigned char)((v >> 24) & 0xFFu);
}

static unsigned char *dup_bytes(const unsigned char *src, size_t n)
{
    unsigned char *p;

    if (n == 0) {
        return NULL;
    }
    p = (unsigned char *)malloc(n);
    if (p == NULL) {
        return NULL;
    }
    memcpy(p, src, n);
    return p;
}

void fvp_save_slot_free(FvpSaveSlot *s)
{
    int i;

    if (s == NULL) {
        return;
    }
    for (i = 0; i < FVP_SAVE_STR_COUNT; ++i) {
        free(s->str[i]);
    }
    free(s->thumb);
    free(s->tail);
    memset(s, 0, sizeof(*s));
}

void fvp_save_bin_free(FvpSaveBin *b)
{
    if (b == NULL) {
        return;
    }
    free(b->globals);
    free(b->readmap);
    free(b->font);
    memset(b, 0, sizeof(*b));
}

/* --------------------------------------------------- 缩略图尺寸推断 */

int fvp_save_thumb_size_guess(const unsigned char *buf, size_t len,
                              size_t offset, int *w, int *h)
{
    size_t pixels = 0;
    size_t max_pixels;
    int cand_w;
    int cand_h;

    if (buf == NULL || offset >= len) {
        return 0;
    }
    /* 每 4 字节里的第 4 个（alpha）恒为 0xFF，一路数到不满足为止。 */
    max_pixels = (len - offset) / 4u;
    while (pixels < max_pixels && buf[offset + pixels * 4u + 3u] == 0xFFu) {
        ++pixels;
    }
    if (pixels == 0) {
        return 0;
    }
    /* 把像素数拆成矩形：宽度上限 200、高度上限 150（SaveThumbSize 的钳制范围），
     * 从最宽的开始试，命中即返回（本作 200x113）。 */
    for (cand_w = FVP_SAVE_THUMB_W_DEFAULT; cand_w >= 20; --cand_w) {
        if (pixels % (size_t)cand_w != 0) {
            continue;
        }
        cand_h = (int)(pixels / (size_t)cand_w);
        if (cand_h < 15 || cand_h > 150) {
            continue;
        }
        if (w != NULL) {
            *w = cand_w;
        }
        if (h != NULL) {
            *h = cand_h;
        }
        return 1;
    }
    return 0;
}

/* --------------------------------------------------------- 槽文件解析 */

static void slot_clear(FvpSaveSlot *s)
{
    memset(s, 0, sizeof(*s));
}

int fvp_save_slot_load(FvpSaveSlot *s, const unsigned char *buf, size_t len,
                       int thumb_w, int thumb_h)
{
    size_t pos = 0;
    size_t thumb_bytes = 0;
    int i;

    if (s == NULL) {
        return 0;
    }
    slot_clear(s);
    if (buf == NULL || len < FVP_SAVE_SLOT_HEADER) {
        return 0;
    }

    s->year = (unsigned short)rd_u16(buf + 0); /* 偏移 0：年，u16 */
    s->month = buf[2];
    s->day = buf[3];
    s->wday = buf[4];
    s->hour = buf[5];
    s->minute = buf[6];
    pos = FVP_SAVE_SLOT_HEADER;

    /* 三条 u16 长度前缀字符串。 */
    for (i = 0; i < FVP_SAVE_STR_COUNT; ++i) {
        unsigned int n;

        if (pos + 2u > len) {
            goto fail;
        }
        n = rd_u16(buf + pos);
        pos += 2u;
        if (pos + n > len) {
            goto fail;
        }
        if (n > 0) {
            s->str[i] = dup_bytes(buf + pos, n);
            if (s->str[i] == NULL) {
                goto fail;
            }
        }
        s->len[i] = n;
        pos += n;
    }

    /* 缩略图：尺寸优先用外部给的（save.bin 头里记录的），否则按 alpha 推断。 */
    if (thumb_w > 0 && thumb_h > 0) {
        thumb_bytes = (size_t)thumb_w * (size_t)thumb_h * 4u;
        if (pos + thumb_bytes > len) {
            goto fail;
        }
        s->thumb_w = thumb_w;
        s->thumb_h = thumb_h;
    } else {
        int gw = 0;
        int gh = 0;

        if (!fvp_save_thumb_size_guess(buf, len, pos, &gw, &gh)) {
            goto fail;
        }
        thumb_bytes = (size_t)gw * (size_t)gh * 4u;
        s->thumb_w = gw;
        s->thumb_h = gh;
    }
    s->thumb = dup_bytes(buf + pos, thumb_bytes);
    if (s->thumb == NULL) {
        goto fail;
    }
    pos += thumb_bytes;

    /* 其余部分：引擎状态快照，原样保留。 */
    s->tail_len = len - pos;
    if (s->tail_len > 0) {
        s->tail = dup_bytes(buf + pos, s->tail_len);
        if (s->tail == NULL) {
            goto fail;
        }
    }
    return 1;

fail:
    fvp_save_slot_free(s);
    return 0;
}

size_t fvp_save_slot_size(const FvpSaveSlot *s)
{
    size_t n;
    int i;

    if (s == NULL) {
        return 0;
    }
    n = FVP_SAVE_SLOT_HEADER;
    for (i = 0; i < FVP_SAVE_STR_COUNT; ++i) {
        if (s->len[i] > 0xFFFFu) {
            return 0; /* u16 长度前缀，超了写不出来 */
        }
        n += 2u + (size_t)s->len[i];
    }
    if (s->thumb != NULL && s->thumb_w > 0 && s->thumb_h > 0) {
        n += (size_t)s->thumb_w * (size_t)s->thumb_h * 4u;
    }
    n += s->tail_len;
    return n;
}

size_t fvp_save_slot_store(const FvpSaveSlot *s, unsigned char *out, size_t cap)
{
    size_t need;
    size_t pos = 0;
    int i;

    if (s == NULL || out == NULL) {
        return 0;
    }
    need = fvp_save_slot_size(s);
    if (need == 0 || need > cap) {
        return 0;
    }

    wr_u16(out + 0, s->year);
    out[2] = s->month;
    out[3] = s->day;
    out[4] = s->wday;
    out[5] = s->hour;
    out[6] = s->minute;
    pos = FVP_SAVE_SLOT_HEADER;

    for (i = 0; i < FVP_SAVE_STR_COUNT; ++i) {
        wr_u16(out + pos, s->len[i]);
        pos += 2u;
        if (s->len[i] > 0) {
            memcpy(out + pos, s->str[i], s->len[i]);
            pos += s->len[i];
        }
    }
    if (s->thumb != NULL && s->thumb_w > 0 && s->thumb_h > 0) {
        size_t n = (size_t)s->thumb_w * (size_t)s->thumb_h * 4u;

        memcpy(out + pos, s->thumb, n);
        pos += n;
    }
    if (s->tail_len > 0) {
        memcpy(out + pos, s->tail, s->tail_len);
        pos += s->tail_len;
    }
    return pos;
}

/* ------------------------------------------------------ save.bin 解析 */

int fvp_save_bin_load(FvpSaveBin *b, const unsigned char *buf, size_t len)
{
    size_t fixed;
    unsigned int font_len = 0;
    size_t font_block = 0;
    size_t globals_len;
    unsigned int cand;

    if (b == NULL) {
        return 0;
    }
    memset(b, 0, sizeof(*b));
    if (buf == NULL) {
        return 0;
    }

    /* 固定部分（不含字体名块）：20B 头 + 1MB 已读位图 + 256B Flag + u32。 */
    fixed = 20u + FVP_SAVE_READMAP_SIZE + 256u + 4u;
    if (len < fixed) {
        return 0;
    }

    /* 字体名长度是 i16（含 NUL），紧挨着文件末尾的字符串。从尾部反推：
     * 布局尾部 = u16(长度含NUL) + 字体名 + NUL，所以 u16 在 len-font_len-3；
     * 试 n = 1..64，若该处恰好等于 n+1，就认定它。 */
    for (cand = 1; cand <= 64u; ++cand) {
        size_t at;

        if (len < (size_t)cand + 3u) {
            break;
        }
        at = len - (size_t)cand - 3u;
        if (rd_u16(buf + at) == cand + 1u) {
            font_len = cand;
            font_block = (size_t)cand + 3u; /* u16 + 字体名 + NUL */
            break;
        }
    }
    if (font_len == 0) {
        return 0;
    }

    if (len < fixed + font_block) {
        return 0;
    }
    globals_len = len - fixed - font_block;
    if (globals_len % 8u != 0) {
        return 0; /* 全局变量是 8 字节定长记录 */
    }

    b->nglobals = (unsigned int)(globals_len / 8u);
    if (b->nglobals > 0) {
        b->globals = dup_bytes(buf, globals_len);
        if (b->globals == NULL) {
            goto fail;
        }
    }
    memcpy(b->hdr, buf + globals_len, 20u);
    b->readmap = dup_bytes(buf + globals_len + 20u, FVP_SAVE_READMAP_SIZE);
    if (b->readmap == NULL) {
        goto fail;
    }
    memcpy(b->flags, buf + globals_len + 20u + FVP_SAVE_READMAP_SIZE, 256u);
    {
        size_t at = globals_len + 20u + FVP_SAVE_READMAP_SIZE + 256u;

        b->tail_u32 = rd_u32(buf + at);
        at += 4u;
        /* at 处是 u16 长度（含 NUL），字符串随后。 */
        b->font_len = font_len;
        b->font = dup_bytes(buf + at + 2u, font_len);
        if (b->font == NULL) {
            goto fail;
        }
    }
    return 1;

fail:
    fvp_save_bin_free(b);
    return 0;
}

size_t fvp_save_bin_size(const FvpSaveBin *b)
{
    if (b == NULL || b->font_len > 0x7FFFu) {
        return 0;
    }
    return (size_t)b->nglobals * 8u + 20u + FVP_SAVE_READMAP_SIZE + 256u + 4u +
           2u + (size_t)b->font_len + 1u; /* +1：字体名的 NUL 结尾 */
}

size_t fvp_save_bin_store(const FvpSaveBin *b, unsigned char *out, size_t cap)
{
    size_t need;
    size_t pos = 0;

    if (b == NULL || out == NULL) {
        return 0;
    }
    need = fvp_save_bin_size(b);
    if (need == 0 || need > cap) {
        return 0;
    }
    if (b->nglobals > 0) {
        if (b->globals == NULL) {
            return 0;
        }
        memcpy(out, b->globals, (size_t)b->nglobals * 8u);
        pos = (size_t)b->nglobals * 8u;
    }
    memcpy(out + pos, b->hdr, 20u);
    pos += 20u;
    if (b->readmap == NULL) {
        return 0;
    }
    memcpy(out + pos, b->readmap, FVP_SAVE_READMAP_SIZE);
    pos += FVP_SAVE_READMAP_SIZE;
    memcpy(out + pos, b->flags, 256u);
    pos += 256u;
    wr_u32(out + pos, b->tail_u32);
    pos += 4u;
    wr_u16(out + pos, b->font_len + 1u); /* 长度含 NUL */
    pos += 2u;
    memcpy(out + pos, b->font, b->font_len);
    pos += b->font_len;
    out[pos] = 0x00; /* NUL 结尾 */
    pos += 1u;
    return pos;
}

/* ------------------------------------------------- 文件级便利函数 */

/* 路径按 UTF-8 处理（游戏目录常带中日文，Win32 上必须转宽字符再开文件）。
 * 但调用者也可能拿到 ANSI 路径（例如从 argv 直接来的），所以先按 UTF-8 严格解码，
 * 失败再退回当前 ANSI 代码页——两条路都能开，不至于因为编码猜错就找不到存档。 */
static FILE *fopen_utf8_read(const char *path)
{
#ifdef _WIN32
    wchar_t wpath[1024];
    int n;

    if (path == NULL) {
        return NULL;
    }
    n = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, path, -1, wpath,
                            (int)(sizeof(wpath) / sizeof(wpath[0])));
    if (n <= 0) {
        n = MultiByteToWideChar(CP_ACP, 0, path, -1, wpath,
                                (int)(sizeof(wpath) / sizeof(wpath[0])));
    }
    if (n <= 0) {
        return NULL;
    }
    return _wfopen(wpath, L"rb");
#else
    if (path == NULL) {
        return NULL;
    }
    return fopen(path, "rb");
#endif
}

int fvp_save_bin_load_file(FvpSaveBin *b, const char *path)
{
    FILE *f;
    long n;
    unsigned char *buf;
    int ok;

    if (b == NULL) {
        return 0;
    }
    memset(b, 0, sizeof(*b));
    f = fopen_utf8_read(path);
    if (f == NULL) {
        return 0;
    }
    if (fseek(f, 0, SEEK_END) != 0) {
        fclose(f);
        return 0;
    }
    n = ftell(f);
    if (n <= 0) {
        fclose(f);
        return 0;
    }
    rewind(f);
    buf = (unsigned char *)malloc((size_t)n);
    if (buf == NULL) {
        fclose(f);
        return 0;
    }
    if (fread(buf, 1, (size_t)n, f) != (size_t)n) {
        free(buf);
        fclose(f);
        return 0;
    }
    fclose(f);
    ok = fvp_save_bin_load(b, buf, (size_t)n);
    free(buf);
    return ok;
}

static int file_exists(const char *path)
{
    FILE *f = fopen_utf8_read(path);

    if (f == NULL) {
        return 0;
    }
    fclose(f);
    return 1;
}

char *fvp_save_find_file(const char *game_dir)
{
    size_t need;
    char *buf;
    const char *candidates[] = {"save/save.bin", "save.bin", "save\\save.bin"};
    size_t i;

    if (game_dir == NULL || game_dir[0] == '\0') {
        return NULL;
    }
    need = strlen(game_dir) + 32u;
    buf = (char *)malloc(need);
    if (buf == NULL) {
        return NULL;
    }
    for (i = 0; i < sizeof(candidates) / sizeof(candidates[0]); ++i) {
        int r = snprintf(buf, need, "%s/%s", game_dir, candidates[i]);

        if (r < 0 || (size_t)r >= need) {
            continue;
        }
        if (file_exists(buf)) {
            return buf;
        }
    }
    free(buf);
    return NULL;
}