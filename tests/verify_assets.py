#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""verify_assets.py -- one-shot acceptance for the C asset layer.

Checks, against the Python reference:

  1. BIN      graph.bin (and every other *.bin in the game directory) index,
              names (CP932 -> UTF-8) and sizes/offsets match the Python
              parser entry by entry; every name resolves through bin_find();
              a sample of entries is dumped and compared byte for byte.
  2. INFLATE  200 random hzc1 payloads + synthetic zlib/raw streams decompress
              byte-identically to Python's zlib.decompress(); a 100k-iteration
              random-input fuzz run (plus truncated and bit-flipped real
              streams) must never crash or write out of bounds.
  3. HZC1     100 stratified random images (cover fmt 0/1/2/3/4) decode to
              RGBA8888 byte-identically to fvp/work/hzc1.py, 0 pixel diff;
              PPM export and the per-frame API are checked too.

Plus timing numbers for a 1920x1080 image (decode and raw inflate).

Options: --quick (fewer samples), --full (use all 1112 files), --seed N,
--game DIR, --extract DIR, --exe PATH.

Run through verify_assets.ps1, or directly:

    python tests/verify_assets.py [--exe bin/c_assets.exe]
                                  [--game GAME_DIR] [--extract DIR]
                                  [--seed N] [--quick]

