#!/usr/bin/env python3
"""Generate C data tables for the C FVP VM port.

Outputs (under fvp/src/):
  gen_cp932.h    Shift-JIS / CP932 -> Unicode decode tables
  gen_gbk.h      GBK -> Unicode decode tables
  gen_cmd_table.h  syscall command names / argc / "returns a value" flags

All tables are derived from Python's own codecs / JSON exports so the C decoder
matches `bytes.decode(enc, 'replace')` byte for byte, and the syscall metadata
matches fvm.py exactly.

The generator also *validates* the decode model against Python for every 1- and
2-byte sequence plus random longer sequences, and (when a .hcb path is given)
against every string in that script.

Usage:
  python fvp/tests/gen_tables.py [iroseka.hcb]
"""
import os
import sys
import json
import random
import struct
import hashlib

HERE = os.path.dirname(os.path.abspath(__file__))
SRC = os.path.abspath(os.path.join(HERE, '..', 'src'))
REF = os.path.abspath(os.path.join(HERE, '..', 'ref'))

NTRAIL = 192          # trail bytes 0x40..0xFF
TRAIL_LO = 0x40
INVALID = 0xFFFF

OPDEF_FMT = {
    0x00: '', 0x01: 'bb', 0x02: 'd', 0x03: 'w', 0x04: '', 0x05: '', 0x06: 'd',
    0x07: 'd', 0x08: '', 0x09: '', 0x0a: 'd', 0x0b: 'w', 0x0c: 'b', 0x0d: 'd',
    0x0e: 's', 0x0f: 'w', 0x10: 'b', 0x11: 'w', 0x12: 'b', 0x13: '', 0x14: '',
    0x15: 'w', 0x16: 'b', 0x17: 'w', 0x18: 'b', 0x19: '', 0x1a: '', 0x1b: '',
    0x1c: '', 0x1d: '', 0x1e: '', 0x1f: '', 0x20: '', 0x21: '', 0x22: '',
    0x23: '', 0x24: '', 0x25: '', 0x26: '', 0x27: '',
}


def build_tables(enc):
    """Return (single[256], lead_row[256], rows[]) for Python codec `enc`."""
    single = [INVALID] * 256
    for b in range(256):
        s = bytes([b]).decode(enc, 'replace')
        if len(s) == 1 and ord(s) != 0xFFFD:
            single[b] = ord(s)
    # discover lead bytes: any byte that is not a valid single byte but can
    # start a valid 2-byte sequence (or at least is reported as a lead by the
    # codec's error position).  Probe by decoding b + every trail byte.
    leads = []
    for b in range(256):
        if single[b] != INVALID:
            continue
        row = None
        for t in range(TRAIL_LO, 0x100):
            s = bytes([b, t]).decode(enc, 'replace')
            if len(s) == 1 and ord(s) != 0xFFFD:
                if row is None:
                    row = [INVALID] * NTRAIL
                row[t - TRAIL_LO] = ord(s)
        if row is not None:
            leads.append((b, row))
    leads.sort()
    lead_row = [0xFF] * 256
    rows = []
    for idx, (b, row) in enumerate(leads):
        lead_row[b] = idx
        rows.extend(row)
    return single, lead_row, rows, [b for b, _ in leads]


def model_decode(bs, single, lead_row, rows):
    out = []
    i = 0
    n = len(bs)
    while i < n:
        b = bs[i]
        c = single[b]
        if c != INVALID:
            out.append(chr(c))
            i += 1
            continue
        row = lead_row[b]
        if row != 0xFF and i + 1 < n and bs[i + 1] >= TRAIL_LO:
            c = rows[row * NTRAIL + (bs[i + 1] - TRAIL_LO)]
            if c != INVALID:
                out.append(chr(c))
                i += 2
                continue
        out.append('\ufffd')
        i += 1
    return ''.join(out)


