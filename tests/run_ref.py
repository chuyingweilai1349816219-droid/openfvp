#!/usr/bin/env python3
"""Load the frozen Python reference VM (fvp/ref/fvm_ref.py) as a module.

fvm_ref.py calls argparse at import time only in main(); importing it also does
not touch stdlib-shadowing modules by itself.  We preload stdlib `dis` because
fvp/work (and other dirs) contain helper modules with stdlib names, in case the
caller has them on sys.path.
"""
import os
import sys
import importlib.util

import dis  # noqa: F401  (preload stdlib dis before adding helper dirs to path)

HERE = os.path.dirname(os.path.abspath(__file__))
REF_PY = os.path.abspath(os.path.join(HERE, '..', 'ref', 'fvm_ref.py'))
WORK_PY = os.path.abspath(os.path.join(HERE, '..', 'work', 'fvm.py'))


def load_fvm(path=None):
    """Import fvp/ref/fvm_ref.py (frozen reference) as a module and return it."""
    path = path or REF_PY
    if not os.path.exists(path):
        path = WORK_PY
    name = 'fvm_ref_mod'
    spec = importlib.util.spec_from_file_location(name, path)
    mod = importlib.util.module_from_spec(spec)
    sys.modules[name] = mod
    spec.loader.exec_module(mod)
    return mod


def load_cmds():
    import json
    p = os.path.join(os.path.dirname(REF_PY), 'cmd_argc.json')
    raw = json.load(open(p, encoding='utf-8'))
    return {int(k): int(v) for k, v in raw.items()}


def make_vm(path, steps=900000, click=600, enc='cp932', quiet=True):
    fvm = load_fvm()
    vm = fvm.VM(path, enc=enc, cmds=load_cmds(), quiet=quiet)
    vm.click_period = click
    vm.start()
    return fvm, vm