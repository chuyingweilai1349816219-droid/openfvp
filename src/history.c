/* 台词历史记录（HistorySet 26 / HistoryGet 25）的存储层。
 * 语义与静态分析出处的逐条对照写在 src/history.h 顶部。 */
#include "history.h"

#include <stdlib.h>
#include <string.h>

static void hist_clear_entry(FvpHistoryEntry *e)
{
    free(e->text0);
    free(e->text1);
    e->text0 = NULL;
    e->text1 = NULL;
    e->value = -1;
}

void fvp_history_init(FvpHistory *h)
{
    int i;

    if (h == NULL) {
        return;
    }
    memset(h, 0, sizeof(*h));
    /* 0x442C4A `movl $0x1,(%esi)`：容器构造时 count 就是 **1**（当前这一行
     * 天生算第 1 条），pos = 0（0x442C0C 清 +4）。存档数据可以印证：
     * s481 是还没开过新一条的存档 → count=1/pos=0；s905 开过 10 次 →
     * count=11/pos=10（pos == count-1，与"先 ++ 再写"一致）。 */
    h->count = 1u;
    for (i = 0; i < FVP_HISTORY_SLOTS; ++i) {
        h->entry[i].value = -1;
    }
}

void fvp_history_free(FvpHistory *h)
{
    int i;

    if (h == NULL) {
        return;
    }
    for (i = 0; i < FVP_HISTORY_SLOTS; ++i) {
        hist_clear_entry(&h->entry[i]);
    }
    memset(h, 0, sizeof(*h));
}

int fvp_history_begin(FvpHistory *h)
{
    FvpHistoryEntry *e;

    if (h == NULL) {
        return 0;
    }
    /* 0x43A5F0: count < 100 才自增（写满后不再长） */
    if (h->count < (uint32_t)FVP_HISTORY_SLOTS) {
        h->count += 1u;
    }
    /* 0x43A5FC: pos++，到 100 回绕到 0 */
    h->pos += 1u;
    if (h->pos >= (uint32_t)FVP_HISTORY_SLOTS) {
        h->pos = 0u;
    }
    /* 0x43A627: 新记录 {NULL, NULL, -1} */
    e = &h->entry[h->pos];
    hist_clear_entry(e);
    return 1;
}

/* 把 s 的前 len 个字节拷成一条独立的 C 字符串（截到 FVP_HISTORY_TEXT_MAX）。
 * 返回 malloc 的缓冲，失败返回 NULL。 */
static char *hist_dup(const char *s, size_t len)
{
    char *out;

    if (len > (size_t)FVP_HISTORY_TEXT_MAX) {
        len = (size_t)FVP_HISTORY_TEXT_MAX;
    }
    out = (char *)malloc(len + 1u);
    if (out == NULL) {
        return NULL;
    }
    if (s != NULL && len > 0u) {
        memcpy(out, s, len);
    }
    out[len] = '\0';
    return out;
}

int fvp_history_set_text(FvpHistory *h, int kind, const char *s, size_t len)
{
    FvpHistoryEntry *e;
    char **field;
    char *copy;

    if (h == NULL) {
        return 0;
    }
    if (kind != 0 && kind != 1) {
        return 0; /* 原版只认 0 / 1（2 走 set_value） */
    }
    e = &h->entry[h->pos];
    copy = hist_dup(s, len);
    if (copy == NULL) {
        return 0;
    }
    field = (kind == 0) ? &e->text0 : &e->text1;
    free(*field); /* 整串替换，不是追加 */
    *field = copy;
    return 1;
}

int fvp_history_set_value(FvpHistory *h, int n)
{
    if (h == NULL) {
        return 0;
    }
    if (n < 0) {
        return 0; /* 0x43A6BD: `test %eax,%eax; jl ret` —— 负值整条忽略 */
    }
    h->entry[h->pos].value = (int32_t)n;
    return 1;
}

int fvp_history_slot(const FvpHistory *h, int idx)
{
    int slot;

    if (h == NULL || idx < 0) {
        return -1;
    }
    if ((uint32_t)idx >= h->count) {
        return -1; /* 0x438094: idx >= count → 取不到 */
    }
    slot = (int)h->pos - idx;
    if (slot < 0) {
        slot += FVP_HISTORY_SLOTS; /* 0x4380A4: 环回绕 */
    }
    if (slot < 0 || slot >= FVP_HISTORY_SLOTS) {
        return -1;
    }
    return slot;
}

const char *fvp_history_text(const FvpHistory *h, int kind, int idx)
{
    int slot;
    const char *s;

    if (kind != 0 && kind != 1) {
        return NULL;
    }
    slot = fvp_history_slot(h, idx);
    if (slot < 0) {
        return NULL;
    }
    s = (kind == 0) ? h->entry[slot].text0 : h->entry[slot].text1;
    return (s != NULL) ? s : ""; /* 未设置时原版返回空串（0x43A518–0x43A540） */
}

int fvp_history_value(const FvpHistory *h, int idx, int32_t *out)
{
    int slot;
    int32_t v;

    slot = fvp_history_slot(h, idx);
    if (slot < 0) {
        return 0;
    }
    v = h->entry[slot].value;
    if (v == -1) {
        return 0; /* 0x43A5B4: -1 → 返回 null */
    }
    if (out != NULL) {
        *out = v;
    }
    return 1;
}

int fvp_history_filled(const FvpHistory *h)
{
    int i;
    int n = 0;

    if (h == NULL) {
        return 0;
    }
    for (i = 0; i < FVP_HISTORY_SLOTS; ++i) {
        if (h->entry[i].text0 != NULL || h->entry[i].text1 != NULL ||
            h->entry[i].value != -1) {
            n += 1;
        }
    }
    return n;
}