def validate(enc, single, lead_row, rows, verbose=True):
    fails = 0
    for b in range(256):
        bs = bytes([b])
        if model_decode(bs, single, lead_row, rows) != bs.decode(enc, 'replace'):
            if verbose:
                print('MISMATCH single %02x' % b)
            fails += 1
    for b in range(256):
        for t in range(256):
            bs = bytes([b, t])
            if model_decode(bs, single, lead_row, rows) != bs.decode(enc, 'replace'):
                if verbose and fails < 10:
                    print('MISMATCH pair %02x %02x: model=%r py=%r' % (
                        b, t,
                        model_decode(bs, single, lead_row, rows),
                        bs.decode(enc, 'replace')))
                fails += 1
    rnd = random.Random(12345)
    for _ in range(30000):
        n = rnd.randint(3, 24)
        bs = bytes(rnd.randrange(256) for _ in range(n))
        if model_decode(bs, single, lead_row, rows) != bs.decode(enc, 'replace'):
            if verbose and fails < 20:
                print('MISMATCH rand %r: model=%r py=%r' % (
                    bs, model_decode(bs, single, lead_row, rows),
                    bs.decode(enc, 'replace')))
            fails += 1
    return fails


def emit_array(f, name, values, fmt, per_line=12, decl='static const uint16_t'):
    f.write('%s %s[%d] = {\n' % (decl, name, len(values)))
    for i in range(0, len(values), per_line):
        chunk = values[i:i + per_line]
        f.write('    ' + ', '.join(fmt % v for v in chunk) + ',\n')
    f.write('};\n\n')


