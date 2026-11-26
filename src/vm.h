/* FVP VM in C -- interpreter state (threads, frames, variables). */
#ifndef FVP_VM_H
#define FVP_VM_H

#include <stdint.h>
#include <stdio.h>

#include "encoding.h"
#include "flags.h"
#include "fvp.h"
#include "hcb.h"
#include "history.h"
#include "snd.h"
#include "textattr.h"
#include "timer.h"

typedef struct FvpFrame {
    uint32_t ret;
    int32_t nlocals;
    FvpValue *locals;
    int32_t nargs;
    FvpValue *args;
    int32_t ndecl;   /* `initstack A|B` 的 A（参数槽个数；FVP_FRAME_SLOTS=1 时用） */
    int pending;
} FvpFrame;

/* 帧槽模型开关：`FVP_FRAME_SLOTS=1` 打开修正版（参数槽 255-A..254、局部变量块在
 * 参数块下面）。默认关闭，保证 ref/ref-900k.txt 这份冻结文本基线逐字节不变。
 *
 * 2026-11-26 第 181 轮结论（tests/verify_text_from_hcb.py，run_all 6.49649）：
 * 修正版把正文窗口位置修对了（(431,880)），但文本通道会**每行多读一个字符**
 * （195 行里有 70 行在 HCB 字符串里找不到），所以**不能**切默认、也不能重新冻结
 * 文本基线——先找"多读一位"的根因再说。
 * 证据与遗留问题见 ref/script-window-geometry.json 的 frame_model 段。 */
#define FVP_FRAME_ARG_END 254

#define FVP_THREAD_SLOTS 32 /* ThreadStart/ThreadExit 的 threadNo 0..31 */
#define FVP_TIMER_COUNT 16  /* TimerSet/TimerGet 的 timerId 0..15 */

typedef struct FvpThread {
    int id;          /* 诊断用：线程编号 */
    int slot;        /* 协程槽 0..31（ThreadStart 分配）；-1 = 不在槽里（主线程） */
    uint64_t ntext;  /* 诊断用：这个线程打印了多少行台词 */
    uint32_t pc;
    const char *name;
    FvpValue *stack;
    int32_t sp;
    int32_t scap;
    FvpFrame *frames;
    int32_t nframes;
    int32_t fcap;
    int alive;
    int blocked;
    int32_t wait;
    uint64_t wake_ms; /* 时间模型：该线程到这个虚拟时刻醒 */
    int timed;        /* 1 = 按 wake_ms 定时唤醒（非旧的 yield 行为） */
    int sleep_ms;     /* ThreadSleep 请求的时长（原版 ctx+0x80C）；-1 = 不是 sleep */
    FvpValue temp;
} FvpThread;

/* (id, value) keyed dict with Python equality/hash semantics; used by the
 * garr_* / larr_* opcodes (the reference uses a Python dict). */
typedef struct FvpKey {
    uint16_t id;
    uint8_t type;
    int64_t i;
    double f;
    const FvpStr *s;
} FvpKey;

typedef struct FvpMapEntry {
    FvpKey key;
    FvpValue val;
    uint8_t used;
} FvpMapEntry;

typedef struct FvpMap {
    FvpMapEntry *entries;
    size_t cap;
    size_t count;
} FvpMap;

typedef struct FvpTextLine {
    FvpValue layer;
    FvpStr *text;
} FvpTextLine;

/* SaveData(95) 的记录。脚本用它把游戏状态摊平成"可存的数据"；本作实测每个存档
 * 只发几次，所以用一个固定上限的环形表既能全收也能报告溢出。字段含义见
 * docs/save-format.md §4（kind 0..13 的语义仍未解）。 */
#define FVP_SAVE_DATA_MAX 256
typedef struct FvpSaveData {
    int kind;
    int idx;
    int64_t value;
} FvpSaveData;

/* 16 个引擎定时器的状态（TimerSet/TimerGet/TimerSuspend）。语义与换算在
 * src/timer.c；elapsed 由 vm.virtual_ms 推导，默认没开 --timeline 时时钟恒 0
 * → TimerGet 返回 0，与旧的「未实现返回 0」行为一致。 */

/* ------------------------------------------------------------------ */
/* Optional render-layer attachment (src/gfx.c)                        */
/*                                                                     */
/* fvm.exe never attaches one: every rendering command in syscall.c     */
/* then takes the exact same code path as before (no-op / temp value).  */
/* A window shell or fvp/tests/render_vm.c can attach a Gfx context so  */
/* the script paints while it runs.                                     */
/* ------------------------------------------------------------------ */

struct Gfx;

#define FVP_GFX_PRIM_MAX 4096 /* must match GFX_PRIM_MAX in src/gfx.h */

typedef struct FvpGfxBridge {
    struct Gfx *gfx;
    /* Shadow of prim.graph: MotionAnim needs the graph slot of a prim and
     * gfx.h intentionally exposes no primitive getter.  Kept in sync by the
     * syscall layer (PrimSetNull/Sprt/Text/Tile resets, PrimSetSprt). */
    int32_t prim_graph[FVP_GFX_PRIM_MAX];
    /* Optional TextPrint/TextClear observer.  Used by the snapshot test to
     * reproduce render.py's --hold-text convention; NULL in fvm.exe.  The
     * text pointer is only valid for the duration of the call. */
    void (*on_text)(void *ud, int layer, const char *text, int clear);
    void *text_ud;
} FvpGfxBridge;

/* ------------------------------------------------------------------ */
/* Optional audio attachment (src/snd.c + src/bin.c)                   */
/*                                                                     */
/* fvm.exe never attaches one: every audio (Audio/Sound) command in syscall.c */
/* then takes the exact same code path as before (unimplemented/stub).  */
/* A window shell or fvp/tests/audio_vm.c attaches an Snd context so    */
/* AudioLoad/AudioPlay/... really play.  Resource names from the script */
/* ("bgm/001", "voice/01001530", "se/201", ...) are resolved lazily    */
/* from the game's BIN packages (src/bin.c) and decoded at play time   */
/* by src/snd.c -- never at load time, an Ogg BGM is ~22 MB decoded.    */
/* ------------------------------------------------------------------ */

struct Snd;
struct BinArchive;

