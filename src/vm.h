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
    int pending;
} FvpFrame;

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
    FvpSndSlot *chan_owner[SND_CH_COUNT]; /* who plays on each SND chan */
    FvpSndStats stats;
    void (*on_event)(void *ud, const FvpSndEvent *ev);
    void *event_ud;
} FvpSndBridge;

/* GraphRGB 的槽号上限（静态分析 0..0xFFF）。 */
#define FVP_GRAPH_SLOTS 4096

/* 字体表上限（外壳实际只会注册几套位图字集）。 */
#define FVP_MAX_FONT_NAMES 16

typedef struct FvpVM {
    FvpHcb *hcb;
    FvpValue globals[FVP_GLOBAL_COUNT];
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
    unsigned char control_mask; /* engine+0x458：0/1（null 分支不可达，见 vm.c） */
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
uint64_t fvp_vm_run(FvpVM *vm, uint64_t max_steps);

/* 打开卡死侦测：连续 n 步没有 syscall 就停下（n == 0 关闭，默认关闭）。
 * 命中后 fvp_vm_stalled() 返回 1，细节看 FvpVM 的 stall_* 字段。 */
void fvp_vm_set_stall_limit(FvpVM *vm, uint64_t n);

/* GraphRGB(23)：记录图形槽的颜色调制，返回 1 = 生效（槽号合法）。 */
int fvp_vm_graph_rgb(FvpVM *vm, const FvpValue *slot, const FvpValue *r,
                     const FvpValue *g, const FvpValue *b);
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