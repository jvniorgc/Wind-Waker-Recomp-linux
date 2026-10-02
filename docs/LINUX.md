# BlueWake for Linux

BlueWake runs natively on Linux (x86-64). It renders with **Vulkan** through
Aurora and Dawn (WebGPU). It is the Windows port (`windows/`,
`scripts/windows/build.py`; see [WINDOWS.md](WINDOWS.md)) with a Linux shell:

- `linux/CMakeLists.txt`: the same host, GXRuntime/Aurora and donor DSP as
  the Windows app, without the Windows compatibility layer.
- `linux/src/linux_entry.c`: default paths, session log, crash report and
  command-line options. It is the counterpart of `win_entry.c`.
- `linux/src/linux_disc.c`: the first launch's disc choice and preparation.
  It uses zenity when it is installed.
- `windows/src/win_settings.cpp`: the settings menu, shared with Windows.
  On Linux it reads the F1/F9/F10/F11/Alt+Enter keys from SDL events.
- `scripts/linux/build.py`: the builder. It runs the same pins, source
  steps and verified composite digest as the Windows builder.

No release is published: you build your own copy from your own disc.

## Requirements

- An x86-64 CPU. AVX2 (x86-64-v3) is preferred; the builder lowers the level
  on older CPUs.
- A Vulkan driver (Mesa RADV/ANV, or NVIDIA's) and `vulkan-icd-loader`.
- clang 17+, lld, llvm (`llvm-profdata`, for the optimization training),
  CMake 3.25+, Ninja, git and Python 3.10+.
- SDL3 (the system's, if installed; otherwise it is fetched).
- Optional: zenity (the disc picker and message boxes), and Rust's cargo
  (to build nodtool for `.rvz` and other compressed images).

Arch: `pacman -S clang lld llvm cmake ninja git python sdl3 vulkan-icd-loader zenity`

Debian/Ubuntu: `apt install clang lld llvm cmake ninja-build git python3 libsdl3-dev libvulkan1 zenity`

## Build

```sh
python3 scripts/linux/build.py /path/to/GZLE01.iso
```

The app folder is `build/linux/BlueWake`. Useful options:

- `--no-train`: skip the local optimization training. The build is faster
  but the game runs slower.
- `--source-only`: check the tools, the disc and the translation in minutes.
- `--jobs N`: the number of parallel compiles. By default it is all cores,
  limited by free memory.

## Run

```sh
build/linux/BlueWake/BlueWake [--widescreen] [--fullscreen] [--betterww] ...
```

`--help` lists the options. Saves, settings, states and logs go to
`~/.local/share/BlueWake` (`$XDG_DATA_HOME/BlueWake`; `BLUEWAKE_DATA_DIR`
overrides). A crash leaves a backtrace in `logs/session-*.log`.

Keys:

- F1: settings
- F11 or Alt+Enter: fullscreen
- F10: Smooth Motion
- F9: frame rate
- F5: save state
- F8: load state

The build folder contains code translated from your disc and a copy of the
disc. Never share or upload it.