#define FVP_SND_ARCHIVE_MAX 12     /* 12 packages shipped with the game */
#define FVP_SND_AUDIO_SLOTS 4      /* AudioLoad/AudioPlay channel 0..3  */
#define FVP_SND_SOUND_SLOTS 256    /* SoundLoad/SoundPlay index 0..255  */
#define FVP_SND_TYPES 10           /* AudioType/SoundType value 0..9    */

typedef enum FvpSndCategory {
    FVP_SND_CAT_UNKNOWN = -1,
    FVP_SND_CAT_BGM = 0,
    FVP_SND_CAT_VOICE = 1,
    FVP_SND_CAT_SE = 2
} FvpSndCategory;

typedef struct FvpSndArchive {
    char *path;               /* archive path (malloc'd)                  */
    struct BinArchive *bin;   /* opened lazily; NULL = not tried/failed   */
    int tried;                /* open was attempted (bin may still be NULL) */
} FvpSndArchive;

/* One Audio* channel slot (0..3) or one legacy Sound* slot (0..255). */
typedef struct FvpSndSlot {
    char *name;      /* resource name from the last Load (malloc'd)     */
    int type;        /* AudioType/SoundType raw 0..9, -1 = unset        */
    int volume;      /* 0..100                                          */
    int loop;        /* loop flag of the last play                      */
    int silent;      /* AudioSilentOn                                   */
    int playing;     /* last play succeeded and not stopped             */
    int snd_chan;    /* SND_CH_* it plays on, -1 = none                 */
} FvpSndSlot;

/* Counters the tests / the shell can print.  Pure diagnostics. */
typedef struct FvpSndStats {
    uint32_t loads, plays, stops, vols, types, states, silent_on;
    uint32_t resolved, unresolved, decode_fail;
    uint32_t plays_bgm, plays_voice, plays_se;
    unsigned bgm_crossfades; /* BGM 交叉淡入淡出次数 */
} FvpSndStats;

/* Event kinds for the optional observer hook. */
enum {
    FVP_SND_EV_LOAD = 1,
    FVP_SND_EV_PLAY = 2,
    FVP_SND_EV_STOP = 3,
    FVP_SND_EV_VOL = 4,
    FVP_SND_EV_TYPE = 5,
    FVP_SND_EV_SILENT = 6,
    FVP_SND_EV_STATE = 7
};

/* One observed audio command.  `name`/`archive` point into bridge-owned
 * memory and are only valid for the duration of the callback. */
typedef struct FvpSndEvent {
    int cid;         /* syscall id (0..6, 100..107)                     */
    int kind;        /* FVP_SND_EV_*                                    */
    int slot;        /* Audio channel or legacy Sound index             */
    int chan;        /* SND_CH_* it plays on, -1 = not playing          */
    int category;    /* FvpSndCategory, -1 = unknown                    */
    int ok;          /* resolve+decode succeeded (play/load events)     */
    int loop;
    int volume;      /* 0..100                                          */
    int type;        /* raw AudioType/SoundType argument, -1 = unset    */
    const char *name;    /* requested resource name (may be NULL)       */
    const char *archive; /* archive basename it resolved to, or NULL    */
} FvpSndEvent;

typedef struct FvpSndBridge {
    struct Snd *snd;
    FvpSndArchive archives[FVP_SND_ARCHIVE_MAX];
    int narchives;
    FvpSndSlot audio[FVP_SND_AUDIO_SLOTS];
    FvpSndSlot sound[FVP_SND_SOUND_SLOTS];
    int master_vol;                    /* 0..100, default 100          */
    int master_configured;             /* a non-zero master value arrived */
    int type_vol[FVP_SND_TYPES];       /* 0..100 per mixing group      */
    int type_configured[FVP_SND_TYPES];
    int se_next;                       /* round-robin SE cursor        */
    int bgm_chan;                      /* 当前承载 BGM 的通道（两个 BGM 通道轮换） */
    int bgm_xfade_ms;                  /* BGM 交叉淡入淡出时长（0 = 硬切） */
    FvpSndSlot *chan_owner[SND_CH_COUNT]; /* who plays on each SND chan */
    FvpSndStats stats;
    void (*on_event)(void *ud, const FvpSndEvent *ev);
    void *event_ud;
} FvpSndBridge;

/* GraphRGB 的槽号上限（静态分析 0..0xFFF）。 */
#define FVP_GRAPH_SLOTS 4096

/* prim 数量（engine 里是 0x61DD64 + id*0x34，id 0..0xFFF）。 */
#define FVP_PRIM_COUNT 4096

/* 字体表上限（外壳实际只会注册几套位图字集）。 */
#define FVP_MAX_FONT_NAMES 16

