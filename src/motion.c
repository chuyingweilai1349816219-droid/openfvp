/* 动画补间核心数学。见 motion.h。 */
#include "motion.h"

#include <stdlib.h>
#include <string.h>

#define FVP_MOTION_MAX 256

void fvp_motion_init(FvpMotion *m)
{
    if (m == NULL) {
        return;
    }
    memset(m, 0, sizeof(*m));
    m->next_id = 1;
}

void fvp_motion_free(FvpMotion *m)
{
    if (m == NULL) {
        return;
    }
    free(m->e);
    memset(m, 0, sizeof(*m));
}

void fvp_motion_clear(FvpMotion *m)
{
    if (m == NULL) {
        return;
    }
    m->n = 0;
}

void fvp_motion_advance(FvpMotion *m, double dt_ms)
{
    if (m == NULL) {
        return;
    }
    if (dt_ms > 0.0) {
        m->now_ms += dt_ms;
    }
}

/* 进度 0..1。dur_ms == 0 视为「立刻完成」。 */
static double tween_progress(const FvpMotion *m, const FvpTween *t)
{
    double p;

    if (t->dur_ms <= 0.0) {
        return 1.0;
    }
    p = (m->now_ms - t->start_ms) / t->dur_ms;
    if (p < 0.0) {
        return 0.0;
    }
    if (p > 1.0) {
        return 1.0;
    }
    return p;
}

int fvp_motion_add(FvpMotion *m, int prim_id, int kind, double from_x,
                   double from_y, double to_x, double to_y, double dur_ms,
                   int mode, int flag)
{
    FvpTween *t;

    if (m == NULL || dur_ms < 0.0) {
        return 0;
    }
    if (m->n >= FVP_MOTION_MAX) {
        fvp_motion_reap(m);
        if (m->n >= FVP_MOTION_MAX) {
            return 0;
        }
    }
    if (m->n == m->cap) {
        size_t cap = m->cap == 0 ? 8 : m->cap * 2;
        FvpTween *e;

        if (cap > FVP_MOTION_MAX) {
            cap = FVP_MOTION_MAX;
        }
        e = (FvpTween *)realloc(m->e, cap * sizeof(FvpTween));
        if (e == NULL) {
            return 0;
        }
        m->e = e;
        m->cap = cap;
    }
    t = &m->e[m->n++];
    memset(t, 0, sizeof(*t));
    t->id = m->next_id++;
    if (m->next_id <= 0) {
        m->next_id = 1;
    }
    t->prim_id = prim_id;
    t->kind = kind;
    t->from_x = from_x;
    t->from_y = from_y;
    t->to_x = to_x;
    t->to_y = to_y;
    t->start_ms = m->now_ms;
    t->dur_ms = dur_ms;
    t->mode = mode;
    t->flag = flag;
    return t->id;
}

/* 缓动曲线：把线性进度 p(0..1) 映射成插值系数。语义来自静态分析
 * （见 docs/animation-baseline.md §2.3 与 docs/vm-commands.md 的 Motion* 行）：
 *
 *   mode 1 → 线性（dx*t/d）
 *   mode 2 → 缓入：dx*t²/d²
 *   mode 3 → 缓出：dx - dx*(d-t)²/d²
 *   mode 4 → 缓入-缓出：以 d/2 分段，前段 (dx/2)*(t/(d/2))²，
 *            后段 dx - (dx/2)*((d-t)/(d/2))²
 *   mode 5 → 先缓出后缓入：前段 (dx/2)*(1-((d/2-t)/(d/2))²)，
 *            后半段按同一分段结构对称推导
 *
 * 原版是 i16 坐标 + 64 位整数乘除（__allmul 0x447DD0 / __alldiv 0x447D20，
 * 除不尽向零截断）；本站是 double，截断点可能差 1 个最小单位，这一点在文档里
 * 写明，不假装逐位一致。mode 0 或表外值按线性处理（原版 `dec %eax; cmp $4;
 * ja` 走 0x423112 的分支）。
 */
double fvp_motion_ease(int mode, double p)
{
    if (p <= 0.0) {
        return 0.0;
    }
    if (p >= 1.0) {
        return 1.0;
    }
    switch (mode) {
    case 2: /* 缓入（二次） */
        return p * p;
    case 3: { /* 缓出（二次） */
        double q = 1.0 - p;
        return 1.0 - q * q;
    }
    case 4: /* 缓入-缓出 */
        if (p <= 0.5) {
            double h = p * 2.0;
            return 0.5 * h * h;
        }
        {
            double q = (1.0 - p) * 2.0;
            return 1.0 - 0.5 * q * q;
        }
    case 5: /* 先缓出、后缓入 */
        if (p <= 0.5) {
            double h = 1.0 - p * 2.0;
            return 0.5 * (1.0 - h * h);
        }
        {
            double q = (p - 0.5) * 2.0;
            return 0.5 + 0.5 * q * q;
        }
    default: /* mode 1 / 0 / 表外 → 线性 */
        return p;
    }
}

int fvp_motion_value(const FvpMotion *m, int id, double *x, double *y)
{
    size_t i;

    if (m == NULL || id <= 0) {
        return 0;
    }
    for (i = 0; i < m->n; ++i) {
        const FvpTween *t = &m->e[i];
        double p;
        double e;

        if (t->id != id) {
            continue;
        }
        p = tween_progress(m, t);
        e = fvp_motion_ease(t->mode, p);
        if (x != NULL) {
            *x = t->from_x + (t->to_x - t->from_x) * e;
        }
        if (y != NULL) {
            *y = t->from_y + (t->to_y - t->from_y) * e;
        }
        return 1;
    }
    return 0;
}

size_t fvp_motion_done_count(const FvpMotion *m)
{
    size_t i;
    size_t n = 0;

    if (m == NULL) {
        return 0;
    }
    for (i = 0; i < m->n; ++i) {
        if (tween_progress(m, &m->e[i]) >= 1.0) {
            n += 1;
        }
    }
    return n;
}

int fvp_motion_test(const FvpMotion *m, int prim_id, int kind)
{
    size_t i;

    if (m == NULL) {
        return 0;
    }
    for (i = 0; i < m->n; ++i) {
        const FvpTween *t = &m->e[i];

        if (t->prim_id != prim_id) {
            continue;
        }
        if (kind >= 0 && t->kind != kind) {
            continue;
        }
        if (tween_progress(m, t) < 1.0) {
            return 1; /* 还在跑 */
        }
    }
    return 0;
}

size_t fvp_motion_active_count(const FvpMotion *m)
{
    if (m == NULL) {
        return 0;
    }
    return m->n - fvp_motion_done_count(m);
}

int fvp_motion_cancel(FvpMotion *m, int prim_id, int kind)
{
    size_t i;
    size_t keep = 0;
    int removed = 0;

    if (m == NULL) {
        return 0;
    }
    for (i = 0; i < m->n; ++i) {
        int hit = (m->e[i].prim_id == prim_id) &&
                  (kind < 0 || m->e[i].kind == kind);

        if (hit) {
            removed += 1;
            continue;
        }
        m->e[keep++] = m->e[i];
    }
    m->n = keep;
    return removed;
}

void fvp_motion_reap(FvpMotion *m)
{
    size_t i;
    size_t keep = 0;

    if (m == NULL) {
        return;
    }
    for (i = 0; i < m->n; ++i) {
        if (tween_progress(m, &m->e[i]) < 1.0) {
            m->e[keep++] = m->e[i];
        }
    }
    m->n = keep;
}