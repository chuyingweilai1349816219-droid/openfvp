/* fuzz_alloc_link.c -- links the fuzz-only allocation checker into tools whose
 * main() lives in an existing file (tests/c_assets.c).  Include this single
 * translation unit in the gcc command line; -DFUZZ_ALLOC_GUARD selects the
 * page-guard mode, otherwise the cheaper canary mode is used.
 *
 * No code is needed here: fuzz_alloc.h itself defines malloc/calloc/realloc/
 * free.  Keeping it in its own file avoids touching the read-only sources.
 */
#include "fuzz_alloc.h"