typedef struct FvpVM {
    FvpHcb *hcb;
    FvpValue globals[FVP_GLOBAL_COUNT];
    /* 哪些 global 被脚本写过。忠实模式（bool_type）下"没写过"要读成 FVP_NULL
     * （原版 Variant 默认 type 0），默认模式不看这张表。 */
    unsigned char globals_set[FVP_GLOBAL_COUNT];
    FvpMap garr;
    FvpMap larr;
    FvpThread **threads;
    size_t nthreads;
    size_t thread_cap;
    FvpThread *cur;
    uint64_t steps;
    uint64_t max_steps;
    /* 卡死侦测（脚本自带的死循环）。连续 stall_after 步没有执行任何 syscall
     * 就判定"卡住了"：stall_at = 命中的步数、stall_span = 那段无 syscall 的
     * 步数、stall_pc = 最后执行的那条指令的地址。stall_after == 0 表示关闭。
     * 为什么用"没 syscall"当判据：正常脚本每几十~几千步就有一条命令（画图、
     * 文本、定时器…），连续几百万步一条命令都没有只可能是脚本在自旋。 */
    /* 控制 / 光标组的状态（ControlMask 8 / ControlPulse 9 / CursorChange 10 /
     * CursorMove 11），见 fvp_vm_control_mask 的注释。 */
    unsigned char control_mask; /* engine+0x458：1 = 参数是 null（type 0） */
    unsigned char control_mask_type; /* 最近一次 ControlMask 参数的 type（诊断） */
    unsigned char pulse;        /* engine+0xfd8：置 1 后不会再被这几条命令清零 */
    int cursor_shape;           /* engine+0x88：0..3，-1 = 还没设过 */
    int cursor_x;
    int cursor_y;
    int cursor_moved;           /* CursorMove 是否生效过 */
    unsigned char cursor_force_type; /* CursorMove 第三参数的类型（诊断） */
    uint64_t control_calls[4];  /* 4 条命令各自的调用次数（诊断用） */

    uint64_t last_syscall_step;
    uint64_t stall_after;
    uint64_t stall_at;
    uint64_t stall_span;
    uint32_t stall_pc;
    uint32_t last_pc;
    int64_t click_period;
    int64_t cx;
    int64_t cy;
    int64_t key_mask;
    int quiet;
    /* 实验开关（默认 0 = 现值模型）：1 = "类型即真假"。
     *
     * 静态分析证据（handler 0x445a30）明确：jmpcond 只看弹出值的**类型字节**
     * （`cmpb $0x0,(%edx)`，type == 0 才跳），比较类 opcode 把结果写进类型字节。
     * 但本项目的冻结基准（ref/ref-900k.txt）是在"数值真假"模型下生成的，
     * 所以默认行为保持不变，把这个语义放在开关后面做 A/B（见 docs/roadmap.md
     * 「值模型的第二步」）。打开后：比较/逻辑/位测试类 opcode 为假时压
     * FVP_NULL（type 0）、为真时压 int 1；jmpcond 只在 type == FVP_NULL 时跳。 */
    int bool_type;
    FvpArena arena;         /* runtime strings (concat, int -> str) */
    FvpTextLine *text;
    size_t ntext;
    size_t text_cap;
    uint32_t *unknown_count; /* 65536 counters */
    int thread_ids;          /* 下一个线程编号 */
    uint16_t *unknown_order;
    size_t nunknown;
    size_t ucap;
    FILE *pctrace;
    int warned_neg_str;
    FvpGfxBridge *gfx_bridge; /* NULL = rendering commands are no-ops */
    FvpSndBridge *snd_bridge; /* NULL = audio commands are no-ops */
    /* 全局 flag 位图（FlagGet/FlagSet, id 0..0x7FF）。save/save.bin 里那 256
     * 字节就是它的原始拷贝，见 src/flags.h 与 docs/save-format.md §3.5。 */
    unsigned char flags[FVP_FLAG_BYTES];
    /* 32 个文本窗口的颜色 / 字号 / 速度（TextColor/TextSize/TextSpeed）。 */
    FvpTextAttr textattr[FVP_TEXT_WINDOWS];
    /* parts（部件/角色合成层）与组移动。静态分析：
     *   66 PartsMotion(partsId 0..0x3F, …)      handler 0x43B3E0：arg1 0..0x3F、arg2 int
     *   67 PartsMotionPause(partsId 0..0x3F, …) handler 0x43B440：同上校验
     *   68 PartsMotionStop(partsId **1..0x3F**) handler 0x43B480：tail-call
     *      `0x4264C0(engine+0x651D70, id)`（和位置补间同一个管理器）
     *   70 PartsRGB(partsId 0..0x3F, r, g, b)   handler 0x43B500：分量规则与 GraphRGB
     *      完全相同（int 且 0..200，否则默认 100）
     *   74 PrimGroupMove(id **1..0xFFF**, …)    handler 0x43B980：arg2 也是 int
     * 本站都没有"部件动画/组位移"的渲染语义，按记录处理（值 + 计数）。 */
    /* ColorSet(7) 写的调色板：256 项 BGRA（原版 engine+0x6A44C0）。
 * 渲染端目前还不用它（tile 的索引色查表是后续一轮），但状态要与原版一致。 */
unsigned char palette[256][4];
unsigned char palette_set[256];
uint64_t color_set_calls;
unsigned char parts_rgb[64][3];
    unsigned char parts_rgb_set[64];
    unsigned char parts_motion[64]; /* 最近一次 PartsMotion 的 arg2（诊断） */
    unsigned int parts_motion_time[64]; /* 最近一次 PartsMotion 的 arg3（时长 ms） */
    int prim_group_move_last[2];
    uint64_t parts_motion_calls;
    uint64_t parts_motion_pause_calls;
    uint64_t parts_motion_stop_calls;
uint64_t parts_motion_test_calls; /* PartsMotionTest(69) 调用次数 */
    uint64_t parts_rgb_calls;
    uint64_t prim_group_move_calls;

    /* SnowStart(98) / SnowStop(99) / PrimSetSnow(84)。静态分析：
     *   98 SnowStart(layer 0..1, …) → handler 0x43C4F0：`snow[layer]+0x688F1C 处 = 1`，
     *      层号按 `layer*0x5048` 索引（两层雪花系统的参数表）
     *   99 SnowStop(layer 0..1, …)  → handler 0x43C520：同一处写 0
     *   84 PrimSetSnow(primId 1..0xFFF, on 0..1, …) → handler 0x43BC40：校验后
     *      调 0x43????（每个图元的积雪参数），本站只记录 on
     * 本站没有粒子/积雪渲染，三条都只记录状态 + 计数（写入用位，不猜画面）。 */
    unsigned char snow_on[2];        /* SnowStart/Stop 的开关 */
    unsigned char prim_snow[FVP_PRIM_COUNT]; /* PrimSetSnow 的 on */
    uint64_t snow_start_calls;
    uint64_t snow_stop_calls;
    uint64_t prim_snow_calls;

    /* Snow(97)：18 个参数的大命令（iroseka 实测调用过 1 次，脚本只压了 16 个值）。
     * 静态分析 handler **0x43C230**：
     *   - arg1 int 0..1（层号）→ 后面的层表基址 `engine+0x683F1C + arg1*0x5048`
     *     （比 SnowStart 的 0x688F1C 早 0x5000，同一块 0x5048 层结构的不同子结构）
     *   - arg2/arg3 int 0..0x1000、arg4 int 0..0xFFF（很像 primId 量级）
     *   - arg5/arg6 int 2..0x40、arg7 int 1..0x10、arg8/arg9 int 10..0x2710
     *     且额外要求 **arg8 <= arg9**（`0x43C356` 处 `cmp %eax,%ebp; jg` 拒绝）
     *   - arg10 int 1..0x400、arg11..arg13 int -0x1000..0x1000、arg14 int 0..0x400
     *   - arg1..arg14 **全部必填**（任何一个不是 int 或越界 → 整条命令丢弃）
     *   - arg15..arg18 可选（`0x43C41A/432/44F/46D` 处失败只跳过赋值）：
     *     arg15 0..0xFF（缺省 0xFF）、arg16 0..0xFF、arg17 10..0x2710、
     *     arg18 0..0xFF
     *   - 全部通过后把 16 个整数压栈调 **0x431DB0(层结构, …)**（雪花参数表初始化）
     * 本站没有雪花渲染：只把校验通过的原值记下来（`snow_vals[18]` + 可选位图）。 */
    int snow_calls;
    int snow_layer;                     /* arg1 */
    int snow_vals[18];                  /* 校验通过的参数原值（可选参数缺省记 0） */
    unsigned short snow_opt_mask;       /* bit k = arg(15+k) 给出了合法值 */

    /* CursorShow(12) / ExitDialog(16) / MovieStop(63)。静态分析：
     *   12 CursorShow(v) → handler 0x4374F0：**只看类型**（null → 0、其它 → 1），
     *      然后 tail-call 0x441860(engine, 值)。本站的值模型没有 null，
     *      脚本压出来的都是具体值 → 恒为 1（与 ControlMask 同一处限制，已记录）。
     *   16 ExitDialog()  → handler 0x437560：`if (0x442860(engine))` 时置
     *      线程 ctx 的 +0x68E7D0 = 1（"本帧有事要做"的标记）；没有返回值。
     *   63 MovieStop()   → handler 0x43B300：`call 0x4220F0(engine+0x8C->0x2D4)`
     *      停掉 WMV 播放器。本站没有视频播放，停 = 空操作（语义上正好成立）。 */
    unsigned char cursor_show;   /* 1 = 显示光标（默认 1） */
    uint64_t exit_dialog_calls;
    uint64_t movie_stop_calls;

    /* LipAnim(38) / LipSync(39)：口型（lip sync）系统。静态分析：
     *   `engine+0x683E74` 是 **4 个槽**的口型表（每槽 0x20 字节）：
     *     +4 = 状态字节（0=空、1=停、2=播放中）
     *     +8 = 槽号（0x42D700 释放时写回 engine+0x84 的空闲栈）
     *     +0xa = i16 的 id
     *   38 LipAnim(id 1..0xFFF, mode 0..3 **或 null**, arg3 1..0xFFF,
     *              arg4 1..300000, arg5 1..0xFFF, arg6 1..300000,
     *              [arg7 1..0xFFF, arg8 1..300000])
     *      handler **0x43A6E0**：arg2 为 null → 只调 0x42D700(id) 停掉；
     *      否则 arg1..arg6 必填、arg7/arg8 可选（缺省补 0），全过才
     *      0x42EFA0(表, id, mode, …)：先停旧的，再从空闲栈挑一个槽写入 8 个参数
     *      并置状态 1。
     *   39 LipSync(id 1..0xFFF, on) → handler **0x43A830**：
     *      `arg2.type == 0`（null）→ 0，否则 → 1，再 0x428870(表, id, on)：
     *      在 4 个槽里按 id 找到条目，on → 状态字节 = 2，off → 0x4283A0 停 +
     *      状态字节 = 1。
     * 本站没有口型渲染，两条都只记录参数 + 计数。`pushfalse` 现在压的是
     * FVP_NULL（原版 Variant type 0），所以 LipAnim 的"只停"分支
     * （arg2 为 null → 0x42D700(id)）与 LipSync 的 on=0 分支都**能走到**了。 */
    uint64_t lip_anim_calls;
    uint64_t lip_anim_stop_calls; /* LipAnim(arg1, null)：只停，不启动 */
    uint64_t lip_sync_calls;
    int lip_anim_id;      /* arg1 */
    int lip_anim_mode;    /* arg2（"只停"分支不写） */
    int lip_anim_arg[8];  /* arg1..arg8 原值；可选参数缺省记 0 */
    int lip_sync_id;      /* arg1 */
    int lip_sync_on;      /* 1 = 第二参数不是 null（播），0 = null（停） */

    /* InputFlash(27) / InputGetRepeat(33) / InputSetClick(37)：输入微调。静态分析：
     *   27 InputFlash()        → handler 0x4376A0，`call 0x442400(engine+0x120)`，无返回值
     *   33 InputGetRepeat()    → handler 0x437860，**返回 int**（engine+0x448）
     *   37 InputSetClick(v)    → handler 0x437920，v 必须是 int 0 或 1，写 engine+0x438
     * 本站：Flash 只计数（原版那一下"闪屏"的视觉没有证据）；GetRepeat 返回记录值
     * （默认 0，和原版初始化一致）；SetClick 按 0/1 校验后存值。 */
    unsigned char input_click; /* engine+0x438：0/1 */
    int input_repeat;          /* engine+0x448：默认 0 */
    uint64_t input_flash_calls;
    uint64_t input_setclick_calls;

    /* MotionPause(60) / TextPause(121)：两条都是"暂停字节"。静态分析：
     *   MotionPause(primId 0..0xFFF, on 0..1) → handler 0x43AB60 写 `prim+9`
     *   TextPause(twIdx 0..31, on 0..1)       → handler 0x43C9C0 写 `tw+0x5DA`
     * 参数必须 int 且 0..1，否则走"等价于 0"的分支（本站按 0 处理）。
     * TextPause 的值会转给 gfx：打字机推进（gfx_text_advance）会跳过暂停的窗口。 */
    unsigned char tw_pause[FVP_TEXT_WINDOWS];
    unsigned char prim_pause[FVP_PRIM_COUNT];
    uint64_t motion_pause_calls;
    uint64_t text_pause_calls;

    /* 字体表（TextFontCount 114 / TextFontGet 115 / TextFontName 116 /
     * TextFontSet 117）。静态分析结论：原版 engine+0x94 是一个 vector<font>，
     *   TextFontCount → (end-begin)/4（条数）
     *   TextFontSet(idx) → 要求 idx >= -5 且 idx <= count-1，然后切换当前字体
     *   TextFontGet      → 返回 engine+0x6a66c4（当前字体号）
     *   TextFontName(idx) → 要求 idx 在 0..count-1，返回该字体的名字字符串
     * 名字本身来自 exe 的注册表（我们没有那份表），所以由**外壳**把实际加载的
     * 字集名字注册进来（fvp_vm_font_register）；fvm 不注册 → 表是空的。
     * -5 这个下界是原版行为（大概是"内置字体"的负号区间），照抄。 */
    char *font_name[FVP_MAX_FONT_NAMES];
    int nfont_names;
    int cur_font;

    /* Load(40) / SaveWrite(96)：存档请求。静态分析（handler 0x43a870 / 0x43c1e0）：
     *   两条都要求参数是 int 且 0..0x3E7（999）的槽号，然后
     *     engine+0xfcc = 槽号
     *     Load(40)      → engine+0xfcb = 1（读档请求），再置线程 ctx 的 +0x68e7d0 = 1
     *     SaveWrite(96) → engine+0xfca = 1（存档请求）；若 engine+0xfc9 != 0，
     *                     还额外 call 0x438540(engine+0x14, 槽号) 立即落盘
     * 也就是说这两条命令**本身不读写文件**，只是把"请求 + 槽号"记进引擎字段，
     * 由界面/外壳那一侧去执行。本站照这个模型做：记录请求，由外壳用 take 取走。 */
    unsigned char load_request;   /* engine+0xfcb */
    unsigned char save_request;   /* engine+0xfca */
    int request_slot;             /* engine+0xfcc：最近一次请求的槽号，-1 = 没有 */
    uint64_t load_calls;
    uint64_t save_calls;

    /* Rand(92)：handler 0x437a20 —— 把 retval 标成 int，然后 `call 0x4483da`
     * （MSVC CRT 的 `rand()`），返回值直接当 retval。也就是"返回一个 0..RAND_MAX
     * 的整数"，MSVC 的 RAND_MAX = 0x7FFF。本站用固定种子的 xorshift32 实现，
     * 保证同一次运行可复现（基准对比依赖确定性），每次取值 & 0x7FFF 对齐范围。 */
    uint32_t rand_state;

    /* TextRepaint(124)：handler 0x437a50 —— `call 0x441790(engine)` 让文本重画。
     * 本站每帧整张画布重画，所以这条是**真·空操作**，只记次数。 */
    uint64_t text_repaint_calls;

    /* GaijiLoad(21)：外字（造字）加载。handler 0x43f4f0：
     *   GaijiLoad(str1, size 12..0x40, str2) —— 两个参数都必须是字符串（原版
     *   type 4/5），中间那个必须是 int 且 12..64，然后
     *   call 0x43e9c0(engine+0x6a49cc, str1, size, str2) 把外字登进表里。
     * 本站没有"外字表 + 字形插入"这条链路，只记录最近一次与调用次数。 */
    uint64_t gaiji_calls;
    int gaiji_size;      /* 最近一次的 size（12..64），0 = 还没调用过 */
    char *gaiji_name1;   /* 最近一次的 str1（arena 拷贝） */
    char *gaiji_name2;   /* 最近一次的 str2（arena 拷贝） */

    /* V3DMotion(142)：3D 补间。handler 0x43cfe0：
     *   arg4 必须是 int 在 1..0x493e0（= 时间 ms，和 MotionMove 同一区间），
     *   arg5 必须是 int 在 1..5（缓动模式），arg6 当 bool；arg1..arg3 是可选的
     *   坐标（缺省取 engine+0x6a65f0/f4/f8）。本站没有 3D 场景，只记录。 */
    uint64_t v3d_calls;
    int v3d_time_ms;     /* 最近一次的时长，0 = 还没调用过 */
    int v3d_mode;        /* 最近一次的模式 1..5 */

    /* WindowMode(147) / ExitMode(17)：只记录最后一次设置的值。
     *   WindowMode handler 0x43d0d0：参数必须是 int 且 -1..6；模式 0/1 会把
     *     engine->[0x8c]->[0x1fc] 置成 0/1 并返回该值，模式 2 返回当前值，
     *     -1 是"退出全屏/切回"这类特殊模式（还有别的分支没读完）。
     *   ExitMode handler 0x43a1a0：参数必须是 int 且 0..4；0 分支会清
     *     engine+0x82 并置当前线程 ctx 的 +0x68e7d4 = 1（"请求退出"一类）。
     * 两者真正改的都是窗口/线程层字段，本站没有对应结构，因此只记录。 */
    int window_mode_last;  /* -1 = 还没设置过；否则是最后一次模式 -1..6 */
    int window_state;      /* 模式 0/1 设置的状态值 */
    uint64_t window_mode_calls;
    int exit_mode_last;    /* -1 = 还没设置过；否则是最后一次 0..4 */
    uint64_t exit_mode_calls;

    /* TextBuff(110)：文本窗口的缓冲尺寸。静态分析结论：
     *   handler 0x43c830 入口校验 twIdx 必须是 int 且在 0..31；
     *   第 2/3 个参数各自"是 int 且 >= 0"才采用，否则用默认 **8**；
     *   然后 call 0x430ea0(tw, a, b)：两者都 > 0x400 时整条命令直接放弃，
     *   否则写 u16 `tw+0x5dc = a` / `tw+0x5de = b` 并重建缓冲（释放 tw+0x520 的旧块）。
     * 本站只记录（我们的文本是按行存的动态串，没有"固定缓冲"这层结构）。 */
    struct {
        unsigned short w;
        unsigned short h;
        unsigned char set;
    } text_buff[FVP_TEXT_WINDOWS];
    uint64_t text_buff_calls;

    /* GraphRGB(23)：每个图形槽的颜色调制。静态分析（handler 0x43a3d0）结论：
     * 槽号必须 int 0..0xFFF；三个分量各自"是 int 且在 0..200 内"才采用，否则取
     * 默认 100（= 不调制）；然后 call 0x428910(engine+0x101064+slot*0x51c, r,g,b)
     * 并 tail-call 0x437330(engine+0x45c, slot) 通知刷新。
     * 本站只记录（渲染端没有"颜色调制怎么合成"的证据，见 docs/render-model.md）。 */
    unsigned char graph_rgb[FVP_GRAPH_SLOTS][3];
    unsigned char graph_rgb_set[FVP_GRAPH_SLOTS];
    uint64_t graph_rgb_calls;

    /* SysProjFolder(109)：handler 0x43c7f0 只接受 type 4/5（字符串）参数，然后
     * tail-call 0x433c60(engine+0x6a7e78, str) 设置"工程目录"。本站把字符串存进
     * arena 记下来（档案路径的语义还没解出，不消费）。 */
    char *proj_folder;
    uint64_t proj_folder_calls;

    /* 存档暂存区（SaveCreate / SaveThumbSize 写入，SaveWrite 将来消费）：
     * str[0]=章节名, str[1]=未用, str[2]=当前对话行（docs/save-format.md §2.2）。 */
    FvpStr *save_str[3];
    int save_slot;   /* SaveCreate(3, n) 选的槽号，-1 = 未选 */
    int save_thumb_w;
    int save_thumb_h;
    /* 鼠标滚轮累计量（120 = 一格）。外壳/测试往里加，脚本用 InputGetWheel 取走。 */
    int wheel_delta;
    /* 虚拟时钟（毫秒）。timeline_on = 0 时不会推进，ThreadSleep/Wait 保持旧行为。 */
    uint64_t virtual_ms;
    int timeline_on;
    /* 协程槽：ThreadStart(136)/ThreadExit(132) 的 threadNo 0..31 指的就是这 32
     * 个槽（docs/vm-commands.md §5：threadNo==0 先重置全部 32 个槽）。主线程不在
     * 槽里。threads_on == 0（默认）时这两个命令仍是空桩，已验收的基线不变。 */
    FvpThread *thread_slots[FVP_THREAD_SLOTS];
    int threads_on;
    uint64_t thread_starts; /* 诊断：ThreadStart 成功创建过多少次 */
    uint64_t thread_exits;  /* 诊断：ThreadExit 收掉过多少个线程 */
    uint64_t thread_resets; /* 诊断：ThreadStart(0)/(无槽可回收) 触发过多少次 */
    /* ThreadRaise(134)：handler 0x437B80 对 32 个槽逐个调 0x4447E0(ctx, ms)，
     * 后者是 `if ((flags & 4) && ctx->wait_ms == ms) flags &= ~4;`
     * —— 也就是"把睡眠时长正好等于 ms 的线程叫醒"。 */
    uint64_t thread_raise_calls; /* 调用次数 */
    int thread_raise_ms;         /* 最近一次的参数 */
    uint64_t thread_raise_woken; /* 累计叫醒的线程数 */
    /* 16 个定时器（TimerSet 139 / TimerGet 138 / TimerSuspend 140）。 */
    FvpTimer timer[FVP_TIMER_COUNT];
    int timer_suspended;  /* TimerSuspend(1) 置位、(0) 清除 */
    uint64_t timer_sets;  /* 诊断：TimerSet 调用次数 */
    /* 诊断：本次 syscall 是否被 gfx 桥处理过。主 switch 没有 case 的渲染命令
     * 走的是 fvp_sys_gfx()，若不再计一次「已处理」，未实现直方图会把
     * PrimSetNull 这类已经实现的命令报成缺口。 */
    int gfx_handled;
    /* SaveData(95) 的记录（见上面 FvpSaveData）。 */
    FvpSaveData save_data[FVP_SAVE_DATA_MAX];
    int save_data_n;          /* 已记录条数（最多 FVP_SAVE_DATA_MAX） */
    uint64_t save_data_seen;  /* 收到的总次数（用来报告溢出） */
    /* 台词历史记录（HistorySet 26 / HistoryGet 25）。100 条环形记录，语义与
     * 静态分析出处见 src/history.h；记录本身是存档槽尾快照的一部分（原版在
     * 0x43F264 处序列化 count/pos/base）。 */
    FvpHistory history;
    /* PrimHit(76) 诊断计数：只有挂了 gfx 的程序（外壳/render_vm）才会累加，
     * fvm.exe 里这两个值恒 0（它保持"未实现"路径）。 */
    uint64_t prim_hit_calls;
    uint64_t prim_hit_hits;
} FvpVM;