def write_codec_header(enc, tag):
    single, lead_row, rows, leads = build_tables(enc)
    fails = validate(enc, single, lead_row, rows)
    if fails:
        raise SystemExit('%s: decoder model mismatch (%d cases)' % (enc, fails))
    print('%s: tables ok (%d lead bytes, %d rows, decoder validated)' % (
        enc, len(leads), len(rows) // NTRAIL))
    path = os.path.join(SRC, 'gen_%s.h' % tag)
    with open(path, 'w', encoding='utf-8') as f:
        f.write('/* Generated by fvp/tests/gen_tables.py -- do not edit.\n'
                ' * %s decode tables, derived from Python codecs.%s\n'
                ' * single[b] : U+xxxx for a valid one-byte char, 0xFFFF for a lead byte.\n'
                ' * lead_row[b]: row index into rows[] (0xFF = not a lead byte).\n'
                ' * rows[row*%d + (trail-0x40)] : U+xxxx or 0xFFFF (invalid pair).\n'
                ' */\n' % (enc.upper(), 'codecs', NTRAIL))
        f.write('#ifndef FVP_GEN_%s_H\n#define FVP_GEN_%s_H\n\n' % (tag.upper(), tag.upper()))
        f.write('#include <stdint.h>\n\n')
        emit_array(f, 'fvp_%s_single' % tag, single, '0x%04X', 16)
        emit_array(f, 'fvp_%s_lead_row' % tag, lead_row, '0x%02X', 16,
                   'static const uint8_t')
        emit_array(f, 'fvp_%s_rows' % tag, rows, '0x%04X', 12)
        f.write('#endif\n')
    print('  -> %s' % path)
    return path


def name_returns(name):
    if name.endswith('Test') or name.startswith('InputGet'):
        return True
    return name in ('Rand', 'IntToText', 'FlagGet', 'HistoryGet', 'TimerGet',
                    'TextTest', 'PrimHit', 'GraphLoad', 'InputFlash',
                    'SoundLoad', 'GraphRGB', 'SaveData', 'HistorySet')


def write_cmd_table():
    tbl_path = os.path.join(REF, 'cmd_table.json')
    argc_path = os.path.join(REF, 'cmd_argc.json')
    tbl_raw = open(tbl_path, 'rb').read()
    argc_raw = open(argc_path, 'rb').read()
    tbl = json.loads(tbl_raw.decode('utf-8'))
    argc = json.loads(argc_raw.decode('utf-8'))
    argc = {int(k): int(v) for k, v in argc.items()}
    n = len(tbl)
    names = {c['id']: c['name'] for c in tbl}
    path = os.path.join(SRC, 'gen_cmd_table.h')
    with open(path, 'w', encoding='utf-8') as f:
        f.write('/* Generated by fvp/tests/gen_tables.py -- do not edit.\n'
                ' * Syscall metadata from the frozen reference:\n'
                ' *   fvp/ref/cmd_table.json  sha256 %s\n'
                ' *   fvp/ref/cmd_argc.json   sha256 %s\n'
                ' * argc comes from cmd_argc.json (as fvm_ref.py --cmds does), names\n'
                ' * drive the "returns a value" set and the *Test fallback.\n'
                ' */\n' % (hashlib.sha256(tbl_raw).hexdigest(),
                            hashlib.sha256(argc_raw).hexdigest()))
        f.write('#ifndef FVP_GEN_CMD_TABLE_H\n#define FVP_GEN_CMD_TABLE_H\n\n')
        f.write('#include <stdint.h>\n\n')
        f.write('#define FVP_CMD_COUNT %d\n\n' % n)
        f.write('static const char *const fvp_cmd_names[FVP_CMD_COUNT] = {\n')
        for i in range(n):
            f.write('    "%s",%s\n' % (names.get(i, ''), ''))
        f.write('};\n\n')
        f.write('static const uint8_t fvp_cmd_argc[FVP_CMD_COUNT] = {\n')
        line = []
        for i in range(n):
            line.append('%d' % argc.get(i, 0))
            if len(line) == 16:
                f.write('    ' + ', '.join(line) + ',\n')
                line = []
        if line:
            f.write('    ' + ', '.join(line) + ',\n')
        f.write('};\n\n')
        f.write('/* fvm.py: temp = 0 for these ids when the handler returns None */\n')
        f.write('static const uint8_t fvp_cmd_returns[FVP_CMD_COUNT] = {\n')
        line = []
        for i in range(n):
            line.append('1' if name_returns(names.get(i, '')) else '0')
            if len(line) == 32:
                f.write('    ' + ', '.join(line) + ',\n')
                line = []
        if line:
            f.write('    ' + ', '.join(line) + ',\n')
        f.write('};\n\n#endif\n')
    print('cmd table: %d commands -> %s' % (n, path))
    return path


def check_hcb(path):
    """Decode every pushstring with the generated model and compare to Python."""
    for enc, tag in (('cp932', 'cp932'), ('gbk', 'gbk')):
        single, lead_row, rows, _ = build_tables(enc)
        data = open(path, 'rb').read()
        script_len = struct.unpack_from('<I', data, 0)[0]
        pos = 4
        nstr = 0
        bad = 0
        while pos < script_len:
            op = data[pos]
            pos += 1
            fmt = OPDEF_FMT[op]
            for t in fmt:
                if t == 'b':
                    pos += 1
                elif t == 'w':
                    pos += 2
                elif t == 'd':
                    pos += 4
                elif t == 's':
                    n = data[pos]
                    pos += 1
                    raw = data[pos:pos + n]
                    pos += n
                    body = raw[:-1]
                    nstr += 1
                    if model_decode(body, single, lead_row, rows) != body.decode(enc, 'replace'):
                        bad += 1
        print('%s: checked %d script strings, %d mismatches' % (enc, nstr, bad))
        if bad:
            raise SystemExit('script string decode mismatch')


def main():
    os.makedirs(SRC, exist_ok=True)
    write_codec_header('cp932', 'cp932')
    write_codec_header('gbk', 'gbk')
    write_cmd_table()
    if len(sys.argv) > 1:
        check_hcb(sys.argv[1])


if __name__ == '__main__':
    main()