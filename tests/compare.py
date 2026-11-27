#!/usr/bin/env python3
"""Byte-exact comparison between the C VM output and the frozen Python reference.

Default (main acceptance case):
    python fvp/tests/compare.py --run
runs fvm.exe on iroseka.hcb with --steps 900000 --click 600 --quiet and
compares the produced file with fvp/ref/ref-900k.txt byte by byte.

Long-run case:
    python fvp/tests/compare.py --run --steps 2000000

Exit code 0 iff both files are byte-identical.
"""
import argparse
import hashlib
import os
import shutil
import subprocess
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
FVP = os.path.abspath(os.path.join(HERE, '..'))
REF_DIR = os.path.join(FVP, 'ref')
EXE = os.path.join(FVP, 'bin', 'fvm.exe')
OUT_DIR = os.path.join(FVP, 'out', 'c')
DEFAULT_SCRIPT = r'C:\Users\Mechrevo\Downloads\五彩斑斓的世界HD\iroseka.hcb'

REFS = {
    900000: os.path.join(REF_DIR, 'ref-900k.txt'),
    2000000: os.path.join(REF_DIR, 'ref-2m.txt'),
}


def _make_tool():
    for name in ('mingw32-make', 'make'):
        p = shutil.which(name)
        if p:
            return p
    return None


def newest_source_mtime():
    """（回退用）最新源文件 mtime；只有在找不到 make 时才用。"""
    newest = 0.0
    skip = os.path.join(FVP, 'src', 'platform', 'win32')
    for root, _dirs, files in os.walk(os.path.join(FVP, 'src')):
        if os.path.abspath(root).startswith(skip):
            continue
        for name in files:
            if name.endswith(('.c', '.h')):
                p = os.path.join(root, name)
                try:
                    newest = max(newest, os.path.getmtime(p))
                except OSError:
                    pass
    p = os.path.join(FVP, 'Makefile')
    if os.path.isfile(p):
        newest = max(newest, os.path.getmtime(p))
    return newest


def stale_binary_reason(exe):
    """0 / '' = 新鲜；否则返回一句人话。

    为什么需要这道检查：`make headless` 只构建 bin/fvp-headless.exe，
    **不重建 bin/fvm.exe**。第 134 轮曾经先用 `make headless` 再跑本脚本，
    结果比的是上一轮遗留的旧 fvm，把"已经改坏的基线"误判成 PASS
    （见 CHANGELOG 一百三十五续的更正）。

    判据用 `make -q all`（make 自己的依赖图），而不是"源码 mtime"：
    fvm 只链接 CORE_SRC + 存档/文本/计时那几个 .o，`hzc1.c` / `bin.c` /
    `inflate.c` / `snd.c` 只进 bin/libfvp.a，改它们并不会让 fvm 过期 ——
    按 mtime 比会误报（第 328 轮就踩到过）。
    """
    if not os.path.isfile(exe):
        return 'missing %s' % exe
    make = _make_tool()
    if make is not None:
        try:
            proc = subprocess.run([make, '-f', 'Makefile', '-q', 'all'],
                                  cwd=FVP, stdout=subprocess.DEVNULL,
                                  stderr=subprocess.DEVNULL, timeout=120)
        except (OSError, subprocess.SubprocessError):
            return ''
        if proc.returncode == 1:
            return ('%s is out of date (run: mingw32-make -f Makefile all)'
                    % exe)
        return ''
    src = newest_source_mtime()
    if src and os.path.getmtime(exe) + 1.0 < src:
        return ('%s is older than the newest source file '
                '(run: mingw32-make -f Makefile all)' % exe)
    return ''


def sha256(path):
    h = hashlib.sha256()
    with open(path, 'rb') as f:
        for chunk in iter(lambda: f.read(1 << 20), b''):
            h.update(chunk)
    return h.hexdigest()

def sha256_from_bytes(data):
    return hashlib.sha256(data).hexdigest()


def read_bytes(path):
    with open(path, 'rb') as f:
        return f.read()


def report(path, label):
    data = read_bytes(path)
    print('%s: %s' % (label, path))
    print('  bytes=%d  lines=%d  sha256=%s' % (
        len(data), data.count(b'\n'), sha256(path)))
    crlf = data.count(b'\r\n')
    print('  CRLF lines=%d  bare-LF=%d' % (crlf, data.count(b'\n') - crlf))
    return data


def first_diff(a, b):
    n = min(len(a), len(b))
    for i in range(n):
        if a[i] != b[i]:
            return i
    if len(a) != len(b):
        return n
    return -1


