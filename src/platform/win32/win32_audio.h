/* FVP Win32 desktop shell -- waveOut audio output. */
#ifndef FVP_WIN32_AUDIO_H
#define FVP_WIN32_AUDIO_H

#include <stdint.h>

#include "snd.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct FvpAudio FvpAudio;

/* Start a waveOut device fed from snd_mix().
 *
 *   rate            engine sample rate (48000)
 *   channels        always 2 for the engine mix
 *   buffers         number of queued buffers (2 = classic double buffering)
 *   frames          frames per buffer (buffer duration = frames/rate)
 *   tone_hz         >0 replaces snd_mix with a sine self-test tone (Hz)
 *
 * Returns NULL when waveOut cannot be opened (no sound card / no driver); the
 * shell keeps running silently in that case. */
FvpAudio *fvp_audio_start(Snd *snd, int rate, int channels, int buffers,
                          int frames, int tone_hz);

void fvp_audio_stop(FvpAudio *a);

/* Counters for the periodic log line.  Any out pointer may be NULL. */
void fvp_audio_stats(FvpAudio *a, int *ok, int *submitted, int *completed,
                     int *underruns, unsigned long *error);

/* One-line description of the device state (for logs). */
const char *fvp_audio_status(FvpAudio *a);

#ifdef __cplusplus
}
#endif

#endif /* FVP_WIN32_AUDIO_H */