void fvp_vm_init(FvpVM *vm, FvpHcb *hcb, int quiet);
void fvp_vm_free(FvpVM *vm);
void fvp_vm_start(FvpVM *vm);
/* 从指定 pc 起跑（不经过 main_start）。用于验证"存档里的脚本位置候选"：
 * 没有对应的栈/全局变量时只能做冒烟测试，不代表真读档。 */
void fvp_vm_start_at(FvpVM *vm, unsigned int pc);
uint64_t fvp_vm_run(FvpVM *vm, uint64_t max_steps);

/* 打开卡死侦测：连续 n 步没有 syscall 就停下（n == 0 关闭，默认关闭）。
 * 命中后 fvp_vm_stalled() 返回 1，细节看 FvpVM 的 stall_* 字段。 */
void fvp_vm_set_stall_limit(FvpVM *vm, uint64_t n);

/* GraphRGB(23)：记录图形槽的颜色调制，返回 1 = 生效（槽号合法）。 */
int fvp_vm_graph_rgb(FvpVM *vm, const FvpValue *slot, const FvpValue *r,
                     const FvpValue *g, const FvpValue *b);
/* PartsMotion(66) / PartsMotionPause(67) / PartsMotionStop(68)：
 * partsId 0..0x3F（Stop 是 1..0x3F）；返回 1 = 生效。 */
