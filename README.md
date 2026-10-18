# Utsuro

An Xbox One (Durango) translation layer for macOS, later iOS. It does not emulate the console's
hardware: it runs the console's own Windows-derived userland (ntdll, kernelx, combase, vccorlib, the CRT)
as x86-64 under FEXCore and supplies what the hardware and the system partitions would have supplied.

**Status:** Minecraft: Xbox One Edition (1.2.0.0) boots, renders its menus and the world through Metal,
takes controller / keyboard / mouse input, plays sound, and can be played.

## What is where

| Area | Files |
| --- | --- |
| x86-64 guest on arm64 | `src/engine/` (FEXCore driver, guest memory, crash handler) |
| NT kernel half | `src/nt/` (`sys_*.cpp` syscalls, `process.cpp`, `vm.cpp`, `registry.cpp`, `kusd.cpp`) |
| WinRT objects | `src/nt/winrt*.cpp`, generated from `ref/WinDurango/idl` by `tools/gen_winrt.py` |
| Direct3D 11.X | `src/nt/d3d.cpp`, `d3d_gpu.inc`, interface tables from `tools/gen_d3d.py` |
| Shaders and rendering | `src/gpu/dxbc.cpp` (DXBC → MSL), `src/gpu/renderer.mm` (Metal) |
| Audio | `src/nt/xaudio2.cpp` (XAudio2 host), `src/host/audio.mm` (CoreAudio) |
| Window and input | `src/host/window.mm`, `src/host/input.mm` |

## Building

Needs Xcode (Metal toolchain) and the FEXCore macOS build from the Fathom project
(`~/Documents/Coding/fathom` and `fathom-fexcore`); see `scripts/build.sh`.

```
python3 -m venv tools/venv && tools/venv/bin/pip install pefile capstone   # for the tools
git clone https://github.com/WinDurango/WinDurango ref/WinDurango            # IDL / header reference
scripts/build.sh
```

## Running

The game is a decrypted dump with `Mount/` (the package) and `EmbeddedXvd/` (the console OS image):

```
build/utsuro-run games/mc-1.2.0.0
```

Controls (keyboard): `WASD` left stick, `IJKL` right stick (or the mouse after a click; `Esc` releases it),
arrows D-pad, `Return`/`Space` A, `Backspace`/`Esc` B, `X`, `Y`, `Q`/`E` shoulders, `Z`/`V` triggers, `Tab` View, `P` Menu.
A game controller works too.

## Debugging

`scripts/run.sh [seconds]` runs headless and kills the process afterwards (log in `/tmp/run.log`).
`scripts/session.sh start` + `scripts/ctl.sh press A` / `scripts/ctl.sh snap /tmp/x.png` drive a live
headless run (no window needed); `scripts/playworld.sh` walks the menus into a new world.
Environment: `UTSURO_LOG=debug`, `UTSURO_DUMPFRAMES=dir`, `UTSURO_DUMPSHADERS=dir`, `UTSURO_DUMPMSL=dir`,
`UTSURO_D3DSTATS=1`, `UTSURO_GPUDEBUG=nodepth,nocull,noblend`, `UTSURO_HEADLESS=1`, `--trace`.
