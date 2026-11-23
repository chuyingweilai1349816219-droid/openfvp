#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""fuzz_assets.py -- malformed-input robustness tests for the FVP asset layer.

The engine's asset readers (src/bin.c, src/hzc1.c, src/inflate.c, src/snd.c) are
driven with deliberately corrupt packages through the real command-line tools:

  * BIN      c_assets.exe list/dump        (src/bin.c)
  * HZC1     c_assets.exe hdr/rgba         (src/hzc1.c)
  * INFLATE  c_assets.exe inflate/rawinflate (src/inflate.c)
  * AUDIO    fuzz_snd.exe                  (src/snd.c: RIFF/WAVE + Ogg Vorbis)

Every mutant is classified as accepted (tool exited 0), rejected (tool exited 1
*with* a diagnostic), memory-fault (harness allocator caught an out-of-bounds
write/read: exit 71/72), crash (Windows exception) or timeout (hang).  The
report gives the rejection rate per class; the process exits non-zero if a
single crash, hang or memory fault was seen.

Harnesses are built into fvp/out/fuzz/ from the read-only sources; nothing under
fvp/src or the existing tests is modified:

  c_assets_guard.exe  tests/c_assets.c + src/{bin,hzc1,inflate}.c + the
                      page-guard allocator from tests/fuzz_alloc.h
  fuzz_snd.exe        tests/fuzz_snd.c + src/snd.c (+ page-guard allocator)

Because the bundled toolchain has no AddressSanitizer, the page-guard allocator
is the substitute: every heap block ends at a PAGE_NOACCESS page, so an
out-of-bounds read/write faults at the exact instruction instead of silently
corrupting memory.  See fvp/docs/testing.md for the limits of this approach.

Usage:
    python fvp/tests/fuzz_assets.py [--scale F] [--fast] [--jobs N]
                                    [--game DIR] [--extract DIR]
                                    [--ogg FILE] [--wav FILE]
                                    [--cc PATH] [--no-build] [--keep]
                                    [--json PATH]

