/* FVP VM in C -- .hcb script loader and instruction decoder. */
#include "hcb.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Instruction formats, indexed by opcode (0x00..0x27):
 *   b = u8, w = u16le, d = u32le, s = u8 length (incl. NUL) + bytes + NUL */
static const char *const fvp_opfmt[40] = {
    "",   "bb", "d",  "w",  "",   "",   "d",  "d",  /* 0x00 */
    "",   "",   "d",  "w",  "b",  "d",  "s",  "w",  /* 0x08 */
    "b",  "w",  "b",  "",   "",   "w",  "b",  "w",  /* 0x10 */
    "b",  "",   "",   "",   "",   "",   "",   "",   /* 0x18 */
    "",   "",   "",   "",   "",   "",   "",   ""    /* 0x20 */
};

static const char *const fvp_opname[40] = {
    "nop", "initstack", "call", "syscall", "ret", "ret2", "jmp", "jmpcond",
    "pushtrue", "pushfalse", "pushint32", "pushint16", "pushint8", "pushfloat",
    "pushstring", "pushglobal", "pushstack", "garr_read", "larr_read",
    "pushtop", "pushtemp", "popglobal", "copystack", "garr_write",
    "larr_write", "neg", "add", "sub", "mul", "div", "mod", "test", "logand",
    "logor", "eq", "neq", "gt", "le", "lt", "ge"
};

const char *fvp_opcode_name(uint8_t op)
{
    return op < 40u ? fvp_opname[op] : "?";
}

static void fvp_hcb_err(char *err, size_t errlen, const char *fmt, ...)
{
    va_list ap;
    if (err == NULL || errlen == 0) {
        return;
    }
    va_start(ap, fmt);
    vsnprintf(err, errlen, fmt, ap);
    va_end(ap);
}

static unsigned char *fvp_read_file(const char *path, size_t *out_len,
                                    char *err, size_t errlen)
{
    FILE *f = fopen(path, "rb");
    unsigned char *data;
    long size;
    size_t got;
    if (f == NULL) {
        fvp_hcb_err(err, errlen, "cannot open script: %s", path);
        return NULL;
    }
    if (fseek(f, 0, SEEK_END) != 0) {
        fclose(f);
        fvp_hcb_err(err, errlen, "cannot seek script");
        return NULL;
    }
    size = ftell(f);
    if (size < 0) {
        fclose(f);
        fvp_hcb_err(err, errlen, "cannot determine script size");
        return NULL;
    }
    if (fseek(f, 0, SEEK_SET) != 0) {
        fclose(f);
        fvp_hcb_err(err, errlen, "cannot rewind script");
        return NULL;
    }
    data = (unsigned char *)fvp_xmalloc((size_t)size + 1u);
    got = fread(data, 1, (size_t)size, f);
    fclose(f);
    if (got != (size_t)size) {
        free(data);
        fvp_hcb_err(err, errlen, "short read on script");
        return NULL;
    }
    data[size] = 0;
    *out_len = (size_t)size;
    return data;
}