/* ColorSet(7)：index 0..0xFF，四个分量各自 int 才写（BGRA），否则保持原值。 */
int fvp_vm_color_set(FvpVM *vm, int index, int r, int g, int b, int a,
                     int has_r, int has_g, int has_b, int has_a);

int fvp_vm_parts_motion(FvpVM *vm, const FvpValue *parts, const FvpValue *arg2,
                        const FvpValue *arg3);
int fvp_vm_parts_motion_pause(FvpVM *vm, const FvpValue *parts,
                              const FvpValue *on);
int fvp_vm_parts_motion_stop(FvpVM *vm, const FvpValue *parts);
/* PartsMotionTest(69)：handler 0x43B4B0，槽号 1..0x3F；返回"该槽的动作是否还在跑"。
 * 我们不跑部件动画，所以恒为 0（不会让脚本卡在等动画的循环里）。 */
int fvp_vm_parts_motion_test(FvpVM *vm, const FvpValue *parts);
/* PartsRGB(70)：partsId 0..0x3F + 三个 0..200 分量（缺省 100）。 */
int fvp_vm_parts_rgb(FvpVM *vm, const FvpValue *parts, const FvpValue *r,
                     const FvpValue *g, const FvpValue *b);
/* PrimGroupMove(74)：id 1..0xFFF + 一个 int 参数。 */
int fvp_vm_prim_group_move(FvpVM *vm, const FvpValue *id,
                           const FvpValue *arg2);