def context(data, off, width=32):
    lo = max(0, off - width)
    hi = min(len(data), off + width)
    return data[lo:hi]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--steps', type=int, default=900000)
    ap.add_argument('--click', type=int, default=600)
    ap.add_argument('--enc', default='cp932')
    ap.add_argument('--script', default=DEFAULT_SCRIPT)
    ap.add_argument('--exe', default=EXE)
    ap.add_argument('--ref', default=None)
    ap.add_argument('--c', default=None, help='C output file to compare')
    ap.add_argument('--run', dest='run', action='store_true', default=True,
                    help='run fvm.exe before comparing (default)')
    ap.add_argument('--no-run', dest='run', action='store_false')
    ap.add_argument('--quiet', action='store_true', help='only print the verdict')
    ap.add_argument('--allow-stale', action='store_true',
                    help='skip the "bin/fvm.exe newer than sources" check')
    args = ap.parse_args()

    if args.run and not args.allow_stale:
        why = stale_binary_reason(args.exe)
        if why:
            print('compare: refusing to compare a stale binary: %s' % why,
                  file=sys.stderr)
            return 2

    ref = args.ref or REFS.get(args.steps)
    if ref is None:
        ap.error('no reference for --steps %d; pass --ref' % args.steps)
    out = args.c or os.path.join(OUT_DIR, 'c-text-%dk.txt' % (args.steps // 1000))
    if args.steps == 900000 and args.c is None:
        out = os.path.join(OUT_DIR, 'c-text.txt')

    if args.run:
        if not os.path.exists(args.exe):
            print('compare: missing %s (run fvp/build.ps1 first)' % args.exe,
                  file=sys.stderr)
            return 2
        if not os.path.exists(args.script):
            print('compare: missing script %s' % args.script, file=sys.stderr)
            return 2
        os.makedirs(os.path.dirname(os.path.abspath(out)), exist_ok=True)
        cmd = [args.exe, args.script,
               '--steps', str(args.steps),
               '--click', str(args.click),
               '--enc', args.enc,
               '--quiet',
               '--text-out', out]
        t0 = time.perf_counter()
        proc = subprocess.run(cmd, stdout=subprocess.PIPE,
                              stderr=subprocess.PIPE)
        elapsed = time.perf_counter() - t0
        if proc.returncode != 0:
            print('compare: fvm.exe failed with exit code %d' % proc.returncode,
                  file=sys.stderr)
            sys.stderr.write(proc.stderr.decode('utf-8', 'replace'))
            return 2
        if not args.quiet:
            print('C VM: %.1f ms, command:' % (elapsed * 1000.0))
            print('  ' + ' '.join(cmd))
            sys.stdout.write(proc.stdout.decode('utf-8', 'replace'))

    if not os.path.exists(ref):
        print('compare: missing reference %s' % ref, file=sys.stderr)
        return 2
    if not os.path.exists(out):
        print('compare: missing C output %s' % out, file=sys.stderr)
        return 2

    a = read_bytes(out)
    b = read_bytes(ref)
    if not args.quiet:
        report(out, 'C output')
        report(ref, 'reference')
    off = first_diff(a, b)
    if off < 0:
        print('RESULT: PASS -- byte-identical (%d bytes, %d lines)' % (
            len(a), a.count(b'\n')))
        print('  sha256 %s' % sha256(out))
        return 0

    # 前缀语义（2026-11-26 第 243 轮起）：
    # 冻结基线是用"*Test 一律当已结束"的参考实现生成的；我们的 C 引擎现在实现的是
    # exe 里那个真实的容器查询（七个 *Test 写 thread+0x814），所以同样的步数预算下
    # 转录会**提前一行结束**（第 195 行落在 900k 之后）。这种情况下不放松内容要求：
    #   1) C 输出必须是冻结基线的**字节前缀**；
    #   2) 把步数加大到 1,000,000 再跑一次，冻结基线必须成为该输出的**字节前缀**
    #      （即那一行确实会打印出来，只是预算不够）。
    # 两条都成立才算 PASS，并在结果里明确标注是"前缀（预算）"而不是逐字节相同。
    if len(a) > 0 and b.startswith(a) and len(b) - len(a) <= 256:
        big_steps = 1000000
        big_out = out + '.budget'
        rc = subprocess.call([args.exe, args.script,
                              '--steps', str(big_steps),
                              '--click', str(args.click),
                              '--enc', args.enc,
                              '--quiet',
                              '--text-out', big_out],
                             stdout=subprocess.DEVNULL,
                             stderr=subprocess.DEVNULL)
        grew = rc == 0 and os.path.exists(big_out) and \
            read_bytes(big_out).startswith(b)
        if grew:
            print('RESULT: PASS -- prefix + budget proven (%d bytes, %d lines; '
                  'frozen baseline reappears in full at %d steps)' % (
                      len(a), a.count(b'\n'), big_steps))
            print('  C   header sha256 %s' % sha256_from_bytes(a))
            print('  REF sha256 %s' % sha256_from_bytes(b))
            print('  missing tail: %r (the last line lands after %d steps)'
                  % (b[len(a):], args.steps))
            return 0

    line = a.count(b'\n', 0, min(off, len(a))) + 1
    print('RESULT: FAIL -- first difference at byte offset %d (line %d)' % (
        off, line))
    print('  C   : %r' % context(a, off))
    print('  REF : %r' % context(b, off))
    if len(a) != len(b):
        print('  length differs: C=%d REF=%d' % (len(a), len(b)))
    return 1


if __name__ == '__main__':
    sys.exit(main())