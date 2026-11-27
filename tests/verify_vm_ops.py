#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""VM 指令表 + 变体比较矩阵门禁（ref/exe-vm-ops.json）。

这条门禁钉的是 ① 那个循环 `0x59264..0x5928C` 背后的**语义**：

  * 栈槽 = 8 字节：+0 类型字节、+4 载荷；**类型字节本身就是布尔值**
    （0 = false，同时是 null；1 = true），所以 `pushfalse`(0x08) 只写 0、
    `pushtrue`(0x09) 只写 1 —— 以前 `ref/fvm_ref.py` 和 `src/vm.c` 把这两个
    操作码的名字/语义写反了，本门禁要求它们与 exe 一致；
  * `jmpcond` 只看类型字节是否为 0（`int 0` 也算真）；
  * `test` 只在两个操作数都是 type 2 时做位测；
  * `eq` 通过 7×7 矩阵分派，矩阵里 type 1 参与的 9 个组合全部"恒真"，
    于是 `eq(DissolveWait 的返回类型, true)` 在状态非 0 时为真、状态为 0 时为假。

任何一条与 exe 字节对不上就算 FAIL（不 SKIP）。
"""
import argparse
import importlib.util
import json
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
FVP = os.path.dirname(HERE)
REF = os.path.join(FVP, 'ref', 'exe-vm-ops.json')
EXTRACTOR = os.path.join(FVP, 'scripts', 'extract-exe-vm-ops.py')


def load_module(name, path):
    spec = importlib.util.spec_from_file_location(name, path)
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--json', default=REF)
    a = ap.parse_args()
    fails = []

    def check(cond, msg):
        print(('  ok    ' if cond else '  FAIL  ') + msg)
        if not cond:
            fails.append(msg)

    if not os.path.isfile(a.json):
        print('verify_vm_ops: FAIL missing %s' % a.json)
        return 1
    with open(a.json, encoding='utf-8') as f:
        ref = json.load(f)

    print('== 指令表')
    check(ref.get('schema') == 'fvp-exe-vm-ops/1', 'schema fvp-exe-vm-ops/1')
    d = ref.get('dispatch', {})
    check(d.get('table_base') == '0x4815e0', '指令表基址 0x4815E0')
    check(d.get('site') == '0x4454c7', '唯一派发点 0x4454C7')
    check(d.get('bytes') == '8b0495e0154800', '派发指令字节（mov 0x4815e0(,%edx,4),%eax）')
    ops = ref.get('opcodes', [])
    check(len(ops) == 40, '40 条指令（0x00..0x27，实际 %d）' % len(ops))
    check([o['op'] for o in ops] == list(range(40)), '指令号连续')
    by_op = {o['op']: o['handler'] for o in ops}
    check(by_op[0x08] == '0x444940', 'op 0x08 -> 0x444940')
    check(by_op[0x09] == '0x444960', 'op 0x09 -> 0x444960')
    check(by_op[0x22] == '0x444f10', 'op 0x22 (eq) -> 0x444F10')
    check(by_op[0x07] == '0x445a30', 'op 0x07 (jmpcond) -> 0x445A30')
    check(by_op[0x14] == '0x4449f0', 'op 0x14 (pushtemp) -> 0x4449F0')

    print('== 类型字节 = 布尔值')
    pc = ref.get('push_constants', {})
    check(pc.get('0x08', {}).get('type_byte') == 0 and
          pc.get('0x08', {}).get('name') == 'pushfalse',
          '0x08 = pushfalse，类型字节 0')
    check(pc.get('0x09', {}).get('type_byte') == 1 and
          pc.get('0x09', {}).get('name') == 'pushtrue',
          '0x09 = pushtrue，类型字节 1')
    tn = ref.get('type_names', {})
    check(tn.get('0', '').startswith('null') or 'false' in tn.get('0', ''),
          'type 0 = null / false')
    check(tn.get('1') == 'bool true', 'type 1 = bool true')
    check(tn.get('3') == 'float', 'type 3 = float（pushfloat 写 3）')
    heads = {h['op']: h for h in ref.get('handler_heads', [])}
    check(heads.get(0x08, {}).get('bytes', '').endswith('c644c10800c3'),
          '0x444940 整段只写类型字节 0')
    check(heads.get(0x09, {}).get('bytes', '').endswith('c644c10801c3'),
          '0x444960 整段只写类型字节 1')

    print('== 比较矩阵')
    mats = {m['name']: m for m in ref.get('matrices', [])}
    check(set(mats) == {'le', 'gt', 'eq/neq'},
          '三张矩阵（实际 %s）' % ','.join(sorted(mats)))
    for name in ('le', 'gt', 'eq/neq'):
        m = mats.get(name, {})
        check('7*type' in m.get('index_rule', ''), '%s 索引规则 7*typeA+typeB' % name)
        check(m.get('default') == '0x444f50', '%s 默认恒返回 0' % name)
        check(len(m.get('entries', [])) == 49, '%s 49 项（实际 %d）'
              % (name, len(m.get('entries', []))))
        check(len(m.get('overrides', [])) == m.get('override_count'),
              '%s 覆盖计数自洽' % name)
    eq = mats.get('eq/neq', {})
    ov = {tuple(o['pair']): o['fn'] for o in eq.get('overrides', [])}
    check(ov.get((0, 0)) == '0x444f70', 'eq(null,null) 恒真')
    check(ov.get((2, 2)) == '0x444fa0', 'eq(int,int) = 载荷比较（sete）')
    check(ov.get((3, 3)) == '0x444fc0', 'eq(float,float) = fucompp')
    check((0, 1) not in ov and (1, 0) not in ov,
          'eq(false,true) / eq(true,false) 落默认 → 假')
    bool_pairs = [(1, 1), (1, 2), (1, 3), (1, 4), (1, 6),
                  (2, 1), (3, 1), (4, 1), (5, 1), (6, 1)]
    check(all(ov.get(p) == '0x444f70' for p in bool_pairs),
          '与 true 比较的 10 个组合全部恒真（原版行为）')
    check((1, 5) not in ov, 'eq(true, 动态字符串) 不在覆盖里 → 假（不对称，原版如此）')

    print('== 语义说明不撒谎')
    sem = ref.get('semantics', {})
    check('int 0' in sem.get('truthiness', ''), 'truthiness 指出 int 0 也被判真')
    check('(1 << b) & a' in sem.get('test', ''), 'test = 位测 (1<<b)&a')
    check('+0x814' in sem.get('pushtemp', '') and '清 0' in sem.get('pushtemp', ''),
          'pushtemp 会先清 +0x814')
    check('0x444F10' in sem.get('eq_matrix_read', '') or '0x444f10' in
          sem.get('eq_matrix_read', '').lower(), 'eq 读取点写在语义里')

    print('== 与解码器/实现一致')
    fm = load_module('fvm_ref_gate', os.path.join(FVP, 'ref', 'fvm_ref.py'))
    opdef = getattr(fm, 'OPDEF', {})
    check(opdef.get(0x08, ('',))[0] == 'pushfalse', 'ref/fvm_ref.py: 0x08 = pushfalse')
    check(opdef.get(0x09, ('',))[0] == 'pushtrue', 'ref/fvm_ref.py: 0x09 = pushtrue')
    vm_c = os.path.join(FVP, 'src', 'vm.c')
    if os.path.isfile(vm_c):
        txt = open(vm_c, encoding='utf-8', errors='replace').read()
        i8 = txt.find('case 0x08:')
        i9 = txt.find('case 0x09:')
        check(i8 > 0 and i9 > i8, 'src/vm.c 有 case 0x08 / 0x09')
        blk8 = txt[i8:i9]
        blk9 = txt[i9:i9 + 900]
        check('faithful_ops' in blk8 and 'fvp_make_null' in blk8,
              'src/vm.c: --faithful-ops 下 case 0x08 压 null（= false）')
        check('faithful_ops' in blk9 and 'fvp_value_bool' in blk9,
              'src/vm.c: --faithful-ops 下 case 0x09 压 bool true')
        i14 = txt.find('case 0x14:')
        check(i14 > 0 and 'faithful_ops' in txt[i14:i14 + 900] and
              'fvp_make_null' in txt[i14:i14 + 900],
              'src/vm.c: --faithful-ops 下 pushtemp 后清返回槽')
    for front in ('src/main.c', 'src/platform/portable/main_headless.c'):
        p = os.path.join(FVP, front)
        if not os.path.isfile(p):
            continue
        t = open(p, encoding='utf-8', errors='replace').read()
        check('--faithful-ops' in t and 'vm.faithful_ops' in t,
              '%s 接上了 --faithful-ops' % front)
    sys_c = os.path.join(FVP, 'src', 'syscall.c')
    if os.path.isfile(sys_c):
        t = open(sys_c, encoding='utf-8', errors='replace').read()
        check('faithful_ops' in t, 'src/syscall.c 读 faithful_ops')
        check(t.count('faithful_ops') >= 4,
              'syscall.c 至少 4 处分支（DissolveWait / *Test / 59 / 62）')

    print('== 文本模板的引号装饰（HCB 0x3A21D）')
    tc = ref.get('eq_table_c', {})
    if tc:
        one = {tuple(p) for p in tc.get('const_one_pairs', [])}
        pay = {tuple(p) for p in tc.get('payload_pairs', [])}
        check((0, 0) in one, 'eq 表 C：(0,0) 是"恒返回 1"的谓词（null == null）')
        check((0, 2) not in one and (0, 2) not in pay,
              'eq 表 C：(0,2)（null vs int 0）没覆盖 → 恒假')
        check((2, 2) in pay and (3, 3) in pay,
              'eq 表 C：(int,int)/(float,float) 走载荷比较')
        check(len(one) == 11 and len(pay) == 6,
              'eq 表 C：11 项恒真 + 6 项载荷比较（实际 %d / %d）'
              % (len(one), len(pay)))
    tpl = ref.get('text_template', {})
    if tpl:
        check(tpl.get('mode_flag_op') == 0x08,
              '模板的模式位来自 op 0x08（pushfalse）')
        check(tpl.get('mode_flag_type') == 0,
              '模式位的类型是 0（null/false）')
        check(tpl.get('cmp_right_type') == 2,
              '模板拿它和 int 0/1 比（右侧 type 2）')
        check(tpl.get('eq_matrix_result') is False,
              'exe 矩阵里 (type 0, type 2) 没被覆盖 → 比较恒假')
        check('」' in tpl.get('appended_when', ''),
              '记录"不相等时才不追加 \\u300d"的因果')

    print('== 两种语义的实测差异（bin/fvm.exe）')
    fvm = os.path.join(FVP, 'bin', 'fvm.exe')
    hcb = (r'C:\Users\Mechrevo\Downloads\五彩斑斓的世界HD\iroseka.hcb')
    if not (os.path.isfile(fvm) and os.path.isfile(hcb)):
        print('  skip  bin/fvm.exe 或 iroseka.hcb 不存在')
    else:
        import subprocess
        import tempfile
        outs = {}
        for tag, extra in (('legacy', []),
                           ('faithful', ['--faithful-ops']),
                           ('matrix', ['--cmp-matrix']),
                           ('matrix_faithful', ['--faithful-ops', '--cmp-matrix'])):
            fd, path = tempfile.mkstemp(suffix='.txt')
            os.close(fd)
            cmd = [fvm, hcb, '--steps', '900000', '--click', '600',
                   '--quiet', '--text-out', path] + extra
            subprocess.run(cmd, stdout=subprocess.DEVNULL,
                           stderr=subprocess.DEVNULL, timeout=600)
            outs[tag] = open(path, encoding='utf-8', errors='replace').read()
            os.unlink(path)
        ref_txt = open(os.path.join(FVP, 'ref', 'ref-900k.txt'),
                       encoding='utf-8', errors='replace').read()
        check(outs['legacy'] == ref_txt, '默认路径仍逐字节命中 ref/ref-900k.txt')
        check(outs['faithful'] != outs['legacy'], '--faithful-ops 的输出与默认不同')
        n_legacy = len(outs['legacy'].splitlines())
        n_faithful = len(outs['faithful'].splitlines())
        check(n_faithful >= 100,
              '--faithful-ops 走过等待环（%d 行文本，默认 %d 行）'
              % (n_faithful, n_legacy))
        # 因果验证：把 eq/neq 换成 exe 的类型对矩阵后，行尾那个多出来的「」」消失
        check('」' in outs['faithful'],
              '--faithful-ops 的每行结尾多一个「」」（宽松 eq 造成的装饰）')
        check('」' not in outs['matrix'],
              '--cmp-matrix 之后「」」消失（eq 走类型对矩阵：(type0,type2) 恒假）')
        check(len(outs['matrix'].splitlines()) >= 100,
              '--cmp-matrix 单独开**不卡**（%d 行）'
              % len(outs['matrix'].splitlines()))
        check('」' not in outs['matrix_faithful'],
              '--faithful-ops --cmp-matrix 也没有「」」')
        check('~' not in outs['matrix_faithful'],
              '--faithful-ops --cmp-matrix 没有那个多出来的 ASCII ~')
        check(len(outs['matrix_faithful'].splitlines()) >= 150,
              '--faithful-ops --cmp-matrix 文本流最接近基线（%d 行）'
              % len(outs['matrix_faithful'].splitlines()))

    print('== 重新抽取 exe 对账')
    mod = load_module('extract_exe_vm_ops', EXTRACTOR)
    if not os.path.isfile(mod.EXE):
        print('  skip  %s 不存在' % mod.EXE)
    else:
        got = mod.build()
        check(got['opcodes'] == ops, '指令表逐字段相等')
        check(got['matrices'] == ref.get('matrices'), '矩阵逐字段相等')
        check(got['handler_heads'] == ref.get('handler_heads'), 'handler 指纹相等')
        check(got['helpers'] == ref.get('helpers'), 'helper 指纹相等')
        check(got['dispatch'] == d, '派发点相等')

    if fails:
        print('verify_vm_ops: %d check(s) FAILED' % len(fails))
        return 1
    print('verify_vm_ops: PASS')
    return 0


if __name__ == '__main__':
    sys.exit(main())