Exit code 0 = all checks passed.
"""
import argparse
import glob
import importlib.util
import os
import random
import re
import shutil
import struct
import subprocess
import sys
import time
import zlib

import numpy as np

HERE = os.path.dirname(os.path.abspath(__file__))
FVP = os.path.dirname(HERE)
DEFAULT_EXE = os.path.join(FVP, 'bin', 'c_assets.exe')
DEFAULT_GAME = r'C:\Users\Mechrevo\Downloads\五彩斑斓的世界HD'
DEFAULT_EXTRACT = os.path.join(FVP, 'extract', 'graph')
HZC1_REF = os.path.join(FVP, 'work', 'hzc1.py')
TMP = os.path.join(FVP, 'out', 'verify')

FORMAT_NAMES = {0: 'BGR24', 1: 'BGRA32', 2: 'BGRA32-multiframe',
                3: 'Gray8', 4: '1bit-mask'}

failures = []


# --------------------------------------------------------------------------
# helpers
# --------------------------------------------------------------------------

def section(title):
    print('')
    print('== %s ==' % title)


def check(cond, msg):
    print(('  PASS ' if cond else '  FAIL ') + msg)
    if not cond:
        failures.append(msg)
    return cond


def run(exe_args, timeout=600):
    return subprocess.run(exe_args, stdout=subprocess.PIPE,
                          stderr=subprocess.PIPE, timeout=timeout)


def out_text(proc):
    return proc.stdout.decode('utf-8', 'replace')


def err_text(proc):
    return proc.stderr.decode('utf-8', 'replace').strip()


def load_hzc1_ref():
    spec = importlib.util.spec_from_file_location('hzc1_ref', HZC1_REF)
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


def fmt(n):
    return '{:,}'.format(n)


# --------------------------------------------------------------------------
# 1. BIN
# --------------------------------------------------------------------------

def parse_bin_py(path):
    """Independent Python parser for the BIN package format."""
    with open(path, 'rb') as f:
        count, nametab_len = struct.unpack('<II', f.read(8))
        ents = [struct.unpack('<III', f.read(12)) for _ in range(count)]
        names_start = 8 + 12 * count
        f.seek(names_start)
        nametab = f.read(nametab_len)
    files = []
    for i, (name_off, data_off, size) in enumerate(ents):
        end = nametab.find(b'\0', name_off)
        if end < 0:
            end = len(nametab)
        raw = nametab[name_off:end]
        files.append(dict(index=i, raw=raw, name=raw.decode('cp932', 'replace'),
                          size=size, offset=data_off))
    return dict(count=count, nametab_len=nametab_len, names_start=names_start,
                data_start=names_start + nametab_len, files=files)


def list_package(exe, path):
    proc = run([exe, 'list', path])
    text = out_text(proc)
    if proc.returncode != 0:
        return None, err_text(proc) or ('exit %d' % proc.returncode)
    lines = text.splitlines()
    header = {}
    for line in lines:
        parts = line.split(' ', 1)
        if len(parts) == 2 and parts[0] in ('count', 'nametab', 'names_start',
                                            'data_start'):
            header[parts[0]] = int(parts[1])
    if len(header) != 4:
        return None, 'unexpected list header: %r' % lines[:5]
    entries = []
    for line in lines[4:]:
        if not line:
            continue
        parts = line.split('\t', 3)
        if len(parts) != 4:
            return None, 'unexpected list entry: %r' % line
        entries.append((int(parts[0]), parts[3], int(parts[1]),
                        int(parts[2])))
    return (header, entries), None


def compare_package(exe, path, pkg_name, dump_sample=0, verbose=True):
    """Compare one package's index/names with the Python parser."""
    py = parse_bin_py(path)
    got, err = list_package(exe, path)
    if got is None:
        return dict(pkg=pkg_name, entries=py['count'], bad=1, dumps=0,
                    error=err)
    header, entries = got
    bad = 0
    problems = []
    expect_header = dict(count=py['count'], nametab=py['nametab_len'],
                         names_start=py['names_start'],
                         data_start=py['data_start'])
    for key, want in expect_header.items():
        if header.get(key) != want:
            bad += 1
            problems.append('%s: %s != %s' % (key, header.get(key), want))
    if len(entries) != py['count']:
        bad += 1
        problems.append('entry count %d != %d' % (len(entries), py['count']))
    n = min(len(entries), py['count'])
    for k in range(n):
        idx, name, size, off = entries[k]
        ref = py['files'][k]
        if idx != k or name != ref['name'] or size != ref['size'] or \
                off != ref['offset']:
            bad += 1
            if len(problems) < 8:
                problems.append('entry %d: got (%d,%r,%d,%d) want (%d,%r,%d,%d)'
                                % (k, idx, name, size, off, k, ref['name'],
                                   ref['size'], ref['offset']))
    dumps = 0
    if dump_sample:
        rng = random.Random(1234 + py['count'])
        picks = sorted(set([0, 1, py['count'] - 1] +
                           rng.sample(range(py['count']),
                                      min(dump_sample, py['count']))))
        with open(path, 'rb') as f:
            for k in picks:
                ref = py['files'][k]
                f.seek(ref['offset'])
                want = f.read(ref['size'])
                out = os.path.join(TMP, 'dump.bin')
                proc = run([exe, 'dump', path, str(k), out])
                if proc.returncode != 0:
                    bad += 1
                    problems.append('dump %d failed: %s' % (k, err_text(proc)))
                    continue
                got_bytes = open(out, 'rb').read()
                dumps += 1
                if got_bytes != want:
                    bad += 1
                    problems.append('dump %d bytes differ (%d vs %d)'
                                    % (k, len(got_bytes), len(want)))
    if verbose and bad:
        for p in problems:
            print('    ! ' + p)
    return dict(pkg=pkg_name, entries=py['count'], bad=bad, dumps=dumps,
                error=None)


def make_synthetic_bin(path):
    """Tiny package whose names exercise CP932 edge cases."""
    names = [b'0001_plain', b'x\x8b\xd6\x8e~', b'\x81\x20', b'\x80', b'\xa0',
             b'\xfd\xfe\xff', b'\x82\xa0', b'\x81', b'x\x81\x7f', b'tab\tname']
    data = [b'entry-0', b'', b'\x00\x00\x01', b'x' * 300,
            bytes(range(256)), b'last']
    data += [b'pad-%d' % i for i in range(len(names) - len(data))]
    nametab = bytearray()
    name_offs = []
    for n in names:
        name_offs.append(len(nametab))
        nametab += n + b'\0'
    count = len(names)
    names_start = 8 + 12 * count
    body_off = names_start + len(nametab)
    entries = []
    cur = body_off
    for i, d in enumerate(data):
        entries.append((name_offs[i], cur, len(d)))
        cur += len(d)
    with open(path, 'wb') as f:
        f.write(struct.pack('<II', count, len(nametab)))
        for e in entries:
            f.write(struct.pack('<III', *e))
        f.write(bytes(nametab))
        for d in data:
            f.write(d)
    return dict(count=count, nametab_len=len(nametab), names_start=names_start,
                data_start=body_off,
                files=[dict(index=i, raw=names[i],
                            name=names[i].decode('cp932', 'replace'),
                            size=len(data[i]), offset=entries[i][1])
                       for i in range(count)])


