#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""fuzz_hcb.py -- malformed-.hcb robustness suite for the FVP engine.

Two layers, both using the real loader/interpreter sources (never modified):

1. In-process mutation fuzz (fast, tens of thousands of cases)
   tests/fuzz_hcb.c is compiled twice against src/{fvp,encoding,hcb,vm,
   syscall}.c:

     fuzz_hcb.exe        heap-canary allocator, the bulk run (default 20 000)
     fuzz_hcb_guard.exe  page-guard allocator, a confirmation subset: every
                         heap block ends in a PAGE_NOACCESS page, so an
                         out-of-bounds read/write faults at the exact
                         instruction instead of corrupting memory silently

   Mutation classes: byte flips/writes, truncation, script_len forgery,
   main_start forgery (outside the file / mid-instruction), jump/call target
   patching (outside the file, into the middle of an instruction), pushstring
   length forgery (0/255/whatever), unknown opcodes, insert/delete (stream
   misalignment), zero runs, splices, appends and header garbage.  The corpus
   is one slice of the real iroseka.hcb (default 32 KiB, the largest slice the
   decoder accepts) plus hand-built minimal scripts that reach every opcode
   family and the syscall/global/array edges.

2. End-to-end through fvm.exe (real CLI, process isolation)
   The harness dumps a sample of mutants; fvm.exe is run on each with a small
   --steps budget and a wall-clock timeout.  This catches CLI-level issues that
   an in-process run cannot (exit codes, diagnostics, hangs).  A subset is
   re-run under fvm_guard.exe (page-guard build of the same sources) for
   out-of-bounds detection.

Every case is classified as rejected (loader refused with a diagnostic),
finished (all threads died), capped (step budget exhausted -- the engine's
own hang protection), crash, memory-fault or timeout.  A single crash, hang or
fault fails the run; the journal lets us regenerate the exact mutant
(--repro) and report a minimal reproduction.

Usage:
    python fvp/tests/fuzz_hcb.py [--fast] [--iters N] [--e2e N]
                                [--guard-iters N] [--steps N] [--seed S]
                                [--script FILE] [--no-build] [--keep]
                                [--json PATH]

