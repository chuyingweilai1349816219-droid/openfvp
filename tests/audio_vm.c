/* audio_vm.c -- end-to-end test: script VM + audio layer.
 *
 * Runs the real HCB script through the C VM (src/vm.c + src/syscall.c) with
 * the verified audio layer (src/snd.c) attached through fvp_vm_attach_snd()
 * and the game's six audio BIN packages registered as the resource bank.
 * There is no sound card involved: the "clock" is a fixed number of VM steps
 * per chunk, after which snd_mix() produces the stereo PCM16 the platform
 * would hand to waveOut / AAudio.
 *
 * The point is the wiring added in src/syscall.c: AudioLoad/AudioPlay/
 * AudioStop/AudioVol/AudioType/AudioState/AudioSilentOn and the legacy
 * SoundLoad/SoundPlay/SoundStop/SoundVol/SoundType/SoundTypeVol/
 * SoundMasterVol commands must resolve the script's resource names
 * ("bgm/001", "voice/01001530", "se/201", ...) to BIN entries, decode them
 * lazily at play time and put non-silent samples through the mixer.
 *
 * Reported numbers: per-command counts, every played resource name (unique),
 * unresolved names, decode failures, mixed-output peak / RMS.  Exit code 0
 * iff at least one BGM or SE (or voice) play succeeded and the mix was not
 * silent.
 *
 * This program is NOT part of bin/fvm.exe: fvm.exe never attaches an audio
 * context and keeps its byte-exact prototype behaviour (compare.py).
 *
 * Usage:
 *   audio_vm.exe <script.hcb> [options]
 *     --game DIR        directory holding the *.bin packages (default: the
 *                       script's directory)
 *     --steps N         VM step budget (default 900000)
 *     --click N         synthetic click period (default 600, same as
 *                       compare.py's main acceptance case)
 *     --enc NAME        text encoding: cp932 (default) or gbk
 *     --mix-frames N    snd_mix() frames per step chunk (default 1024)
 *     --chunk N         VM steps per mix chunk (default 1000)
 *     --wav-out FILE    also write the mixed PCM16 stream as a RIFF/WAVE
 *     --quiet           only print the final summary / RESULT line
 *
 * Build (deliberately not part of the Makefile):
 *   gcc -std=c99 -O2 -Isrc -o bin/audio_vm.exe tests/audio_vm.c \
 *       src/fvp.c src/encoding.c src/hcb.c src/vm.c src/syscall.c \
 *       src/snd.c src/bin.c -lm
 */
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "encoding.h"
#include "fvp.h"
#include "hcb.h"
#include "snd.h"
#include "vm.h"

#define AUDIO_MAX_UNIQUE 256

typedef struct PlayName {
    char *name;
    char *archive;
    int category;
    int loop;
    int ok;
    int count;
} PlayName;

static PlayName g_unique[AUDIO_MAX_UNIQUE];
static int g_nunique;
static int g_play_lines;
static int g_quiet;
static int g_debug;
static int g_events_shown;
static int g_chunks_with_sound;
static double g_sum_sq;
static double g_peak;
static double g_last_chunk_peak;
static unsigned long long g_mix_samples;
static unsigned long long g_mix_frames;
static uint64_t g_first_play_step;
static uint64_t g_now_step;

static void *xmalloc(size_t n)
{
    void *p = malloc(n);
    if (p == NULL) {
        fprintf(stderr, "audio_vm: out of memory\n");
        exit(2);
    }
    return p;
}

static char *xstrdup(const char *s)
{
    size_t n = strlen(s) + 1u;
    char *p = (char *)xmalloc(n);
    memcpy(p, s, n);
    return p;
}

static const char *cat_name(int cat)
{
    switch (cat) {
    case FVP_SND_CAT_BGM: return "BGM";
    case FVP_SND_CAT_VOICE: return "VOICE";
    case FVP_SND_CAT_SE: return "SE";
    default: return "?";
    }
}