/* SnowStart(98) / SnowStop(99)：layer 0..1；返回 1 = 生效。 */
int fvp_vm_snow(FvpVM *vm, const FvpValue *layer, int on);
/* Snow(97)：18 个参数的大命令，arg1..arg14 必填、arg15..arg18 可选；
 * 校验规则见 `FvpVM` 里的注释。返回 1 = 整条命令生效。 */
int fvp_vm_snow_ex(FvpVM *vm, const FvpValue *args, int argc);
/* PrimSetSnow(84)：primId 1..0xFFF + on 0..1；返回 1 = 生效。 */
int fvp_vm_prim_snow(FvpVM *vm, const FvpValue *prim, const FvpValue *on);

/* CursorShow(12)：按"值非空"置显示位（我们的模型里恒为 1）。 */
int fvp_vm_cursor_show(FvpVM *vm, const FvpValue *v);
/* ExitDialog(16)：只计数（原版按 0x442860 的条件置线程标记）。 */
void fvp_vm_exit_dialog(FvpVM *vm);
/* MovieStop(63)：没有视频在放 → 空操作，只计数。 */
void fvp_vm_movie_stop(FvpVM *vm);

/* LipAnim(38)：id 1..0xFFF + mode 0..3 + 四段区间校验（arg7/arg8 可选）。
 * argc >= 6 才可能成立；返回 1 = 整条命令生效。 */
