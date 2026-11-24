/* 引擎定时器：TimerSet / TimerGet / TimerSuspend 的存储与换算。
 *
 * 语义来自静态分析（`docs/vm-commands.md` §5）：
 *
 *   TimerSet(timerId 0..15, ms 1..100000)
 *       → engine+0x1a997f+id*12 处写 {active=1, elapsed=0, duration=ms}
 *   TimerGet(timerId 0..15, opt 1..10000)
 *       → 返回已过毫秒；给了 opt 则返回 elapsed*opt/duration
 *   TimerSuspend(arg)  arg==1 置暂停标志 engine+0x6a66bc，0 清除
 *
 * 这一层只做「存起点 + 按调用方给的 now_ms 换算」，不持有时钟：虚拟时钟在
 * `FvpVM.virtual_ms`（`--timeline` 开启时随帧推进，默认恒 0）。所以默认配置下
 * TimerGet 恒返回 0，与「未实现时返回 0」的旧行为完全一致，不会改动基线。
 */
#ifndef FVP_TIMER_H
#define FVP_TIMER_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define FVP_TIMER_COUNT 16
#define FVP_TIMER_MS_MIN 1
#define FVP_TIMER_MS_MAX 100000

typedef struct FvpTimer {
    int active;
    uint64_t start_ms;
    int duration_ms;
} FvpTimer;

/* 全部复位（active = 0）。 */
void fvp_timer_reset(FvpTimer *timers, int count);

/* TimerSet：id 越界或 NULL 返回 0；ms 钳到 1..100000；成功返回 1。 */
int fvp_timer_set(FvpTimer *timers, int count, int id, int ms, uint64_t now_ms);

/* 已过毫秒。未激活 / id 越界返回 0；now_ms 早于起点也返回 0。 */
uint64_t fvp_timer_elapsed(const FvpTimer *timers, int count, int id,
                           uint64_t now_ms);

/* TimerGet：opt <= 0 返回已过毫秒；否则返回 elapsed*opt/duration（夹到
 * INT32_MAX）。未激活一律返回 0。 */
int fvp_timer_get(const FvpTimer *timers, int count, int id, int opt,
                  uint64_t now_ms);

/* 已激活的定时器个数（日志 / 测试用）。 */
int fvp_timer_active_count(const FvpTimer *timers, int count);

#ifdef __cplusplus
}
#endif

#endif /* FVP_TIMER_H */