static void record_play(const FvpSndEvent *ev)
{
    int i;
    if (ev->name == NULL) {
        return;
    }
    for (i = 0; i < g_nunique; i++) {
        if (strcmp(g_unique[i].name, ev->name) == 0) {
            g_unique[i].count += 1;
            if (ev->ok) g_unique[i].ok = 1;
            return;
        }
    }
    if (g_nunique >= AUDIO_MAX_UNIQUE) {
        return;
    }
    g_unique[g_nunique].name = xstrdup(ev->name);
    g_unique[g_nunique].archive = ev->archive != NULL ? xstrdup(ev->archive) : NULL;
    g_unique[g_nunique].category = ev->category;
    g_unique[g_nunique].loop = ev->loop;
    g_unique[g_nunique].ok = ev->ok;
    g_unique[g_nunique].count = 1;
    g_nunique += 1;
}

static void on_audio_event(void *ud, const FvpSndEvent *ev)
{
    FvpVM *vm = (FvpVM *)ud;
    Snd *snd = (vm != NULL && vm->snd_bridge != NULL) ? vm->snd_bridge->snd : NULL;
    if (ev->kind != FVP_SND_EV_PLAY) {
        if (g_debug && g_events_shown < 400) {
            printf("  [%8llu] cid=%-3d kind=%d slot=%-3d chan=%2d type=%2d "
                   "vol=%3d ok=%d %s\n",
                   (unsigned long long)g_now_step, ev->cid, ev->kind, ev->slot,
                   ev->chan, ev->type, ev->volume, ev->ok,
                   ev->name != NULL ? ev->name : "(null)");
            g_events_shown += 1;
        }
        return;
    }
    if (ev->ok && g_first_play_step == 0) {
        g_first_play_step = g_now_step;
    }
    record_play(ev);
    if (!g_quiet && g_play_lines < 40) {
        int frames = (snd != NULL && ev->chan >= 0)
                         ? snd_channel_frames(snd, ev->chan)
                         : 0;
        printf("  [%8llu] %-14s slot=%3d chan=%2d %-6s loop=%d %-18s -> "
               "%-12s %s frames=%d\n",
               (unsigned long long)g_now_step,
               ev->cid == 1 ? "AudioPlay" : "SoundPlay",
               ev->slot, ev->chan, cat_name(ev->category), ev->loop,
               ev->name != NULL ? ev->name : "(null)",
               ev->archive != NULL ? ev->archive : "(unresolved)",
               ev->ok ? "ok" : "FAILED", frames);
        g_play_lines += 1;
    }
}

static double now_ms(void)
{
    return (double)clock() * 1000.0 / (double)CLOCKS_PER_SEC;
}

static int dir_of(const char *path, char *out, size_t cap)
{
    const char *slash = strrchr(path, '/');
    const char *bslash = strrchr(path, '\\');
    const char *cut = slash;
    size_t n;
    if (bslash != NULL && (cut == NULL || bslash > cut)) {
        cut = bslash;
    }
    if (cut == NULL) {
        if (cap < 2u) return -1;
        strcpy(out, ".");
        return 0;
    }
    n = (size_t)(cut - path);
    if (n >= cap) n = cap - 1u;
    memcpy(out, path, n);
    out[n] = '\0';
    return 0;
}

static void mix_and_measure(Snd *s, int16_t *buf, int frames)
{
    int i;
    g_last_chunk_peak = 0.0;
    snd_mix(s, buf, frames);
    for (i = 0; i < frames * 2; i++) {
        double v = (double)buf[i];
        if (v < 0.0) v = -v;
        if (v > g_last_chunk_peak) {
            g_last_chunk_peak = v;
        }
        if (v > g_peak) {
            g_peak = v;
        }
        g_sum_sq += (double)buf[i] * (double)buf[i];
    }
    if (g_last_chunk_peak > 0.0) {
        g_chunks_with_sound += 1;
    }
    g_mix_samples += (unsigned long long)frames * 2u;
    g_mix_frames += (unsigned long long)frames;
}

