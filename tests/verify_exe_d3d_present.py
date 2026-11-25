#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""D3D9 设备创建参数 / D3DPRESENT_PARAMETERS 的门禁（A 类事实）。

上一轮（`tests/verify_exe_d3d_states.py`）抄出了混合状态：`SRCALPHA`/`INVSRCALPHA`、
`ALPHABLENDENABLE=TRUE`、`BLENDOP` 从未设置。剩下的问题是**后台缓冲有没有 alpha
通道** —— 这决定"加色混合时目标 alpha 怎么算"会不会影响画面。

答案在 `IDirect3D9::CreateDevice` 的 `D3DPRESENT_PARAMETERS` 里，本门禁断言
`ref/exe-d3d-present.json`（由 `scripts/extract-exe-d3d-present.py` 从静态分析抄出）：

  * 结构体大小 **56 字节**（= sizeof(D3DPRESENT_PARAMETERS)，两个结构体都 memset 0x38）；
  * **窗口模式**参数（`this+0x14c`）：`BackBufferFormat = D3DFMT_UNKNOWN`、
    `Windowed = TRUE`、`SwapEffect = COPY`、`BackBufferCount = 1`、
    多重采样关、自动深度模板关、`PresentationInterval = DEFAULT`；
  * **全屏模式**参数（`this+0x1bc`）：`BackBufferFormat = D3DFMT_A8R8G8B8`、
    `Windowed = FALSE`，宽高来自设置对象成员 `+0x60`/`+0x64`；
  * `CreateDevice`（vtable 0x40）实参：`Adapter = 0`（DEFAULT）、`DeviceType = HAL`、
    `BehaviorFlags = 0x24 = MULTITHREADED | SOFTWARE_VERTEXPROCESSING`；
  * 结论：窗口模式跟桌面格式（无 alpha 语义），全屏的 A8R8G8B8 也不参与 Present 显示
    ⇒ 混合公式里目标 alpha 不影响可见结果（我们渲染器 `d[3]=255` 无害）。

缺原版程序清单时 SKIP 并返回 0。

