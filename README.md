# Vigilante 8 Arcade - Static Recompilation Project

A static recompilation of **Vigilante 8 Arcade** (Xbox 360 / XBLA) to native x86-64 PC using [XenonRecomp](https://github.com/hedge-dev/XenonRecomp).

## Project Goal

Translate the Xbox 360 PowerPC executable of Vigilante 8 Arcade into native C++ code that can be compiled and run on modern PCs, preserving the original game logic while replacing Xbox 360 system services with PC equivalents.

## What is Static Recompilation?

Unlike emulation (which interprets instructions at runtime), static recompilation translates the entire binary ahead of time into equivalent C++ source code. Each PowerPC instruction maps to C++ code operating on a CPU state struct. The result is a native executable that runs at full speed without an emulator.

## Project Status

See [PROGRESS.md](PROGRESS.md) for detailed progress tracking.

**Current Phase:** Runtime skeleton functional, CRT initialization in progress

| Milestone | Status |
|-----------|--------|
| XEX extraction & analysis | Done |
| ABI address detection | Done |
| Jump table analysis (80 tables) | Done |
| Switch case boundary fixes | Done |
| Altivec/VMX instruction support (30+ opcodes added to XenonRecomp) | Done |
| Clean recompilation (0 errors, 0 warnings) | Done |
| Runtime skeleton & build system | Done |
| PE extraction & data section loading | Done |
| Xbox 360 kernel import stubs (205 functions) | Done |
| Successful build & link (19.7 MB native exe) | Done |
| CRT initialization & game boot | In Progress |
| Graphics (Xenos -> D3D12/Vulkan) | Not Started |
| Audio / Input / Integration | Not Started |

**Recompilation Output:** 1.8M+ lines of C++ across 49 source files (63 MB)

## Repository Structure

```
vig8/
├── config/                        # XenonRecomp configuration
│   ├── vig8.toml                  # Main recomp config (ABI addresses, manual function defs)
│   └── vig8_switch_tables.toml    # 80 jump table definitions
├── docs/                          # Documentation and research notes
│   ├── xenonrecomp-workflow.md
│   └── binary-analysis.md
├── src/                           # Runtime implementation source code
│   ├── main.cpp                   # Entry point, PPC context setup, launches _xstart
│   ├── memory.cpp/h               # 4GB PPC memory space (VirtualAlloc/mmap)
│   ├── xex_loader.cpp/h           # PE image data section loader
│   ├── kernel_stubs.cpp           # 205 Xbox 360 kernel/XAM/system stubs
│   └── math_polyfill.cpp          # C23 math function polyfills for MSVC
├── tools/                         # Toolchain (gitignored, built locally)
│   ├── XenonRecomp/               # Patched XenonRecomp with Altivec/VMX extensions
│   ├── patches/                   # XenonRecomp source patches
│   ├── dump_pe.cpp                # PE image extractor (links against XenonUtils)
│   └── extract_pe.py              # Python XEX decryption tool (requires pycryptodome)
├── extracted/                     # Extracted game files (gitignored)
│   ├── default.xex                # Xbox 360 executable
│   └── pe_image.bin               # Decrypted/decompressed PE image
├── ppc/                           # Generated C++ output (gitignored, reproducible)
├── build/                         # CMake build directory (gitignored)
├── CMakeLists.txt                 # Build system (Clang + Ninja)
├── .gitignore
├── PROGRESS.md                    # Detailed progress log
└── README.md                      # This file
```

## Prerequisites

- **CMake 3.20+**
- **Clang 18+** (required by XenonRecomp and generated code)
- **Ninja** (recommended build system)
- **Python 3.8+** with `pycryptodome` (for XEX decryption)
- **Git**
- A legally obtained copy of **Vigilante 8 Arcade** XEX

## Quick Start

```bash
# 1. Clone this repo
git clone https://github.com/sp00nznet/vig8.git
cd vig8

# 2. Place your extracted default.xex in extracted/default.xex

# 3. Clone and build XenonRecomp (with our Altivec/VMX patches)
git clone --recursive https://github.com/hedge-dev/XenonRecomp.git tools/XenonRecomp
# Apply patches from tools/patches/ (see below)
cd tools/XenonRecomp
cmake -B build -G Ninja
cmake --build build --config Release
cd ../..

# 4. Extract PE image from XEX (decrypt + decompress)
# Option A: Using the C++ tool (faster, links against XenonUtils)
clang++ -std=c++20 -O2 -I tools/XenonRecomp/XenonUtils -I tools/XenonRecomp/thirdparty \
    tools/dump_pe.cpp tools/XenonRecomp/build/XenonUtils/XenonUtils.lib -o tools/dump_pe.exe
tools/dump_pe.exe extracted/default.xex extracted/pe_image.bin

# Option B: Using Python (requires pycryptodome)
pip install pycryptodome
python tools/extract_pe.py extracted/default.xex extracted/pe_image.bin

# 5. Run analysis and recompilation
cd config
../tools/XenonRecomp/build/XenonAnalyse/XenonAnalyse ../extracted/default.xex vig8_switch_tables.toml
../tools/XenonRecomp/build/XenonRecomp/XenonRecomp vig8.toml ../tools/XenonRecomp/XenonUtils/ppc_context.h
cd ..

# 6. Build the native executable
cmake -B build -G Ninja -DCMAKE_CXX_COMPILER=clang++ \
    -DCMAKE_RC_COMPILER=llvm-rc
cmake --build build

# 7. Run
./build/vig8.exe
```

## XenonRecomp Patches

This project required adding 30+ missing Altivec/VMX instruction handlers to XenonRecomp.
Patches are maintained in the `tools/patches/` directory. Key additions:

- **Vector arithmetic:** `vaddsbs`, `vaddsws`, `vsubsbs`, `vsubshs`, `vsububm`
- **Vector shifts:** `vslh`, `vsrh`, `vsrah`, `vsrab`, `vrlh`
- **Vector compare:** `vcmpequh`, `vcmpgtsh`, `vcmpgtsw` (with RC bit)
- **Vector pack:** `vpkshss`, `vpkswss`, `vpkswus`, `vpkuhus` (and 128-bit variants)
- **Vector misc:** `vmaxsh`, `vminsh`, `vavguh`, `vspltish`, `vslo`, `vnor`
- **Float conversion:** `vcfpuxws128` (float -> unsigned int with saturation)
- **Integer logical:** `eqv` (equivalence)
- **CR bit ops:** `cror`, `crorc` (condition register OR/OR-complement)
- **Other:** `cctph` (Cell hint, no-op), `rldicl.` RC bit fix
- **Bug fixes:** Added missing RC bit (`.` suffix) handling for `vcmpgtub`, `vcmpgtuh`

## Xbox 360 Kernel Stubs

The runtime includes 205 stub implementations for Xbox 360 kernel/system imports, organized by subsystem:

| Subsystem | Count | Description |
|-----------|-------|-------------|
| Ke* | 25 | Kernel core (threads, sync, TLS, timing) |
| Nt* | 18 | NT kernel (files, memory, events, timers) |
| Rtl* | 12 | Runtime library (strings, memory, critical sections) |
| Vd* | 18 | Video/display (Xenos GPU, ring buffers) |
| Xam* | 40+ | Xbox Application Manager (UI, profiles, input) |
| XAudio/XMA | 7 | Audio subsystem |
| NetDll_* | 25+ | Networking (XNet, Winsock) |
| Ex* | 10 | Executive (memory pools, threads, locks) |
| Ob* | 5 | Object manager |
| Xex* | 5 | XEX loader |
| Other | 20+ | XUsbcam, Mm*, Hal*, C runtime |

## Toolchain

- [XenonRecomp](https://github.com/hedge-dev/XenonRecomp) - Xbox 360 static recompiler
- [XenosRecomp](https://github.com/hedge-dev/XenosRecomp) - Xenos GPU shader recompiler (future)

## References

- [UnleashedRecomp](https://github.com/hedge-dev/UnleashedRecomp) - Reference project (Sonic Unleashed recomp)
- [N64: Recompiled](https://github.com/N64Recomp/N64Recomp) - Inspiration for the approach

## License

This project contains no copyrighted game assets. You must provide your own legally obtained copy of Vigilante 8 Arcade.
