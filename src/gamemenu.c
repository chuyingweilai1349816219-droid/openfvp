/* 引擎侧游戏内菜单的纯逻辑实现（见 gamemenu.h 的说明）。 */
#include "gamemenu.h"

#include <string.h>

static void copy_label(FvpGameMenuItem *it, const char *src)
{
    size_t n;

    if (src == NULL) {
        it->label[0] = '\0';
        return;
    }
    n = strlen(src);
    if (n >= sizeof(it->label)) {
        n = sizeof(it->label) - 1;
    }
    memcpy(it->label, src, n);
    it->label[n] = '\0';
}

void fvp_gamemenu_init(FvpGameMenu *m, const char *const *labels, int count,
                       int y, int pitch)
{
    int i;

    if (m == NULL) {
        return;
    }
    memset(m, 0, sizeof(*m));
    if (count < 0) {
        count = 0;
    }
    if (count > FVP_GAMEMENU_MAX) {
        count = FVP_GAMEMENU_MAX;
    }
    m->count = count;
    m->hover = -1;
    m->selected = -1;
    m->y = y;
    m->pitch = (pitch > 0) ? pitch : 56;
    m->item_h = m->pitch;
    m->width = 320;
    m->x = 0;
    for (i = 0; i < count; ++i) {
        copy_label(&m->items[i], labels != NULL ? labels[i] : NULL);
        m->items[i].enabled = 1;
    }
}

void fvp_gamemenu_layout(FvpGameMenu *m, int client_w, int text_width, int pad)
{
    int w;

    if (m == NULL) {
        return;
    }
    if (pad < 0) {
        pad = 0;
    }
    w = text_width + pad * 2;
    if (w < 120) {
        w = 120;
    }
    m->width = w;
    m->x = (client_w - w) / 2;
    if (m->x < 0) {
        m->x = 0;
    }
    m->item_h = m->pitch;
}

void fvp_gamemenu_open(FvpGameMenu *m)
{
    if (m == NULL) {
        return;
    }
    m->active = 1;
    if (m->count > 0) {
        m->hover = 0;
        while (m->hover < m->count && !m->items[m->hover].enabled) {
            m->hover += 1;
        }
        if (m->hover >= m->count) {
            m->hover = -1;
        }
    }
}

void fvp_gamemenu_close(FvpGameMenu *m)
{
    if (m != NULL) {
        m->active = 0;
        m->hover = -1;
    }
}

int fvp_gamemenu_item_y(const FvpGameMenu *m, int i)
{
    if (m == NULL || i < 0) {
        return 0;
    }
    return m->y + i * m->pitch;
}

int fvp_gamemenu_hit(const FvpGameMenu *m, int mx, int my)
{
    int i;

    if (m == NULL || !m->active) {
        return -1;
    }
    if (mx < m->x || mx >= m->x + m->width) {
        return -1;
    }
    for (i = 0; i < m->count; ++i) {
        int y0 = fvp_gamemenu_item_y(m, i);
        int y1 = y0 + m->item_h;

        if (m->items[i].enabled && my >= y0 && my < y1) {
            return i;
        }
    }
    return -1;
}

void fvp_gamemenu_move(FvpGameMenu *m, int mx, int my)
{
    int hit;

    if (m == NULL || !m->active) {
        return;
    }
    hit = fvp_gamemenu_hit(m, mx, my);
    if (hit >= 0) {
        m->hover = hit;
    }
}

int fvp_gamemenu_nav(FvpGameMenu *m, int dir)
{
    int i;

    if (m == NULL || !m->active || m->count <= 0 || dir == 0) {
        return -1;
    }
    if (m->hover < 0) {
        m->hover = (dir > 0) ? -1 : 0;
    }
    for (i = 0; i < m->count; ++i) {
        m->hover += (dir > 0) ? 1 : -1;
        if (m->hover >= m->count) {
            m->hover = 0;
        } else if (m->hover < 0) {
            m->hover = m->count - 1;
        }
        if (m->items[m->hover].enabled) {
            return m->hover;
        }
    }
    return -1;
}

int fvp_gamemenu_activate(FvpGameMenu *m)
{
    if (m == NULL || !m->active || m->hover < 0 || m->hover >= m->count) {
        return -1;
    }
    if (!m->items[m->hover].enabled) {
        return -1;
    }
    m->selected = m->hover;
    return m->hover;
}

int fvp_gamemenu_click(FvpGameMenu *m, int mx, int my)
{
    int hit;

    if (m == NULL || !m->active) {
        return -1;
    }
    hit = fvp_gamemenu_hit(m, mx, my);
    if (hit < 0) {
        return -1;
    }
    m->hover = hit;
    m->selected = hit;
    return hit;
}