def test_bin(exe, game_dir, quick):
    section('1. BIN package index / names (CP932 -> UTF-8)')

    graph = os.path.join(game_dir, 'graph.bin')
    if not os.path.isfile(graph):
        check(False, 'graph.bin not found: %s' % graph)
        return
    py = parse_bin_py(graph)
    res = compare_package(exe, graph, 'graph.bin', dump_sample=2 if quick else 12)
    check(res['bad'] == 0,
          'graph.bin: %s entries, names/sizes/offsets match Python (%d bad), '
          '%d entries dumped byte-exact'
          % (fmt(res['entries']), res['bad'], res['dumps']))

    # every name must resolve through bin_find()
    names = [f['name'] for f in py['files']]
    missing = 'no_such_entry_zzz'
    names_file = os.path.join(TMP, 'names.txt')
    with open(names_file, 'w', encoding='utf-8', newline='\n') as f:
        for n in names:
            f.write(n + '\n')
        f.write(missing + '\n')
    proc = run([exe, 'findall', graph, names_file])
    ok_find = proc.returncode == 0
    lines = out_text(proc).splitlines()
    if ok_find and len(lines) == len(names) + 1:
        for k, line in enumerate(lines):
            parts = line.split('\t', 1)
            want = k if k < len(names) else -1
            if int(parts[0]) != want:
                ok_find = False
                break
    else:
        ok_find = False
    check(ok_find, 'bin_find: all %s names resolve to their index, unknown '
                   'name -> -1' % fmt(len(names)))

    # the one non-ASCII name in graph.bin must decode to the reference text
    nonascii = [f for f in py['files'] if any(b > 0x7f for b in f['raw'])]
    check([f['name'] for f in nonascii] == ['x禁止'],
          'non-ASCII name decodes to %r (CP932 x禁止)'
          % [f['name'] for f in nonascii])

    # names must line up with the bin_archiver extraction (if available)
    extract = os.path.join(FVP, 'extract', 'graph')
    if os.path.isdir(extract):
        extracted = set(os.listdir(extract))
        mismatched = 0
        for f in py['files']:
            fn = '%04d_%s' % (f['index'], f['name'])
            if fn not in extracted:
                mismatched += 1
        check(mismatched == 0,
              'names match the %s bin_archiver_extracted files'
              % fmt(len(extracted)))

    # every other package in the game directory: full index comparison
    packages = sorted(glob.glob(os.path.join(game_dir, '*.bin')))
    packages = [p for p in packages if os.path.abspath(p) != os.path.abspath(graph)]
    total_entries = 0
    total_bad = 0
    total_dumps = 0
    failed = []
    for path in packages:
        r = compare_package(exe, path, os.path.basename(path), dump_sample=0,
                            verbose=True)
        total_entries += r['entries']
        total_bad += r['bad']
        total_dumps += r['dumps']
        if r['error']:
            failed.append('%s (%s)' % (r['pkg'], r['error']))
    if packages:
        check(total_bad == 0 and not failed,
              'other packages: %d packages / %s entries match Python%s'
              % (len(packages), fmt(total_entries),
                 ('; failures: ' + ', '.join(failed)) if failed else ''))

    # synthetic package: names with invalid CP932 sequences, empty payloads
    synth = os.path.join(TMP, 'synth.bin')
    syn = make_synthetic_bin(synth)
    got, err = list_package(exe, synth)
    bad = 0
    if got is None:
        bad = 1
        print('    ! ' + str(err))
    else:
        header, entries = got
        if header['count'] != syn['count'] or header['nametab'] != syn['nametab_len']:
            bad += 1
        if len(entries) != syn['count']:
            bad += 1
        for k, (idx, name, size, off) in enumerate(entries):
            ref = syn['files'][k]
            if idx != k or name != ref['name'] or size != ref['size'] or \
                    off != ref['offset']:
                bad += 1
                if bad < 6:
                    print('    ! synth entry %d: %r vs %r' % (k, name, ref['name']))
    check(bad == 0, 'synthetic package: %d CP932 edge-case names + empty/odd '
                    'payloads match Python' % syn['count'])


