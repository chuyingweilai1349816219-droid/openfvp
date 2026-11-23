#!/usr/bin/env python3
"""fvpprobe —— 扫描一个 FVP 游戏目录，输出兼容性报告。

用途：拿到一部没见过的 FVP 游戏时，先跑这个，看它和已验证版本的差异在哪。

    python tools/fvpprobe.py <游戏目录> [--json out.json] [-v]

报告内容：
  * 资源包（BIN）：条目数、名字表长度、前几个名字
  * 脚本（HCB）：script_len、主脚本起点、指令数、用到的 opcode、内嵌命令表
  * 可执行文件：PE 架构、版本信息里的产品名/版本号
  * 与参考实现（いろとりどりのセカイ HD）的差异摘要
"""
import argparse
import collections
import json
import os
import struct
import sys

OPDEF = {
    0x00: ('nop', ''), 0x01: ('initstack', 'bb'), 0x02: ('call', 'd'),
    0x03: ('syscall', 'w'), 0x04: ('ret', ''), 0x05: ('ret2', ''),
    0x06: ('jmp', 'd'), 0x07: ('jmpcond', 'd'), 0x08: ('pushtrue', ''),
    0x09: ('pushfalse', ''), 0x0a: ('pushint32', 'd'), 0x0b: ('pushint16', 'w'),
    0x0c: ('pushint8', 'b'), 0x0d: ('pushfloat', 'd'), 0x0e: ('pushstring', 's'),
    0x0f: ('pushglobal', 'w'), 0x10: ('pushstack', 'b'), 0x11: ('garr_read', 'w'),
    0x12: ('larr_read', 'b'), 0x13: ('pushtop', ''), 0x14: ('pushtemp', ''),
    0x15: ('popglobal', 'w'), 0x16: ('copystack', 'b'), 0x17: ('garr_write', 'w'),
    0x18: ('larr_write', 'b'), 0x19: ('neg', ''), 0x1a: ('add', ''),
    0x1b: ('sub', ''), 0x1c: ('mul', ''), 0x1d: ('div', ''), 0x1e: ('mod', ''),
    0x1f: ('test', ''), 0x20: ('logand', ''), 0x21: ('logor', ''),
    0x22: ('eq', ''), 0x23: ('neq', ''), 0x24: ('gt', ''), 0x25: ('le', ''),
    0x26: ('lt', ''), 0x27: ('ge', ''),
}

REFERENCE = {
    'game': 'iroseka HD',
    'hcb_instr': 948219,
    'hcb_strings': 55461,
    'commands': 148,
}


def probe_bin(path):
    """读 BIN 包的索引（不读数据）。"""
    try:
        with open(path, 'rb') as f:
            head = f.read(8)
            if len(head) < 8:
                return None
            count, name_len = struct.unpack('<II', head)
            if count == 0 or count > 1000000:
                return None
            table = f.read(count * 12)
            if len(table) < count * 12:
                return None
            names = []
            base = 8 + count * 12
            for i in range(min(count, 3)):
                no, off, size = struct.unpack_from('<III', table, i * 12)
                f.seek(base + no)
                raw = b''
                while True:
                    c = f.read(1)
                    if c in (b'\x00', b''):
                        break
                    raw += c
                names.append(raw.decode('cp932', 'replace'))
            return {'count': count, 'name_table_bytes': name_len, 'sample': names}
    except Exception as e:
        return {'error': str(e)}


