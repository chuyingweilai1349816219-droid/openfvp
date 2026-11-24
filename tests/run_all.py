#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""run_all.py -- one-shot robustness suite for the FVP engine.

Runs, in order:

  1. allocator-selfcheck  proves the fuzz-only OOB detector actually fires
                          (guard build faults, canary build exits 72)
  2. compare.py --run     existing acceptance: byte-identical text vs the
                          frozen Python reference for iroseka.hcb (900k steps)
  3. verify_assets.py --quick
                          existing asset acceptance (BIN/HZC1/INFLATE/HZC1
                          decode against the Python reference)
  4. test_snd (ogg/wav)   existing audio acceptance on the real corpus
  5. test_vm_edge.py      VM boundary conditions (new)
  6. fuzz_hcb.py          malformed .hcb mutation fuzz, in-process + fvm.exe
                          (new, >= 20,000 mutations by default)
  7. fuzz_assets.py       malformed BIN/HZC1/inflate/WAV/Ogg fuzz (new)

Nothing under fvp/src, fvp/tests/*.c (existing) or the Makefile is modified:
the new harnesses are built into fvp/out/fuzz/ from read-only sources, and the
allowed (read-only) existing tests are invoked as-is.

Usage:
    python fvp/tests/run_all.py [--fast] [--only NAMES] [--exclude NAMES]
                               [--list] [--keep] [--json PATH]

Exit code 0 = every non-skipped step passed.
"""
import argparse
import json
import os
import subprocess
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
FVP = os.path.dirname(HERE)
OUT = os.path.join(FVP, 'out', 'fuzz')
TMP = os.path.join(OUT, 'tmp')

sys.path.insert(0, HERE)
import harness_build as hb  # noqa: E402
import fuzz_assets as fa  # noqa: E402
import fuzz_hcb as fh  # noqa: E402

PY = sys.executable


def log(msg):
    print(msg)
    sys.stdout.flush()


def tail(text, lines=25):
    rows = text.strip().splitlines()
    return '\n'.join(rows[-lines:])


def has_module(name):
    proc = subprocess.run([PY, '-c', 'import %s' % name],
                          stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    return proc.returncode == 0


def note_from(output, keys):
    """Pick the most informative summary line, in key priority order."""
    lines = [ln.strip() for ln in output.splitlines() if ln.strip()]
    for key in keys:
        for line in lines:
            if key in line and not line.startswith('base '):
                return line[:120]
    return ''


class Step(object):
    def __init__(self, name, argv, timeout, parsers=(), skip_reason=None,
                 note=''):
        self.name = name
        self.argv = argv
        self.timeout = timeout
        self.parsers = parsers
        self.skip_reason = skip_reason
        self.note = note


def build_steps(args):
    tools = hb.public_tools()
    fvm = tools['fvm']
    test_snd = tools['test_snd']
    game = fa.find_game_dir()
    script = fh.find_script()
    steps = []

    # 1. allocator self-check (built + run by the fuzz scripts too, but listed
    #    explicitly so "0 faults" is not taken on faith).
    steps.append(Step('allocator-selfcheck', None, 120,
                      note='page-guard and canary builds must both fault'))

    # 2. reference equivalence
    compare = os.path.join(HERE, 'compare.py')
    ref = os.path.join(FVP, 'ref', 'ref-900k.txt')
    if not (os.path.isfile(fvm) and os.path.isfile(compare) and
            os.path.isfile(ref) and script):
        steps.append(Step('compare.py --run', None, 0,
                          skip_reason='missing fvm.exe, ref-900k.txt or '
                                      'iroseka.hcb'))
    else:
        steps.append(Step('compare.py --run',
                          [PY, compare, '--run', '--quiet'], 900,
                          note='byte-identical vs Python reference'))

    # 2c. 多游戏兼容性门禁（第二部作品也要能跑、可重复、编码正确）
    vmulti = os.path.join(HERE, 'verify_multigame.py')
    if not (os.path.isfile(fvm) and os.path.isfile(vmulti) and script):
        steps.append(Step('verify_multigame.py', None, 0,
                          skip_reason='missing fvm.exe or iroseka.hcb'))
    else:
        vt_multi = [PY, vmulti]
        if args.fast:
            vt_multi += ['--steps', '300000']
        steps.append(Step('verify_multigame.py', vt_multi, 900,
                          note='two FVP titles: decode, run twice, deterministic, '
                               'JP text ratio'))

    # 2b. coroutine slots must stay behaviour-neutral
    vthreads = os.path.join(HERE, 'verify_threads.py')
    if not (os.path.isfile(fvm) and os.path.isfile(vthreads) and
            os.path.isfile(ref) and script):
        steps.append(Step('verify_threads.py', None, 0,
                          skip_reason='missing fvm.exe, ref-900k.txt or '
                                      'iroseka.hcb'))
    else:
        vt_argv = [PY, vthreads]
        if args.fast:
            vt_argv.append('--fast')
        steps.append(Step('verify_threads.py',
                          vt_argv,
                          900,
                          note='ThreadStart/ThreadExit on/off both byte-exact'))

    # 3. asset acceptance
    verify = os.path.join(HERE, 'verify_assets.py')
    extract = os.path.join(FVP, 'extract', 'graph')
    if not (os.path.isfile(verify) and game and has_module('numpy')):
        steps.append(Step('verify_assets.py --quick', None, 0,
                          skip_reason='verify_assets.py, numpy or the game '
                                      'directory is unavailable'))
    else:
        steps.append(Step('verify_assets.py --quick',
                          [PY, verify, '--quick'], 3600,
                          note='BIN/HZC1/inflate vs Python reference'))

    # 4. audio acceptance (existing test)
    ogg = os.path.join(FVP, 'work', 'bgm001.ogg')
    wav = os.path.join(FVP, 'work', 'se001.wav')
    for label, path in (('test_snd (ogg)', ogg), ('test_snd (wav)', wav)):
        if not (os.path.isfile(test_snd) and os.path.isfile(path)):
            steps.append(Step(label, None, 0,
                              skip_reason='bin/test_snd.exe or %s missing' %
                                          os.path.basename(path)))
        else:
            out_wav = os.path.join(TMP, 'run_all_%s.wav' %
                                   ('ogg' if label.endswith('(ogg)') else 'wav'))
            steps.append(Step(label, [test_snd, path, out_wav], 300,
                              note='decode + mix on the real corpus'))

    # 4b. save layer: synthetic self-test, then real slots (byte-identical)
    test_save = tools['test_save']
    if not os.path.isfile(test_save):
        steps.append(Step('test_save (selftest)', None, 0,
                          skip_reason='bin/test_save.exe missing '
                                      '(run: make test_save)'))
    else:
        steps.append(Step('test_save (selftest)', [test_save], 120,
                          note='synthetic slot/save.bin round-trip + malformed '
                               'input rejection'))
        save_dir = os.path.join(game, 'save') if game else None
        sbin = os.path.join(save_dir, 'save.bin') if save_dir else None
        slots = []
        if save_dir and os.path.isdir(save_dir):
            for n in sorted(os.listdir(save_dir)):
                if (len(n) == 8 and n[0] == 's' and n[1:4].isdigit() and
                        n.endswith('.bin')):
                    slots.append(os.path.join(save_dir, n))
        if sbin and os.path.isfile(sbin) and slots:
            steps.append(Step('test_save (real slots)',
                              [test_save, '--savebin', sbin] + slots, 300,
                              note='%d slots: byte-identical re-serialisation '
                                   'against docs/save-format.md' % len(slots)))
        else:
            steps.append(Step('test_save (real slots)', None, 0,
                              skip_reason='no save/save.bin + save/sNNN.bin '
                                          'under the game directory'))

    # 4c. save.bin -> VM globals bridge
    test_save_vm = tools['test_save_vm']
    if not os.path.isfile(test_save_vm):
        steps.append(Step('test_save_vm (selftest)', None, 0,
                          skip_reason='bin/test_save_vm.exe missing '
                                      '(run: make test_save_vm)'))
    else:
        steps.append(Step('test_save_vm (selftest)', [test_save_vm], 120,
                          note='type mapping / int32 wrap / capacity / stats '
                               'only'))
        if sbin and os.path.isfile(sbin):
            steps.append(Step('test_save_vm (real save.bin)',
                              [test_save_vm, sbin], 300,
                              note='1471 = 145 int + 846 unknown + 480 unset, '
                                   'values vs docs §3.3'))
        else:
            steps.append(Step('test_save_vm (real save.bin)', None, 0,
                              skip_reason='no save/save.bin under the game '
                                          'directory'))

    # 4d. global flag bitmap (FlagGet/FlagSet storage + save.bin seeding)
    test_flags = tools['test_flags']
    if not os.path.isfile(test_flags):
        steps.append(Step('test_flags (selftest)', None, 0,
                          skip_reason='bin/test_flags.exe missing '
                                      '(run: make test_flags)'))
    else:
        steps.append(Step('test_flags (selftest)', [test_flags], 120,
                          note='2048-bit map: get/set/clear, bounds, popcount, '
                               'NULL safety'))
        if sbin and os.path.isfile(sbin):
            steps.append(Step('test_flags (real save.bin)',
                              [test_flags, sbin], 300,
                              note='save.bin flag bitmap equals the seeded map '
                                   '(docs §3.5: 0 bits in this title)'))
        else:
            steps.append(Step('test_flags (real save.bin)', None, 0,
                              skip_reason='no save/save.bin under the game '
                                          'directory'))

    # 4e. text window attributes (TextColor/TextSize/TextSpeed storage)
    test_textattr = tools['test_textattr']
    if not os.path.isfile(test_textattr):
        steps.append(Step('test_textattr', None, 0,
                          skip_reason='bin/test_textattr.exe missing '
                                      '(run: make test_textattr)'))
    else:
        steps.append(Step('test_textattr', [test_textattr], 120,
                          note='colour/size/speed clamping, window bounds, '
                               'configured count'))

    # 4f. save thumbnail scaling (1920x1080 BGRA -> 200x113)
    test_thumb = tools['test_thumb']
    if not os.path.isfile(test_thumb):
        steps.append(Step('test_thumb', None, 0,
                          skip_reason='bin/test_thumb.exe missing '
                                      '(run: make test_thumb)'))
    else:
        steps.append(Step('test_thumb', [test_thumb], 120,
                          note='box filter colour/alpha, 2x2 average, real '
                               '1920x1080 -> 200x113'))

    # 4g. UTF-8 -> CP932 / GBK encoding (needed to write save strings)
    test_encode = tools['test_encode']
    if not os.path.isfile(test_encode):
        steps.append(Step('test_encode', None, 0,
                          skip_reason='bin/test_encode.exe missing '
                                      '(run: make test_encode)'))
    else:
        steps.append(Step('test_encode', [test_encode], 120,
                          note='UTF-8 -> CP932/GBK bytes + decode round-trip, '
                               'unmappable -> ?'))

    # 4h. mouse wheel accumulate/take semantics (InputGetWheel, command 36)
    test_wheel = tools['test_wheel']
    if not os.path.isfile(test_wheel):
        steps.append(Step('test_wheel', None, 0,
                          skip_reason='bin/test_wheel.exe missing '
                                      '(run: make test_wheel)'))
    else:
        steps.append(Step('test_wheel', [test_wheel], 120,
                          note='wheel accumulate / take-clears / negative / '
                               'NULL safety'))

    # 4i. animation tween core (Motion* timeline math, not wired to gfx yet)
    test_motion = tools['test_motion']
    if not os.path.isfile(test_motion):
        steps.append(Step('test_motion', None, 0,
                          skip_reason='bin/test_motion.exe missing '
                                      '(run: make test_motion)'))
    else:
        steps.append(Step('test_motion', [test_motion], 120,
                          note='lerp timeline, zero/negative duration, reap, '
                               'multi-tween, NULL safety'))

    # 4i2. Control* / Cursor* state (ControlMask 8 / ControlPulse 9 /
    #      CursorChange 10 / CursorMove 11): type-driven validation only.
    test_control = tools.get('test_control')
    if not test_control or not os.path.isfile(test_control):
        steps.append(Step('test_control', None, 0,
                          skip_reason='bin/test_control.exe missing '
                                      '(run: make test_control)'))
    else:
        steps.append(Step('test_control', [test_control], 120,
                          note='mask/pulse/cursor state: type-vs-value rules, '
                               '0..3 clamp, NULL args, per-command counters'))

    # 4j. motion wired into gfx (disabled by default -> old frames unaffected)
    test_gfx_motion = tools['test_gfx_motion']
    if not os.path.isfile(test_gfx_motion):
        steps.append(Step('test_gfx_motion', None, 0,
                          skip_reason='bin/test_gfx_motion.exe missing '
                                      '(run: make test_gfx_motion)'))
    else:
        steps.append(Step('test_gfx_motion', [test_gfx_motion], 120,
                          note='disabled = no movement; enabled = lerp to '
                               'midpoint/target for xy/alpha/scale'))

    # 4k. texture cache LRU cap (long runs must not grow without bound)
    test_tex_cache = tools.get('test_tex_cache')
    if not test_tex_cache or not os.path.isfile(test_tex_cache):
        steps.append(Step('test_tex_cache', None, 0,
                          skip_reason='bin/test_tex_cache.exe missing '
                                      '(run: make test_tex_cache)'))
    else:
        steps.append(Step('test_tex_cache', [test_tex_cache], 120,
                          note='cap evicts LRU refs==0 entries only; '
                               'referenced images survive'))

    # 4l. engine timers (TimerSet/TimerGet/TimerSuspend)
    test_timer = tools.get('test_timer')
    if not test_timer or not os.path.isfile(test_timer):
        steps.append(Step('test_timer', None, 0,
                          skip_reason='bin/test_timer.exe missing '
                                      '(run: make test_timer)'))
    else:
        steps.append(Step('test_timer', [test_timer], 120,
                          note='16 timers: set/elapsed/percent/clamp/overflow'))

    # 4m. typewriter (逐字显示，默认关闭 -> 像素基线不变)
    test_typewriter = tools.get('test_typewriter')
    if not test_typewriter or not os.path.isfile(test_typewriter):
        steps.append(Step('test_typewriter', None, 0,
                          skip_reason='bin/test_typewriter.exe missing '
                                      '(run: make test_typewriter)'))
    else:
        steps.append(Step('test_typewriter', [test_typewriter], 120,
                          note='off = fully shown; on = per-char by TextSpeed; '
                               'UTF-8 bounds, speed 0, multi-window'))

    # 4n. 台词历史（HistorySet 26 / HistoryGet 25 的环形缓冲）
    test_history = tools.get('test_history')
    if not test_history or not os.path.isfile(test_history):
        steps.append(Step('test_history', None, 0,
                          skip_reason='bin/test_history.exe missing '
                                      '(run: make test_history)'))
    else:
        steps.append(Step('test_history', [test_history], 120,
                          note='100-slot ring: count cap, pos wrap, slot = pos-idx, '
                               'kind/type filters, NULL safety'))

    # 4o. 命中测试（PrimHit 76 → gfx_prim_hit）
    test_prim_hit = tools.get('test_prim_hit')
    if not test_prim_hit or not os.path.isfile(test_prim_hit):
        steps.append(Step('test_prim_hit', None, 0,
                          skip_reason='bin/test_prim_hit.exe missing '
                                      '(run: make test_prim_hit)'))
    else:
        steps.append(Step('test_prim_hit', [test_prim_hit], 120,
                          note='parent-chain offsets, draw gating, min(prim,image) WH, '
                               'UV clip, open right/bottom edges, group recursion'))

    # 4p. 跨图集字形兜底（缺字时缩放取别的图集，不再画豆腐块）
    test_glyph_fallback = tools.get('test_glyph_fallback')
    if not test_glyph_fallback or not os.path.isfile(test_glyph_fallback):
        steps.append(Step('test_glyph_fallback', None, 0,
                          skip_reason='bin/test_glyph_fallback.exe missing '
                                      '(run: make test_glyph_fallback)'))
    else:
        steps.append(Step('test_glyph_fallback', [test_glyph_fallback], 120,
                          note='hit / scaled fallback / tofu counters, '
                               'per-render reset'))

    # 5. VM edges
    vm_edge = os.path.join(HERE, 'test_vm_edge.py')
    guard = hb.find_cc()
    steps.append(Step('test_vm_edge.py', [PY, vm_edge], 900,
                      note='43 boundary cases, fvm.exe + fvm_guard.exe'))

    # 6. HCB fuzz
    fuzz_hcb = os.path.join(HERE, 'fuzz_hcb.py')
    hcb_argv = [PY, fuzz_hcb]
    if args.fast:
        hcb_argv.append('--fast')
    steps.append(Step('fuzz_hcb.py%s' % (' --fast' if args.fast else ''),
                      hcb_argv, 3600,
                      skip_reason=None if guard else
                      'gcc unavailable: in-process harness cannot be built',
                      note='>= 20,000 mutations + fvm.exe E2E'))

    # 7. asset fuzz
    fuzz_assets = os.path.join(HERE, 'fuzz_assets.py')
    fa_argv = [PY, fuzz_assets]
    if args.fast:
        fa_argv.append('--fast')
    steps.append(Step('fuzz_assets.py%s' % (' --fast' if args.fast else ''),
                      fa_argv, 3600,
                      skip_reason=None,
                      note='BIN/HZC1/inflate/WAV/Ogg malformed inputs'))
    if args.keep:
        for s in steps:
            if s.argv and s.name.startswith('fuzz_'):
                s.argv.append('--keep')
    return steps


def run_step(step):
    if step.argv is None:
        return {'name': step.name, 'status': 'SKIP',
                'reason': step.skip_reason or '', 'elapsed': 0.0, 'note': ''}
    t0 = time.perf_counter()
    try:
        proc = subprocess.run(step.argv, stdout=subprocess.PIPE,
                              stderr=subprocess.STDOUT, timeout=step.timeout)
        out = proc.stdout.decode('utf-8', 'replace')
        rc = proc.returncode
        elapsed = time.perf_counter() - t0
        status = 'PASS' if rc == 0 else 'FAIL'
        # A script that exits 0 but printed RESULT: FAIL is still a failure.
        if status == 'PASS' and 'RESULT: FAIL' in out:
            status = 'FAIL'
        note = note_from(out, ('mutant executions:', 'cases=', 'peak=',
                               'byte-identical', 'OVERALL:', 'RESULT:')) or \
            step.note
        return {'name': step.name, 'status': status, 'elapsed': elapsed,
                'note': note, 'tail': '' if status == 'PASS' else tail(out),
                'exit': rc}
    except subprocess.TimeoutExpired:
        return {'name': step.name, 'status': 'TIMEOUT', 'elapsed': step.timeout,
                'note': 'exceeded %d s' % step.timeout, 'tail': '', 'exit': None}
    except OSError as exc:
        return {'name': step.name, 'status': 'ERROR', 'elapsed': 0.0,
                'note': str(exc), 'tail': '', 'exit': None}


def run_selfcheck():
    """Run the allocator self-test binaries directly (fast, no harness build)."""
    t0 = time.perf_counter()
    cc = hb.find_cc()
    if cc is None:
        return {'name': 'allocator-selfcheck', 'status': 'SKIP',
                'reason': 'gcc unavailable', 'elapsed': 0.0, 'note': ''}
    tools = hb.build_all(cc)
    selftest = hb.probe_selftest(tools)
    elapsed = time.perf_counter() - t0
    status = 'PASS' if selftest.get('ok') else 'FAIL'
    note = 'guard exit=%s canary exit=%s' % (
        selftest.get('guard', {}).get('exit'),
        selftest.get('canary', {}).get('exit'))
    return {'name': 'allocator-selfcheck', 'status': status,
            'elapsed': elapsed, 'note': note,
            'tail': '' if status == 'PASS' else repr(selftest), 'exit': 0}


def main(argv=None):
    ap = argparse.ArgumentParser()
    ap.add_argument('--fast', action='store_true',
                    help='quick pass (smaller fuzz counts)')
    ap.add_argument('--only', default=None,
                    help='comma-separated step name substrings to run')
    ap.add_argument('--exclude', default=None,
                    help='comma-separated step name substrings to skip')
    ap.add_argument('--list', action='store_true', help='list steps and exit')
    ap.add_argument('--keep', action='store_true',
                    help='keep intermediate mutant/audio files')
    ap.add_argument('--json', default=os.path.join(OUT, 'run_all.json'))
    args = ap.parse_args(argv)

    os.makedirs(TMP, exist_ok=True)
    steps = build_steps(args)

    def selected(step):
        if args.only:
            want = [s.strip() for s in args.only.split(',') if s.strip()]
            if not any(w.lower() in step.name.lower() for w in want):
                return False
        if args.exclude:
            drop = [s.strip() for s in args.exclude.split(',') if s.strip()]
            if any(w.lower() in step.name.lower() for w in drop):
                return False
        return True

    steps = [s for s in steps if selected(s)]
    if args.list:
        for s in steps:
            print('%-32s %s' % (s.name, s.skip_reason or 'ready'))
        return 0
    if not steps:
        log('run_all: no steps selected')
        return 2

    log('=' * 78)
    log('FVP robustness suite (%d steps%s)' %
        (len(steps), ', fast mode' if args.fast else ''))
    log('=' * 78)
    rows = []
    t_start = time.perf_counter()
    for idx, step in enumerate(steps, 1):
        log('')
        log('[%d/%d] %s' % (idx, len(steps), step.name))
        if step.name == 'allocator-selfcheck':
            row = run_selfcheck()
        else:
            row = run_step(step)
        if row['status'] == 'SKIP':
            log('  SKIP  %s' % row.get('reason', ''))
        elif row['status'] == 'PASS':
            log('  PASS  %.1f s  %s' % (row['elapsed'], row.get('note', '')))
        else:
            log('  %s  %.1f s' % (row['status'], row['elapsed']))
            if row.get('tail'):
                log(tail(row['tail'], 30))
        rows.append(row)

    elapsed = time.perf_counter() - t_start
    log('')
    log('=' * 78)
    log('SUMMARY')
    log('=' * 78)
    log('  %-32s %-8s %9s  %s' % ('step', 'status', 'time', 'note'))
    for row in rows:
        log('  %-32s %-8s %8.1fs  %s' %
            (row['name'], row['status'], row['elapsed'], row.get('note', '')))
    passed = sum(1 for r in rows if r['status'] == 'PASS')
    failed = sum(1 for r in rows if r['status'] in ('FAIL', 'TIMEOUT', 'ERROR'))
    skipped = sum(1 for r in rows if r['status'] == 'SKIP')
    log('')
    log('  %d passed, %d failed, %d skipped in %.1f s' %
        (passed, failed, skipped, elapsed))
    verdict = 'PASS' if failed == 0 else 'FAIL'
    log('  OVERALL: %s' % verdict)
    report = {'when': time.strftime('%Y-%m-%d %H:%M:%S'), 'fast': args.fast,
              'rows': [{k: v for k, v in r.items() if k != 'tail'}
                       for r in rows],
              'elapsed_s': elapsed, 'verdict': verdict}
    hb.write_json(args.json, report)
    log('  json -> %s' % args.json)
    return 0 if verdict == 'PASS' else 1


if __name__ == '__main__':
    sys.exit(main())