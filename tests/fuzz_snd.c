/* fuzz_snd.c -- audio-path robustness harness (Ogg Vorbis / RIFF-WAVE).
 *
 * c_assets.exe only covers the BIN/HZC1/inflate layers, so the audio path is
 * exercised through this tiny driver instead of the GUI.  It decodes one file
 * with the same snd_play_file() the engine uses, then mixes a bounded number
 * of frames (so a malformed file cannot make us write past the mix buffer).
 *
 * Output protocol (parsed by tests/fuzz_assets.py):
 *   KIND ogg|wav|unknown
 *   RESULT OK frames=<n> rate=<r> channels=<c>
 *   RESULT REJECT kind=<k> rc=<n>
 *   ALLOC mode=... ...          (from fuzz_alloc.h)
 * Exit code: 0 = decoded, 1 = rejected with a diagnostic, 71/72 = memory
 * fault detected by the harness allocator.
 *
 * Build (driven by fuzz_assets.py):
 *   gcc ... -I src -I tests -o out/fuzz/fuzz_snd.exe \
 *       tests/fuzz_snd.c src/snd.c -lm
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "fuzz_alloc.h"

#include "../src/snd.h"

#define MIX_FRAMES 2048

int main(int argc, char **argv)
{
    const char *path;
    Snd *s;
    const char *kind = "unknown";
    int rc;
    int frames;
    int rate = 0;
    int ch = 0;
    int16_t *mix;
    int i;

    if (argc < 2) {
        fprintf(stderr, "usage: %s <input.ogg|input.wav>\n", argv[0]);
        return 2;
    }
    path = argv[1];
    {
        FILE *f = fopen(path, "rb");
        unsigned char hdr[4];
        if (f != NULL) {
            if (fread(hdr, 1, 4, f) == 4) {
                if (memcmp(hdr, "OggS", 4) == 0) {
                    kind = "ogg";
                } else if (memcmp(hdr, "RIFF", 4) == 0) {
                    kind = "wav";
                }
            }
            fclose(f);
        }
    }
    printf("KIND %s\n", kind);

    s = snd_create(44100, 2);
    if (s == NULL) {
        fprintf(stderr, "fuzz_snd: cannot create the mixer\n");
        return 2;
    }
    rc = snd_play_file(s, SND_CH_BGM, path, 0, 1.0f);
    if (rc != 0) {
        printf("RESULT REJECT kind=%s rc=%d\n", kind, rc);
        fflush(stdout);
        snd_destroy(s);
        faq_print_stats();
        return 1;
    }
    frames = snd_channel_frames(s, SND_CH_BGM);
    (void)snd_last_info(&rate, &ch, NULL);
    printf("RESULT OK frames=%d rate=%d channels=%d\n", frames, rate, ch);
    mix = (int16_t *)malloc((size_t)MIX_FRAMES * 2u * sizeof(int16_t));
    if (mix == NULL) {
        snd_destroy(s);
        fprintf(stderr, "fuzz_snd: out of memory\n");
        return 2;
    }
    snd_mix(s, mix, MIX_FRAMES);
    for (i = 0; i < MIX_FRAMES * 2; ++i) {
        if (mix[i] != 0) { /* touch the samples so the mix is not dead code */
            break;
        }
    }
    free(mix);
    snd_destroy(s);
    faq_print_stats();
    return 0;
}