static int wav_open(FILE **out, const char *path)
{
    unsigned char hdr[44];
    FILE *f = fopen(path, "wb");
    if (f == NULL) {
        return -1;
    }
    memset(hdr, 0, sizeof hdr);
    memcpy(hdr, "RIFF", 4);
    memcpy(hdr + 8, "WAVE", 4);
    memcpy(hdr + 12, "fmt ", 4);
    hdr[16] = 16;
    hdr[20] = 1;            /* PCM */
    hdr[22] = 2;            /* stereo */
    hdr[24] = 0x80;         /* 48000 Hz */
    hdr[25] = 0xBB;
    hdr[28] = 0x00;         /* byte rate 192000 */
    hdr[29] = 0xEE;
    hdr[30] = 0x02;
    hdr[32] = 4;            /* block align */
    hdr[34] = 16;           /* bits */
    memcpy(hdr + 36, "data", 4);
    if (fwrite(hdr, 1, sizeof hdr, f) != sizeof hdr) {
        fclose(f);
        return -1;
    }
    *out = f;
    return 0;
}

static void wav_close(FILE *f, unsigned long long frames)
{
    unsigned long data_size = (unsigned long)(frames * 4u);
    unsigned char b[4];
    if (f == NULL) {
        return;
    }
    b[0] = (unsigned char)(data_size & 0xFFu);
    b[1] = (unsigned char)((data_size >> 8) & 0xFFu);
    b[2] = (unsigned char)((data_size >> 16) & 0xFFu);
    b[3] = (unsigned char)((data_size >> 24) & 0xFFu);
    fseek(f, 40, SEEK_SET);
    fwrite(b, 1, 4, f);
    b[0] = (unsigned char)((data_size + 36u) & 0xFFu);
    b[1] = (unsigned char)(((data_size + 36u) >> 8) & 0xFFu);
    b[2] = (unsigned char)(((data_size + 36u) >> 16) & 0xFFu);
    b[3] = (unsigned char)(((data_size + 36u) >> 24) & 0xFFu);
    fseek(f, 4, SEEK_SET);
    fwrite(b, 1, 4, f);
    fclose(f);
}

static void usage(const char *prog)
{
    printf("usage: %s <script.hcb> [options]\n"
           "  --game DIR       directory with the *.bin packages (default: "
           "script dir)\n"
           "  --steps N        VM step budget (default 900000)\n"
           "  --click N        synthetic click period (default 600)\n"
           "  --enc NAME       cp932 (default) or gbk\n"
           "  --mix-frames N   mixer frames per chunk (default 1024)\n"
           "  --chunk N        VM steps per chunk (default 1000)\n"
           "  --wav-out FILE   write the mixed stream as RIFF/WAVE\n"
           "  --quiet          only the final summary\n"
           "  --debug          print every audio event (load/stop/vol/type)\n", prog);
}