# --------------------------------------------------------------------------
# 2. inflate
# --------------------------------------------------------------------------

def test_inflate(exe, extract, seed, quick, full=False):
    section('2. inflate (zlib + raw DEFLATE)')

    if not os.path.isdir(extract):
        check(False, 'extracted image directory not found: %s' % extract)
        return
    all_files = sorted(os.listdir(extract))
    all_files = [os.path.join(extract, n) for n in all_files
                 if os.path.isfile(os.path.join(extract, n))]
    if not all_files:
        check(False, 'no files in %s' % extract)
        return
    rng = random.Random(seed)
    n_sample = len(all_files) if full else (40 if quick else 200)
    sample = rng.sample(all_files, min(n_sample, len(all_files)))
    out = os.path.join(TMP, 'inflate.out')
    bad = []
    total_bytes = 0
    for path in sample:
        data = open(path, 'rb').read()
        payload = data[44:]
        expect = zlib.decompress(payload)
        usize = struct.unpack_from('<I', data, 4)[0]
        if usize != len(expect):
            bad.append('%s: header usize mismatch' % os.path.basename(path))
            continue
        proc = run([exe, 'inflate', path, out, str(len(expect)), '44'])
        if proc.returncode != 0:
            bad.append('%s: %s' % (os.path.basename(path), err_text(proc)))
            continue
        got = open(out, 'rb').read()
        if got != expect:
            bad.append('%s: %d bytes differ from Python' % (os.path.basename(path),
                                                            len(got)))
        total_bytes += len(expect)
    check(not bad,
          '%d %s hzc1 payloads: %s bytes decompressed byte-identically to '
          'Python zlib.decompress%s'
          % (len(sample), 'all' if full else 'random', fmt(total_bytes),
             ('; first errors: ' + '; '.join(bad[:4])) if bad else ''))

    # synthetic streams: every level, zlib and bare deflate, empty/tiny/large
    cases = []
    patterns = [b'', b'a', b'abc', bytes(range(256)), bytes(range(256)) * 4096,
                (b'The quick brown fox jumps over the lazy dog. ' * 5000),
                bytes(rng.getrandbits(8) for _ in range(70000))]
    for level in (0, 1, 2, 5, 6, 9):
        for i, pat in enumerate(patterns):
            zo = zlib.compressobj(level)
            cases.append(('zlib level %d pat %d' % (level, i),
                          zo.compress(pat) + zo.flush(), pat, False))
            ro = zlib.compressobj(level, zlib.DEFLATED, -15)
            cases.append(('raw level %d pat %d' % (level, i),
                          ro.compress(pat) + ro.flush(), pat, True))
    bad = []
    for desc, stream, expect, raw in cases:
        inp = os.path.join(TMP, 'inflate.in')
        with open(inp, 'wb') as f:
            f.write(stream)
        cmd = 'rawinflate' if raw else 'inflate'
        proc = run([exe, cmd, inp, out, str(len(expect)), '0'])
        if proc.returncode != 0:
            bad.append('%s: %s' % (desc, err_text(proc)))
            continue
        if open(out, 'rb').read() != expect:
            bad.append('%s: output differs' % desc)
    check(not bad, '%d synthetic streams (%d zlib + %d raw, levels 0-9, '
                   '0 B .. 70000 B) round-trip exactly%s'
                   % (len(cases), len(cases) // 2, len(cases) // 2,
                      ('; ' + '; '.join(bad[:4])) if bad else ''))

    # fuzz: random inputs / mutated / truncated real streams
    fuzz_iters = 20000 if quick else 100000
    seedlist = os.path.join(TMP, 'fuzz_seeds.txt')
    seed_files = rng.sample(all_files, min(8, len(all_files)))
    with open(seedlist, 'w', encoding='utf-8', newline='\n') as f:
        for p in seed_files:
            f.write(p + '\n')
    proc = run([exe, 'fuzz', str(fuzz_iters), str(seed), seedlist])
    line = out_text(proc).strip()
    m = re.search(r'iterations=(\d+) ok=(\d+) err=(\d+) guard_violations=(\d+)',
                  line)
    ok_fuzz = proc.returncode == 0 and m is not None
    if m:
        ok_fuzz = ok_fuzz and int(m.group(4)) == 0 and \
            int(m.group(1)) == fuzz_iters and \
            int(m.group(2)) + int(m.group(3)) == fuzz_iters
    check(ok_fuzz, 'fuzz: %s random/mutated/truncated streams, no crash, '
                   'no guard-byte violation (%s)' % (fmt(fuzz_iters), line))
    # second run, different seed, no corpus
    proc = run([exe, 'fuzz', str(max(1, fuzz_iters // 4)), '777'])
    line2 = out_text(proc).strip()
    m2 = re.search(r'guard_violations=(\d+)', line2)
    check(proc.returncode == 0 and m2 is not None and int(m2.group(1)) == 0,
          'fuzz (second seed): %s' % line2)

    # corrupted real streams must return an error, not a crash
    corrupt_bad = []
    n_corrupt = 10 if quick else 25
    for _ in range(n_corrupt):
        path = rng.choice(all_files)
        payload = bytearray(open(path, 'rb').read()[44:])
        if len(payload) < 12:
            continue
        mode = rng.randrange(2)
        if mode == 0:
            cut = rng.randrange(1, len(payload))
            payload = payload[:cut]
        else:
            for _ in range(rng.randrange(1, 4)):
                pos = rng.randrange(len(payload))
                payload[pos] ^= 1 << rng.randrange(8)
        inp = os.path.join(TMP, 'corrupt.bin')
        with open(inp, 'wb') as f:
            f.write(payload)
        usize = struct.unpack_from('<I', open(path, 'rb').read(8), 4)[0]
        proc = run([exe, 'inflate', inp, out, str(usize), '0'])
        if proc.returncode not in (0, 1):
            corrupt_bad.append('crash rc=%d' % proc.returncode)
    check(not corrupt_bad, '%d truncated/bit-flipped real streams: error or '
                           'clean success, never a crash%s'
                           % (n_corrupt,
                              ('; ' + '; '.join(corrupt_bad[:4]))
                              if corrupt_bad else ''))


# --------------------------------------------------------------------------
# 3. HZC1
# --------------------------------------------------------------------------

def scan_hzc1_headers(extract):
    infos = []
    for fn in sorted(os.listdir(extract)):
        path = os.path.join(extract, fn)
        if not os.path.isfile(path):
            continue
        try:
            with open(path, 'rb') as f:
                head = f.read(44)
        except OSError:
            continue
        if len(head) < 44 or head[:4] != b'hzc1' or head[12:16] != b'NVSG':
            continue
        usize, hdrsize = struct.unpack_from('<II', head, 4)
        unknown, fmt, w, h, x, y = struct.unpack_from('<6H', head, 0x10)
        frame_count = struct.unpack_from('<I', head, 0x20)[0]
        frames = (frame_count if frame_count else 1) if fmt == 2 else 1
        bpp = {0: 3, 1: 4, 2: 4, 3: 1, 4: 1}.get(fmt, 0)
        infos.append(dict(path=path, name=fn, fmt=fmt, w=w, h=h, x=x, y=y,
                          frames=frames, frame_count=frame_count, bpp=bpp,
                          usize=usize))
    return infos


def expected_rgba(hz):
    """Reference RGBA8888 (frames, h, w, 4) built from hzc1.py's array()."""
    arr = hz.array()
    if hz.fmt == 0:
        a = arr.reshape(hz.frames, hz.height, hz.width, 3)
        out = np.empty((hz.frames, hz.height, hz.width, 4), np.uint8)
        out[..., :3] = a
        out[..., 3] = 255
        return out
    if hz.fmt in (1, 2):
        return arr.reshape(hz.frames, hz.height, hz.width, 4)
    a = arr.reshape(hz.frames, hz.height, hz.width)
    out = np.empty((hz.frames, hz.height, hz.width, 4), np.uint8)
    out[..., 0] = a
    out[..., 1] = a
    out[..., 2] = a
    out[..., 3] = 255
    return out


def parse_hdr(text):
    m = re.match(r'fmt=(\d+) format=(\S+) w=(\d+) h=(\d+) frames=(\d+) '
                 r'bpp=(\d+) usize=(\d+) x=(\d+) y=(\d+) rawlen=(\d+)\s*$',
                 text)
    if not m:
        return None
    keys = ['fmt', 'format', 'w', 'h', 'frames', 'bpp', 'usize', 'x', 'y',
            'rawlen']
    d = {}
    for k, v in zip(keys, m.groups()):
        d[k] = v if k == 'format' else int(v)
    return d


def choose_hzc1_samples(infos, seed, total, per_fmt):
    rng = random.Random(seed)
    by_fmt = {}
    for info in infos:
        by_fmt.setdefault(info['fmt'], []).append(info)
    chosen = []
    seen = set()
    for fmt_id in sorted(by_fmt):
        pool = by_fmt[fmt_id][:]
        rng.shuffle(pool)
        for info in pool[:per_fmt]:
            chosen.append(info)
            seen.add(info['path'])
    rest = [i for i in infos if i['path'] not in seen]
    rng.shuffle(rest)
    while len(chosen) < total and rest:
        chosen.append(rest.pop())
    # make sure at least one multi-frame file is present
    if not any(i['frames'] > 1 for i in chosen):
        multi = [i for i in infos if i['frames'] > 1]
        if multi:
            chosen[-1] = multi[0]
    return chosen


def test_hzc1(exe, extract, seed, quick, hzref, full=False):
    section('3. HZC1 image decode -> RGBA8888')

    if not os.path.isdir(extract):
        check(False, 'extracted image directory not found: %s' % extract)
        return
    infos = scan_hzc1_headers(extract)
    check(len(infos) > 0, 'scanned %s hzc1 files in %s'
          % (fmt(len(infos)), extract))
    if not infos:
        return
    if full:
        sample = infos
    else:
        n_total = 20 if quick else 100
        per_fmt = 2 if quick else 5
        sample = choose_hzc1_samples(infos, seed, n_total, per_fmt)
    rgba_out = os.path.join(TMP, 'frame.rgba')

    bad_hdr = []
    bad_pix = []
    total_pixels = 0
    total_frames = 0
    diff_pixels = 0
    fmt_count = {}
    for info in sample:
        path = info['path']
        proc = run([exe, 'hdr', path])
        hdr = parse_hdr(out_text(proc))
        if hdr is None:
            bad_hdr.append('%s: %s' % (info['name'], err_text(proc)))
            continue
        want = dict(fmt=info['fmt'], w=info['w'], h=info['h'],
                    frames=info['frames'], bpp=info['bpp'],
                    usize=info['usize'], x=info['x'], y=info['y'],
                    rawlen=info['usize'])
        mism = [k for k, v in want.items() if hdr.get(k) != v]
        if mism:
            bad_hdr.append('%s: %s' % (info['name'], mism))
            continue
        fmt_count[info['fmt']] = fmt_count.get(info['fmt'], 0) + 1

        proc = run([exe, 'rgba', path, rgba_out, 'all'])
        if proc.returncode != 0:
            bad_pix.append('%s: %s' % (info['name'], err_text(proc)))
            continue
        got = open(rgba_out, 'rb').read()
        hz = hzref.Hzc1Image(path)
        exp = expected_rgba(hz).tobytes()
        total_pixels += info['w'] * info['h'] * info['frames']
        total_frames += info['frames']
        if len(got) != len(exp):
            bad_pix.append('%s: size %d != %d' % (info['name'], len(got),
                                                  len(exp)))
            continue
        g = np.frombuffer(got, np.uint8).reshape(-1, 4)
        e = np.frombuffer(exp, np.uint8).reshape(-1, 4)
        d = int(np.count_nonzero(np.any(g != e, axis=1)))
        diff_pixels += d
        if d:
            bad_pix.append('%s: %d differing pixels' % (info['name'], d))
        del g, e, hz, exp

    check(not bad_hdr, 'header metadata matches Python for all %d sampled files'
          % len(sample))
    counts = ' '.join('fmt%d=%d' % (k, v) for k, v in sorted(fmt_count.items()))
    check(not bad_pix,
          '%d images / %s frames / %s pixels: %d differing pixels (%s)%s'
          % (len(sample), fmt(total_frames), fmt(total_pixels), diff_pixels,
             counts,
             ('; ' + '; '.join(bad_pix[:4])) if bad_pix else ''))

    # per-frame API on multi-frame files
    multi = [i for i in infos if i['frames'] > 2][:3]
    bad_frame = []
    for info in multi:
        hz = hzref.Hzc1Image(info['path'])
        arr = expected_rgba(hz)
        for idx in (0, info['frames'] // 2, info['frames'] - 1):
            proc = run([exe, 'rgba', info['path'], rgba_out, str(idx)])
            if proc.returncode != 0:
                bad_frame.append('%s frame %d: %s'
                                 % (info['name'], idx, err_text(proc)))
                continue
            if open(rgba_out, 'rb').read() != arr[idx].tobytes():
                bad_frame.append('%s frame %d differs' % (info['name'], idx))
    check(not bad_frame,
          'per-frame API: %d multi-frame files, frames 0/mid/last byte-exact%s'
          % (len(multi), ('; ' + '; '.join(bad_frame[:4])) if bad_frame else ''))

    # PPM export (one sample per format)
    by_fmt = {}
    for info in infos:
        by_fmt.setdefault(info['fmt'], info)
    bad_ppm = []
    n_ppm = 0
    for fmt_id in sorted(by_fmt):
        info = by_fmt[fmt_id]
        ppm = os.path.join(TMP, 'out.ppm')
        proc = run([exe, 'ppm', info['path'], ppm])
        if proc.returncode != 0:
            bad_ppm.append('%s: %s' % (info['name'], err_text(proc)))
            continue
        raw = open(ppm, 'rb').read()
        m = re.match(br'P6\n(\d+) (\d+)\n255\n', raw)
        hz = hzref.Hzc1Image(info['path'])
        arr = expected_rgba(hz)[0]
        if m is None:
            bad_ppm.append('%s: bad PPM header' % info['name'])
            continue
        w, h = int(m.group(1)), int(m.group(2))
        body = raw[m.end():]
        rgb = np.ascontiguousarray(arr[..., :3]).tobytes()
        if w != info['w'] or h != info['h'] or body != rgb:
            bad_ppm.append('%s: %dx%d body %d/%d bytes'
                           % (info['name'], w, h, len(body), len(rgb)))
        n_ppm += 1
    check(not bad_ppm,
          'PPM export: %d formats (fmt %s) valid P6 with exact RGB pixels%s'
          % (n_ppm, ','.join(str(k) for k in sorted(by_fmt)),
             ('; ' + '; '.join(bad_ppm[:4])) if bad_ppm else ''))


# --------------------------------------------------------------------------
# 4. performance
# --------------------------------------------------------------------------

def bench_decode(exe, path, iters):
    proc = run([exe, 'bench', path, str(iters)])
    text = out_text(proc).strip()
    if proc.returncode != 0:
        return None, text
    m = re.search(r'iters=(\d+) total_ms=([\d.]+) per_iter_ms=([\d.]+) '
                  r'raw_bytes=(\d+) mib_per_s=([\d.]+)', text)
    if not m:
        return None, text
    return dict(iters=int(m.group(1)), total_ms=float(m.group(2)),
                per_iter_ms=float(m.group(3)), raw_bytes=int(m.group(4)),
                mib_per_s=float(m.group(5))), text


def test_perf(exe, extract):
    section('4. performance (informational)')
    if not os.path.isdir(extract):
        print('  SKIP no extracted images')
        return
    infos = scan_hzc1_headers(extract)
    picks = {}

    def first(pred):
        for info in infos:
            if pred(info):
                return info
        return None

    picks['fmt1 1920x1080'] = first(lambda i: i['fmt'] == 1 and i['w'] == 1920
                                    and i['h'] == 1080)
    picks['fmt0 1920x1080'] = first(lambda i: i['fmt'] == 0 and i['w'] == 1920
                                    and i['h'] == 1080)
    picks['fmt2 multiframe'] = first(lambda i: i['fmt'] == 2)
    picks['fmt3 1920x1080'] = first(lambda i: i['fmt'] == 3)
    for label, info in picks.items():
        if info is None:
            continue
        iters = 10
        r, text = bench_decode(exe, info['path'], iters)
        if r is None:
            print('  FAIL %s (%s): %s' % (label, info['name'], text))
            failures.append('bench %s' % label)
            continue
        print('  %-16s %-24s fmt=%d %dx%d frames=%d  %.3f ms/frame  '
              '%.1f MiB/s (raw pixel bytes)'
              % (label, info['name'], info['fmt'], info['w'], info['h'],
                 info['frames'], r['per_iter_ms'] / info['frames'],
                 r['mib_per_s']))
    fmt1_1080 = picks.get('fmt1 1920x1080')
    if fmt1_1080:
        proc = run([exe, 'bench-inflate', fmt1_1080['path'],
                    str(fmt1_1080['usize']), '20', '44'])
        text = out_text(proc).strip()
        print('  inflate only     %-24s %s' % (fmt1_1080['name'], text))
        if proc.returncode != 0:
            failures.append('bench-inflate')


# --------------------------------------------------------------------------
# main
# --------------------------------------------------------------------------

def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    ap.add_argument('--exe', default=DEFAULT_EXE, help='c_assets executable')
    ap.add_argument('--game', default=DEFAULT_GAME, help='game data directory')
    ap.add_argument('--extract', default=DEFAULT_EXTRACT,
                    help='directory of extracted hzc1 files')
    ap.add_argument('--seed', type=int, default=20261123)
    ap.add_argument('--quick', action='store_true',
                    help='fewer samples, 20k fuzz iterations')
    ap.add_argument('--full', action='store_true',
                    help='use every extracted file (all 1112) instead of samples')
    args = ap.parse_args(argv)

    exe = os.path.abspath(args.exe)
    if not os.path.isfile(exe):
        print('FATAL: c_assets.exe not found: %s' % exe)
        return 2
    if not os.path.isfile(HZC1_REF):
        print('FATAL: Python reference not found: %s' % HZC1_REF)
        return 2
    if os.path.isdir(TMP):
        shutil.rmtree(TMP)
    os.makedirs(TMP, exist_ok=True)

    t0 = time.time()
    hzref = load_hzc1_ref()
    print('verify_assets: exe=%s' % exe)
    print('               game=%s' % args.game)
    print('               extract=%s' % args.extract)
    print('               seed=%d%s' % (args.seed,
                                        ' (quick)' if args.quick else
                                        (' (full)' if args.full else '')))

    test_bin(exe, args.game, args.quick)
    test_inflate(exe, args.extract, args.seed, args.quick, full=args.full)
    test_hzc1(exe, args.extract, args.seed, args.quick, hzref, full=args.full)
    test_perf(exe, args.extract)

    dt = time.time() - t0
    section('SUMMARY')
    if failures:
        print('OVERALL: FAIL (%d failing check(s), %.1f s)' % (len(failures), dt))
        for f in failures:
            print('  - ' + f)
        return 1
    print('OVERALL: PASS (all checks, %.1f s)' % dt)
    return 0


if __name__ == '__main__':
    sys.exit(main())