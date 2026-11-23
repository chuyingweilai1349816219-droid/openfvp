/* FVP VM in C -- syscall dispatch table.
 *
 * Prototype-stage behaviour only: every command is a stub whose return value
 * matches the Python reference implementation (fvp/ref/fvm_ref.py).  Rendering,
 * audio and video are out of scope here; the point is bit-identical text
 * output, which depends only on these return values and the input model.
 */
#ifndef FVP_SYSCALL_H
#define FVP_SYSCALL_H

#include "vm.h"

void fvp_syscall(FvpVM *vm, FvpThread *t, uint16_t cid);

#endif /* FVP_SYSCALL_H */