Exit code 0 = PASS, 1 = failure, 2 = setup problem.
"""
import argparse
import concurrent.futures
import json
import os
import shutil
import subprocess
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
FVP = os.path.dirname(HERE)
OUT = os.path.join(FVP, 'out', 'fuzz')
MUTANTS = os.path.join(OUT, 'mutants')
TMP = os.path.join(OUT, 'tmp')
REPRO = os.path.join(OUT, 'repro')

sys.path.insert(0, HERE)
import harness_build as hb  # noqa: E402


def log(msg):
    print(msg)
    sys.stdout.flush()


def find_script(explicit=None):
    """Locate iroseka.hcb: explicit --script, then $FVP_SCRIPT, then the game
    directory discovered next to Downloads (same rule as fuzz_assets.py)."""
    if explicit:
        return explicit
    env = os.environ.get('FVP_SCRIPT')
    if env and os.path.isfile(env):
        return env
    env = os.environ.get('FVP_GAME')
    game = env if env else None
    if game is None:
        downloads = os.path.join(os.path.expanduser('~'), 'Downloads')
        try:
            for name in sorted(os.listdir(downloads)):
                path = os.path.join(downloads, name)
                if os.path.isfile(os.path.join(path, 'iroseka.hcb')):
                    game = path
                    break
        except OSError:
            game = None
    if game:
        cand = os.path.join(game, 'iroseka.hcb')
        if os.path.isfile(cand):
            return cand
    return None


def parse_kv(line):
    out = {}
    for token in line.split():
        if '=' in token:
            k, v = token.split('=', 1)
            if v.isdigit() or (v.startswith('-') and v[1:].isdigit()):
                out[k] = int(v)
            else:
                out[k] = v
    return out


def parse_harness_output(stdout):
    info = {'summary': {}, 'cats': [], 'bases': [], 'alloc': {},
            'result': None, 'base_lines': []}
    for line in stdout.splitlines():
        if line.startswith('SUMMARY '):
            info['summary'] = parse_kv(line)
        elif line.startswith('CAT '):
            kv = parse_kv(line)
            info['cats'].append(kv)
        elif line.startswith('BASEAGG '):
            info['bases'].append(parse_kv(line))
        elif line.startswith('BASE '):
            info['base_lines'].append(line)
        elif line.startswith('ALLOC '):
            info['alloc'] = parse_kv(line)
        elif line.startswith('RESULT '):
            info['result'] = line.split(None, 1)[1].strip()
        elif line.startswith('REPRO '):
            info['repro'] = line
    return info


def run_harness(exe, script, iters, steps, extra, timeout):
    cmd = [exe, script if script else '-', '--iters', str(iters),
           '--steps', str(steps), '--tmp', os.path.join(TMP, 'hcb_case.hcb')]
    cmd += list(extra)
    t0 = time.perf_counter()
    try:
        proc = subprocess.run(cmd, stdout=subprocess.PIPE,
                              stderr=subprocess.PIPE, timeout=timeout)
    except subprocess.TimeoutExpired:
        return None, 'harness timeout', time.perf_counter() - t0
    elapsed = time.perf_counter() - t0
    stdout = proc.stdout.decode('utf-8', 'replace')
    stderr = proc.stderr.decode('utf-8', 'replace')
    if proc.returncode != 0:
        return None, ('harness exit=%d\n%s' %
                      (proc.returncode, stderr[-3000:])), elapsed
    return parse_harness_output(stdout), stderr, elapsed


def classify_e2e(rc, stdout, stderr):
    if rc is None:
        return 'timeout'
    if rc < 0:
        return 'crash'
    if rc == 0:
        return 'accepted' if b'=== summary ===' in stdout else 'accepted-no-summary'
    if rc in (71, 72):
        return 'memfault'
    if rc == 3:
        return 'rejected' if stderr.strip().startswith(b'fvm:') else 'undiagnosed'
    if rc == 2:
        # Documented fail-closed path (fvp_fatal: string repeat/concat too
        # large, or out of memory): prints "fvm: <reason>" and exits 2.  It is
        # a safe, bounded outcome -- count it separately, not as a failure.
        return 'fatal' if stderr.strip().startswith(b'fvm:') else 'other'
    return 'other'


def e2e_one(exe, path, steps, timeout):
    cmd = [exe, path, '--steps', str(steps), '--quiet']
    t0 = time.perf_counter()
    try:
        proc = subprocess.run(cmd, stdout=subprocess.PIPE,
                              stderr=subprocess.PIPE, timeout=timeout)
    except subprocess.TimeoutExpired:
        return 'timeout', None, time.perf_counter() - t0, b'', b'timeout'
    elapsed = time.perf_counter() - t0
    status = classify_e2e(proc.returncode, proc.stdout, proc.stderr)
    return status, proc.returncode, elapsed, proc.stdout, proc.stderr


def read_dump_manifest(path):
    cases = []
    if not os.path.isfile(path):
        return cases
    with open(path, 'r', encoding='utf-8', errors='replace') as f:
        header = f.readline().strip().split(',')
        for line in f:
            parts = line.rstrip('\n').split(',')
            if len(parts) < len(header):
                continue
            row = dict(zip(header, parts))
            row['path'] = os.path.join(MUTANTS, row['file'])
            cases.append(row)
    return cases


def run_e2e(exe, cases, steps, jobs, timeout, label):
    stats = {'total': 0, 'accepted': 0, 'rejected': 0, 'crash': 0,
             'timeout': 0, 'memfault': 0, 'undiagnosed': 0, 'other': 0,
             'fatal': 0, 'accepted-no-summary': 0}
    by_cat = {}
    failures = []
    max_ms = 0.0

    def one(case):
        status, rc, elapsed, out, err = e2e_one(exe, case['path'], steps,
                                                timeout)
        return case, status, rc, elapsed, out, err

    with concurrent.futures.ThreadPoolExecutor(max_workers=jobs) as pool:
        for case, status, rc, elapsed, out, err in pool.map(one, cases):
            stats['total'] += 1
            stats[status] = stats.get(status, 0) + 1
            max_ms = max(max_ms, elapsed * 1000.0)
            cat = by_cat.setdefault(case.get('category', '?'),
                                    {'total': 0, 'accepted': 0,
                                     'rejected': 0, 'bad': 0})
            cat['total'] += 1
            if status == 'accepted':
                cat['accepted'] += 1
            elif status == 'rejected':
                cat['rejected'] += 1
            else:
                cat['bad'] += 1
                failures.append({'category': case.get('category'),
                                 'file': case['path'], 'status': status,
                                 'rc': rc,
                                 'detail': (err or out or b'')[:400]
                                 .decode('utf-8', 'replace')})
    stats['max_ms'] = max_ms
    stats['by_category'] = by_cat
    stats['failures'] = failures
    return stats


def print_harness_table(name, info, elapsed):
    s = info['summary']
    total = s.get('total', 0)
    rej = s.get('rejected', 0)
    log('')
    log('== %s ==' % name)
    log('  total=%d rejected=%d finished=%d capped=%d (%.1f%% rejected, %.1f s)'
        % (total, rej, s.get('finished', 0), s.get('capped', 0),
           100.0 * rej / total if total else 0.0, elapsed))
    log('  %-18s %7s %9s %9s %8s' %
        ('category', 'cases', 'rejected', 'finished', 'capped'))
    for cat in info['cats']:
        t = cat.get('total', 0)
        r = cat.get('rejected', 0)
        log('  %-18s %7d %9d %9d %8d   %5.1f%%' %
            (cat.get('name', '?'), t, r, cat.get('finished', 0),
             cat.get('capped', 0), 100.0 * r / t if t else 0.0))
    for base in info['bases']:
        log('  base %-16s cases=%d rejected=%d' %
            (base.get('name', '?'), base.get('total', 0),
             base.get('rejected', 0)))
    alloc = info['alloc']
    if alloc:
        log('  alloc mode=%s allocs=%s live=%s corrupt=%s oom=%s' %
            (alloc.get('mode'), alloc.get('allocs'), alloc.get('live'),
             alloc.get('corrupt'), alloc.get('oom')))
    return s, alloc


def main(argv=None):
    ap = argparse.ArgumentParser()
    ap.add_argument('--iters', type=int, default=20000,
                    help='in-process mutations (heap-canary build)')
    ap.add_argument('--guard-iters', type=int, default=3000,
                    help='in-process mutations under the page-guard allocator')
    ap.add_argument('--e2e', type=int, default=4000,
                    help='mutants also executed through fvm.exe')
    ap.add_argument('--guard-e2e', type=int, default=300,
                    help='E2E mutants re-run under fvm_guard.exe')
    ap.add_argument('--steps', type=int, default=2048,
                    help='VM instruction budget per case')
    ap.add_argument('--seed', type=int, default=20261123)
    ap.add_argument('--script', default=None)
    ap.add_argument('--slice', type=int, default=32768)
    ap.add_argument('--jobs', type=int, default=min(8, os.cpu_count() or 4))
    ap.add_argument('--fast', action='store_true',
                    help='quick sanity run (3k in-process, 600 E2E)')
    ap.add_argument('--no-build', action='store_true')
    ap.add_argument('--keep', action='store_true')
    ap.add_argument('--json', default=os.path.join(OUT, 'fuzz_hcb.json'))
    args = ap.parse_args(argv)

    if args.fast:
        args.iters = 3000
        args.guard_iters = 500
        args.e2e = 600
        args.guard_e2e = 100

    t_start = time.perf_counter()
    os.makedirs(TMP, exist_ok=True)
    shutil.rmtree(MUTANTS, ignore_errors=True)
    os.makedirs(MUTANTS, exist_ok=True)
    os.makedirs(REPRO, exist_ok=True)

    script = find_script(args.script)
    log('fuzz_hcb: FVP=%s' % FVP)
    log('  script: %s' % (script if script else '<none: synthetic corpus only>'))

    cc = None if args.no_build else hb.find_cc()
    tools = {}
    if cc is not None:
        tools = hb.build_all(cc, force=False)
    else:
        log('  note: gcc not found; in-process phase unavailable')

    failures = []
    report = {'when': time.strftime('%Y-%m-%d %H:%M:%S'),
              'script': script, 'seed': args.seed, 'steps': args.steps,
              'iters': args.iters, 'guard_iters': args.guard_iters,
              'e2e': args.e2e, 'phases': {}, 'failures': []}

    selftest = hb.probe_selftest(tools) if tools else {'ok': False}
    log('')
    log('== allocator self-test (does the OOB detector actually fire?) ==')
    if selftest.get('ok'):
        log('  guard: exit=%s (detected)  canary: exit=%s (detected)  -> PASS'
            % (selftest['guard']['exit'], selftest['canary']['exit']))
    else:
        log('  WARNING: allocator self-test did not fire: %r' % selftest)
        failures.append({'phase': 'selftest', 'status': 'not-detected',
                         'detail': repr(selftest)})
    report['phase0_allocator_selftest'] = selftest

    # ---- phase 1: bulk in-process run (heap canary) ----------------------
    phase1 = None
    if tools.get('fuzz_hcb'):
        dump_every = max(1, args.iters // max(1, args.e2e)) if args.e2e else 0
        extra = ['--seed', str(args.seed), '--slice', str(args.slice),
                 '--enc', 'cp932', '--tmp',
                 os.path.join(TMP, 'hcb_case.hcb'),
                 '--journal', os.path.join(OUT, 'fuzz_hcb.journal'),
                 '--csv', os.path.join(OUT, 'fuzz_hcb_cases.csv')]
        if dump_every:
            extra += ['--dump-dir', MUTANTS, '--dump-every', str(dump_every)]
        if script is None:
            extra += ['--synthetic-only']
        info, err, elapsed = run_harness(tools['fuzz_hcb'], script, args.iters,
                                         args.steps, extra,
                                         timeout=max(120.0, args.iters / 50.0))
        if info is None:
            log('  in-process harness failed: %s' % err)
            failures.append({'phase': 'in-process', 'status': 'harness-failed',
                             'detail': err})
        else:
            s, alloc = print_harness_table('in-process mutations (heap canary)',
                                           info, elapsed)
            report['phases']['in_process'] = {'summary': s,
                                              'categories': info['cats'],
                                              'bases': info['bases'],
                                              'alloc': alloc,
                                              'elapsed_s': elapsed}
            if info['result'] != 'PASS' or not s:
                failures.append({'phase': 'in-process', 'status': 'not-pass',
                                 'detail': repr(info['result'])})
            if alloc.get('corrupt'):
                failures.append({'phase': 'in-process',
                                 'status': 'heap-corruption',
                                 'detail': str(alloc)})
            if s.get('total', 0) < args.iters:
                failures.append({'phase': 'in-process',
                                 'status': 'incomplete',
                                 'detail': 'ran %d of %d' %
                                           (s.get('total', 0), args.iters)})
            phase1 = info
    else:
        log('  skipping in-process phase (no harness)')

    # ---- phase 2: in-process confirmation run (page guard) ---------------
    if tools.get('fuzz_hcb_guard'):
        extra = ['--seed', str(args.seed + 1), '--slice', str(args.slice),
                 '--enc', 'cp932', '--tmp',
                 os.path.join(TMP, 'hcb_case_guard.hcb'), '--quiet']
        if script is None:
            extra += ['--synthetic-only']
        guard_steps = min(args.steps, 512)
        info, err, elapsed = run_harness(tools['fuzz_hcb_guard'], script,
                                         args.guard_iters, guard_steps, extra,
                                         timeout=600.0)
        if info is None:
            log('  guard harness failed: %s' % err)
            failures.append({'phase': 'guard', 'status': 'harness-failed',
                             'detail': err})
        else:
            s, alloc = print_harness_table(
                'in-process mutations (page guard)', info, elapsed)
            report['phases']['guard'] = {'summary': s,
                                         'categories': info['cats'],
                                         'alloc': alloc,
                                         'elapsed_s': elapsed}
            if info['result'] != 'PASS' or alloc.get('corrupt'):
                failures.append({'phase': 'guard', 'status': 'not-pass',
                                 'detail': repr(info['result'])})

    # ---- phase 3: end-to-end through the real CLI ------------------------
    e2e_ok = True
    fvm = hb.public_tools()['fvm']
    dump_cases = read_dump_manifest(os.path.join(MUTANTS, 'cases.csv'))
    if args.e2e and dump_cases:
        log('')
        log('== end-to-end through fvm.exe (%d mutants) ==' % len(dump_cases))
        t0 = time.perf_counter()
        stats = run_e2e(fvm, dump_cases, args.steps, args.jobs, timeout=20.0,
                        label='fvm')
        elapsed = time.perf_counter() - t0
        log('  total=%d accepted=%d rejected=%d fatal=%d crash=%d timeout=%d '
            'memfault=%d undiagnosed=%d other=%d (%.1f s)' %
            (stats['total'], stats['accepted'], stats['rejected'],
             stats['fatal'], stats['crash'], stats['timeout'],
             stats['memfault'], stats['undiagnosed'], stats['other'], elapsed))
        report['phases']['e2e'] = stats
        if (stats['crash'] or stats['timeout'] or stats['memfault'] or
                stats['undiagnosed'] or stats['other'] or
                stats.get('accepted-no-summary')):
            e2e_ok = False
            failures.extend([dict(phase='e2e', **f) for f in stats['failures']])

    if args.guard_e2e and dump_cases and tools.get('fvm_guard'):
        sample = dump_cases[::max(1, len(dump_cases) // args.guard_e2e)]
        sample = sample[:args.guard_e2e]
        log('')
        log('== end-to-end through fvm_guard.exe (%d mutants, page guard) =='
            % len(sample))
        t0 = time.perf_counter()
        stats = run_e2e(tools['fvm_guard'], sample, args.steps, args.jobs,
                        timeout=20.0, label='fvm_guard')
        elapsed = time.perf_counter() - t0
        log('  total=%d accepted=%d rejected=%d fatal=%d crash=%d timeout=%d '
            'memfault=%d undiagnosed=%d other=%d (%.1f s)' %
            (stats['total'], stats['accepted'], stats['rejected'],
             stats['fatal'], stats['crash'], stats['timeout'],
             stats['memfault'], stats['undiagnosed'], stats['other'], elapsed))
        report['phases']['e2e_guard'] = stats
        if (stats['crash'] or stats['timeout'] or stats['memfault'] or
                stats['undiagnosed'] or stats['other'] or
                stats.get('accepted-no-summary')):
            e2e_ok = False
            failures.extend([dict(phase='e2e_guard', **f)
                             for f in stats['failures']])

    # ---- minimal repro for anything that failed -------------------------
    if failures and phase1 is not None and tools.get('fuzz_hcb'):
        report['repro'] = minimal_repro(tools['fuzz_hcb'], script, args,
                                        os.path.join(OUT, 'fuzz_hcb.journal'))

    elapsed = time.perf_counter() - t_start
    total_cases = 0
    for key in ('in_process', 'guard'):
        ph = report['phases'].get(key)
        if ph:
            total_cases += ph['summary'].get('total', 0)
    for key in ('e2e', 'e2e_guard'):
        ph = report['phases'].get(key)
        if ph:
            total_cases += ph['total']

    log('')
    log('== TOTAL ==')
    log('  mutant executions: %d  (in-process %d + guard %d + fvm.exe %d + '
        'fvm_guard.exe %d)'
        % (total_cases, args.iters, args.guard_iters,
           report['phases'].get('e2e', {}).get('total', 0),
           report['phases'].get('e2e_guard', {}).get('total', 0)))
    log('  elapsed=%.1f s' % elapsed)
    if failures:
        log('  failures: %d' % len(failures))
        for f in failures[:10]:
            log('    - %s' % json.dumps(f, ensure_ascii=False)[:300])
    verdict = 'PASS' if not failures else 'FAIL'
    log('  RESULT: %s' % verdict)
    report['totals'] = {'executions': total_cases, 'elapsed_s': elapsed,
                        'failures': len(failures)}
    report['verdict'] = verdict
    hb.write_json(args.json, report)
    log('  json -> %s' % args.json)

    if not args.keep:
        shutil.rmtree(MUTANTS, ignore_errors=True)
    return 0 if verdict == 'PASS' else 1


def minimal_repro(exe, script, args, journal):
    """Regenerate the last journal case and run it through fvm.exe."""
    idx = None
    try:
        with open(journal, 'r', encoding='utf-8', errors='replace') as f:
            for line in f:
                if line.startswith('FAULT'):
                    continue
                parts = line.rstrip('\n').split('\t')
                if parts and parts[0].isdigit():
                    idx = int(parts[0])
    except OSError:
        return None
    if idx is None:
        return None
    out = os.path.join(REPRO, 'case_%06d.hcb' % idx)
    cmd = [exe, script if script else '-', '--iters', str(args.iters),
           '--seed', str(args.seed), '--steps', str(args.steps),
           '--repro', str(idx), '--out', out,
           '--slice', str(args.slice), '--tmp',
           os.path.join(TMP, 'hcb_case.hcb')]
    if script is None:
        cmd.append('--synthetic-only')
    proc = subprocess.run(cmd, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                          timeout=120)
    if proc.returncode != 0:
        return {'case': idx, 'error': proc.stderr.decode('utf-8', 'replace')}
    fvm = hb.public_tools()['fvm']
    run = subprocess.run([fvm, out, '--steps', str(args.steps), '--quiet'],
                         stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                         timeout=60)
    result = {'case': idx, 'file': out, 'fvm_exit': run.returncode,
              'stdout': run.stdout.decode('utf-8', 'replace')[-800:],
              'stderr': run.stderr.decode('utf-8', 'replace')[-800:],
              'descriptor': proc.stdout.decode('utf-8', 'replace').strip()}
    log('')
    log('== minimal repro ==')
    log('  case %d -> %s' % (idx, out))
    log('  %s' % result['descriptor'])
    log('  fvm.exe exit=%s stderr=%r' % (run.returncode,
                                        result['stderr'][:300]))
    return result


if __name__ == '__main__':
    sys.exit(main())