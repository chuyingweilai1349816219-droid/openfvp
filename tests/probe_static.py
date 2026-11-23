#!/usr/bin/env python3
"""Static + dynamic facts needed to size the C port.

Usage: python fvp/tests/probe_static.py <script.hcb> [steps]

Prints opcode histograms, dynamic use of the four unconfirmed opcodes, neg
operand types, max VM stack/frame/thread depths, integer magnitudes and the
argument types TextPrint receives.
"""
import sys
import os
import collections

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
from run_ref import make_vm  # noqa: E402


def typename(v):
    if isinstance(v, bool):
        return 'bool'
    if isinstance(v, int):
        return 'int'
    if isinstance(v, float):
        return 'float'
    if isinstance(v, str):
        return 'str'
    return type(v).__name__


class P(object):
    """Wrap the reference VM's step/binop/cmd_123; does not alter semantics."""

    def __init__(self, vm):
        self.vm = vm
        self.static_op = collections.Counter()
        for at, (op, name, args) in vm.code.instr.items():
            self.static_op[(hex(op), name)] += 1
        orig_step = vm.step
        orig_binop = vm.binop
        orig_syscall = vm.do_syscall
        orig_123 = vm.cmd_123

        def step(t):
            if len(t.stack) > self.max_stack:
                self.max_stack = len(t.stack)
            if len(t.frames) > self.max_frames:
                self.max_frames = len(t.frames)
            if len(vm.threads) > self.max_threads:
                self.max_threads = len(vm.threads)
            ins = vm.code.at(t.pc)
            if ins is not None and ins[0] in (0x11, 0x12, 0x17, 0x18):
                op = ins[0]
                if op in (0x11, 0x12):
                    idx = t.stack[-1] if t.stack else 0
                    self.dyn_rare[(hex(op), 'read', typename(idx))] += 1
                else:
                    idx = t.stack[-2] if len(t.stack) >= 2 else 0
                    val = t.stack[-1] if t.stack else 0
                    self.dyn_rare[(hex(op), 'write', typename(idx), typename(val))] += 1
            elif ins is not None and ins[0] == 0x19:
                v = t.stack[-1] if t.stack else 0
                self.neg_types[typename(v)] += 1
            return orig_step(t)

        def binop(t, op):
            a = t.stack[-2] if len(t.stack) >= 2 else 0
            b = t.stack[-1] if t.stack else 0
            self.optype[(hex(op), typename(a), typename(b))] += 1
            r = orig_binop(t, op)
            v = t.stack[-1] if t.stack else None
            if isinstance(v, int) and not isinstance(v, bool):
                if abs(v) > self.max_abs:
                    self.max_abs = abs(v)
            return r

        def do_syscall(t, cid):
            before = t.temp
            r = orig_syscall(t, cid)
            if typename(before) != typename(t.temp):
                self.temp_type_change[(cid, typename(before), typename(t.temp))] += 1
            return r

        def cmd_123(t, a):
            self.text_types[tuple(typename(x) for x in a)] += 1
            for x in a:
                if isinstance(x, str):
                    pass
            return orig_123(t, a)

        vm.step = step
        vm.binop = binop
        vm.do_syscall = do_syscall
        vm.cmd_123 = cmd_123
        self.max_stack = 0
        self.max_frames = 0
        self.max_threads = 0
        self.dyn_rare = collections.Counter()
        self.neg_types = collections.Counter()
        self.optype = collections.Counter()
        self.temp_type_change = collections.Counter()
        self.text_types = collections.Counter()
        self.max_abs = 0


def main():
    path = sys.argv[1]
    steps = int(sys.argv[2]) if len(sys.argv) > 2 else 900000
    fvm, vm = make_vm(path, steps=steps)
    p = P(vm)
    vm.run(steps)
    print('steps', vm.steps, 'text', len(vm.text), 'threads', len(vm.threads))
    print('\n-- static opcode histogram --')
    for k, v in sorted(p.static_op.items(), key=lambda kv: -kv[1]):
        print('%-22s %d' % (str(k), v))
    print('\n-- rare opcode dynamic use --')
    for k, v in sorted(p.dyn_rare.items()):
        print(k, v)
    print('\n-- neg operand types --')
    print(dict(p.neg_types))
    print('\nmax stack depth:', p.max_stack)
    print('max frame depth:', p.max_frames)
    print('max threads:', p.max_threads)
    print('max abs int:', p.max_abs)
    print('\ntext arg types:', p.text_types)
    print('\ntemp type changes:', p.temp_type_change)
    print('\n-- binop operand types --')
    for k, v in sorted(p.optype.items(), key=lambda kv: -kv[1]):
        print('%-30s %d' % (str(k), v))
    print('\n-- top syscalls --')
    print(sorted(vm.cmd_hist.items(), key=lambda kv: -kv[1])[:50])
    print('\n-- threads at end --')
    for t in vm.threads:
        print(hex(t.pc), t.name, len(t.frames), len(t.stack), 'blocked=', t.blocked)


if __name__ == '__main__':
    main()