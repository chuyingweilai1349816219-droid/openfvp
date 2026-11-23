/* FVP Win32 desktop shell -- waveOut double buffering.
 *
 * A dedicated thread owns the waveOut device: it waits for a buffer-completion
 * event, refills every finished buffer from snd_mix() (or from a sine
 * generator in self-test mode) and re-queues it.  The mixer is only ever
 * touched from this thread, which matches snd.c's "single consumer" design.
 *
 * The device is opened with CALLBACK_EVENT (no window messages) so the shell
 * works even when the rendering thread is busy in GDI/VM code.
 */
#include "win32_audio.h"

#include <windows.h>
#include <mmsystem.h>

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "win32_log.h"

struct FvpAudio {
    HWAVEOUT hwo;
    HANDLE event;
    HANDLE thread;
    WAVEHDR *hdrs;
    int *prepared;
    int16_t *pcm;
    int buffers;
    int frames;
    int rate;
    int channels;
    int tone_hz;
    double phase;
    Snd *snd;
    volatile LONG quit;
    volatile LONG ok;
    volatile LONG submitted;
    volatile LONG completed;
    volatile LONG underruns;
    volatile LONG last_error;
    char status[160];
};

static const char *wave_out_error_string(MMRESULT r)
{
    switch (r) {
    case MMSYSERR_ALLOCATED:
        return "device already in use";
    case MMSYSERR_BADDEVICEID:
        return "bad device id";
    case MMSYSERR_NODRIVER:
        return "no audio driver";
    case MMSYSERR_NOMEM:
        return "out of memory";
    case WAVERR_BADFORMAT:
        return "unsupported format";
    case WAVERR_SYNC:
        return "device opened synchronously";
    default:
        return "waveOut error";
    }
}

static int wave_format(FvpAudio *a, WAVEFORMATEX *fmt)
{
    memset(fmt, 0, sizeof(*fmt));
    fmt->wFormatTag = WAVE_FORMAT_PCM;
    fmt->nChannels = (WORD)a->channels;
    fmt->nSamplesPerSec = (DWORD)a->rate;
    fmt->wBitsPerSample = 16;
    fmt->nBlockAlign = (WORD)(a->channels * 2);
    fmt->nAvgBytesPerSec = (DWORD)(a->rate * fmt->nBlockAlign);
    fmt->cbSize = 0;
    return 1;
}

/* Fill buffer `idx` with one buffer worth of stereo PCM16. */
static void fill_buffer(FvpAudio *a, int idx)
{
    int16_t *dst = a->pcm + (size_t)idx * (size_t)a->frames * 2u;
    int i;

    if (a->tone_hz > 0) {
        double step = 2.0 * 3.14159265358979323846 * (double)a->tone_hz /
                      (double)a->rate;
        for (i = 0; i < a->frames; ++i) {
            int16_t v = (int16_t)(sin(a->phase) * 6000.0);
            dst[i * 2] = v;
            dst[i * 2 + 1] = v;
            a->phase += step;
            if (a->phase > 2.0 * 3.14159265358979323846) {
                a->phase -= 2.0 * 3.14159265358979323846;
            }
        }
        return;
    }
    if (a->snd != NULL) {
        snd_mix(a->snd, dst, a->frames);
        return;
    }
    memset(dst, 0, (size_t)a->frames * 2u * sizeof(int16_t));
}

static int queue_buffer(FvpAudio *a, int idx)
{
    WAVEHDR *h = &a->hdrs[idx];
    MMRESULT r;

    memset(&h->dwUser, 0, sizeof(h->dwUser));
    h->lpData = (LPSTR)(a->pcm + (size_t)idx * (size_t)a->frames * 2u);
    h->dwBufferLength = (DWORD)((size_t)a->frames * 2u * sizeof(int16_t));
    h->dwBytesRecorded = 0;
    h->dwFlags = 0;

    r = waveOutPrepareHeader(a->hwo, h, sizeof(WAVEHDR));
    if (r != MMSYSERR_NOERROR) {
        fvp_logf("audio: waveOutPrepareHeader(%d) failed: %lu", idx,
                 (unsigned long)r);
        return 0;
    }
    a->prepared[idx] = 1;
    fill_buffer(a, idx);
    r = waveOutWrite(a->hwo, h, sizeof(WAVEHDR));
    if (r != MMSYSERR_NOERROR) {
        fvp_logf("audio: waveOutWrite(%d) failed: %lu", idx, (unsigned long)r);
        waveOutUnprepareHeader(a->hwo, h, sizeof(WAVEHDR));
        a->prepared[idx] = 0;
        return 0;
    }
    InterlockedIncrement(&a->submitted);
    return 1;
}

static DWORD WINAPI audio_thread(LPVOID param)
{
    FvpAudio *a = (FvpAudio *)param;
    int i;

    while (InterlockedCompareExchange(&a->quit, 0, 0) == 0) {
        DWORD wait = WaitForSingleObject(a->event, 200);
        if (InterlockedCompareExchange(&a->quit, 0, 0) != 0) {
            break;
        }
        if (wait == WAIT_OBJECT_0) {
            int did = 0;
            for (i = 0; i < a->buffers; ++i) {
                WAVEHDR *h = &a->hdrs[i];
                if (a->prepared[i] && (h->dwFlags & WHDR_DONE) != 0) {
                    waveOutUnprepareHeader(a->hwo, h, sizeof(WAVEHDR));
                    a->prepared[i] = 0;
                    InterlockedIncrement(&a->completed);
                    if (!queue_buffer(a, i)) {
                        InterlockedIncrement(&a->underruns);
                    }
                    did = 1;
                }
            }
            /* Several completions may collapse into one auto-reset event, so
             * a wakeup with nothing to refill is benign and NOT an underrun. */
            (void)did;
        } else if (wait == WAIT_TIMEOUT) {
            /* No buffer finished within four buffer durations: the device is
             * not consuming data (no sound card / suspended endpoint).  Keep
             * counting so the log shows it instead of hanging. */
            InterlockedIncrement(&a->underruns);
        }
    }
    return 0;
}