def probe_hcb(path):
    """解码脚本，统计结构。"""
    try:
        with open(path, 'rb') as f:
            data = f.read()
        if len(data) < 8:
            return {'error': 'too small'}
        script_len = struct.unpack_from('<I', data, 0)[0]
        if script_len + 4 > len(data):
            return {'error': 'script_len out of range', 'script_len': script_len}
        main_start = struct.unpack_from('<I', data, script_len)[0]

        pos = 4
        instr = 0
        strings = 0
        hist = collections.Counter()
        cmds = collections.Counter()
        desync = None
        while pos < script_len:
            at = pos
            op = data[pos]
            if op not in OPDEF:
                desync = {'offset': at, 'opcode': op}
                break
            pos += 1
            name, fmt = OPDEF[op]
            hist[name] += 1
            instr += 1
            for t in fmt:
                if t == 'b':
                    pos += 1
                elif t == 'w':
                    if op == 0x03:
                        cmds[struct.unpack_from('<H', data, pos)[0]] += 1
                    pos += 2
                elif t == 'd':
                    pos += 4
                elif t == 's':
                    n = data[pos]
                    pos += 1 + n
                    strings += 1

        # 内嵌命令表
        cmd_table = None
        try:
            extra = data[script_len + 4:]
            p = 7
            end = extra.index(b'\x00', p)
            title = extra[p:end].decode('cp932', 'replace')
            p = end + 1
            cnt = struct.unpack_from('<H', extra, p)[0]
            p += 2
            entries = []
            for _ in range(cnt):
                argc = extra[p]
                ln = extra[p + 1]
                entries.append({'argc': argc,
                                'name': extra[p + 2:p + ln].decode('ascii', 'replace')})
                p += 2 + ln
            cmd_table = {'title': title, 'count': cnt, 'entries': entries}
        except Exception as e:
            cmd_table = {'error': str(e)}

        return {
            'file': os.path.basename(path),
            'size': len(data),
            'script_len': script_len,
            'main_start': main_start,
            'instructions': instr,
            'strings': strings,
            'syscalls': sum(cmds.values()),
            'distinct_commands_used': len(cmds),
            'opcode_hist': dict(hist),
            'desync': desync,
            'command_table': cmd_table,
        }
    except Exception as e:
        return {'error': str(e)}


def probe_exe(path):
    try:
        with open(path, 'rb') as f:
            data = f.read(4096)
        if data[:2] != b'MZ':
            return None
        lfanew = struct.unpack_from('<I', data, 0x3C)[0]
        return {'file': os.path.basename(path), 'pe': True}
    except Exception:
        return None


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('game')
    ap.add_argument('--json')
    ap.add_argument('-v', '--verbose', action='store_true')
    a = ap.parse_args()

    root = a.game
    report = {'game_dir': root, 'archives': {}, 'scripts': {}, 'executables': []}

    for name in sorted(os.listdir(root)):
        p = os.path.join(root, name)
        if not os.path.isfile(p):
            continue
        low = name.lower()
        if low.endswith('.bin'):
            info = probe_bin(p)
            if info:
                report['archives'][name] = info
        elif low.endswith('.hcb'):
            report['scripts'][name] = probe_hcb(p)
        elif low.endswith('.exe'):
            info = probe_exe(p)
            if info:
                report['executables'].append(info)

    print('== 资源包 ==')
    for name, info in report['archives'].items():
        if 'error' in info:
            print('  %-18s 解析失败: %s' % (name, info['error']))
        else:
            print('  %-18s %6d 条  样本 %s' % (name, info['count'], ' / '.join(info['sample'])))

    print('\n== 脚本 ==')
    for name, info in report['scripts'].items():
        if 'error' in info:
            print('  %-18s 解析失败: %s' % (name, info['error']))
            continue
        ct = info.get('command_table') or {}
        print('  %-18s %d 字节  指令 %d  字符串 %d  syscall %d  命令种类 %d'
              % (name, info['size'], info['instructions'], info['strings'],
                 info['syscalls'], info['distinct_commands_used']))
        if ct.get('count'):
            print('     cmd table: %d 条  标题「%s」' % (ct['count'], ct.get('title')))
        if info.get('desync'):
            print('     !! 解析中断于 0x%x (opcode 0x%02x)' % (
                info['desync']['offset'], info['desync']['opcode']))

        # 与参考实现的差异
        if not info.get('desync'):
            print('     对照 %s：指令 %d vs %d，命令 %d vs %d'
                  % (REFERENCE['game'], info['instructions'], REFERENCE['hcb_instr'],
                     ct.get('count') or -1, REFERENCE['commands']))

    print('\n== 可执行文件 ==')
    for e in report['executables']:
        print('  ', e['file'])

    if a.verbose:
        for name, info in report['scripts'].items():
            if 'opcode_hist' in info:
                print('\n  %s opcode 分布:' % name)
                for k, v in sorted(info['opcode_hist'].items(), key=lambda kv: -kv[1]):
                    print('     %-12s %d' % (k, v))

    if a.json:
        with open(a.json, 'w', encoding='utf-8') as f:
            json.dump(report, f, ensure_ascii=False, indent=1)
        print('\n-> %s' % a.json)


if __name__ == '__main__':
    main()