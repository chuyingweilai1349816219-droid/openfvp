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