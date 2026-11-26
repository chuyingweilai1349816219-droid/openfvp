#!/usr/bin/env python3
"""FVP script VM prototype.

Executes a .hcb script with a best-effort syscall layer, printing text output.
Unknown commands are logged and treated as no-ops returning 0.

Usage:
  python fvm.py <script.hcb> [--enc cp932|gbk] [--steps N] [--trace N] [--text-out FILE]
"""
import struct
import sys
import json
import os
import argparse

OPDEF = {
    0x00: ('nop', ''),
    0x01: ('initstack', 'bb'),
    0x02: ('call', 'd'),
    0x03: ('syscall', 'w'),
    0x04: ('ret', ''),
    0x05: ('ret2', ''),
    0x06: ('jmp', 'd'),
    0x07: ('jmpcond', 'd'),
    0x08: ('pushfalse', ''),
    0x09: ('pushtrue', ''),
    0x0a: ('pushint32', 'd'),
    0x0b: ('pushint16', 'w'),
    0x0c: ('pushint8', 'b'),
    0x0d: ('pushfloat', 'd'),
    0x0e: ('pushstring', 's'),
    0x0f: ('pushglobal', 'w'),
    0x10: ('pushstack', 'b'),
    0x11: ('garr_read', 'w'),
    0x12: ('larr_read', 'b'),
    0x13: ('pushtop', ''),
    0x14: ('pushtemp', ''),
    0x15: ('popglobal', 'w'),
    0x16: ('copystack', 'b'),
    0x17: ('garr_write', 'w'),
    0x18: ('larr_write', 'b'),
    0x19: ('neg', ''),
    0x1a: ('add', ''),
    0x1b: ('sub', ''),
    0x1c: ('mul', ''),
    0x1d: ('div', ''),
    0x1e: ('mod', ''),
    0x1f: ('test', ''),
    0x20: ('logand', ''),
    0x21: ('logor', ''),
    0x22: ('eq', ''),
    0x23: ('neq', ''),
    0x24: ('gt', ''),
    0x25: ('le', ''),
    0x26: ('lt', ''),
    0x27: ('ge', ''),
}


class Code(object):
    """Decoded instruction store keyed by byte offset."""

    def __init__(self, data, enc):
        self.data = data
        self.enc = enc
        self.script_len = struct.unpack_from('<I', data, 0)[0]
        self.main_start = struct.unpack_from('<I', data, self.script_len)[0]
        self.instr = {}
        self.size = {}
        self.func_starts = set()
        pos = 4
        while pos < self.script_len:
            at = pos
            op = data[pos]
            pos += 1
            name, fmt = OPDEF[op]
            args = []
            for t in fmt:
                if t == 'b':
                    args.append(data[pos]); pos += 1
                elif t == 'w':
                    args.append(struct.unpack_from('<H', data, pos)[0]); pos += 2
                elif t == 'd':
                    args.append(struct.unpack_from('<I', data, pos)[0]); pos += 4
                elif t == 's':
                    n = data[pos]; pos += 1
                    raw = data[pos:pos + n]; pos += n
                    args.append(raw[:-1].decode(enc, 'replace'))
            self.instr[at] = (op, name, args)
            self.size[at] = pos - at

    def at(self, pc):
        return self.instr.get(pc)


class Frame(object):
    __slots__ = ('ret', 'locals', 'args', 'pending')

    def __init__(self, ret, size=0, args=None, pending=False):
        self.ret = ret
        self.locals = [0] * max(size, 0)
        self.args = args or []
        self.pending = pending


class Thread(object):
    __slots__ = ('pc', 'stack', 'frames', 'alive', 'blocked', 'name', 'wait', 'temp')

    def __init__(self, pc, name='main'):
        self.pc = pc
        self.stack = []
        self.frames = []
        self.alive = True
        self.blocked = False
        self.name = name
        self.wait = 0
        self.temp = 0

    def top(self):
        return self.frames[-1] if self.frames else None


