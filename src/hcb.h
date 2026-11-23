/* FVP VM in C -- .hcb script loader and instruction decoder. */
#ifndef FVP_HCB_H
#define FVP_HCB_H

#include "encoding.h"
#include "fvp.h"

/* One decoded instruction.  All jump targets / pcs are absolute file offsets,
 * exactly like the reference implementation's dict keyed by byte offset.
 *
 *   u  : u32 operand (call / jmp / jmpcond / pushint32 / pushfloat bits)
 *   w  : u16 operand (syscall id, pushglobal, popglobal, garr id, initstack b1)
 *   a  : u8 operand (initstack locals, pushstack / copystack / larr index)
 *   b  : second u8 operand (initstack only)
 *   str: decoded UTF-8 string for pushstring
 */
typedef struct FvpInstr {
    uint32_t pc;
    uint32_t u;
    uint32_t size;
    uint16_t w;
    uint8_t op;
    uint8_t a;
    uint8_t b;
    uint8_t pad;
    FvpStr *str;
} FvpInstr;

typedef struct FvpHcb {
    unsigned char *data;   /* whole file */
    size_t data_len;
    uint32_t script_len;   /* u32 at offset 0 */
    uint32_t main_start;   /* u32 at script_len */
    FvpInstr *instrs;
    uint32_t ninstr;
    uint32_t instr_cap;
    int32_t *pcidx;        /* script_len entries: instr index or -1 */
    FvpArena strings;      /* interned pushstring strings */
    FvpEncoding enc;
} FvpHcb;

/* Load + decode.  Returns 1 on success; on failure fills err and returns 0. */
int fvp_hcb_load(FvpHcb *hcb, const char *path, FvpEncoding enc,
                 char *err, size_t errlen);
void fvp_hcb_free(FvpHcb *hcb);

/* Instruction at a byte offset, or NULL (reference: Code.at() -> None). */
const FvpInstr *fvp_hcb_at(const FvpHcb *hcb, uint32_t pc);

const char *fvp_opcode_name(uint8_t op);

#endif /* FVP_HCB_H */