Exit code 0 = no crash / hang / memory fault, 2 = setup problem, 1 = failures.
"""
import argparse
import concurrent.futures
import json
import os
import random
import shutil
import struct
import subprocess
import sys
import time
import zlib

HERE = os.path.dirname(os.path.abspath(__file__))
FVP = os.path.dirname(HERE)
OUT = os.path.join(FVP, 'out', 'fuzz')
TMP = os.path.join(OUT, 'tmp', 'assets')
DEFAULT_GAME = None  # auto-discovered by find_game_dir()
DEFAULT_EXTRACT = os.path.join(FVP, 'extract', 'graph')
DEFAULT_OGG = os.path.join(FVP, 'work', 'bgm001.ogg')
DEFAULT_WAV = os.path.join(FVP, 'work', 'se001.wav')
PUBLIC_CASSETS = os.path.join(FVP, 'bin', 'c_assets.exe')

SEED = 20261123
def find_game_dir(explicit=None):
    """Locate the installed game directory (the one holding iroseka.hcb).

    Explicit --game wins, then $FVP_GAME, then every Downloads subdirectory
    that contains iroseka.hcb.  Discovery avoids hard-coding the non-ASCII
    path, which is fragile across shells and encodings.
    """
    if explicit:
        return explicit
    env = os.environ.get('FVP_GAME')
    if env and os.path.isdir(env):
        return env
    downloads = os.path.join(os.path.expanduser('~'), 'Downloads')
    try:
        names = sorted(os.listdir(downloads))
    except OSError:
        names = []
    for name in names:
        path = os.path.join(downloads, name)
        if os.path.isdir(path) and os.path.isfile(os.path.join(path,
                                                               'iroseka.hcb')):
            return path
    return None


TIMEOUT = 20.0


# ---------------------------------------------------------------------------
# small helpers
# ---------------------------------------------------------------------------

def log(msg):
    print(msg)
    sys.stdout.flush()


def ensure_dirs():
    os.makedirs(TMP, exist_ok=True)


def read_bytes(path):
    with open(path, 'rb') as f:
        return f.read()


def write_bytes(name, data):
    path = os.path.join(TMP, name)
    with open(path, 'wb') as f:
        f.write(data)
    return path


def patch(data, off, fmt, value):
    b = bytearray(data)
    struct.pack_into(fmt, b, off, value)
    return bytes(b)


def flip_bytes(data, rng, count, lo=0, hi=None):
    b = bytearray(data)
    if hi is None:
        hi = len(b)
    if hi <= lo:
        return bytes(b)
    for _ in range(count):
        pos = rng.randrange(lo, hi)
        b[pos] ^= 1 << rng.randrange(8)
    return bytes(b)


def truncate(data, n):
    return data[:max(0, min(n, len(data)))]


class Result(object):
    __slots__ = ('case', 'category', 'status', 'rc', 'elapsed', 'detail')

    def __init__(self, case, category, status, rc, elapsed, detail=''):
        self.case = case
        self.category = category
        self.status = status
        self.rc = rc
        self.elapsed = elapsed
        self.detail = detail


# ---------------------------------------------------------------------------
# runner
# ---------------------------------------------------------------------------

def run_tool(cmd, timeout=TIMEOUT):
    """Run one command; returns (status, rc, stdout, stderr, elapsed)."""
    t0 = time.perf_counter()
    try:
        p = subprocess.run(cmd, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                           timeout=timeout)
    except subprocess.TimeoutExpired:
        return 'timeout', None, b'', b'', time.perf_counter() - t0
    except OSError as exc:
        return 'os-error', None, b'', str(exc).encode(), time.perf_counter() - t0
    return 'ran', p.returncode, p.stdout, p.stderr, time.perf_counter() - t0


def diagnose(prefix, stdout, stderr):
    text = (stderr or b'') + (stdout or b'')
    if len(text.strip()) == 0:
        return False
    if prefix == 'snd':
        return b'RESULT REJECT' in text or b'fuzz_snd:' in text
    return b'c_assets:' in text


def classify(tool, rc, stdout, stderr, expected):
    """Map an exit code to accepted / rejected / crash / memfault / other."""
    if rc is None:
        return 'other'
    if rc < 0:  # Windows NTSTATUS exceptions arrive negative in Python.
        return 'crash'
    if rc == 0:
        return 'accepted'
    if rc in (71, 72):
        return 'memfault'
    if rc == 1:
        if not diagnose(tool, stdout, stderr):
            return 'undiagnosed'
        return 'rejected'
    return 'other'


class Section(object):
    def __init__(self, name):
        self.name = name
        self.cats = {}
        self.order = []
        self.failures = []

    def add(self, res, expected):
        c = self.cats.get(res.category)
        if c is None:
            c = {'name': res.category, 'total': 0, 'accepted': 0,
                 'rejected': 0, 'crash': 0, 'timeout': 0, 'memfault': 0,
                 'undiagnosed': 0, 'other': 0, 'expected_mismatch': 0,
                 'max_ms': 0.0}
            self.cats[res.category] = c
            self.order.append(res.category)
        c['total'] += 1
        c[res.status] = c.get(res.status, 0) + 1
        c['max_ms'] = max(c['max_ms'], res.elapsed * 1000.0)
        if res.status in ('crash', 'timeout', 'memfault', 'undiagnosed',
                          'other'):
            self.failures.append(res)
        if expected and res.status in ('accepted', 'rejected'):
            if (expected == 'reject') != (res.status == 'rejected'):
                c['expected_mismatch'] += 1

    def summary(self):
        tot = sum(c['total'] for c in self.cats.values())
        out = {'total': tot}
        for key in ('accepted', 'rejected', 'crash', 'timeout', 'memfault',
                    'undiagnosed', 'other', 'expected_mismatch'):
            out[key] = sum(c.get(key, 0) for c in self.cats.values())
        out['categories'] = [self.cats[k] for k in self.order]
        return out


# ---------------------------------------------------------------------------
# builders
# ---------------------------------------------------------------------------

def build_bin(entries, name_table_override=None):
    """entries: [(name_bytes, payload_bytes)] -> a valid BIN package."""
    names = bytearray()
    name_offs = []
    for name, _ in entries:
        name_offs.append(len(names))
        names += name + b'\x00'
    name_tab = names if name_table_override is None else name_table_override
    data_start = 8 + 12 * len(entries) + len(name_tab)
    blobs = bytearray()
    data_offs = []
    for _, payload in entries:
        data_offs.append(data_start + len(blobs))
        blobs += payload
    idx = bytearray()
    for i in range(len(entries)):
        idx += struct.pack('<III', name_offs[i], data_offs[i],
                           len(entries[i][1]))
    return struct.pack('<II', len(entries), len(name_tab)) + bytes(idx) + \
        bytes(name_tab) + bytes(blobs)


def wav_valid(rate=22050, channels=1, bits=16, frames=1000, seed=1):
    rng = random.Random(seed)
    pcm = bytearray()
    for i in range(frames):
        v = int(8000 * ((i % 37) - 18) / 18.0)
        for _ in range(channels):
            pcm += struct.pack('<h', v)
    block = channels * bits // 8
    data = bytes(pcm)
    fmt = struct.pack('<HHIIHH', 1, channels, rate, rate * block, block, bits)
    body = b'WAVE' + b'fmt ' + struct.pack('<I', len(fmt)) + fmt
    if len(fmt) & 1:
        body += b'\x00'
    body += b'data' + struct.pack('<I', len(data)) + data
    return b'RIFF' + struct.pack('<I', len(body)) + body


def parse_hzc1(data):
    if len(data) < 44 or data[:4] != b'hzc1' or data[12:16] != b'NVSG':
        return None
    return {
        'usize': struct.unpack_from('<I', data, 4)[0],
        'fmt': struct.unpack_from('<H', data, 0x12)[0],
        'w': struct.unpack_from('<H', data, 0x14)[0],
        'h': struct.unpack_from('<H', data, 0x16)[0],
        'frames_field': struct.unpack_from('<I', data, 0x20)[0],
    }


def bpp_of(fmt):
    return {0: 3, 1: 4, 2: 4, 3: 1, 4: 1}.get(fmt, 0)


def hzc1_raw_len(hdr):
    fmt = hdr['fmt']
    frames = hdr['frames_field'] if (fmt == 2 and hdr['frames_field']) else 1
    return hdr['w'] * hdr['h'] * bpp_of(fmt) * frames


# ---------------------------------------------------------------------------
# build harnesses
# ---------------------------------------------------------------------------

def find_cc(explicit=None):
    if explicit:
        return [explicit]
    candidates = []
    root = os.path.dirname(FVP)
    candidates.append(os.path.join(root, 'tools', 'w64devkit', 'w64devkit',
                                   'bin', 'gcc.exe'))
    for name in ('gcc.exe', 'gcc'):
        found = shutil.which(name)
        if found:
            candidates.append(found)
    for c in candidates:
        if os.path.isfile(c):
            return [c]
    return None


def build_env():
    """Compiler environment: keep gcc's scratch files inside the workspace.

    The sandbox's system temp directory is not always writable by child
    processes, which makes gcc fail with "Cannot create temporary file".
    """
    env = os.environ.copy()
    tmpdir = os.path.join(OUT, 'tmp', 'gcctmp')
    try:
        os.makedirs(tmpdir, exist_ok=True)
    except OSError:
        return env
    env['TMP'] = tmpdir
    env['TEMP'] = tmpdir
    env['TMPDIR'] = tmpdir
    return env


def build_harnesses(cc, force=False):
    """Compile the instrumented tools.  Returns a dict of paths (or None)."""
    tools = {'c_assets': None, 'snd': None}
    if cc is None:
        return tools
    ensure_dirs()
    os.makedirs(os.path.join(OUT, 'tmp', 'gcctmp'), exist_ok=True)
    common = ['-std=c99', '-O1', '-g', '-Wall', '-Wextra',
              '-Wno-unused-function', '-I', os.path.join(FVP, 'src'),
              '-I', os.path.join(FVP, 'tests')]
    guard = ['-DFUZZ_ALLOC_GUARD']

    ca = os.path.join(OUT, 'c_assets_guard.exe')
    ca_cmd = cc + common + guard + ['-o', ca,
                                    os.path.join(HERE, 'fuzz_alloc_link.c'),
                                    os.path.join(HERE, 'c_assets.c'),
                                    os.path.join(FVP, 'src', 'bin.c'),
                                    os.path.join(FVP, 'src', 'hzc1.c'),
                                    os.path.join(FVP, 'src', 'inflate.c')]
    snd = os.path.join(OUT, 'fuzz_snd.exe')
    snd_cmd = cc + common + guard + ['-o', snd,
                                     os.path.join(HERE, 'fuzz_snd.c'),
                                     os.path.join(FVP, 'src', 'snd.c'), '-lm']
    for path, cmd, key in ((ca, ca_cmd, 'c_assets'), (snd, snd_cmd, 'snd')):
        if os.path.isfile(path) and not force:
            tools[key] = path
            continue
        log('building %s ...' % os.path.basename(path))
        p = subprocess.run(cmd, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                           env=build_env())
        if p.returncode != 0:
            log('  FAILED to build %s:\n%s' % (os.path.basename(path),
                                               p.stderr.decode('utf-8',
                                                               'replace')[-2000:]))
        else:
            tools[key] = path
    return tools


# ---------------------------------------------------------------------------
# BIN section
# ---------------------------------------------------------------------------

def run_bin(ca, args, jobs, scale, real_bin):
    sec = Section('BIN')
    base_entries = [
        (b'plain.dat', b'hello'),
        (b'\x83e\x83X\x83g.bmp', b'\x00\x01\x02\x03'),
        (b'', b''),
        (b'a' * 60, bytes(range(16))),
        (b'x' * 300, b'Z' * 300),
        (b'tab\tname', b'\xff'),
        (b'\x80\xff\x00bad', b'1234567890'),
        (b'last', b'L' * 7),
    ]
    base = build_bin(base_entries)
    n = len(base_entries)
    name_len = struct.unpack_from('<I', base, 4)[0]
    per = max(1, int(round(20 * scale)))
    cases = []

    def add(cat, data, expect, cmd_extra=None):
        path = write_bytes('bin_%03d_%s.bin' % (len(cases), cat), data)
        cases.append((cat, path, expect, cmd_extra))

    # 1. name offset outside the name table
    for k, off in enumerate((name_len, name_len + 1, 0xFFFFFFFF, 0x7FFFFFFF)):
        add('name_off_oob', patch(base, 8 + 12 * (k % n), '<I', off), 'reject')
    # 2. data offset outside the file
    for k, off in enumerate((len(base), len(base) - 1, 0xFFFFFFFF)):
        data = patch(base, 8 + 12 * (k % n) + 4, '<I', off)
        add('data_off_oob', data, 'reject')
    # 3. data offset + size past the end
    for k, size in enumerate((0xFFFFFFFF, len(base), len(base) - 1)):
        data = patch(base, 8 + 12 * (k % n) + 8, '<I', size)
        add('size_oob', data, 'reject')
    # 4. entry count too large
    for count in (0x1000000, 0x1000001, 0xFFFFFFFF, 0x7FFFFFFF, 0x00FFFFFF):
        add('count_huge', patch(base, 0, '<I', count), 'reject')
    # 5. name table length too large / too small
    for nl in (0x10000000, 0x10000001, 0xFFFFFFFF, name_len + 1, 0):
        add('namelen_huge', patch(base, 4, '<I', nl), 'reject')
    # 6. truncated files (entry table, name table, data area)
    for cut in (0, 1, 4, 7, 8, 9, 12, 8 + 12 * n - 1,
                8 + 12 * n + 1, len(base) - 1, len(base) // 2):
        add('truncated', truncate(base, cut), 'reject')
    # 7. name table without a NUL terminator for the last entry
    no_nul = bytearray(base)
    last_name_off = struct.unpack_from('<I', base, 8 + 12 * (n - 1))[0]
    no_nul[8 + 12 * n + last_name_off + 4] = 0x41  # 'last' -> 'lasA' + no NUL
    add('nametab_nul', bytes(no_nul), 'accept')
    # 8. name offset pointing into the middle of a name
    mid = patch(base, 8 + 12 * 0, '<I',
                struct.unpack_from('<I', base, 8)[0] + 1)
    add('nametab_mid', mid, 'accept')
    # 9. header zeroed
    add('header_zeros', b'\x00' * 8 + base[8:], 'reject')
    # 10. random byte flips / random garbage in the index
    rng = random.Random(SEED + 1)
    for i in range(per):
        add('flips', flip_bytes(base, rng, rng.randint(1, 6)), None)
    for i in range(max(2, per // 2)):
        pos = rng.randrange(8, min(len(base), 8 + 12 * n))
        add('flips', patch(base, pos, '<I', rng.getrandbits(32)), None)
    # 11. extra trailing bytes are ignored by the loader
    add('flips', base + b'\xde\xad\xbe\xef' * 64, None)
    # 12. dump a payload (exercises bin_read + 64-bit seek)
    add('dump_inside', base, 'accept', 0)
    # 13. a real shipped package: header field mutations
    if real_bin is not None:
        rb = read_bytes(real_bin)
        try:
            rcount = struct.unpack_from('<I', rb, 0)[0]
        except struct.error:
            rcount = 0
        for i, (off, fmt, val) in enumerate((
                (0, '<I', 0xFFFFFFFF), (0, '<I', rcount + 1),
                (4, '<I', 0xFFFFFFFF), (4, '<I', len(rb)),
                (8, '<I', 0xFFFFFFFF), (8 + 4, '<I', len(rb)),
                (8 + 8, '<I', 0xFFFFFFFF))):
            add('real_pkg', patch(rb, off, fmt, val), None)
        add('real_pkg', truncate(rb, len(rb) // 2), 'reject')

    return execute(sec, cases, ca, 'bin', jobs)


def execute(sec, cases, exe, tool, jobs):
    def one(case):
        cat, path, expect, extra = case
        if tool == 'bin':
            cmd = [exe, 'dump' if extra is not None else 'list', path]
            if extra is not None:
                cmd += [str(extra), os.path.join(TMP, 'bin_dump.out')]
            tname = 'bin'
        else:
            cmd = [exe, path]
            tname = 'snd'
        status, rc, out, err, elapsed = run_tool(cmd)
        st = classify(tname, rc, out, err, expect)
        detail = ''
        if st not in ('accepted', 'rejected'):
            detail = (err or out or b'')[:300].decode('utf-8', 'replace')
        return Result(path, cat, st, rc, elapsed, detail)

    with concurrent.futures.ThreadPoolExecutor(max_workers=jobs) as pool:
        for res, case in zip(pool.map(one, cases), cases):
            sec.add(res, case[2])
    return sec


# ---------------------------------------------------------------------------
# HZC1 / INFLATE section
# ---------------------------------------------------------------------------

def pick_hzc1_base(extract_dir):
    best = None
    if not os.path.isdir(extract_dir):
        return None
    for name in sorted(os.listdir(extract_dir)):
        path = os.path.join(extract_dir, name)
        if not os.path.isfile(path):
            continue
        try:
            head = read_bytes(path)[:64]
        except OSError:
            continue
        info = parse_hzc1(head)
        if info is None or bpp_of(info['fmt']) == 0:
            continue
        raw = hzc1_raw_len(info)
        size = os.path.getsize(path)
        if raw <= 2 * 1024 * 1024 and 44 < size < 4 * 1024 * 1024:
            if best is None or size < best[0]:
                best = (size, path)
    return None if best is None else best[1]


def run_hzc1(ca, args, jobs, scale, base_path):
    sec = Section('HZC1')
    cases = []
    if base_path is not None:
        base = read_bytes(base_path)
    else:
        # Synthetic fallback: a 64x64 Gray8 image compressed with zlib.
        w = h = 64
        raw = bytes((x * 3 + y) & 0xFF for y in range(h) for x in range(w))
        hdr = bytearray(44)
        hdr[0:4] = b'hzc1'
        struct.pack_into('<I', hdr, 4, len(raw))
        struct.pack_into('<I', hdr, 8, 0x20)
        hdr[12:16] = b'NVSG'
        struct.pack_into('<H', hdr, 0x10, 0x0100)
        struct.pack_into('<HH', hdr, 0x12, 3, w)
        struct.pack_into('<HH', hdr, 0x14, w, h)
        struct.pack_into('<I', hdr, 0x20, 0)
        base = bytes(hdr) + zlib.compress(raw, 9)
    info = parse_hzc1(base)
    if info is None:
        raise SystemExit('fuzz_assets: cannot build an hzc1 base')
    raw_len = hzc1_raw_len(info)
    per = max(1, int(round(12 * scale)))
    sizes = (0, 1, 4, 12, 43, 44, 45, len(base) - 1)
    dims_ok = info['w'] * info['h'] <= 512 * 512

    def add(cat, data, cmd, expect, extra=None):
        path = write_bytes('hz_%03d_%s.bin' % (len(cases), cat), data)
        cases.append((cat, path, cmd, expect, extra))

    # 1. header truncation
    for cut in sizes:
        add('trunc_header', truncate(base, cut), 'hdr', 'reject'
            if cut < 44 else None)
    # 2. magic corruption
    for off, magic in ((0, b'xzc1'), (12, b'NVSX')):
        add('magic_bad', patch(base, off, '4s', magic), 'hdr', 'reject')
    # 3. invalid pixel format
    for fmt in (5, 6, 0xFFFF, 0x8000):
        add('fmt_invalid', patch(base, 0x12, '<H', fmt), 'hdr', 'reject')
    # 4. zero dimensions
    for off in (0x14, 0x16):
        add('dim_zero', patch(base, off, '<H', 0), 'hdr', 'reject')
    # 5. width*height overflow: parse rejects anything > 2 GiB
    for (w, h, fmt) in ((65535, 65535, 1), (65535, 65535, 3),
                        (65535, 65535, 4), (32768, 32768, 1),
                        (65535, 16384, 0)):
        data = patch(patch(patch(base, 0x14, '<H', w), 0x16, '<H', h),
                     0x12, '<H', fmt)
        add('dim_overflow', data, 'hdr', 'reject')
    # 6. huge frame count (format 2): > 65536 is rejected outright
    for frames in (0xFFFFFFFF, 0x7FFFFFFF, 65537, 65536):
        data = patch(patch(patch(patch(base, 0x12, '<H', 2),
                                 0x14, '<H', 1), 0x16, '<H', 1),
                     0x20, '<I', frames)
        add('frames_huge', data, 'hdr', 'reject' if frames > 65536 else None)
    # 7. uncompressed_size disagreeing with the geometry
    for usize in (raw_len + 1, raw_len - 1 if raw_len else 0, 0, 0xFFFFFFFF):
        add('usize_mismatch', patch(base, 4, '<I', usize), 'hdr', 'reject')
    # 8. other header fields
    for off, fmt, val in ((0x10, '<H', 0xFFFF), (0x18, '<H', 0xFFFF),
                          (0x1A, '<H', 0xFFFF), (0x08, '<I', 0xFFFFFFFF),
                          (0x1C, '<I', 0xFFFFFFFF), (0x24, '<I', 0xFFFFFFFF)):
        add('header_fields', patch(base, off, fmt, val),
            'rgba' if dims_ok else 'hdr', None)
    # 9. payload truncation
    cuts = sorted({45, 46, 60, 100, len(base) // 2, len(base) - 2,
                   len(base) - 1})
    for cut in cuts:
        add('payload_trunc', truncate(base, cut), 'rgba', 'reject')
    # 10. payload bit flips
    rng = random.Random(SEED + 2)
    for i in range(per * 3):
        data = flip_bytes(base, rng, rng.randint(1, 4), lo=44)
        add('payload_flip', data, 'rgba', None)
    # 11. payload replaced by garbage
    for i in range(per):
        data = base[:44] + bytes(rng.getrandbits(8)
                                 for _ in range(len(base) - 44))
        add('payload_garbage', data, 'rgba', 'reject')
    # 12. random headers on a real payload
    for i in range(per):
        hdr = bytearray(rng.getrandbits(8) for _ in range(44))
        hdr[0:4] = b'hzc1'
        hdr[12:16] = b'NVSG'
        struct.pack_into('<H', hdr, 0x12, rng.randrange(5))
        struct.pack_into('<H', hdr, 0x14, rng.randrange(1, 512))
        struct.pack_into('<H', hdr, 0x16, rng.randrange(1, 512))
        add('header_random', bytes(hdr) + base[44:], 'hdr', None)
    # 13. inflate output-buffer boundary: too small / too big expectations
    for delta in (-1, -16, -raw_len if raw_len else -1, 1, 16, 1 << 20):
        expect = raw_len + delta
        add('inflate_expect', base, 'inflate', 'reject' if delta < 0 else None,
            expect)
    # 14. bare deflate stream (rawinflate path)
    bare_raw = bytes((x ^ y) & 0xFF for y in range(48) for x in range(48))
    co = zlib.compressobj(9, zlib.DEFLATED, -15)
    bare = co.compress(bare_raw) + co.flush()
    bare_file = write_bytes('bare.bin', bare)
    cases.append(('raw_valid', bare_file, 'rawinflate', 'accept',
                  len(bare_raw)))
    for cut in (0, 1, len(bare) // 3, len(bare) // 2, len(bare) - 1):
        cases.append(('raw_trunc', write_bytes('raw_%03d.bin' % len(cases),
                                               truncate(bare, cut)),
                      'rawinflate', 'reject', len(bare_raw)))
    for i in range(per * 2):
        data = flip_bytes(bare, rng, rng.randint(1, 3))
        cases.append(('raw_flip', write_bytes('rawf_%03d.bin' % len(cases),
                                              data),
                      'rawinflate', None, len(bare_raw)))
    if dims_ok and base_path:
        cases.append(('rgba_valid', base_path, 'rgba', 'accept', None))

    def one(case):
        cat, path, cmd, expect, extra = case
        if cmd == 'hdr':
            argv = [ca, 'hdr', path]
            tname = 'c_assets'
        elif cmd == 'rgba':
            argv = [ca, 'rgba', path, os.path.join(TMP, 'hz.rgba')]
            tname = 'c_assets'
        elif cmd == 'inflate':
            argv = [ca, 'inflate', path, os.path.join(TMP, 'hz.inflate.out'),
                    str(extra), '44']
            tname = 'c_assets'
        else:  # rawinflate
            argv = [ca, 'rawinflate', path, os.path.join(TMP, 'hz.raw.out'),
                    str(extra), '0']
            tname = 'c_assets'
        status, rc, out, err, elapsed = run_tool(argv)
        st = classify(tname, rc, out, err, expect)
        detail = ''
        if st not in ('accepted', 'rejected'):
            detail = (err or out or b'')[:300].decode('utf-8', 'replace')
        elif st == 'rejected' and cat in ('payload_trunc',):
            detail = (err or out or b'')[:120].decode('utf-8', 'replace')
        return Result(path, cat, st, rc, elapsed, detail)

    with concurrent.futures.ThreadPoolExecutor(max_workers=jobs) as pool:
        for res, case in zip(pool.map(one, cases), cases):
            sec.add(res, case[3])
    return sec


# ---------------------------------------------------------------------------
# AUDIO section (WAV + Ogg)
# ---------------------------------------------------------------------------

def run_audio(snd, args, jobs, scale, ogg_path, wav_path):
    sec = Section('AUDIO')
    cases = []
    per = max(1, int(round(10 * scale)))
    wav = wav_valid()
    wav_len = len(wav)

    def add(cat, data, expect, suffix='.wav'):
        path = write_bytes('au_%03d_%s%s' % (len(cases), cat, suffix), data)
        cases.append((cat, path, expect))

    add('wav_valid', wav, 'accept')
    for cut in (0, 4, 8, 12, 20, 43, 44, wav_len - 1):
        add('wav_trunc', truncate(wav, cut), 'reject')
    # fmt chunk id (offset 20) and payload fields (44..60)
    for val in (2, 3, 6, 0xFFFE):
        add('wav_bad_fmt', patch(wav, 20, '<H', val), 'reject')
    for bits in (8, 24, 32, 0, 0xFFFF):
        add('wav_bad_bits', patch(wav, 34, '<H', bits), 'reject')
    for ch in (0, 2, 3, 8, 0xFFFF):
        add('wav_channels', patch(wav, 22, '<H', ch), None)
    for rate in (0, 1, 0x7FFFFFFF, 0x80000000):
        add('wav_rate', patch(wav, 24, '<I', rate), None)
    # fmt chunk declares 15 / 0 bytes but 16 are present
    for sz in (0, 15):
        add('wav_fmt_short', patch(wav, 16, '<I', sz), None)
    # data chunk size lies
    data_off = wav.find(b'data')
    for sz in (0, 0xFFFFFFFF, wav_len - data_off - 8 - 1):
        add('wav_data_size', patch(wav, data_off + 4, '<I', sz), None)
    # no data chunk at all
    add('wav_data_missing', wav[:data_off], 'reject')
    # an odd-sized unknown chunk before fmt (exercises the RIFF chunk walker)
    odd = b'odd ' + struct.pack('<I', 3) + b'xyz' + b'\x00'
    add('wav_odd_chunk', wav[:12] + odd + wav[12:], None)
    # truthfully invalid chunk sizes: walk off the end
    add('wav_chunk_walk', patch(wav, 16, '<I', 0x00FFFFFF), None)
    # real WAV file: truncation + flips
    if wav_path is not None and os.path.isfile(wav_path):
        real = read_bytes(wav_path)
        for cut in (0, 12, 44, 100, 4096, len(real) // 2, len(real) - 1):
            add('wav_real', truncate(real, cut), 'reject')
        rng = random.Random(SEED + 3)
        for i in range(per):
            add('wav_real', flip_bytes(real, rng, rng.randint(1, 4)), None)
    # Ogg
    if ogg_path is not None and os.path.isfile(ogg_path):
        ogg = read_bytes(ogg_path)
        olen = len(ogg)
        add('ogg_valid', ogg, 'accept', '.ogg')
        for cut in (0, 4, 27, 28, 1000, 4096, 65536, 262144, olen - 1):
            add('ogg_trunc', truncate(ogg, cut), 'reject', '.ogg')
        for off, magic in ((0, b'OggX'), (1, b'ggS\x00')):
            add('ogg_magic', patch(ogg, off, '4s', magic), 'reject', '.ogg')
        rng = random.Random(SEED + 4)
        for i in range(per * 4):
            add('ogg_prefix_flip',
                flip_bytes(ogg, rng, rng.randint(1, 8), hi=min(olen, 65536)),
                None, '.ogg')
        for i in range(per):
            add('ogg_append', ogg + bytes(rng.getrandbits(8)
                                          for _ in range(rng.randrange(1, 64))),
                None, '.ogg')
        for i in range(per):
            add('ogg_random_prefix',
                b'OggS' + bytes(rng.getrandbits(8)
                                for _ in range(rng.randrange(4, 4096))),
                'reject', '.ogg')
        full_per = max(2, int(round(2 * scale)))
        for i in range(full_per):
            add('ogg_full_flip', flip_bytes(ogg, rng, rng.randint(1, 3)),
                None, '.ogg')
    else:
        log('  note: no Ogg corpus found, skipping Ogg cases')

    def one(case):
        cat, path, expect = case
        status, rc, out, err, elapsed = run_tool([snd, path], timeout=60.0)
        st = classify('snd', rc, out, err, expect)
        detail = ''
        if st not in ('accepted', 'rejected'):
            detail = (err or out or b'')[:300].decode('utf-8', 'replace')
        return Result(path, cat, st, rc, elapsed, detail)

    with concurrent.futures.ThreadPoolExecutor(max_workers=jobs) as pool:
        for res, case in zip(pool.map(one, cases), cases):
            sec.add(res, case[2])
    return sec


# ---------------------------------------------------------------------------
# in-process inflate fuzz (existing c_assets command, not a mutation harness)
# ---------------------------------------------------------------------------

def run_inflate_fuzz(ca, iterations, seed, seedlist):
    argv = [ca, 'fuzz', str(iterations), str(seed)]
    if seedlist:
        argv.append(seedlist)
    status, rc, out, err, elapsed = run_tool(argv, timeout=600.0)
    text = (out + err).decode('utf-8', 'replace')
    entry = {'iters': iterations, 'seed': seed, 'status': status,
             'exit': rc, 'ms': elapsed * 1000.0,
             'guard_violations': None, 'raw': text[-400:]}
    ok = status == 'ran' and rc == 0
    if ok:
        for line in text.splitlines():
            if line.startswith('fuzz '):
                for token in line.split():
                    if token.startswith('guard_violations='):
                        try:
                            entry['guard_violations'] = int(token.split('=')[1])
                        except ValueError:
                            pass
    ok = ok and entry['guard_violations'] == 0
    entry['ok'] = ok
    return entry


# ---------------------------------------------------------------------------
# main
# ---------------------------------------------------------------------------

def print_section(sec, s):
    log('')
    log('== %s ==' % sec.name)
    log('  %-18s %6s %8s %8s  %s' %
        ('category', 'cases', 'accepted', 'rejected', 'reject-rate'))
    for c in s['categories']:
        rate = (100.0 * c['rejected'] / c['total']) if c['total'] else 0.0
        log('  %-18s %6d %8d %8d  %6.1f%%   max %.0f ms' %
            (c['name'], c['total'], c['accepted'], c['rejected'], rate,
             c['max_ms']))
    log('  %-18s %6d %8d %8d' % ('TOTAL', s['total'], s['accepted'],
                                 s['rejected']))
    flags = []
    for key in ('crash', 'timeout', 'memfault', 'undiagnosed', 'other'):
        if s.get(key):
            flags.append('%s=%d' % (key, s[key]))
    log('  safety: %s' % ('OK (0 crash, 0 hang, 0 memory fault)'
                          if not flags else 'FAIL ' + ', '.join(flags)))


def main(argv=None):
    ap = argparse.ArgumentParser()
    ap.add_argument('--scale', type=float, default=1.0,
                    help='scale the per-category case counts (default 1.0)')
    ap.add_argument('--fast', action='store_true',
                    help='shorthand for --scale 0.4')
    ap.add_argument('--jobs', type=int, default=min(8, os.cpu_count() or 4))
    ap.add_argument('--game', default=DEFAULT_GAME,
                    help='game data directory (auto-discovered if omitted)')
    ap.add_argument('--extract', default=DEFAULT_EXTRACT)
    ap.add_argument('--ogg', default=DEFAULT_OGG)
    ap.add_argument('--wav', default=DEFAULT_WAV)
    ap.add_argument('--cc', default=None, help='gcc used to build harnesses')
    ap.add_argument('--no-build', action='store_true',
                    help='use only the prebuilt/plain c_assets.exe')
    ap.add_argument('--keep', action='store_true',
                    help='keep mutant files after the run')
    ap.add_argument('--json', default=os.path.join(OUT, 'fuzz_assets.json'))
    ap.add_argument('--inflate-iters', type=int, default=40000)
    args = ap.parse_args(argv)

    scale = 0.4 if args.fast else args.scale
    args.game = args.game or find_game_dir()
    ensure_dirs()
    t_start = time.perf_counter()
    log('fuzz_assets: FVP=%s' % FVP)

    cc = None if args.no_build else find_cc(args.cc)
    tools = {'c_assets': PUBLIC_CASSETS, 'snd': None}
    built = {'c_assets': None, 'snd': None}
    build_ok = True
    if not args.no_build and cc is None:
        log('  note: gcc not found; falling back to bin/c_assets.exe '
            '(no memory instrumentation)')
    if not args.no_build and cc is not None:
        built = build_harnesses(cc, force=True)
        if built['c_assets']:
            tools['c_assets'] = built['c_assets']
        else:
            build_ok = False
        if built['snd']:
            tools['snd'] = built['snd']
        else:
            build_ok = False
    if tools['c_assets'] is None or not os.path.isfile(tools['c_assets']):
        log('fuzz_assets: missing c_assets executable')
        return 2
    if tools['snd'] is None:
        if build_ok:
            log('  note: audio harness unavailable; the AUDIO section is '
                'skipped')
        else:
            log('  ERROR: the instrumented harnesses could not be built; '
                'AUDIO coverage and memory instrumentation are missing')
    else:
        log('  audio harness: %s' % tools['snd'])

    real_bin = None
    if args.game:
        log('  game dir: %s' % args.game)
        for cand in ('se_sys.bin', 'se_env.bin'):
            p = os.path.join(args.game, cand)
            if os.path.isfile(p):
                real_bin = p
                break
    if real_bin:
        log('  real BIN package: %s' % real_bin)
    else:
        log('  note: no real BIN package found (synthetic only)')

    hzc1_base = pick_hzc1_base(args.extract)
    if hzc1_base:
        log('  hzc1 base: %s' % hzc1_base)
    else:
        log('  note: no real hzc1 file found, using a synthetic base')
    for label, path in (('ogg', args.ogg), ('wav', args.wav)):
        log('  %s corpus: %s (%s)' % (label, path,
                                      'ok' if os.path.isfile(path) else 'missing'))

    sections = {}
    sections['BIN'] = run_bin(tools['c_assets'], args, args.jobs, scale,
                              real_bin)
    sections['HZC1'] = run_hzc1(tools['c_assets'], args, args.jobs, scale,
                                hzc1_base)
    if tools['snd'] is not None:
        sections['AUDIO'] = run_audio(tools['snd'], args, args.jobs, scale,
                                      args.ogg, args.wav)

    # Existing in-process inflate fuzz: 40k random/mutated streams with its own
    # guard pages in front of and behind the output buffer.
    seedlist = None
    if hzc1_base:
        seedlist = os.path.join(TMP, 'hzc1_seeds.txt')
        seeds = []
        try:
            for name in sorted(os.listdir(args.extract))[:40]:
                p = os.path.join(args.extract, name)
                if os.path.isfile(p) and parse_hzc1(read_bytes(p)[:64]):
                    seeds.append(p)
        except OSError:
            pass
        if seeds:
            with open(seedlist, 'w', encoding='utf-8') as f:
                f.write('\n'.join(seeds) + '\n')
        else:
            seedlist = None
    inflate_fuzz = run_inflate_fuzz(tools['c_assets'], args.inflate_iters,
                                    SEED, seedlist)

    # ---- report ----
    log('')
    totals = {'total': 0, 'accepted': 0, 'rejected': 0, 'crash': 0,
              'timeout': 0, 'memfault': 0, 'undiagnosed': 0, 'other': 0,
              'expected_mismatch': 0}
    report = {'when': time.strftime('%Y-%m-%d %H:%M:%S'),
              'toolchain': 'gcc ASan unavailable -> fuzz_alloc.h page-guard',
              'tools': {k: v for k, v in tools.items()},
              'corpora': {'real_bin': real_bin, 'hzc1_base': hzc1_base,
                          'ogg': args.ogg, 'wav': args.wav},
              'scale': scale, 'sections': {}, 'inflate_fuzz': inflate_fuzz,
              'harnesses_built': build_ok,
              'failures': []}
    for name, sec in sections.items():
        s = sec.summary()
        report['sections'][name] = s
        print_section(sec, s)
        for key in totals:
            totals[key] += s.get(key, 0)
        for res in sec.failures:
            report['failures'].append({
                'section': name, 'category': res.category,
                'file': os.path.basename(res.case), 'status': res.status,
                'rc': res.rc, 'detail': res.detail})

    log('')
    log('== inflate random fuzz (c_assets fuzz) ==')
    log('  iterations=%d seed=%d guard_violations=%s exit=%s %.0f ms -> %s' %
        (inflate_fuzz['iters'], inflate_fuzz['seed'],
         inflate_fuzz['guard_violations'], inflate_fuzz['exit'],
         inflate_fuzz['ms'], 'PASS' if inflate_fuzz['ok'] else 'FAIL'))

    log('')
    log('== TOTAL ==')
    log('  cases=%d accepted=%d rejected=%d' %
        (totals['total'], totals['accepted'], totals['rejected']))
    log('  crash=%d timeout=%d memfault=%d undiagnosed=%d other=%d' %
        (totals['crash'], totals['timeout'], totals['memfault'],
         totals['undiagnosed'], totals['other']))
    elapsed = time.perf_counter() - t_start
    log('  elapsed=%.1f s' % elapsed)

    hard_fail = (totals['crash'] or totals['timeout'] or
                 totals['memfault'] or totals['undiagnosed'] or
                 totals['other'] or not inflate_fuzz['ok'] or not build_ok)
    verdict = 'FAIL' if hard_fail else 'PASS'
    log('  RESULT: %s' % verdict)
    report['totals'] = totals
    report['elapsed_s'] = elapsed
    report['verdict'] = verdict
    try:
        os.makedirs(os.path.dirname(os.path.abspath(args.json)), exist_ok=True)
        with open(args.json, 'w', encoding='utf-8') as f:
            json.dump(report, f, indent=1, ensure_ascii=False)
        log('  json -> %s' % args.json)
    except OSError as exc:
        log('  warning: cannot write %s (%s)' % (args.json, exc))

    if not args.keep:
        try:
            shutil.rmtree(TMP)
        except OSError:
            pass
    return 0 if verdict == 'PASS' else 1


if __name__ == '__main__':
    sys.exit(main())