用法：python tests/verify_exe_d3d_present.py
"""
import io
import json
import os
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
FVP = os.path.dirname(HERE)
PY = sys.executable
EXTRACT = os.path.join(FVP, 'scripts', 'extract-exe-d3d-present.py')
REF = os.path.join(FVP, 'ref', 'exe-d3d-present.json')
ASM = os.path.join(FVP, 'work', 'iroseka_HD.asm')

fails = []


def check(cond, msg):
    print(('  ok   ' if cond else '  FAIL ') + msg)
    if not cond:
        fails.append(msg)
    return cond


def field(pp, key, name):
    return ((pp.get(key) or {}).get('fields') or {}).get(name) or {}


def value(pp, key, name):
    return field(pp, key, name).get('value')


def main():
    if not os.path.isfile(ASM):
        print('SKIP: 缺原版程序清单 %s（work/ 不入库）' % ASM)
        return 0
    if not os.path.isfile(REF):
        print('FAIL: 缺基线 %s' % REF)
        return 1

    print('== 1. 重新提取并与基线逐字节比对 ==')
    proc = subprocess.run([PY, '-X', 'utf8', EXTRACT, '--check'],
                          stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                          timeout=600)
    print(proc.stdout.decode('utf-8', 'replace').rstrip())
    check(proc.returncode == 0,
          'extract-exe-d3d-present.py --check 退出码 0（实际 %d）'
          % proc.returncode)

    with io.open(REF, encoding='utf-8') as fp:
        ref = json.load(fp)
    pp = ref.get('present_params') or {}
    cd = ref.get('create_device') or {}

    print('== 2. 结构体大小 = sizeof(D3DPRESENT_PARAMETERS) ==')
    check(pp.get('sizeof_struct') == 56,
          'sizeof_struct == 56（实际 %r）' % pp.get('sizeof_struct'))
    for key in ('windowed', 'fullscreen'):
        check((pp.get(key) or {}).get('memset_size') == 56,
              '%s 的 memset 大小 == 0x38（实际 %r）'
              % (key, (pp.get(key) or {}).get('memset_size')))
    check((pp.get('windowed') or {}).get('struct_member') == '0x14c'
          and (pp.get('fullscreen') or {}).get('struct_member') == '0x1bc',
          '两套参数分别落在 this+0x14c / this+0x1bc（实际 %r / %r）'
          % ((pp.get('windowed') or {}).get('struct_member'),
             (pp.get('fullscreen') or {}).get('struct_member')))

    print('== 3. 窗口模式参数（this+0x14c）==')
    check(value(pp, 'windowed', 'BackBufferFormat') == 0
          and field(pp, 'windowed', 'BackBufferFormat').get('enum') == 'UNKNOWN',
          'BackBufferFormat = D3DFMT_UNKNOWN（实际 %r / %r）'
          % (value(pp, 'windowed', 'BackBufferFormat'),
             field(pp, 'windowed', 'BackBufferFormat').get('enum')))
    check(value(pp, 'windowed', 'Windowed') == 1
          and field(pp, 'windowed', 'Windowed').get('enum') == 'TRUE',
          'Windowed = TRUE')
    check(value(pp, 'windowed', 'SwapEffect') == 3
          and field(pp, 'windowed', 'SwapEffect').get('enum') == 'COPY',
          'SwapEffect = D3DSWAPEFFECT_COPY(3)')
    check(value(pp, 'windowed', 'BackBufferCount') == 1,
          'BackBufferCount = 1（实际 %r）'
          % (value(pp, 'windowed', 'BackBufferCount'),))
    check(value(pp, 'windowed', 'MultiSampleType') == 0
          and value(pp, 'windowed', 'MultiSampleQuality') == 0,
          '多重采样关闭')
    check(value(pp, 'windowed', 'EnableAutoDepthStencil') == 0,
          'EnableAutoDepthStencil = FALSE')
    check(value(pp, 'windowed', 'Flags') == 0,
          'Flags = 0（没有 D3DPRESENTFLAG_*）')
    check(value(pp, 'windowed', 'PresentationInterval') == 0
          and field(pp, 'windowed', 'PresentationInterval').get('enum') == 'DEFAULT',
          'PresentationInterval = DEFAULT（没有显式 IMMEDIATE/ONE）')
    check(value(pp, 'windowed', 'hDeviceWindow') == 0
          and value(pp, 'windowed', 'FullScreen_RefreshRateInHz') == 0,
          'hDeviceWindow / 刷新率都是 0（窗口模式由 D3D 填）')

    print('== 4. 全屏模式参数（this+0x1bc）==')
    check(value(pp, 'fullscreen', 'BackBufferFormat') == 21
          and field(pp, 'fullscreen', 'BackBufferFormat').get('enum') == 'A8R8G8B8',
          'BackBufferFormat = D3DFMT_A8R8G8B8（实际 %r / %r）'
          % (value(pp, 'fullscreen', 'BackBufferFormat'),
             field(pp, 'fullscreen', 'BackBufferFormat').get('enum')))
    check(value(pp, 'fullscreen', 'Windowed') == 0
          and field(pp, 'fullscreen', 'Windowed').get('enum') == 'FALSE',
          'Windowed = FALSE')
    check(value(pp, 'fullscreen', 'SwapEffect') == 3
          and value(pp, 'fullscreen', 'BackBufferCount') == 1,
          '全屏也是 SwapEffect=COPY、BackBufferCount=1')
    check(value(pp, 'fullscreen', 'BackBufferWidth') == 'member:+0x60'
          and value(pp, 'fullscreen', 'BackBufferHeight') == 'member:+0x64',
          '全屏宽高来自设置对象成员 +0x60/+0x64（实际 %r / %r）'
          % (value(pp, 'fullscreen', 'BackBufferWidth'),
             value(pp, 'fullscreen', 'BackBufferHeight')))

    print('== 5. CreateDevice 实参（vtable 0x40）==')
    check(cd.get('vtable_off') == '0x40', 'vtable 偏移 0x40（实际 %r）'
          % cd.get('vtable_off'))
    check(cd.get('arg_count') == 6, '6 个实参（实际 %r）' % cd.get('arg_count'))
    check(cd.get('adapter') == 0, 'Adapter = D3DADAPTER_DEFAULT(0)')
    check(cd.get('device_type') == 1 and cd.get('device_type_name') == 'HAL',
          'DeviceType = D3DDEVTYPE_HAL(1)（实际 %r/%r）'
          % (cd.get('device_type'), cd.get('device_type_name')))
    check(cd.get('behavior_flags') == 0x24,
          'BehaviorFlags = 0x24（实际 %r）' % cd.get('behavior_flags'))
    check(cd.get('behavior_flags_names')
          == ['D3DCREATE_MULTITHREADED', 'D3DCREATE_SOFTWARE_VERTEXPROCESSING'],
          'BehaviorFlags 解出 MULTITHREADED | SOFTWARE_VERTEXPROCESSING（实际 %s）'
          % cd.get('behavior_flags_names'))
    check(cd.get('pp_device_member') == 'this+0x8',
          '设备指针写回 this+0x8')
    leas = cd.get('present_params_lea_candidates') or []
    check('0x14c' in leas and '0x1bc' in leas,
          'CreateDevice 的 pPresentationParameters 可能指向 0x14c 或 0x1bc（实际 %s）'
          % leas)

    print('== 6. Direct3DCreate9 入口 ==')
    d3d = ref.get('direct3dcreate9') or {}
    check(d3d.get('call_pc') == '0x42c9f4',
          '调用点在 0x42c9f4（实际 %r）' % d3d.get('call_pc'))
    check(d3d.get('result_member') == 'this+0x4',
          'IDirect3D9* 存进 this+0x4（实际 %r）' % d3d.get('result_member'))

    print('== 7. 后台缓冲 alpha 的结论 ==')
    ba = ref.get('backbuffer_alpha') or {}
    check(ba.get('affects_visible_output') is False,
          'affects_visible_output = false')
    check('UNKNOWN' in (ba.get('windowed_format') or ''),
          '窗口模式格式说明里点明 D3DFMT_UNKNOWN（实际 %r）'
          % ba.get('windowed_format'))
    check('A8R8G8B8' in (ba.get('fullscreen_format') or ''),
          '全屏格式是 A8R8G8B8（实际 %r）' % ba.get('fullscreen_format'))
    check('d[3]' in (ba.get('conclusion') or ''),
          '结论里写清"目标 alpha 不影响可见结果"（实际 %r）'
          % ba.get('conclusion'))

    if fails:
        print('\nFAIL: %d 项不满足' % len(fails))
        return 1
    print('\nOK: 原版窗口模式 = D3DFMT_UNKNOWN + COPY + Windowed，全屏 = A8R8G8B8；'
          'HAL + 软件顶点处理；后台缓冲 alpha 不参与显示')
    return 0


if __name__ == '__main__':
    sys.exit(main())