FvpAudio *fvp_audio_start(Snd *snd, int rate, int channels, int buffers,
                          int frames, int tone_hz)
{
    FvpAudio *a;
    WAVEFORMATEX fmt;
    MMRESULT r;
    int i;

    if (rate <= 0) {
        rate = 48000;
    }
    if (channels != 2) {
        channels = 2;
    }
    if (buffers < 2) {
        buffers = 2;
    }
    if (buffers > 16) {
        buffers = 16;
    }
    if (frames < 64) {
        frames = 64;
    }
    if (frames > 8192) {
        frames = 8192;
    }

    a = (FvpAudio *)calloc(1, sizeof(FvpAudio));
    if (a == NULL) {
        fvp_logf("audio: out of memory");
        return NULL;
    }
    a->rate = rate;
    a->channels = channels;
    a->buffers = buffers;
    a->frames = frames;
    a->tone_hz = tone_hz;
    a->snd = snd;
    a->phase = 0.0;

    a->hdrs = (WAVEHDR *)calloc((size_t)buffers, sizeof(WAVEHDR));
    a->prepared = (int *)calloc((size_t)buffers, sizeof(int));
    a->pcm = (int16_t *)calloc((size_t)buffers * (size_t)frames * 2u,
                               sizeof(int16_t));
    if (a->hdrs == NULL || a->prepared == NULL || a->pcm == NULL) {
        fvp_logf("audio: out of memory");
        fvp_audio_stop(a);
        return NULL;
    }

    a->event = CreateEventW(NULL, FALSE, FALSE, NULL);
    if (a->event == NULL) {
        fvp_logf("audio: CreateEvent failed (%lu)",
                 (unsigned long)GetLastError());
        fvp_audio_stop(a);
        return NULL;
    }

    wave_format(a, &fmt);
    r = waveOutOpen(&a->hwo, WAVE_MAPPER, &fmt, (DWORD_PTR)a->event, 0,
                    CALLBACK_EVENT);
    if (r != MMSYSERR_NOERROR) {
        fvp_logf("audio: waveOutOpen failed: %lu (%s) -- running silent",
                 (unsigned long)r, wave_out_error_string(r));
        a->hwo = NULL;
        fvp_audio_stop(a);
        return NULL;
    }

    a->thread = CreateThread(NULL, 0, audio_thread, a, 0, NULL);
    if (a->thread == NULL) {
        fvp_logf("audio: CreateThread failed (%lu)",
                 (unsigned long)GetLastError());
        fvp_audio_stop(a);
        return NULL;
    }

    for (i = 0; i < a->buffers; ++i) {
        if (!queue_buffer(a, i)) {
            break;
        }
    }
    if (i != a->buffers) {
        fvp_logf("audio: only %d/%d buffers queued", i, a->buffers);
    }

    a->ok = 1;
    snprintf(a->status, sizeof(a->status),
             "waveOut %d Hz %dch PCM16, %d x %d frames (%.1f ms), tone=%d Hz",
             rate, channels, buffers, frames,
             1000.0 * (double)frames / (double)rate, tone_hz);
    fvp_logf("audio: %s", a->status);
    return a;
}

void fvp_audio_stop(FvpAudio *a)
{
    int i;

    if (a == NULL) {
        return;
    }
    InterlockedExchange(&a->quit, 1);
    if (a->event != NULL) {
        SetEvent(a->event);
    }
    if (a->thread != NULL) {
        WaitForSingleObject(a->thread, 3000);
        CloseHandle(a->thread);
        a->thread = NULL;
    }
    if (a->hwo != NULL) {
        waveOutReset(a->hwo);
        for (i = 0; i < a->buffers; ++i) {
            if (a->prepared != NULL && a->prepared[i]) {
                waveOutUnprepareHeader(a->hwo, &a->hdrs[i], sizeof(WAVEHDR));
                a->prepared[i] = 0;
            }
        }
        waveOutClose(a->hwo);
        a->hwo = NULL;
    }
    if (a->event != NULL) {
        CloseHandle(a->event);
        a->event = NULL;
    }
    free(a->hdrs);
    free(a->prepared);
    free(a->pcm);
    free(a);
}

void fvp_audio_stats(FvpAudio *a, int *ok, int *submitted, int *completed,
                     int *underruns, unsigned long *error)
{
    if (a == NULL) {
        if (ok != NULL) {
            *ok = 0;
        }
        if (submitted != NULL) {
            *submitted = 0;
        }
        if (completed != NULL) {
            *completed = 0;
        }
        if (underruns != NULL) {
            *underruns = 0;
        }
        if (error != NULL) {
            *error = 0;
        }
        return;
    }
    if (ok != NULL) {
        *ok = (int)a->ok;
    }
    if (submitted != NULL) {
        *submitted = (int)InterlockedCompareExchange(&a->submitted, 0, 0);
    }
    if (completed != NULL) {
        *completed = (int)InterlockedCompareExchange(&a->completed, 0, 0);
    }
    if (underruns != NULL) {
        *underruns = (int)InterlockedCompareExchange(&a->underruns, 0, 0);
    }
    if (error != NULL) {
        *error = (unsigned long)InterlockedCompareExchange(&a->last_error, 0, 0);
    }
}

const char *fvp_audio_status(FvpAudio *a)
{
    if (a == NULL) {
        return "disabled";
    }
    return a->status;
}