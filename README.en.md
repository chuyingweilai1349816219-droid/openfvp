# openfvp

**A from-scratch C99 reimplementation of the runtime for FVP (FAVORITE VIEW POINT SYSTEM).**

FVP is the closed-source visual-novel engine behind FAVORITE's titles such as
*Irotoridori no Sekai HD* (いろとりどりのセカイ HD), *Hoshizora no Memoria* and
*Astraea no Shiroki Eien*. The goal is to make those games runnable on modern
platforms, with **Android as the primary target**.

> **Status: 0.1.0 prototype.** The script virtual machine is byte-identical to the
> frozen Python reference. The asset layer (BIN / inflate / HZC1) and the audio layer
> (Ogg / WAV / mixing) exist in C and have acceptance scripts. A C renderer is being
> ported from the Python prototype (**not accepted yet**), a Win32 desktop shell builds
> but has **no acceptance run** either, and real command semantics, save data, the
> thread/time model and the Android backend are **not** done.
> Everything claimed below maps to code or documentation in this repository.

This repository contains **no game data** — no `.bin`, `.hcb`, audio, images or
script text. You need your own legally purchased copy to run anything.

---

## What this is

| | |
|---|---|
| Engine | FVP (FAVORITE VIEW POINT SYSTEM), closed source; PE i386 + Direct3D 9 + DirectSound |
| This project | A from-scratch C99 runtime: `.hcb` script interpreter + asset layer + audio layer |
| Output | `bin/fvm.exe` (script VM that runs real scripts and dumps text; output names keep the `.exe` suffix) · `bin/libfvp.a` (engine core + asset layer + audio layer) · `bin/fvp.exe` (Win32 desktop runner, under development) |
| Reference point | [OpenBGI](https://openbgi.net) — another C99 reimplementation of a closed-source VN engine (BGI/Ethornell) |

What it is **not**:

- Not an emulator: the original `iroseka_HD.exe` is never loaded or executed.
- Not a Wine/compat wrapper: no dependency on the original PE, D3D9 or DirectSound.
- The original game files are never modified; everything is written under `out/`.

## Why

1. **The engine is closed and has no open runtime.** Community tooling for FVP
   (HCB dumper/rebuilder, BIN archivers, image converters) stops at "inspect and
   repack"; nothing executes the scripts.
2. **BGI has OpenBGI; FVP has no equivalent.** FVP's interface surface is small —
   **40 opcodes and 148 commands** (see [`docs/roadmap.md`](docs/roadmap.md)) —
   which makes a reimplementation tractable for a small team.
3. **Wine and virtual machines do not solve Android.** A portable engine with no
   Win32 dependency is required.
4. **Turn format analysis knowledge into verifiable code.** The formats are
   documented in [`docs/`](docs/README.md) and pinned by byte-exact comparison
   against a frozen Python reference.

---

## What works today

| Capability | State | Code / docs |
|---|---|---|
| `.hcb` loading and decoding | **948,219 instructions** and **55,461 strings** of the reference script are decoded; every jump target is valid | [`src/hcb.c`](src/hcb.c) · [`docs/c-vm.md`](docs/c-vm.md) |
| VM instruction set | all **40 opcodes** implemented (stack, frames, globals/locals, `temp`); thread scheduling is simplified — there is no time model yet | [`src/vm.c`](src/vm.c) · [`docs/05-threads.md`](docs/05-threads.md) |
| Command table | **148 commands** with id / name / argc / returns-value, read from the script's embedded metadata; **131** of them are used by this title | [`src/gen_cmd_table.h`](src/gen_cmd_table.h) · [`src/syscall.c`](src/syscall.c) · [`docs/vm-commands.md`](docs/vm-commands.md) |
| **Byte-identical to the Python reference** | 900k steps: 6,496 bytes / 195 lines (`SHA256 7F9837FF…`); 2M steps: 15,378 bytes / 450 lines (`SHA256 32D7D4E2…`); 20M steps and 123457 / 777777 / 1500000 steps also matched on the development machine | [`tests/compare.py`](tests/compare.py) · [`ref/REFERENCE.md`](ref/REFERENCE.md) |
| Asset layer: BIN / inflate / HZC1 | **21,725** package entries; **1,112/1,112** zlib streams (**2,539,861,820** bytes) match `zlib.decompress` byte for byte; **1,112 images / 3,654 frames / 727,573,943 pixels with 0 differences** (formats 0–4) | [`src/bin.c`](src/bin.c) · [`src/inflate.c`](src/inflate.c) · [`src/hzc1.c`](src/hzc1.c) · [`docs/c-assets.md`](docs/c-assets.md) |
| Audio layer | Ogg Vorbis (stb_vorbis) + RIFF WAVE decoding, resampling to 48 kHz stereo PCM16, 8-channel mixing; measured: 48 kHz / 5,587,774 frames (116.41 s), peak=19297 rms=4385; 22.05 kHz WAV resampled 5.64 s → 5.644 s, peak=22123 | [`src/snd.c`](src/snd.c) · [`tests/test_snd.c`](tests/test_snd.c) · [`docs/04-audio.md`](docs/04-audio.md) |
| Renderer (Python prototype, **not in repo**) | 1920×1080 compositing of background + sprites + dialogue box + text; 12 frames + 10 story checkpoints exported as PNGs; model and known deviations documented | [`docs/render-model.md`](docs/render-model.md) |
| Renderer (C, **port in progress**) | the C renderer is being ported from the validated Python prototype and compared pixel by pixel; **not accepted yet** | [`src/gfx.c`](src/gfx.c) · [`src/gfx.h`](src/gfx.h) · [`docs/render-model.md`](docs/render-model.md) |
| Win32 desktop shell (**under development, not accepted**) | window + GDI presentation + 60 Hz frame loop + mouse/keyboard input + waveOut output; `make win32` builds `bin/fvp.exe`, but there are **no acceptance numbers** for picture, input or audio yet | [`src/platform/win32/main_win32.c`](src/platform/win32/main_win32.c) · [`Makefile`](Makefile) |
| Chinese script rebuild (method verified) | a GBK script damaged by a translation patch rebuilt to a structurally valid 3,712,944-byte `.hcb` with all 948,219 instructions and a matching command table (artifact not in repo) | [`docs/cn-script-rebuild.md`](docs/cn-script-rebuild.md) |
| Android shell (design, in progress) | APK pipeline, JNI, EGL native window, touch input, asset layout; not the current main line, and the shell is not in this repository | [`docs/android-shell.md`](docs/android-shell.md) |

"Byte-identical" means: run the script for N steps, dump every text line (`[layer] text`,
CRLF) and compare **raw bytes** against the frozen Python reference, reporting the offset of
the first differing byte. Without game data the test prints `SKIPPED` instead of pretending
to pass — see [`scripts/ci-compare.py`](scripts/ci-compare.py).

## What does not work yet

1. **The C renderer and the Win32 shell are not accepted.** `src/gfx.c` is a port in
   progress with **no acceptance numbers yet**; the Win32 shell builds `bin/fvp.exe` with
   window, frame loop, input and waveOut, but **nothing has been verified** (picture,
   input, audio). Rendering that demonstrably produces frames is the local Python
   prototype (`work/`, not committed).
2. **Command semantics are stubs.** [`src/syscall.c`](src/syscall.c) handles 29 IDs
   (`TextPrint` / `Debmess` are real; input commands return synthetic values; animation and
   video `*Test` commands report "finished"); the other **119** fall back to the reference's
   default policy. No drawing, saves, settings, video or animation tweening.
3. **No coroutine/time model.** `ThreadNext` / `ThreadWait` only yield, `Timer*` /
   `ThreadSleep` have no virtual clock, and the 32 thread slots are not scheduled. As a
   consequence roughly 50 dialogue lines per 900k steps are lost. Facts and failed attempts
   are in [`docs/05-threads.md`](docs/05-threads.md).
4. **Audio is not wired to the VM.** Decoding, resampling and mixing work, and the Win32
   shell has a waveOut output backend, but **no syscall triggers playback**; there is no
   A/V sync or volume model, and no Android audio backend.
5. **WMV video is skipped** (`Movie` / `MovieState` report "already finished").
6. **The Chinese version is not playable end to end** (no renderer/audio, no per-scene checks).
7. **Only one title was tested** (`いろとりどりのセカイ HD Re:GENERATION`,
   `iroseka_HD.exe` 1.0.0.95). Other FVP titles are untested.
8. **Four opcode semantics are unconfirmed** (`0x11` / `0x12` / `0x17` / `0x18`, 0.18% of
   instructions in this title).
9. No save-format compatibility, no options menu, no Android backend in-tree.
10. The Python prototypes in `work/` are **not** committed, so rendering/HZC1 reference
    decoding cannot be re-run from a fresh clone; the C acceptance tools in
    [`tests/verify_assets.ps1`](tests/verify_assets.ps1) can.

---

## Build

Requirements: **a C99 compiler and GNU make**. No zlib, iconv, libvorbis or SDL
dependency. The only third-party source is `stb_vorbis` (see [`THIRD-PARTY.md`](THIRD-PARTY.md)).

```bash
make            # build bin/fvm.exe (script VM)
make lib        # build bin/libfvp.a (engine core + asset layer + audio layer)
make test       # build bin/test_snd.exe (audio decode self-test)
make win32      # build bin/fvp.exe (Win32 desktop runner; builds, but not accepted)
make clean
```

Flags: `-std=c99 -O2 -Wall -Wextra -Wpedantic -fwrapv`, **zero warnings required**.
Output names keep the `.exe` suffix on every platform (`EXE ?= .exe` in the Makefile);
use `make EXE=` on POSIX if you want `bin/fvm` without the suffix.
The generated headers `src/gen_cp932.h`, `src/gen_gbk.h`, `src/gen_cmd_table.h` are
committed, so building needs no Python. Use `make CC=gcc` if `cc` is not on PATH.

On Windows, [`build.ps1`](build.ps1) is a developer convenience that hard-codes a local
w64devkit path; in a fresh clone just use `make`.

## Run

```bash
./bin/fvm.exe /path/to/iroseka.hcb --steps 900000 --click 600 --quiet --text-out c-text.txt
./bin/fvm.exe --help
```

> The binary is `bin/fvm.exe` (see Build); with `make EXE=` it is `bin/fvm` instead.

| Option | Meaning |
|---|---|
| `--enc cp932\|gbk` | text encoding, default `cp932` (Japanese); use `gbk` for the Chinese patch |
| `--steps N` | instruction budget, default 2,000,000 |
| `--click N` | synthesize a click every N steps, default 0 (acceptance uses 600) |
| `--cursor X,Y` | synthetic cursor position, default `960,540` |
| `--text-out FILE` | write `[layer] text` lines (CRLF by default, matching the Python reference) |
| `--eol crlf\|lf` | line endings for `--text-out` |
| `--quiet` | do not echo text / debmess to stdout |
| `--pctrace FILE` | debug: log every executed pc |

Exit codes: `0` ok · `2` bad arguments · `3` script load/decode failure · `4` output write failure.

### Byte-exact comparison

```bash
python tests/compare.py --run --script /path/to/iroseka.hcb
# -> RESULT: PASS -- byte-identical (6496 bytes, 195 lines)
```

Without game data:

```bash
python scripts/ci-compare.py     # -> RESULT: SKIPPED (still verifies frozen reference SHA256s)
```

> `ref/*.txt` are CRLF byte-exact baselines. Keep `git config core.autocrlf false` when
> cloning, otherwise line-ending conversion makes the comparison fail spuriously. The
> repository's [`.gitattributes`](.gitattributes) pins this with `* -text`.

---

## Layout

```
.
├── README.md / README.en.md   project overview (Chinese / English)
├── VERSION                    0.1.0
├── LICENSE                    GPL-2.0-only
├── THIRD-PARTY.md             third-party code and generated files
├── CONTRIBUTING.md            bug reports, equivalence testing, style, pre-commit checks
├── .gitattributes             byte-exact baseline policy (* -text)
├── .gitignore                 artifacts / local work areas
├── Makefile                   make / make lib / make test / make win32 / make clean
├── build.ps1                  Windows one-shot build (hard-coded w64devkit path)
├── docs/                      format analysis and implementation docs (14 + index)
├── src/                       C99 engine core
│   ├── fvp.c                  value model, Python-compatible comparisons, arena, float repr
│   ├── encoding.c             CP932 / GBK -> UTF-8 (Python `decode('replace')` semantics)
│   ├── hcb.c                  .hcb loading, instruction decoding, pc -> index
│   ├── vm.c                   interpreter: stack, frames, threads, 40 opcodes
│   ├── syscall.c              148-command dispatch table (mostly stubs, see above)
│   ├── main.c                 CLI, text output, summary
│   ├── bin.c                  BIN package index / reader
│   ├── inflate.c              own zlib / raw deflate implementation (no zlib)
│   ├── hzc1.c                 HZC1 / NVSG image decoder (formats 0-4, multi-frame)
│   ├── gfx.c                  renderer (port in progress): prim table, compositing, text
│   ├── snd.c                  audio: stb_vorbis + RIFF WAVE, resampling, 8-channel mix
│   ├── gen_*.h                generated tables (committed)
│   ├── platform/win32/        Win32 desktop shell (under development)
│   └── third_party/           stb_vorbis v1.25
├── tests/                     equivalence, benchmarks, table generation, asset/rendering tools
├── ref/                       frozen Python reference + expected outputs
├── scripts/                   check-docs.py / ci-compare.py / release.ps1
└── .github/workflows/         CI (ubuntu + windows)
```

Not committed (see [`.gitignore`](.gitignore)): `work/` (Python format analysis
prototypes), `out/` `extract/` `bin/` (build/extract artifacts) and `tools-src/`
(downloaded third-party tools).

## Contributing

Read [`CONTRIBUTING.md`](CONTRIBUTING.md) first. In short:

- **Never commit game data** (or translation patches / cracked builds). File names, sizes
  and SHA256s are enough for a bug report.
- **Never weaken the baseline to make a test green**: [`ref/`](ref/REFERENCE.md) is frozen;
  new behaviour needs a new baseline plus evidence.
- Run `make && make lib`, `python scripts/check-docs.py` and (with data)
  `python tests/compare.py` before opening a PR.
- C code is C99, zero warnings, no new third-party dependencies.

Good first tasks: implement a stub command in [`src/syscall.c`](src/syscall.c), wire
`src/snd.c` to syscalls, help accept the C renderer (model in
[`docs/render-model.md`](docs/render-model.md)), implement the thread/time model (notes in
[`docs/05-threads.md`](docs/05-threads.md)), or test a second FVP title.

## License

**GPL-2.0-only** — see [`LICENSE`](LICENSE). The only third-party code, `stb_vorbis`
(public domain / MIT dual license), is GPL-compatible; see [`THIRD-PARTY.md`](THIRD-PARTY.md).

## Disclaimer

- No game data is included or distributed here. You must own the game to use this.
- The engine code is an independent rewrite: it contains and links no original game
  binary code. Formats and command semantics were derived by format analysis for
  interoperability and research.
- FVP, game titles and company names belong to their respective owners. This project is
  not affiliated with or endorsed by FAVORITE.
- Do not use this project to distribute game data or to infringe on copyright.
- Provided "as is", without warranty of any kind.