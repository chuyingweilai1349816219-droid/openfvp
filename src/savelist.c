/* FVP engine -- 读档列表（只读，见 savelist.h）。 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "fvp.h"
#include "save.h"
#include "savelist.h"
#include "savetail.h"

void fvp_savelist_init(FvpSaveList *l)
{
    if (l == NULL) {
        return;
    }
    memset(l, 0, sizeof(*l));
    l->hover = -1;
    l->selected = -1;
    l->x = 120;
    l->y = 150;
    l->row_h = 64;
    l->width = 1680;
    l->thumb_w = FVP_SAVE_THUMB_W_DEFAULT;
    l->thumb_h = FVP_SAVE_THUMB_H_DEFAULT;
}

void fvp_savelist_set_thumb_size(FvpSaveList *l, int w, int h)
{
    if (l == NULL) {
        return;
    }
    l->thumb_w = w > 0 ? w : 0;
    l->thumb_h = h > 0 ? h : 0;
}

static unsigned char *read_whole_file(const char *path, size_t *len)
{
    FILE *f;
    long n;
    unsigned char *buf;

    if (path == NULL || len == NULL) {
        return NULL;
    }
    f = fopen(path, "rb");
    if (f == NULL) {
        return NULL;
    }
    if (fseek(f, 0, SEEK_END) != 0) {
        fclose(f);
        return NULL;
    }
    n = ftell(f);
    if (n <= 0 || fseek(f, 0, SEEK_SET) != 0) {
        fclose(f);
        return NULL;
    }
    buf = (unsigned char *)fvp_xmalloc((size_t)n);
    if (fread(buf, 1, (size_t)n, f) != (size_t)n) {
        free(buf);
        fclose(f);
        return NULL;
    }
    fclose(f);
    *len = (size_t)n;
    return buf;
}

/* 把原编码字节解成 UTF-8 放进定长缓冲（截断时保证不切半个字符）。 */
static void decode_into(char *dst, size_t cap, FvpEncoding enc,
                        const unsigned char *src, unsigned int n)
{
    FvpArena arena;
    FvpStr *s;
    size_t take;

    if (dst == NULL || cap == 0) {
        return;
    }
    dst[0] = '\0';
    if (src == NULL || n == 0) {
        return;
    }
    fvp_arena_init(&arena);
    s = fvp_decode(&arena, enc, src, n);
    if (s != NULL) {
        take = s->len;
        while (take > 0 && take + 1 > cap - 1) {
            take -= 1;
        }
        /* 简单截断：回退到 UTF-8 码点边界，避免半个字符 */
        while (take > 0 && ((unsigned char)s->data[take] & 0xC0) == 0x80) {
            take -= 1;
        }
        if (take > cap - 1) {
            take = cap - 1;
        }
        memcpy(dst, s->data, take);
        dst[take] = '\0';
    }
    fvp_arena_free(&arena);
}

int fvp_savelist_add_slot_bytes(FvpSaveList *l, const unsigned char *buf,
                               size_t len, int slot, FvpEncoding enc)
{
    FvpSaveSlot s;
    FvpSaveEntry *e;

    if (l == NULL || buf == NULL || len == 0 ||
        l->count >= FVP_SAVELIST_MAX) {
        return 0;
    }
    memset(&s, 0, sizeof(s));
    if (!fvp_save_slot_load(&s, buf, len, l->thumb_w, l->thumb_h)) {
        return 0;
    }
    e = &l->items[l->count];
    memset(e, 0, sizeof(*e));
    e->slot = slot;
    e->year = s.year;
    e->month = s.month;
    e->day = s.day;
    e->hour = s.hour;
    e->minute = s.minute;
    decode_into(e->chapter, sizeof(e->chapter), enc, s.str[0], s.len[0]);
    decode_into(e->line, sizeof(e->line), enc, s.str[2], s.len[2]);
    if (s.tail != NULL && s.tail_len > 0) {
        FvpSaveTailInfo info;

        if (fvp_savetail_parse(&info, s.tail, s.tail_len, enc) &&
            info.graph_index >= 0) {
            strncpy(e->graph, info.str[info.graph_index], sizeof(e->graph) - 1);
            e->graph[sizeof(e->graph) - 1] = '\0';
        }
    }
    e->has_thumb = s.thumb != NULL;
    e->file_bytes = len;
    l->count += 1;
    return 1;
}

int fvp_savelist_add_slot_file(FvpSaveList *l, const char *path, int slot,
                               FvpEncoding enc)
{
    unsigned char *buf;
    size_t len = 0;
    int ok;

    if (l == NULL) {
        return 0;
    }
    buf = read_whole_file(path, &len);
    if (buf == NULL) {
        return 0;
    }
    ok = fvp_savelist_add_slot_bytes(l, buf, len, slot, enc);
    free(buf);
    return ok;
}

char *fvp_savelist_row_text(const FvpSaveList *l, int i, char *buf,
                            size_t cap)
{
    const FvpSaveEntry *e;

    if (buf == NULL || cap == 0) {
        return buf;
    }
    buf[0] = '\0';
    if (l == NULL || i < 0 || i >= l->count) {
        return buf;
    }
    e = &l->items[i];
    if (e->year > 0 && e->month >= 1 && e->month <= 12) {
        char g[32];
        char c[FVP_SAVELIST_CHAPTER_MAX + 4];

        if (e->graph[0] != '\0') {
            snprintf(g, sizeof g, "[%s] ", e->graph);
        } else {
            g[0] = '\0';
        }
        snprintf(c, sizeof c, "%s%s", g,
                 e->chapter[0] != '\0' ? e->chapter : "-");
        snprintf(buf, cap, "No.%03d   %04u/%02u/%02u %02u:%02u   %s", e->slot,
                 (unsigned)e->year, (unsigned)e->month, (unsigned)e->day,
                 (unsigned)e->hour, (unsigned)e->minute, c);
    } else {
        snprintf(buf, cap, "No.%03d   (no timestamp)   %s", e->slot,
                 e->chapter[0] != '\0' ? e->chapter : "-");
    }
    return buf;
}

int fvp_savelist_hit(const FvpSaveList *l, int mx, int my)
{
    int i;

    if (l == NULL || l->count <= 0 || l->row_h <= 0) {
        return -1;
    }
    if (mx < l->x || mx > l->x + l->width) {
        return -1;
    }
    for (i = 0; i < l->count; ++i) {
        int top = l->y + i * l->row_h;

        if (my >= top && my < top + l->row_h) {
            return i;
        }
    }
    return -1;
}