static uint32_t fvp_rd32(const unsigned char *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static void fvp_hcb_push_instr(FvpHcb *hcb, const FvpInstr *ins)
{
    if (hcb->ninstr == hcb->instr_cap) {
        uint32_t cap = hcb->instr_cap ? hcb->instr_cap * 2u : 4096u;
        hcb->instrs = (FvpInstr *)fvp_xrealloc(hcb->instrs,
                                               (size_t)cap * sizeof(FvpInstr));
        hcb->instr_cap = cap;
    }
    hcb->instrs[hcb->ninstr] = *ins;
    hcb->ninstr += 1;
}

int fvp_hcb_load(FvpHcb *hcb, const char *path, FvpEncoding enc,
                 char *err, size_t errlen)
{
    size_t pos;
    memset(hcb, 0, sizeof(*hcb));
    fvp_arena_init(&hcb->strings);
    hcb->enc = enc;
    hcb->data = fvp_read_file(path, &hcb->data_len, err, errlen);
    if (hcb->data == NULL) {
        return 0;
    }
    if (hcb->data_len < 8u) {
        fvp_hcb_err(err, errlen, "script too short (%lu bytes)",
                    (unsigned long)hcb->data_len, 0);
        return 0;
    }
    hcb->script_len = fvp_rd32(hcb->data);
    if (hcb->script_len < 4u || (size_t)hcb->script_len + 4u > hcb->data_len) {
        fvp_hcb_err(err, errlen, "bad script length 0x%lx (file %lu bytes)",
                    (unsigned long)hcb->script_len,
                    (unsigned long)hcb->data_len);
        return 0;
    }
    hcb->main_start = fvp_rd32(hcb->data + hcb->script_len);
    hcb->pcidx = (int32_t *)fvp_xmalloc((size_t)hcb->script_len * sizeof(int32_t));
    memset(hcb->pcidx, 0xFF, (size_t)hcb->script_len * sizeof(int32_t));

    pos = 4u;
    while (pos < hcb->script_len) {
        FvpInstr ins;
        const char *fmt;
        size_t at = pos;
        uint8_t op = hcb->data[pos];
        int bad = 0;
        int bcount = 0;
        memset(&ins, 0, sizeof(ins));
        if (op > 39u) {
            fvp_hcb_err(err, errlen, "unknown opcode 0x%lx at offset 0x%lx",
                        (unsigned long)op, (unsigned long)at);
            return 0;
        }
        pos += 1u;
        ins.pc = (uint32_t)at;
        ins.op = op;
        fmt = fvp_opfmt[op];
        for (; *fmt != '\0' && !bad; ++fmt) {
            switch (*fmt) {
            case 'b':
                if (pos + 1u > hcb->data_len) {
                    bad = 1;
                    break;
                }
                if (bcount == 0) {
                    ins.a = hcb->data[pos];
                } else {
                    ins.b = hcb->data[pos];
                }
                bcount += 1;
                pos += 1u;
                break;
            case 'w':
                if (pos + 2u > hcb->data_len) {
                    bad = 1;
                    break;
                }
                ins.w = (uint16_t)((uint16_t)hcb->data[pos] |
                                   (uint16_t)((uint16_t)hcb->data[pos + 1u] << 8));
                pos += 2u;
                break;
            case 'd':
                if (pos + 4u > hcb->data_len) {
                    bad = 1;
                    break;
                }
                ins.u = fvp_rd32(hcb->data + pos);
                pos += 4u;
                break;
            case 's': {
                uint32_t n;
                size_t avail;
                size_t take;
                size_t body;
                if (pos + 1u > hcb->data_len) {
                    bad = 1;
                    break;
                }
                n = hcb->data[pos];
                pos += 1u;
                avail = hcb->data_len - pos;
                take = (size_t)n < avail ? (size_t)n : avail;
                body = take > 0u ? take - 1u : 0u;
                ins.str = fvp_decode(&hcb->strings, hcb->enc,
                                     hcb->data + pos, body);
                pos += (size_t)n;
                break;
            }
            default:
                bad = 1;
                break;
            }
        }
        if (bad) {
            fvp_hcb_err(err, errlen, "truncated instruction 0x%lx at 0x%lx",
                        (unsigned long)op, (unsigned long)at);
            return 0;
        }
        ins.size = (uint32_t)(pos - at);
        hcb->pcidx[at] = (int32_t)hcb->ninstr;
        fvp_hcb_push_instr(hcb, &ins);
    }
    return 1;
}

void fvp_hcb_free(FvpHcb *hcb)
{
    if (hcb == NULL) {
        return;
    }
    free(hcb->data);
    free(hcb->instrs);
    free(hcb->pcidx);
    fvp_arena_free(&hcb->strings);
    memset(hcb, 0, sizeof(*hcb));
}

const FvpInstr *fvp_hcb_at(const FvpHcb *hcb, uint32_t pc)
{
    int32_t idx;
    if (pc < 4u || pc >= hcb->script_len) {
        return NULL;
    }
    idx = hcb->pcidx[pc];
    if (idx < 0) {
        return NULL;
    }
    return &hcb->instrs[idx];
}