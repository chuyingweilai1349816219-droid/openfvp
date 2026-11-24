/* 逐字显示（打字机效果）测试。
 *
 * 语义：
 *   - 默认关闭：TextPrint 的整行一次可见，gfx_text_advance() 不做任何事，
 *     已验收的帧因此一个像素都不变（由 verify_gfx 静态门保证）；
 *   - 打开后：新行从 0 个字符开始，按 TextSpeed 的"每字毫秒"推进，
 *     speed_ms == 0 表示不等待（一次显示完）；
 *   - gfx_text_busy() 报告"还在打"，供 TextSuspendChr(130) 决定要不要挂起线程。
 *
 * 这里只测状态机与 UTF-8 边界，不比对像素（像素由静态门兜住）。
 */
#include <stdio.h>
#include <string.h>

#include "../src/gfx.h"

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

int main(void)
{
    Gfx *g = gfx_create(320, 180, NULL);

    CHECK(g != NULL, "gfx_create");
    if (g == NULL) {
        return 1;
    }

    /* --- 1. 默认关闭：一次可见，advance 不做任何事 --- */
    CHECK(gfx_typewriter_enabled(g) == 0, "typewriter off by default");
    gfx_text_append(g, 0, "こんにちは");            /* 5 个 UTF-8 码点 */
    CHECK(gfx_text_total_chars(g, 0) == 5, "5 chars counted (got %d)",
          gfx_text_total_chars(g, 0));
    CHECK(gfx_text_shown_chars(g, 0) == 5, "off = fully shown (got %d)",
          gfx_text_shown_chars(g, 0));
    CHECK(gfx_text_busy(g, 0) == 0, "off is never busy");
    gfx_text_advance(g, 1000);
    CHECK(gfx_text_shown_chars(g, 0) == 5, "advance is a no-op when off");
    gfx_text_clear(g, 0);

    /* --- 2. 打开：新行从 0 开始 --- */
    gfx_typewriter_set_enabled(g, 1);
    CHECK(gfx_typewriter_enabled(g) == 1, "typewriter on");
    gfx_text_set_speed(g, 0, 100);                  /* 100 ms / 字 */
    gfx_text_append(g, 0, "あいうえお");            /* 5 chars */
    CHECK(gfx_text_shown_chars(g, 0) == 0, "on = starts at 0 (got %d)",
          gfx_text_shown_chars(g, 0));
    CHECK(gfx_text_busy(g, 0) == 1, "busy right after TextPrint");

    /* --- 3. 按 ms 逐字推进 --- */
    gfx_text_advance(g, 50);
    CHECK(gfx_text_shown_chars(g, 0) == 0, "50 ms < 100 -> still 0 (got %d)",
          gfx_text_shown_chars(g, 0));
    gfx_text_advance(g, 60);                        /* 累计 110 -> 1 字，余 10 */
    CHECK(gfx_text_shown_chars(g, 0) == 1, "110 ms -> 1 char (got %d)",
          gfx_text_shown_chars(g, 0));
    gfx_text_advance(g, 190);                       /* 累计 300 -> 3 字，余 0 */
    CHECK(gfx_text_shown_chars(g, 0) == 3, "300 ms -> 3 chars (got %d)",
          gfx_text_shown_chars(g, 0));
    gfx_text_advance(g, 1000);                      /* 一次跳到结尾 */
    CHECK(gfx_text_shown_chars(g, 0) == 5, "big dt finishes the line (got %d)",
          gfx_text_shown_chars(g, 0));
    CHECK(gfx_text_busy(g, 0) == 0, "not busy after finishing");
    gfx_text_advance(g, 1000);
    CHECK(gfx_text_shown_chars(g, 0) == 5, "advance never overshoots");
    gfx_text_clear(g, 0);

    /* --- 4. speed 0 = 立刻显示完 --- */
    gfx_text_set_speed(g, 0, 0);
    gfx_text_append(g, 0, "スキップ");
    gfx_text_advance(g, 1);
    CHECK(gfx_text_shown_chars(g, 0) == 4, "speed 0 shows everything (got %d)",
          gfx_text_shown_chars(g, 0));
    CHECK(gfx_text_busy(g, 0) == 0, "speed 0 is not busy");
    gfx_text_clear(g, 0);

    /* --- 5. UTF-8：ASCII / 2 字节 / 3 字节混排的码点计数 --- */
    gfx_text_set_speed(g, 0, 10);
    gfx_text_append(g, 0, "aéあ");      /* a(1) + é(2) + あ(3) = 3 码点 */
    CHECK(gfx_text_total_chars(g, 0) == 3, "mixed UTF-8 = 3 chars (got %d)",
          gfx_text_total_chars(g, 0));
    gfx_text_advance(g, 25);            /* 2 字 + 余 5ms */
    CHECK(gfx_text_shown_chars(g, 0) == 2, "2 of 3 after 25 ms (got %d)",
          gfx_text_shown_chars(g, 0));
    gfx_text_clear(g, 0);

    /* --- 6. 关掉时立刻把半句拉满 --- */
    gfx_text_append(g, 0, "とちゅうで止まる");
    gfx_text_advance(g, 15);            /* 1 字 */
    CHECK(gfx_text_shown_chars(g, 0) == 1, "1 char shown (got %d)",
          gfx_text_shown_chars(g, 0));
    gfx_typewriter_set_enabled(g, 0);
    CHECK(gfx_text_shown_chars(g, 0) == gfx_text_total_chars(g, 0),
          "disabling snaps to fully shown (%d/%d)", gfx_text_shown_chars(g, 0),
          gfx_text_total_chars(g, 0));
    gfx_text_clear(g, 0);

    /* --- 7. 多个窗口各自推进，互不影响 --- */
    gfx_typewriter_set_enabled(g, 1);
    gfx_text_set_speed(g, 0, 100);
    gfx_text_set_speed(g, 8, 50);
    gfx_text_append(g, 0, "ああああ");
    gfx_text_append(g, 8, "いいいい");
    gfx_text_advance(g, 100);
    CHECK(gfx_text_shown_chars(g, 0) == 1, "tw0 = 1 char (got %d)",
          gfx_text_shown_chars(g, 0));
    CHECK(gfx_text_shown_chars(g, 8) == 2, "tw8 = 2 chars (got %d)",
          gfx_text_shown_chars(g, 8));
    gfx_text_clear(g, 0);
    gfx_text_clear(g, 8);

    /* --- 7b. TextPause(121)：暂停的窗口不推进逐字显示 --- */
    {
        gfx_text_set_speed(g, 0, 10); /* 每字 10 ms */
        gfx_text_append(g, 0, "あいうえお");
        CHECK(gfx_text_set_pause(g, 0, 1) == 1, "pause accepted");
        gfx_text_advance(g, 1000);
        CHECK(gfx_text_shown_chars(g, 0) == 0, "paused: no advance (got %d)",
              gfx_text_shown_chars(g, 0));
        CHECK(gfx_text_set_pause(g, 0, 0) == 1, "unpause accepted");
        gfx_text_advance(g, 100);
        CHECK(gfx_text_shown_chars(g, 0) == 5, "resumed: fully shown (got %d)",
              gfx_text_shown_chars(g, 0));
        CHECK(gfx_text_set_pause(g, -1, 1) == 0, "pause layer -1 rejected");
        CHECK(gfx_text_set_pause(g, 32, 1) == 0, "pause layer 32 rejected");
        CHECK(gfx_text_set_pause(NULL, 0, 1) == 0, "pause NULL rejected");
        gfx_text_clear(g, 0);
    }

    /* --- 8. 边界 / NULL 安全 --- */
    CHECK(gfx_text_shown_chars(NULL, 0) == 0, "shown(NULL)");
    CHECK(gfx_text_total_chars(NULL, 0) == 0, "total(NULL)");
    CHECK(gfx_text_busy(NULL, 0) == 0, "busy(NULL)");
    CHECK(gfx_typewriter_enabled(NULL) == 0, "enabled(NULL)");
    gfx_typewriter_set_enabled(NULL, 1);
    gfx_text_set_speed(NULL, 0, 10);
    gfx_text_advance(NULL, 16);
    CHECK(gfx_text_shown_chars(g, -1) == 0, "layer -1");
    CHECK(gfx_text_shown_chars(g, 32) == 0, "layer 32");
    CHECK(1, "NULL calls survive");
    gfx_text_set_speed(g, 0, -5);       /* 负速度按 0 处理 */
    gfx_text_append(g, 0, "マイナス");
    gfx_text_advance(g, 1);
    CHECK(gfx_text_shown_chars(g, 0) == 4, "negative speed clamps to 0 (got %d)",
          gfx_text_shown_chars(g, 0));
    gfx_text_clear(g, 0);

    /* --- 9. TextOutSize(120)：两个装饰分量各自 0..12 / 0..8，逐分量校验 --- */
    {
        int oa = -1;
        int ob = -1;

        CHECK(gfx_text_get_out_size(g, 3, &oa, &ob) == 0,
              "TextOutSize 未作用过时返回 0");
        gfx_text_set_out_size(g, 3, 12, 8); /* 上界 */
        CHECK(gfx_text_get_out_size(g, 3, &oa, &ob) == 1 &&
              oa == 12 && ob == 8, "上界 (12,8) 记录 (%d,%d)", oa, ob);
        gfx_text_set_out_size(g, 3, 13, 9); /* 两个都越界 -> 都保留旧值 */
        CHECK(gfx_text_get_out_size(g, 3, &oa, &ob) == 1 &&
              oa == 12 && ob == 8, "越界整对跳过 (%d,%d)", oa, ob);
        gfx_text_set_out_size(g, 3, 5, 9); /* 只有 b 越界 -> a 生效 */
        CHECK(gfx_text_get_out_size(g, 3, &oa, &ob) == 1 &&
              oa == 5 && ob == 8, "分量独立：a=5 b 保持 8 (%d,%d)", oa, ob);
        gfx_text_set_out_size(g, 3, -1, 3); /* 只有 a 越界 -> b 生效 */
        CHECK(gfx_text_get_out_size(g, 3, &oa, &ob) == 1 &&
              oa == 5 && ob == 3, "分量独立：b=3 a 保持 5 (%d,%d)", oa, ob);
        oa = -1;
        ob = -1;
        CHECK(gfx_text_get_out_size(g, 99, &oa, &ob) == 0,
              "越界层号返回 0 (%d,%d)", oa, ob);
        CHECK(gfx_text_get_out_size(NULL, 0, &oa, &ob) == 0, "NULL gfx");
        gfx_text_set_out_size(NULL, 0, 1, 1);
        gfx_text_set_out_size(g, -1, 1, 1);
        gfx_text_set_out_size(g, 99, 1, 1);
        CHECK(1, "越界/NULL 的 set 不崩");
    }

    /* 像素级验证在单元测试里做不了：没有加载图元树/字体链路就画不出字。
 * 默认关闭时像素不变由 verify_gfx 静态门保证；开启后的实机效果用外壳
 * --typewriter 跑一次 + --screenshot 看（已验证：台词能完整显示）。 */

    gfx_destroy(g);

    printf("\n%d passed, %d failed\n", g_pass, g_fail);
    if (g_fail == 0) {
        printf("test_typewriter: PASS\n");
        return 0;
    }
    printf("test_typewriter: FAIL\n");
    return 1;
}