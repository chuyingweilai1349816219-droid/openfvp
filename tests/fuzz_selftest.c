/* fuzz_selftest.c -- proves the fuzz allocator really detects overflows.
 *
 * Deliberately writes one byte past a heap block:
 *
 *   guard build  (-DFUZZ_ALLOC_GUARD)  the byte lands in the PAGE_NOACCESS
 *                guard page, so the process dies with an access violation
 *                (run_all treats a non-zero exit as "detected").
 *   canary build the byte lands in the 16-byte 0xA5 trailer, so free()
 *                reports "heap write past end" and exits 72.
 *
 * If the allocator were a no-op (plain libc malloc) this program would simply
 * exit 0 -- which is exactly what the self-test is there to rule out.
 */
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "fuzz_alloc.h"

int main(void)
{
    unsigned char *p = (unsigned char *)malloc(32);
    if (p == NULL) {
        fprintf(stderr, "selftest: malloc failed\n");
        return 2;
    }
    memset(p, 0x41, 32);
    fprintf(stderr, "selftest: writing 1 byte past a 32-byte block\n");
    fflush(stderr);
    p[32] = 0x42; /* the fault this self-test is supposed to trigger */
    free(p);
    printf("SELFTEST NOT DETECTED\n");
    return 0;
}