class VM(object):
    def __init__(self, path, enc='cp932', cmds=None, trace=0, quiet=False):
        with open(path, 'rb') as f:
            data = f.read()
        self.code = Code(data, enc)
        self.enc = enc
        self.globals = [0] * 8192
        self.garr = {}
        self.larr = {}
        self.threads = []
        self.cur = None
        self.steps = 0
        self.click_period = 0
        self.cx = 960
        self.cy = 540
        self.key_mask = 0
        self.trace = trace
        self.quiet = quiet
        self.text = []
        self.calls = {}
        self.cmd_hist = {}
        self.log = []
        self.cmds = cmds or {}
        self.names = {}
        tbl = os.path.join(os.path.dirname(os.path.abspath(__file__)), 'cmd_table.json')
        if os.path.exists(tbl):
            try:
                raw = json.load(open(tbl, encoding='utf-8'))
                self.names = {c['id']: c['name'] for c in raw}
            except Exception:
                pass
        self.returns = set()
        for cid, name in self.names.items():
            if (name.endswith('Test') or name.startswith('InputGet')
                    or name in ('Rand', 'IntToText', 'FlagGet', 'HistoryGet', 'TimerGet',
                                'TextTest', 'PrimHit', 'GraphLoad', 'InputFlash',
                                'SoundLoad', 'GraphRGB', 'SaveData', 'HistorySet')):
                self.returns.add(cid)
        self.unknown = {}
        self.result = 0

    # ---------- value helpers ----------
    def push(self, t, v):
        t.stack.append(v)

    def pop(self, t):
        if not t.stack:
            return 0
        return t.stack.pop()

    def truth(self, v):
        if isinstance(v, str):
            return len(v) > 0
        return bool(v)

    # ---------- entry ----------
    def start(self):
        t = Thread(self.code.main_start, 'main')
        self.threads.append(t)
        self.cur = t

    # ---------- execution ----------
    def run(self, max_steps=2000000):
        while self.threads and self.steps < max_steps:
            t = self.threads.pop(0)
            if not t.alive:
                continue
            self.threads.append(t)
            self.cur = t
            if t.blocked:
                t.wait += 1
                if t.wait < 2:
                    continue
                t.blocked = False
                t.wait = 0
            if not self.step(t):
                break
        return self.steps

    def step(self, t):
        ins = self.code.at(t.pc)
        if ins is None:
            t.alive = False
            return True
        op, name, args = ins
        self.steps += 1

        if self.trace and self.steps <= self.trace:
            print('  [%s 0x%06x] %-12s %s' % (t.name, t.pc, name, args))

        if op == 0x01:  # initstack locals, ?args
            f = t.frames[-1] if t.frames else None
            if f is not None and f.pending:
                f.locals = [0] * args[0]
                f.pending = False
            else:
                t.frames.append(Frame(t.pc, args[0]))
            t.pc += 3
        elif op == 0x02:  # call
            t.frames.append(Frame(t.pc + 5, 0, list(t.stack), True))
            t.stack.clear()
            t.pc = args[0]
            self.calls[args[0]] = self.calls.get(args[0], 0) + 1
        elif op == 0x03:  # syscall
            self.do_syscall(t, args[0])
            t.pc += 3
        elif op == 0x04 or op == 0x05:  # ret / ret2
            if t.frames:
                f = t.frames.pop()
                t.pc = f.ret
            else:
                t.alive = False
                # fall back to main script
                t.pc = self.code.script_len
        elif op == 0x06:  # jmp
            t.pc = args[0]
        elif op == 0x07:  # jmpcond: jump when false
            c = self.pop(t)
            if not self.truth(c):
                t.pc = args[0]
            else:
                t.pc += 5
        elif op == 0x08:  # pushfalse：handler 0x444940 只写类型字节 0（= false / null）
            self.push(t, False); t.pc += 1
        elif op == 0x09:  # pushtrue：handler 0x444960 只写类型字节 1
            self.push(t, True); t.pc += 1
        elif op == 0x0a:
            self.push(t, args[0]); t.pc += 5
        elif op == 0x0b:
            self.push(t, args[0]); t.pc += 3
        elif op == 0x0c:
            self.push(t, args[0]); t.pc += 2
        elif op == 0x0d:
            self.push(t, struct.unpack('<f', struct.pack('<I', args[0]))[0]); t.pc += 5
        elif op == 0x0e:
            self.push(t, args[0]); t.pc += self.code.size[t.pc]
        elif op == 0x0f:
            self.push(t, self.globals[args[0] & 0x1fff] if args[0] < len(self.globals) else 0); t.pc += 3
        elif op == 0x10:
            f = t.top()
            if f is None:
                self.push(t, 0)
            elif args[0] < len(f.locals):
                self.push(t, f.locals[args[0]])
            else:
                idx = args[0] - (255 - len(f.args))
                self.push(t, f.args[idx] if 0 <= idx < len(f.args) else 0)
            t.pc += 2
        elif op == 0x11:  # garr_read base
            i = self.pop(t)
            self.push(t, self.garr.get((args[0], i), 0)); t.pc += 3
        elif op == 0x12:  # larr_read id
            i = self.pop(t)
            self.push(t, self.larr.get((args[0], i), 0)); t.pc += 2
        elif op == 0x13:  # pushtop
            f = t.top()
            self.push(t, f.locals[0] if f and f.locals else 0); t.pc += 1
        elif op == 0x14:  # pushtemp: push last command return value
            self.push(t, t.temp); t.pc += 1
        elif op == 0x15:  # popglobal
            v = self.pop(t)
            if args[0] < len(self.globals):
                self.globals[args[0]] = v
            t.pc += 3
        elif op == 0x16:  # copystack
            v = self.pop(t)
            f = t.top()
            if f and args[0] < len(f.locals):
                f.locals[args[0]] = v
            t.pc += 2
        elif op == 0x17:  # garr_write base
            v = self.pop(t)
            i = self.pop(t)
            self.garr[(args[0], i)] = v; t.pc += 3
        elif op == 0x18:  # larr_write id
            v = self.pop(t)
            i = self.pop(t)
            self.larr[(args[0], i)] = v; t.pc += 2
        elif op == 0x19:  # neg
            self.push(t, -self.pop(t)); t.pc += 1
        elif 0x1a <= op <= 0x27:
            self.binop(t, op)
            t.pc += 1
        else:
            t.alive = False
        return True

    def binop(self, t, op):
        b = self.pop(t)
        a = self.pop(t)
        try:
            if op == 0x1a:
                r = a + b
            elif op == 0x1b:
                r = a - b
            elif op == 0x1c:
                r = a * b
            elif op == 0x1d:
                r = a / b if b else 0
            elif op == 0x1e:
                r = a % b if b else 0
            elif op == 0x1f:
                # 位测试（0x444e40）：(a & (1 << (b & 31))) != 0，非 int 一律 0
                if isinstance(a, bool) or isinstance(b, bool):
                    r = 0
                elif isinstance(a, int) and isinstance(b, int):
                    r = 1 if (a & (1 << (b & 31))) else 0
                else:
                    r = 0
            elif op == 0x20:
                r = 1 if (self.truth(a) and self.truth(b)) else 0
            elif op == 0x21:
                r = 1 if (self.truth(a) or self.truth(b)) else 0
            elif op == 0x22:
                r = 1 if a == b else 0
            elif op == 0x23:
                r = 1 if a != b else 0
            elif op == 0x24:
                r = 1 if a > b else 0
            elif op == 0x25:
                r = 1 if a <= b else 0
            elif op == 0x26:
                r = 1 if a < b else 0
            else:
                r = 1 if a >= b else 0
        except TypeError:
            r = 0
        self.push(t, r)

    # ---------- syscalls ----------
    def do_syscall(self, t, cid):
        self.cmd_hist[cid] = self.cmd_hist.get(cid, 0) + 1
        argc = self.cmds.get(cid, 0)
        args = []
        for _ in range(argc):
            args.insert(0, self.pop(t))
        h = getattr(self, 'cmd_%d' % cid, None)
        if h is not None:
            r = h(t, args)
        else:
            key = cid
            self.unknown[key] = self.unknown.get(key, 0) + 1
            # 未实现的 *Test 命令一律当作「动画已结束」，否则脚本会卡在等待循环里
            r = 1 if self.names.get(cid, '').endswith('Test') else None
        if r is not None:
            t.temp = r
        elif cid in self.returns:
            t.temp = 0

    # --- implemented subset ---
    def cmd_123(self, t, a):  # TextPrint(layer, text)
        txt = a[1] if len(a) > 1 else ''
        layer = a[0] if a else 0
        if not isinstance(txt, str):
            txt = str(txt)
        self.text.append((layer, txt))
        if not self.quiet:
            print('%s' % txt)
        return None

    def cmd_111(self, t, a):  # TextClear(layer)
        return None

    # --- input model: periodic synthetic click ---
    def cmd_31(self, t, a):  # InputGetDown
        if self.click_period and (self.steps % self.click_period) < 2:
            return 1
        return 0

    def cmd_36(self, t, a):  # InputGetUp
        return 0

    def cmd_29(self, t, a):  # InputGetCursX
        return self.cx

    def cmd_30(self, t, a):  # InputGetCursY
        return self.cy

    def cmd_34(self, t, a):  # InputGetState
        return self.key_mask

    def cmd_32(self, t, a):  # InputGetEvent
        return 0

    def cmd_28(self, t, a):  # InputGetCursIn
        return 1

    # --- animation / wait commands: report finished immediately ---
    def cmd_15(self, t, a):  # DissolveWait
        return 1

    def cmd_44(self, t, a):  # MotionAlphaTest
        return 1

    def cmd_47(self, t, a):  # MotionAnimTest
        return 1

    def cmd_50(self, t, a):  # MotionMoveTest
        return 1

    def cmd_53(self, t, a):  # MotionMoveRTest
        return 1

    def cmd_56(self, t, a):  # MotionMoveS2Test
        return 1

    def cmd_59(self, t, a):  # MotionMoveZTest
        return 1

    def cmd_145(self, t, a):  # V3DMotionTest (unused 3D path, always finished)
        return 1

    def cmd_144(self, t, a):  # V3DMotionStop
        return None

    def cmd_143(self, t, a):  # V3DMotionPause
        return None

    def cmd_146(self, t, a):  # V3DSet
        return None

    def cmd_131(self, t, a):  # TextTest
        return 0

    def cmd_61(self, t, a):  # Movie (WMV, 原型阶段直接跳过)
        return None

    def cmd_62(self, t, a):  # MovieState: 报告「已播放结束」，否则脚本会一直等待
        return 1

    def cmd_64(self, t, a):  # MovieStop
        return None

    def cmd_14(self, t, a):  # Dissolve
        return None

    def cmd_133(self, t, a):  # ThreadNext
        t.blocked = True
        return None

    def cmd_137(self, t, a):  # ThreadWait
        t.blocked = True
        return None

    def cmd_13(self, t, a):  # Debmess
        if not self.quiet:
            print('[debmess] %s' % (a,))
        return None

    def cmd_22(self, t, a):  # GraphLoad
        return 1


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('script')
    ap.add_argument('--enc', default='cp932')
    ap.add_argument('--steps', type=int, default=2000000)
    ap.add_argument('--trace', type=int, default=0)
    ap.add_argument('--cmds', default=os.path.join(os.path.dirname(__file__), 'cmd_argc.json'))
    ap.add_argument('--text-out')
    ap.add_argument('--quiet', action='store_true')
    ap.add_argument('--click', type=int, default=0, help='synthetic click every N steps')
    ap.add_argument('--cursor', default='960,540', help='synthetic cursor x,y')
    a = ap.parse_args()
    a.cursor = tuple(int(v) for v in a.cursor.split(','))

    cmds = {}
    if os.path.exists(a.cmds):
        raw = json.load(open(a.cmds, encoding='utf-8'))
        cmds = {int(k): int(v) for k, v in raw.items()}

    vm = VM(a.script, enc=a.enc, cmds=cmds, trace=a.trace, quiet=a.quiet)
    vm.click_period = a.click
    vm.cx, vm.cy = a.cursor
    vm.start()
    steps = vm.run(a.steps)

    print('\n=== summary ===')
    print('steps      %d' % steps)
    print('text lines %d' % len(vm.text))
    print('threads    %d' % len(vm.threads))
    top = sorted(vm.unknown.items(), key=lambda kv: -kv[1])[:15]
    print('unknown cmds: %s' % ', '.join('%d×%d' % (k, v) for k, v in top))
    if a.text_out:
        with open(a.text_out, 'w', encoding='utf-8') as f:
            for layer, txt in vm.text:
                f.write('[%d] %s\n' % (layer, txt))
        print('text -> %s' % a.text_out)


if __name__ == '__main__':
    main()