int main(int argc, char **argv)
{
    const char *script = NULL;
    const char *game_arg = NULL;
    const char *encname = "cp932";
    const char *wav_path = NULL;
    uint64_t steps = 900000u;
    uint64_t chunk = 1000u;
    int64_t click = 600;
    int mix_frames = 1024;
    int i;
    FvpEncoding enc;
    FvpHcb hcb;
    FvpVM vm;
    Snd *snd = NULL;
    FILE *wav = NULL;
    int16_t *mix = NULL;
    char err[256];
    char game_dir[1024];
    double t0, t1;
    int played = 0;
    double peak;
    double rms;
    int rc = 0;

    for (i = 1; i < argc; i++) {
        const char *a = argv[i];
        if (strcmp(a, "--game") == 0 && i + 1 < argc) {
            game_arg = argv[++i];
        } else if (strcmp(a, "--steps") == 0 && i + 1 < argc) {
            steps = (uint64_t)strtoull(argv[++i], NULL, 10);
        } else if (strcmp(a, "--click") == 0 && i + 1 < argc) {
            click = (int64_t)strtoll(argv[++i], NULL, 10);
        } else if (strcmp(a, "--enc") == 0 && i + 1 < argc) {
            encname = argv[++i];
        } else if (strcmp(a, "--mix-frames") == 0 && i + 1 < argc) {
            mix_frames = (int)strtol(argv[++i], NULL, 10);
        } else if (strcmp(a, "--chunk") == 0 && i + 1 < argc) {
            chunk = (uint64_t)strtoull(argv[++i], NULL, 10);
        } else if (strcmp(a, "--wav-out") == 0 && i + 1 < argc) {
            wav_path = argv[++i];
        } else if (strcmp(a, "--quiet") == 0) {
            g_quiet = 1;
        } else if (strcmp(a, "--debug") == 0) {
            g_debug = 1;
        } else if (strcmp(a, "--help") == 0 || strcmp(a, "-h") == 0) {
            usage(argv[0]);
            return 0;
        } else if (a[0] == '-' && a[1] != '\0') {
            usage(argv[0]);
            return 2;
        } else if (script == NULL) {
            script = a;
        } else {
            fprintf(stderr, "audio_vm: unexpected argument %s\n", a);
            return 2;
        }
    }
    if (script == NULL) {
        usage(argv[0]);
        return 2;
    }
    if (chunk < 1) chunk = 1;
    if (mix_frames < 1) mix_frames = 1;
    if (click < 0) click = 0;
    if (!fvp_encoding_from_name(encname, &enc)) {
        fprintf(stderr, "audio_vm: unknown encoding %s\n", encname);
        return 2;
    }
    if (game_arg == NULL) {
        if (dir_of(script, game_dir, sizeof game_dir) != 0) {
            strcpy(game_dir, ".");
        }
        game_arg = game_dir;
    }

    if (!fvp_hcb_load(&hcb, script, enc, err, sizeof err)) {
        fprintf(stderr, "audio_vm: %s\n", err);
        fvp_hcb_free(&hcb);
        return 3;
    }

    snd = snd_create(48000, 2);
    if (snd == NULL) {
        fprintf(stderr, "audio_vm: snd_create failed\n");
        fvp_hcb_free(&hcb);
        return 3;
    }
    mix = (int16_t *)xmalloc((size_t)mix_frames * 2u * sizeof(int16_t));

    memset(&vm, 0, sizeof vm);
    fvp_vm_init(&vm, &hcb, 1);
    vm.click_period = click;
    fvp_vm_attach_snd(&vm, snd);
    if (vm.snd_bridge == NULL) {
        fprintf(stderr, "audio_vm: fvp_vm_attach_snd failed\n");
        rc = 3;
        goto done;
    }
    vm.snd_bridge->on_event = on_audio_event;
    vm.snd_bridge->event_ud = &vm;
    {
        int added = fvp_vm_snd_add_audio_dir(&vm, game_arg);
        if (added == 0) {
            fprintf(stderr, "audio_vm: no audio packages registered under '%s'\n",
                    game_arg);
            rc = 3;
            goto done;
        }
    }
    if (wav_path != NULL) {
        if (wav_open(&wav, wav_path) != 0) {
            fprintf(stderr, "audio_vm: cannot write %s\n", wav_path);
            rc = 3;
            goto done;
        }
    }

    fvp_vm_start(&vm);
    t0 = now_ms();
    while (vm.steps < steps) {
        uint64_t before = vm.steps;
        uint64_t next = vm.steps + chunk;
        if (next > steps) next = steps;
        g_now_step = vm.steps;
        (void)fvp_vm_run(&vm, next);
        g_now_step = vm.steps;
        if (vm.steps == before) {
            break; /* no runnable thread left */
        }
        mix_and_measure(snd, mix, mix_frames);
        if (wav != NULL) {
            fwrite(mix, sizeof(int16_t) * 2u, (size_t)mix_frames, wav);
        }
        if (g_debug && g_last_chunk_peak > 0.0 && g_chunks_with_sound < 20) {
            printf("  [%8llu] mix peak=%.0f\n",
                   (unsigned long long)vm.steps, g_last_chunk_peak);
        }
        if (g_debug && vm.steps >= 29000u && vm.steps <= 45000u) {
            FvpSndSlot *own = vm.snd_bridge->chan_owner[SND_CH_BGM];
            printf("  [%8llu] state bgm: active=%d pos=%d owner=%s vol=%d "
                   "type=%d silent=%d master=%d master_configured=%d\n",
                   (unsigned long long)vm.steps,
                   snd_is_playing(snd, SND_CH_BGM),
                   snd_channel_pos(snd, SND_CH_BGM),
                   (own != NULL && own->name != NULL) ? own->name : "(none)",
                   own != NULL ? own->volume : -1,
                   own != NULL ? own->type : -1,
                   own != NULL ? own->silent : -1,
                   vm.snd_bridge->master_vol,
                   vm.snd_bridge->master_configured);
        }
    }
    t1 = now_ms();

    if (wav != NULL) {
        wav_close(wav, g_mix_frames);
        wav = NULL;
    }

    /* A little tail so a BGM that started near the step limit still shows up
     * in the measured output. */
    if (vm.snd_bridge->stats.plays > 0) {
        played = 1;
    }
    if (played) {
        int k;
        for (k = 0; k < 8; k++) {
            mix_and_measure(snd, mix, mix_frames);
        }
        if (wav_path != NULL) {
            /* the tail is not part of the dumped file; keep it simple */
        }
    }

    peak = g_peak;
    rms = g_mix_samples > 0
              ? sqrt(g_sum_sq / (double)g_mix_samples) : 0.0;

    printf("\n=== audio summary ===\n");
    printf("steps             %llu (%.1f ms)\n",
           (unsigned long long)vm.steps, t1 - t0);
    printf("audio commands    loads=%u plays=%u stops=%u vol=%u type=%u "
           "state=%u silent=%u\n",
           vm.snd_bridge->stats.loads, vm.snd_bridge->stats.plays,
           vm.snd_bridge->stats.stops, vm.snd_bridge->stats.vols,
           vm.snd_bridge->stats.types, vm.snd_bridge->stats.states,
           vm.snd_bridge->stats.silent_on);
    printf("plays by cat      BGM=%u VOICE=%u SE=%u\n",
           vm.snd_bridge->stats.plays_bgm, vm.snd_bridge->stats.plays_voice,
           vm.snd_bridge->stats.plays_se);
    printf("resource          resolved=%u unresolved=%u decode_fail=%u\n",
           vm.snd_bridge->stats.resolved, vm.snd_bridge->stats.unresolved,
           vm.snd_bridge->stats.decode_fail);
    printf("first successful play at step %llu\n",
           (unsigned long long)g_first_play_step);
    printf("mixed %llu frames: peak=%.0f rms=%.1f (non-silent chunks: %d)\n",
           g_mix_frames, peak, rms, g_chunks_with_sound);
    if (g_nunique > 0) {
        int k;
        printf("played resources (%d unique):\n", g_nunique);
        for (k = 0; k < g_nunique; k++) {
            printf("  %-6s x%-4d %-20s -> %-12s loop=%d %s\n",
                   cat_name(g_unique[k].category), g_unique[k].count,
                   g_unique[k].name,
                   g_unique[k].archive != NULL ? g_unique[k].archive : "?",
                   g_unique[k].loop, g_unique[k].ok ? "ok" : "FAILED");
        }
    }
    {
        int ok = (vm.snd_bridge->stats.plays_bgm + vm.snd_bridge->stats.plays_se +
                  vm.snd_bridge->stats.plays_voice) > 0;
        if (ok && (vm.snd_bridge->stats.plays_bgm +
                   vm.snd_bridge->stats.plays_se) == 0) {
            ok = 0; /* the acceptance target explicitly wants BGM or SE */
        }
        if (ok && peak <= 0.0) {
            ok = 0;
        }
        if (vm.snd_bridge->stats.decode_fail != 0) {
            ok = 0;
        }
        if (!ok) {
            rc = 1;
        }
        printf("RESULT: %s -- %s\n", ok ? "PASS" : "FAIL",
               ok ? "audio commands fired and the mix is non-silent"
                  : "no audible BGM/SE play (see the counters above)");
    }

done:
    if (wav != NULL) {
        wav_close(wav, g_mix_frames);
    }
    free(mix);
    fvp_vm_free(&vm);
    if (snd != NULL) {
        snd_destroy(snd);
    }
    fvp_hcb_free(&hcb);
    return rc;
}