int fvp_vm_lip_anim(FvpVM *vm, const FvpValue *args, int argc);
/* LipSync(39)：id 1..0xFFF；on 为 NULL（第二参数缺失）时按 0 记。
 * 返回 1 = 生效。 */
int fvp_vm_lip_sync(FvpVM *vm, const FvpValue *id, const FvpValue *on);

/* HistorySet(26)：handler 0x43A5E0。
 *   args[0] == null            → 开新的一条（count+1 上限 100、pos 环进）
 *   args[0] 是 int kind 0/1    → 需要 args[1] 是字符串，整串替换 text0/text1
 *   args[0] 是 int kind 2      → 需要 args[1] 是 int 且 >= 0，写 value
 *   其它 kind / 类型不符       → 整条命令被忽略
 * 返回 1 = 命中上述任一分支（诊断用），0 = 被忽略。 */
int fvp_vm_history_set(FvpVM *vm, const FvpValue *args, int argc);

/* HistoryGet(25)：handler 0x43A490。
 *   args[0] == null                     → *out = count（int）
 *   args[0] 是 int kind 0..2 且 idx 合法 → *out = text/kinds/value
 * 返回 1 = 有返回值（*out 有效），0 = 原版不写返回值（越界/类型不符）。 */
int fvp_vm_history_get(FvpVM *vm, const FvpValue *args, int argc,
                       FvpValue *out);

/* InputFlash(27)：只计数（原版 call 0x442400(engine+0x120)）。 */
void fvp_vm_input_flash(FvpVM *vm);
/* InputGetRepeat(33)：返回 engine+0x448 的记录值（默认 0）。 */
int fvp_vm_input_repeat(const FvpVM *vm);
/* InputSetClick(37)：arg 必须是 int 0/1；返回 1 = 生效。 */
int fvp_vm_input_set_click(FvpVM *vm, const FvpValue *v);

/* MotionPause(60)：primId 0..0xFFF + on 0..1；返回 1 = 生效。 */
int fvp_vm_motion_pause(FvpVM *vm, const FvpValue *prim, const FvpValue *on);
/* TextPause(121)：twIdx 0..31 + on 0..1；返回 1 = 生效。 */
int fvp_vm_text_pause(FvpVM *vm, const FvpValue *idx, const FvpValue *on);

/* 字体表：注册一个名字（外壳调用），返回新表长；超过上限返回 -1。 */
int fvp_vm_font_register(FvpVM *vm, const char *name);
int fvp_vm_font_count(const FvpVM *vm);
int fvp_vm_font_set(FvpVM *vm, const FvpValue *idx);
int fvp_vm_font_get(const FvpVM *vm);
const char *fvp_vm_font_name(const FvpVM *vm, const FvpValue *idx);

/* Load(40) / SaveWrite(96)：记下请求（槽号 0..999），返回 1 = 参数合法。
 * fvp_vm_take_load_request / take_save_request 由外壳取走请求并清标志
 * （取走语义和 InputGetWheel 一致）；没有待处理请求时返回 -1。 */
int fvp_vm_request_load(FvpVM *vm, const FvpValue *slot);
int fvp_vm_request_save(FvpVM *vm, const FvpValue *slot);
int fvp_vm_take_load_request(FvpVM *vm);
int fvp_vm_take_save_request(FvpVM *vm);

/* Rand(92)：返回 0..0x7FFF 的伪随机数（固定种子，可复现）。 */
int fvp_vm_rand(FvpVM *vm);
/* GaijiLoad(21)：记录外字加载参数，返回 1 = 生效（两个字符串 + size 12..64）。 */
int fvp_vm_gaiji_load(FvpVM *vm, const FvpValue *s1, const FvpValue *size,
                      const FvpValue *s2);
/* V3DMotion(142)：记录 3D 补间参数，返回 1 = 生效（time 1..300000、mode 1..5）。 */
int fvp_vm_v3d_motion(FvpVM *vm, const FvpValue *time_ms, const FvpValue *mode);
/* WindowMode(147) / ExitMode(17)：记录最后一次模式值，返回 1 = 生效
 * （参数必须是 int，且分别落在 -1..6 / 0..4）。 */
int fvp_vm_window_mode(FvpVM *vm, const FvpValue *mode);
int fvp_vm_exit_mode(FvpVM *vm, const FvpValue *mode);
/* TextBuff(110)：记录文本窗口缓冲尺寸，返回 1 = 生效。
 * a / b 为 NULL 时用默认 8；任一分量 > 0x400 时整条命令放弃（返回 0）。 */
int fvp_vm_text_buff(FvpVM *vm, const FvpValue *idx, const FvpValue *a,
                     const FvpValue *b);
