#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""test_vm_edge.py -- VM boundary-condition tests for the FVP engine.

Builds minimal .hcb scripts that hit the interpreter's edges and runs them
through the real CLI:

  * empty file, header-only, bad/huge/truncated script_len, main_start
    pointing at 0 / past the end / into the middle of an instruction
  * jumps to self (infinite loop), past the end, into the middle of an
    instruction: the --steps budget must stop them (no hang)
  * stack underflow (operators with an empty stack), pushed operands with no
    frame, initstack/pushstack/copystack at index 255
  * stack flood (1,000,000 pushes) and recursion 100,000 frames deep
  * pushglobal/popglobal 65535, syscall ids 147/200/65535 (unknown command)
  * truncated pushstring, negative/garbage string lengths, string repeat
    overflow (the one intentional fatal-abort path), division/modulo by zero

Assertions, per case:
  * exit code is 0 (ran, bounded), 3 (loader refused with a diagnostic) or 2
    (the documented fatal-abort path with a message) -- never a crash;
  * the reported step count never exceeds the --steps budget;
  * cases marked expect_steps=N really stop at exactly N steps (proving the
    loop guard fired rather than the script ending by accident);
  * a control script (push 12345 / TextPrint) must produce the exact expected
    text, proving the generated HCB files are valid.

Every case is also re-run under fvm_guard.exe, a page-guard build of the same
sources: each heap block ends at a PAGE_NOACCESS page, so an out-of-bounds read
or write in the VM/loader faults immediately.  (The bundled gcc has no
AddressSanitizer; see fvp/docs/testing.md for the trade-offs.)

Usage:
    python fvp/tests/test_vm_edge.py [--no-build] [--jobs N] [--json PATH]
