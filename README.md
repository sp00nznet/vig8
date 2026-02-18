# Vigilante 8 Arcade - Static Recompilation Project

A static recompilation of **Vigilante 8 Arcade** (Xbox 360 / XBLA) to native x86-64 PC using [XenonRecomp](https://github.com/hedge-dev/XenonRecomp).

## Project Goal

Translate the Xbox 360 PowerPC executable of Vigilante 8 Arcade into native C++ code that can be compiled and run on modern PCs, preserving the original game logic while replacing Xbox 360 system services with PC equivalents.

## What is Static Recompilation?

Unlike emulation (which interprets instructions at runtime), static recompilation translates the entire binary ahead of time into equivalent C++ source code. Each PowerPC instruction maps to C++ code operating on a CPU state struct. The result is a native executable that runs at full speed without an emulator.

## Project Status

See [PROGRESS.md](PROGRESS.md) for detailed progress tracking.

**Current Phase:** Setup & Initial Analysis

## Repository Structure

```
vig8/
├── config/              # XenonRecomp TOML configuration files
│   └── vig8.toml        # Main recompilation config
├── docs/                # Documentation and research notes
│   ├── xenonrecomp-workflow.md
│   └── binary-analysis.md
├── tools/               # Toolchain (gitignored, cloned locally)
│   └── XenonRecomp/     # XenonRecomp toolchain (git submodule or local clone)
├── src/                 # Runtime implementation source code
├── ppc/                 # Generated C++ output (gitignored, reproducible)
├── .gitignore
├── PROGRESS.md          # Progress log
└── README.md            # This file
```

## Prerequisites

- **CMake 3.20+**
- **Clang 18+** (required by XenonRecomp)
- **Git**
- A legally obtained copy of **Vigilante 8 Arcade** XEX

## Quick Start

```bash
# 1. Clone this repo
git clone https://github.com/sp00nznet/vig8.git
cd vig8

# 2. Place your "Vigilante 8 Arcade" XEX in the root directory

# 3. Clone and build XenonRecomp
git clone --recursive https://github.com/hedge-dev/XenonRecomp.git tools/XenonRecomp
cd tools/XenonRecomp
mkdir build && cd build
cmake ..
cmake --build . --config Release
cd ../../..

# 4. Run analysis
./tools/XenonRecomp/build/Release/XenonAnalyse "Vigilante 8 Arcade" config/vig8_switch_tables.toml

# 5. Run recompilation
mkdir -p ppc
./tools/XenonRecomp/build/Release/XenonRecomp config/vig8.toml tools/XenonRecomp/XenonUtils/ppc_context.h
```

## Toolchain

- [XenonRecomp](https://github.com/hedge-dev/XenonRecomp) - Xbox 360 static recompiler
- [XenosRecomp](https://github.com/hedge-dev/XenosRecomp) - Xenos GPU shader recompiler (future)

## References

- [UnleashedRecomp](https://github.com/hedge-dev/UnleashedRecomp) - Reference project (Sonic Unleashed recomp)
- [N64: Recompiled](https://github.com/N64Recomp/N64Recomp) - Inspiration for the approach

## License

This project contains no copyrighted game assets. You must provide your own legally obtained copy of Vigilante 8 Arcade.