/* SysProjFolder(109)：记录工程目录字符串，返回 1 = 生效（参数是字符串）。 */
int fvp_vm_set_proj_folder(FvpVM *vm, const FvpValue *arg);
int fvp_vm_stalled(const FvpVM *vm);

/* 控制 / 光标这一组（静态分析证据见 docs/vm-commands.md §5 补充表）：
 *   ControlMask(8)   → control_mask = (arg.type == 0)
 *   ControlPulse(9)  → pulse = 1
 *   CursorChange(10) → cursor_shape = 0..3（别的值直接忽略）
 *   CursorMove(11)   → cursor_x / cursor_y，第三参数是"强制"标志（type==0 就忽略）
 * 这几条在原版里写的是 engine 的字节字段（+0x458 / +0xfd8 / +0x88）和鼠标坐标，
 * 消费方分别是动画时间轴、帧更新和 Win32 SetCursor/鼠标位置 —— 本站只把这几个
 * 状态记录下来对外可见（与 TextFont 等一样：有证据的部分落库，不猜下游行为）。 */
void fvp_vm_control_mask(FvpVM *vm, const FvpValue *arg);
void fvp_vm_control_pulse(FvpVM *vm);
int fvp_vm_cursor_change(FvpVM *vm, const FvpValue *arg);
int fvp_vm_cursor_move(FvpVM *vm, const FvpValue *x, const FvpValue *y,
                       const FvpValue *force);

/* 鼠标滚轮：外壳/测试累加增量（120 = 一格），脚本通过 InputGetWheel(36) 取走。
 * fvp_vm_wheel_take() 返回累计值并把累加器清零（取走语义）。 */
void fvp_vm_wheel_add(FvpVM *vm, int delta);
int fvp_vm_wheel_take(FvpVM *vm);

/* Attach (or detach, gfx == NULL) a rendering context.  Only the syscall
 * layer uses it; the VM's own semantics (steps, stacks, temp values, text
 * log) are unchanged whether or not a context is attached. */
void fvp_vm_attach_gfx(FvpVM *vm, struct Gfx *gfx);

/* Attach (or detach, snd == NULL) an audio context.  Only the syscall layer
 * uses it; without an attachment every audio command behaves exactly as it
 * did before (unimplemented / unknown-command counting), which is what keeps
 * bin/fvm.exe byte-identical to the reference.
 *
 * fvp_vm_snd_add_archive() / fvp_vm_snd_add_audio_dir() register the BIN
 * packages the bridge resolves resource names against; they are opened and
 * indexed lazily on the first play (a voice.bin index is ~22k names). */
void fvp_vm_attach_snd(FvpVM *vm, struct Snd *snd);

/* 音频三级音量（槽 × 主 × 类型）合成的有效增益，0..1；静音恒为 0。
 * 抽成纯函数便于单测（不依赖音频设备/解码器），syscall 侧按此实现。 */
float fvp_snd_gain_calc(int slot_vol, int master_vol, int type_vol,
                        int silent);

/* 运行中改主音量（0..100，越界夹紧），立即把新增益推到正在播放的通道。
 * 返回夹紧后的值；桥不存在时返回 -1。 */
int fvp_snd_bridge_set_master(FvpSndBridge *b, int vol);
/* 当前主音量（未配置时按 100 算）。 */
int fvp_snd_bridge_master(const FvpSndBridge *b);
/* BGM 交叉淡入淡出时长（ms，0 = 硬切） */
void fvp_vm_snd_set_bgm_xfade(FvpVM *vm, int ms);
int fvp_vm_snd_add_archive(FvpVM *vm, const char *path);
int fvp_vm_snd_add_audio_dir(FvpVM *vm, const char *dir);

/* Resolve a script resource name to a freshly malloc'd copy of the BIN entry
 * ("bgm/001" -> entry "001" of bgm.bin).  Returns 0 on success, -1 when the
 * name is not found, -2 when the package layer is unavailable / unreadable.
 * *archive_out receives the archive's base name when non-NULL. */
int fvp_snd_bank_read(FvpSndBridge *b, const char *name, uint8_t **data,
                      size_t *size, const char **archive_out);

void fvp_vm_add_text(FvpVM *vm, const FvpValue *layer, FvpStr *text);
void fvp_vm_count_unknown(FvpVM *vm, uint16_t cid);

/* ---- 协程（ThreadStart / ThreadExit）---------------------------------
 * threads_on 关着时（默认）这两个命令是空桩，逐字节基线不变。开着时：
 *   fvp_vm_thread_start(vm, no, pc)  no 越界钳到 0..31；no==0 先把 32 个槽
 *       全部重置（正在跑的线程除外）；槽里原有的线程标记为死、交给调度器回收。
 *   fvp_vm_thread_exit(vm, self, which)  which==0 = 收掉除 self 外所有槽里的
 *       线程；which!=0 = 收掉 which 号槽的线程。返回收掉的个数。
 * 两者都返回 1 = 语义执行了，0 = threads_on 关着（调用方照旧当空桩）。 */
int fvp_vm_thread_start(FvpVM *vm, int no, uint32_t pc);
int fvp_vm_thread_exit(FvpVM *vm, FvpThread *self, int which);
/* ThreadRaise(134)：ms 必须是 int；把 32 个槽里"正在 sleep 且时长 == ms"的
 * 线程叫醒（清 blocked/timed）。返回叫醒的线程数，参数非法返回 -1。 */
int fvp_vm_thread_raise(FvpVM *vm, const FvpValue *ms);

/* ---- 定时器（TimerSet 139 / TimerGet 138 / TimerSuspend 140）----------
 * 实现在 src/timer.c，VM 只负责把 vm.virtual_ms 当 now_ms 传下去。 */
int fvp_vm_timer_set(FvpVM *vm, int id, int ms);
int fvp_vm_timer_get(FvpVM *vm, int id, int opt);
void fvp_vm_timer_suspend(FvpVM *vm, int on);

/* Stack helpers (shared with syscall.c). */
void fvp_thread_push(FvpThread *t, FvpValue v);
FvpValue fvp_thread_pop(FvpThread *t);

void fvp_map_init(FvpMap *m);
void fvp_map_free(FvpMap *m);
FvpValue *fvp_map_get(FvpMap *m, const FvpKey *k);
void fvp_map_set(FvpMap *m, const FvpKey *k, FvpValue v);

#endif /* FVP_VM_H */