Exit code 0 = PASS, 1 = failure, 2 = setup problem.
"""
import argparse
import concurrent.futures
import json
import os
import struct
import subprocess
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
FVP = os.path.dirname(HERE)
OUT = os.path.join(FVP, 'out', 'fuzz')
TMP = os.path.join(OUT, 'tmp', 'vm')

sys.path.insert(0, HERE)
import harness_build as hb  # noqa: E402

# Instruction formats, same table as src/hcb.c (b=u8, w=u16le, d=u32le,
# s=u8 length incl. NUL + bytes + NUL).
FMT = {
    0x00: '', 0x01: 'bb', 0x02: 'd', 0x03: 'w', 0x04: '', 0x05: '', 0x06: 'd',
    0x07: 'd', 0x08: '', 0x09: '', 0x0a: 'd', 0x0b: 'w', 0x0c: 'b',
    0x0d: 'd', 0x0e: 's', 0x0f: 'w', 0x10: 'b', 0x11: 'w', 0x12: 'b',
    0x13: '', 0x14: '', 0x15: 'w', 0x16: 'b', 0x17: 'w', 0x18: 'b',
    0x19: '', 0x1a: '', 0x1b: '', 0x1c: '', 0x1d: '', 0x1e: '', 0x1f: '',
    0x20: '', 0x21: '', 0x22: '', 0x23: '', 0x24: '', 0x25: '', 0x26: '',
    0x27: '',
}


def emit(op, *operands):
    fmt = FMT[op]
    out = bytearray([op])
    ai = 0
    for tag in fmt:
        val = operands[ai]
        ai += 1
        if tag == 'b':
            out += struct.pack('<B', val & 0xFF)
        elif tag == 'w':
            out += struct.pack('<H', val & 0xFFFF)
        elif tag == 'd':
            out += struct.pack('<I', val & 0xFFFFFFFF)
        else:  # 's': operands[ai-1] is the raw body bytes (NUL included)
            body = val
            out += struct.pack('<B', len(body) & 0xFF) + body
    return bytes(out)


def pushstr(text, declared=None):
    body = text.encode('cp932', 'replace') + b'\x00'
    if declared is None:
        declared = len(body)
    return bytes([0x0e, declared & 0xFF]) + body


def hcb(code, main_start=4):
    """A well-formed .hcb from an instruction byte string."""
    return struct.pack('<I', 4 + len(code)) + code + struct.pack('<I', main_start)


def program(*parts):
    return b''.join(parts)


def case(name, data, steps, expect_exit=0, expect_steps=None,
         expect_min_steps=None, expect_stderr=None, text_out=None,
         text_contains=None, guard_steps=None):
    return {'name': name, 'data': data, 'steps': steps,
            'expect_exit': expect_exit, 'expect_steps': expect_steps,
            'expect_min_steps': expect_min_steps,
            'expect_stderr': expect_stderr, 'text_out': text_out,
            'text_contains': text_contains,
            'guard_steps': steps if guard_steps is None else guard_steps}


def build_cases():
    cases = []
    add = cases.append

    # ---- header / loader edges ------------------------------------------
    add(case('empty-file', b'', 100, expect_exit=3,
             expect_stderr='script too short'))
    add(case('one-byte', b'\x00', 100, expect_exit=3))
    add(case('short-7', b'\x01' * 7, 100, expect_exit=3))
    add(case('header-only', struct.pack('<II', 4, 4), 100, expect_steps=0))
    add(case('script-len-zero', struct.pack('<II', 0, 4), 100, expect_exit=3,
             expect_stderr='bad script length'))
    add(case('script-len-3', struct.pack('<II', 3, 4), 100, expect_exit=3))
    add(case('script-len-huge', struct.pack('<II', 0xFFFFFFFF, 4), 100,
             expect_exit=3, expect_stderr='bad script length'))
    add(case('script-len-overrun', struct.pack('<II', 0x7FFFFFFF, 4), 100,
             expect_exit=3))
    add(case('main-start-zero', hcb(emit(0x04), main_start=0), 100,
             expect_steps=0))
    add(case('main-start-outside', hcb(emit(0x04), main_start=0xFFFFFFFF), 100,
             expect_steps=0))
    add(case('main-start-mid-instr', hcb(emit(0x0c, 1) + emit(0x04),
                                         main_start=5), 100, expect_steps=0))
    add(case('nop-only', hcb(emit(0x00)), 100, expect_steps=1))
    add(case('ret-no-frame', hcb(emit(0x04)), 100, expect_steps=1))
    add(case('ret2-no-frame', hcb(emit(0x05)), 100, expect_steps=1))
    add(case('truncated-main-start', struct.pack('<I', 4 + 1) + b'\x04' +
             b'\x00\x00\x00' + struct.pack('<I', 4), 100, expect_steps=0))
    add(case('unknown-opcode', hcb(bytes([40]) + b'\x00' * 4), 100,
             expect_exit=3, expect_stderr='unknown opcode'))
    add(case('steps-zero', hcb(emit(0x0c, 1) + emit(0x06, 4)), 0,
             expect_steps=0))

    # ---- control: prove the generated HCB really runs -------------------
    control = hcb(program(emit(0x0c, 0), emit(0x0a, 12345), emit(0x03, 123),
                          emit(0x04)))
    add(case('control-textprint', control, 100, expect_steps=4,
             text_out='control.txt', text_contains='[0] 12345'))

    # ---- loops: the step budget is the hang guard -----------------------
    add(case('jmp-self', hcb(emit(0x06, 4)), 1000, expect_steps=1000))
    add(case('jmpcond-self', hcb(emit(0x09) + emit(0x07, 4)), 1000,
             expect_steps=1000))
    add(case('call-self-recursion', hcb(emit(0x02, 4)), 20000,
             expect_steps=20000))
    add(case('call-big-recursion', hcb(emit(0x02, 4)), 100000,
             expect_steps=100000, guard_steps=100000))
    add(case('jmp-script-end', hcb(emit(0x06, 5)), 100, expect_steps=1))
    add(case('jmp-mid-instr', hcb(emit(0x06, 5)), 100, expect_steps=1))

    # ---- stack underflow ------------------------------------------------
    add(case('underflow-add', hcb(emit(0x1a) + emit(0x04)), 100,
             expect_steps=2))
    add(case('underflow-all-binops',
             hcb(program(*[emit(op) for op in range(0x1a, 0x28)],
                         emit(0x19), emit(0x13), emit(0x14),
                         emit(0x03, 123), emit(0x04))), 100, expect_steps=19))
    add(case('div-mod-zero',
             hcb(program(emit(0x0c, 1), emit(0x0c, 0), emit(0x1d),
                         emit(0x0c, 1), emit(0x0c, 0), emit(0x1e),
                         emit(0x04))), 100, expect_steps=7))
    add(case('pop-no-frame', hcb(program(emit(0x10, 255), emit(0x16, 255),
                                         emit(0x13), emit(0x04))), 100,
             expect_steps=4))
    # initstack 建的是「栈底帧」：它的返回地址是 script_len（脚本末尾），所以末尾的
    # ret 直接结束线程。2026-11-24 之前这里写的是 initstack 自身 pc，ret 会跳回入口
    # 无限自循环，把 200 步预算跑满；修正后是 50 步就干净退出。
    add(case('initstack-255',
             hcb(program(emit(0x01, 255, 0),
                         *[emit(0x10, i) for i in range(0, 256, 8)],
                         *[emit(0x16, i) for i in range(0, 256, 17)],
                         emit(0x04))), 200, expect_steps=50))

    # ---- stack overflow (1,000,000 pushes) ------------------------------
    flood_code = emit(0x0c, 1) * 500000 + emit(0x06, 4)
    add(case('stack-flood-500k', hcb(flood_code), 200000, expect_steps=200000,
             guard_steps=200000))
    million = emit(0x0c, 1) * 1000000
    add(case('stack-million', hcb(million), 1000000, expect_steps=1000000,
             guard_steps=20000))

    # ---- globals / syscalls / arrays ------------------------------------
    add(case('pushglobal-65535',
             hcb(program(emit(0x0f, 65535), emit(0x0f, 8192),
                         emit(0x0f, 8191), emit(0x15, 65535),
                         emit(0x15, 8192), emit(0x04))), 100,
             expect_steps=6))
    add(case('syscall-unknown-200',
             hcb(program(emit(0x03, 200), emit(0x04))), 100, expect_steps=2))
    add(case('syscall-unknown-65535',
             hcb(program(emit(0x03, 65535), emit(0x04))), 100,
             expect_steps=2))
    add(case('syscall-147',
             hcb(program(emit(0x03, 147), emit(0x04))), 100, expect_steps=2))
    add(case('syscall-threadwait-loop',
             hcb(emit(0x03, 137) + emit(0x06, 4)), 500, expect_steps=500))
    add(case('garr-string-key',
             hcb(program(emit(0x0e, b'k\x00'), emit(0x0c, 1), emit(0x17, 3),
                         emit(0x0e, b'k\x00'), emit(0x11, 3), emit(0x04))),
             100, expect_steps=6))

    # ---- strings --------------------------------------------------------
    add(case('pushstring-truncated',
             hcb(bytes([0x0e, 200]) + b'a\x00' + emit(0x04)), 100,
             expect_steps=1))
    add(case('pushstring-255',
             hcb(pushstr('A' * 253) + emit(0x04)), 100, expect_steps=2))
    add(case('str-repeat-overflow',
             hcb(program(emit(0x0a, 0x7FFFFFFF), emit(0x0e, b'ab\x00'),
                         emit(0x1c), emit(0x04))), 100, expect_exit=2,
             expect_stderr='string repeat too large'))
    add(case('neg-string',
             hcb(program(emit(0x0e, b's\x00'), emit(0x19), emit(0x04))), 100,
             expect_steps=3))
    add(case('float-nan-compare',
             hcb(program(emit(0x0d, 0x7FC00000), emit(0x0d, 0x7FC00000),
                         emit(0x22), emit(0x04))), 100, expect_steps=4))

    # ---- encoding switch ------------------------------------------------
    add(case('gbk-encoded-string',
             hcb(program(emit(0x0e, b'\x81\x40\x00'), emit(0x04))), 100,
             expect_steps=2))

    return cases


def parse_summary(stdout):
    info = {'steps': None, 'text lines': None, 'threads': None}
    for line in stdout.splitlines():
        for key in info:
            if line.startswith(key):
                parts = line.split()
                if len(parts) >= 2:
                    try:
                        info[key] = int(parts[-1])
                    except ValueError:
                        pass
    return info


def run_case(exe, path, steps, text_path=None, enc='cp932', timeout=120.0):
    cmd = [exe, path, '--steps', str(steps), '--enc', enc, '--quiet']
    if text_path:
        cmd += ['--text-out', text_path, '--eol', 'lf']
    t0 = time.perf_counter()
    try:
        proc = subprocess.run(cmd, stdout=subprocess.PIPE,
                              stderr=subprocess.PIPE, timeout=timeout)
    except subprocess.TimeoutExpired:
        return {'status': 'timeout', 'exit': None, 'ms': 0.0,
                'stdout': b'', 'stderr': b'', 'summary': {}}
    elapsed = (time.perf_counter() - t0) * 1000.0
    return {'status': 'ran', 'exit': proc.returncode, 'ms': elapsed,
            'stdout': proc.stdout, 'stderr': proc.stderr,
            'summary': parse_summary(proc.stdout.decode('utf-8', 'replace'))}


def verdict(c, res, text_ok):
    """Return (ok, reason) for one fvm run."""
    if res['status'] != 'ran':
        return False, '%s (no exit code)' % res['status']
    rc = res['exit']
    if rc is None:
        return False, 'no exit code'
    if rc < 0:
        return False, 'crash (NTSTATUS 0x%08X)' % (rc & 0xFFFFFFFF)
    if rc in (71, 72):
        return False, 'memory fault detected (exit %d)' % rc
    expected = c['expect_exit']
    if rc != expected:
        tail = (res['stderr'] or b'')[-160:].decode('utf-8', 'replace')
        return False, 'exit %s, expected %s (%s)' % (rc, expected,
                                                     tail.strip())
    if rc in (2, 3):
        err = (res['stderr'] or b'').decode('utf-8', 'replace')
        needle = c.get('expect_stderr')
        if needle and needle not in err:
            return False, 'diagnostic %r missing from %r' % (needle,
                                                             err[:200])
        if rc == 3 and not err.strip().startswith('fvm:'):
            return False, 'loader error without an fvm: diagnostic'
        return True, 'rejected: %s' % err.strip().splitlines()[0][:60]
    s = res['summary']
    steps = s.get('steps')
    budget = c['steps']
    if steps is not None and steps > budget:
        return False, 'ran %d steps > budget %d' % (steps, budget)
    if c['expect_steps'] is not None and steps != c['expect_steps']:
        return False, 'ran %s steps, expected %s' % (steps, c['expect_steps'])
    if c['expect_min_steps'] is not None and (steps or 0) < c['expect_min_steps']:
        return False, 'ran %s steps < expected minimum %s' % (
            steps, c['expect_min_steps'])
    if not text_ok:
        return False, 'text output mismatch'
    return True, 'steps=%s threads=%s' % (steps, s.get('threads'))


def main(argv=None):
    ap = argparse.ArgumentParser()
    ap.add_argument('--no-build', action='store_true')
    ap.add_argument('--json', default=os.path.join(OUT, 'vm_edge.json'))
    args = ap.parse_args(argv)

    os.makedirs(TMP, exist_ok=True)
    cc = None if args.no_build else hb.find_cc()
    tools = hb.build_all(cc) if cc else {}
    fvm = hb.public_tools()['fvm']
    fvm_guard = tools.get('fvm_guard')
    if not os.path.isfile(fvm):
        print('test_vm_edge: missing %s (run fvp/build.ps1 first)' % fvm)
        return 2
    print('test_vm_edge: fvm=%s' % fvm)
    print('  guard build: %s' % (fvm_guard if fvm_guard else '<unavailable>'))

    cases = build_cases()
    rows = []
    failures = []
    t_start = time.perf_counter()
    for c in cases:
        path = os.path.join(TMP, c['name'].replace('/', '_') + '.hcb')
        with open(path, 'wb') as f:
            f.write(c['data'])
        text_path = None
        if c['text_out']:
            text_path = os.path.join(TMP, c['text_out'])
            if os.path.exists(text_path):
                os.unlink(text_path)
        res = run_case(fvm, path, c['steps'], text_path)
        text_ok = True
        if c['text_contains'] is not None:
            try:
                with open(text_path, 'rb') as f:
                    got = f.read().decode('utf-8', 'replace')
            except OSError:
                got = ''
            text_ok = c['text_contains'] in got
            if not text_ok:
                text_ok = False
        ok, reason = verdict(c, res, text_ok)

        guard_res = None
        guard_ok = None
        if fvm_guard and ok:
            guard_res = run_case(fvm_guard, path, c['guard_steps'])
            grc = guard_res['exit']
            if guard_res['status'] != 'ran':
                guard_ok = False
                guard_reason = 'guard run %s' % guard_res['status']
            elif grc is not None and (grc < 0 or grc in (71, 72)):
                guard_ok = False
                guard_reason = ('guard memory fault exit=%s' %
                                (grc if grc >= 0 else
                                 '0x%08X' % (grc & 0xFFFFFFFF)))
            elif grc != c['expect_exit'] and grc not in (0, 2, 3):
                guard_ok = False
                guard_reason = 'guard exit=%s' % grc
            else:
                guard_ok = True
                guard_reason = 'exit=%s' % grc
        elif fvm_guard is None:
            guard_reason = 'skipped (no guard build)'
        else:
            guard_reason = 'skipped (fvm case failed)'

        if not ok:
            failures.append({'name': c['name'], 'reason': reason})
        if guard_ok is False:
            failures.append({'name': c['name'], 'reason': guard_reason,
                             'phase': 'guard'})
        rows.append({'name': c['name'], 'exit': res['exit'],
                     'expected_exit': c['expect_exit'],
                     'steps': (res['summary'] or {}).get('steps'),
                     'budget': c['steps'], 'guard': guard_reason,
                     'ok': ok and guard_ok is not False,
                     'reason': reason, 'ms': res['ms']})

    print('')
    print('  %-24s %5s %7s %8s  %-34s %s' %
          ('case', 'exit', 'steps', 'budget', 'verdict', 'guard'))
    for row in rows:
        print('  %-24s %5s %7s %8s  %-34s %s' %
              (row['name'], row['exit'], row['steps'], row['budget'],
               'PASS' if row['ok'] else 'FAIL ' + row['reason'],
               row['guard']))
    elapsed = time.perf_counter() - t_start
    print('')
    print('== TOTAL ==')
    print('  cases=%d failed=%d elapsed=%.2f s' % (len(rows), len(failures),
                                                   elapsed))
    verdict_str = 'PASS' if not failures else 'FAIL'
    print('  RESULT: %s' % verdict_str)
    report = {'when': time.strftime('%Y-%m-%d %H:%M:%S'), 'cases': rows,
              'failures': failures, 'elapsed_s': elapsed,
              'guard_build': fvm_guard, 'verdict': verdict_str}
    hb.write_json(args.json, report)
    print('  json -> %s' % args.json)
    return 0 if verdict_str == 'PASS' else 1


if __name__ == '__main__':
    sys.exit(main())