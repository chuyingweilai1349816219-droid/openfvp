#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""从 exe 的命令注册代码里抄出 **命令名 → (handler, argc)**（改进版：名字驱动）。

上一版按 `mov $handler,%ecx` 紧随 `mov %ecx,(%eax)` 的模式扫，漏了 15 条——因为
编译器在它们中间插了 `xor %edx,%edx` / `sub $0x10,%esp`。这版改成**名字驱动**：

  1. 找所有 `push $imm32`，其目标在 .rdata 里解出一个 ASCII 标识符（命令名）；
  2. 从这条 push 往回扫 ≤56 字节，取最近的 `mov $imm32,%ecx`（handler，须在 .text）；
  3. 同一窗口里的 `push imm8/imm32` 小常数当作 argc。

名字在注册顺序里是字典序，命令 id 由 HCB 侧的 ref/cmd_table.json 反查。
"""
import argparse
import io
import json
import os
import struct
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
EXE = os.path.join(r'C:\Users\Mechrevo\Downloads', '五彩斑斓的世界HD',
                   'iroseka_HD.exe')
CMD_TABLE = os.path.join(ROOT, 'ref', 'cmd_table.json')
OUT_JSON = os.path.join(ROOT, 'ref', 'exe-cmd-dispatch.json')

IMAGE_BASE = 0x400000
TEXT_LO, TEXT_HI = 0x401000, 0x492000


def sections(buf):
    pe = struct.unpack_from('<I', buf, 0x3C)[0]
    nsec = struct.unpack_from('<H', buf, pe + 6)[0]
    optsize = struct.unpack_from('<H', buf, pe + 20)[0]
    out = []
    for i in range(nsec):
        s = pe + 24 + optsize + i * 40
        name = buf[s:s + 8].rstrip(b'\x00').decode('latin1')
        vsize, vaddr, rawsize, rawptr = struct.unpack_from('<IIII', buf, s + 8)
        out.append((name, vaddr, vsize, rawptr, rawsize))
    return out


def main(argv=None):
    ap = argparse.ArgumentParser()
    ap.add_argument('--check', action='store_true')
    opt = ap.parse_args(argv)
    if not os.path.isfile(EXE):
        print('SKIP: %s 不存在' % EXE)
        return 0
    buf = io.open(EXE, 'rb').read()
    secs = sections(buf)

    def va_to_off(va):
        rva = va - IMAGE_BASE
        for name, vaddr, vsize, rawptr, rawsize in secs:
            if vaddr <= rva < vaddr + vsize:
                return rawptr + (rva - vaddr)
        return None

    def off_to_va(off):
        for name, vaddr, vsize, rawptr, rawsize in secs:
            if rawptr <= off < rawptr + rawsize:
                return vaddr + (off - rawptr) + IMAGE_BASE
        return None

    def cstr(va, limit=48):
        off = va_to_off(va)
        if off is None:
            return None
        end = buf.find(b'\x00', off, off + limit)
        if end < 0:
            return None
        try:
            s = buf[off:end].decode('ascii')
        except UnicodeDecodeError:
            return None
        if len(s) < 3 or not s[0].isalpha():
            return None
        return s

    # .text range (raw)
    text_raw = None
    for name, vaddr, vsize, rawptr, rawsize in secs:
        if name == '.text':
            text_raw = (rawptr, rawsize)
    lo, hi = text_raw

    entries = []
    i = lo
    while i + 5 < hi:
        if buf[i] == 0x68:                      # push imm32
            imm = struct.unpack_from('<I', buf, i + 1)[0]
            nm = cstr(imm)
            if nm is not None:
                handler = None
                argc = None
                for k in range(i - 1, max(lo, i - 56), -1):
                    if buf[k] == 0xB9:          # mov $imm32,%ecx
                        cand = struct.unpack_from('<I', buf, k + 1)[0]
                        if TEXT_LO <= cand < TEXT_HI:
                            handler = cand
                            break
                for k in range(i, min(hi, i + 30)):
                    if buf[k] == 0x6A and argc is None:
                        v = buf[k + 1]
                        if v < 0x40:
                            argc = v
                    elif buf[k] == 0x68 and argc is None:
                        v = struct.unpack_from('<I', buf, k + 1)[0]
                        if v < 0x40:
                            argc = v
                if handler is not None:
                    entries.append({
                        'name': nm,
                        'argc': argc,
                        'handler': '0x%06x' % handler,
                        'name_va': '0x%06x' % imm,
                        'push_pc': off_to_va(i),
                    })
                i += 5
                continue
        i += 1

    by_name = {}
    for e in entries:
        prev = by_name.get(e['name'])
        # 同名多条时保留第一次（注册表里同一命令只会注册一次）
        if prev is None:
            by_name[e['name']] = e

    cmd_table = json.load(io.open(CMD_TABLE, encoding='utf-8'))
    joined = []
    missing = []
    for c in cmd_table:
        e = by_name.get(c['name'])
        if e is None:
            missing.append({'id': c['id'], 'name': c['name']})
            continue
        joined.append({'id': c['id'], 'name': c['name'], 'argc': c['argc'],
                       'handler': e['handler'], 'name_va': e['name_va'],
                       'push_pc': '0x%06x' % e['push_pc']})

    result = {
        'note': '命令 id → handler 地址（A 类）：原版逐条注册命令'
                '（push 名字串 + 最近的 mov $handler,%ecx + push argc，'
                '然后 call 0x4387d0 register(entry, name)）；名字是字典序，'
                'id 由 HCB 侧 ref/cmd_table.json 反查。'
                '由 scripts/extract-exe-cmd-dispatch.py 生成',
        'registrar': '0x004387d0（register(entry, name)）',
        'method': 'name-driven: 每个 push $名字串 往回找最近的 mov $handler,%ecx',
        'registered_names': len(entries),
        'joined': len(joined),
        'missing': missing,
        'commands': joined,
    }
    text = json.dumps(result, ensure_ascii=False, indent=1,
                      sort_keys=True) + '\n'
    print('注册条目 %d，与 HCB 命令表连接上 %d / %d（缺 %d）'
          % (len(entries), len(joined), len(cmd_table), len(missing)))
    for want in (78, 81, 86, 89, 90, 110, 113, 141):
        hit = [j for j in joined if j['id'] == want]
        print('  cmd %-3d -> %s' % (want, (hit[0]['name'] + ' ' + hit[0]['handler'])
                                    if hit else 'NOT FOUND'))
    if missing:
        print('  缺:', ', '.join('%d:%s' % (m['id'], m['name'])
                                 for m in missing[:12]))
    if opt.check:
        if not os.path.isfile(OUT_JSON):
            print('FAIL: %s 不存在' % OUT_JSON)
            return 1
        if io.open(OUT_JSON, encoding='utf-8').read() != text:
            print('FAIL: %s 与提取结果不一致' % OUT_JSON)
            return 1
        print('OK: %s 与提取结果逐字节一致' % OUT_JSON)
        return 0
    io.open(OUT_JSON, 'w', encoding='utf-8', newline='\n').write(text)
    print('written %s' % OUT_JSON)
    return 0


if __name__ == '__main__':
    sys.exit(main())