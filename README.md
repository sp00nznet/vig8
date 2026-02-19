# Vigilante 8 Arcade - Static Recompilation Project

A static recompilation of **Vigilante 8 Arcade** (Xbox 360 / XBLA) to native x86-64 PC using [XenonRecomp](https://github.com/hedge-dev/XenonRecomp).

## Project Goal

Translate the Xbox 360 PowerPC executable of Vigilante 8 Arcade into native C++ code that can be compiled and run on modern PCs, preserving the original game logic while replacing Xbox 360 system services with PC equivalents.

## What is Static Recompilation?

Unlike emulation (which interprets instructions at runtime), static recompilation translates the entire binary ahead of time into equivalent C++ source code. Each PowerPC instruction maps to C++ code operating on a CPU state struct. The result is a native executable that runs at full speed without an emulator.

## Project Status

See [PROGRESS.md](PROGRESS.md) for detailed progress tracking.

**Current Phase:** Recompilation Complete - Building Runtime

| Milestone | Status |
|-----------|--------|
| XEX extraction & analysis | Done |
| ABI address detection | Done |
| Jump table analysis (80 tables) | Done |
| Switch case boundary fixes | Done |
| Altivec/VMX instruction support (30+ opcodes added to XenonRecomp) | Done |
| Clean recompilation (0 errors, 0 warnings) | Done |
| Runtime skeleton & build system | In Progress |
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
├── tools/                         # Toolchain (gitignored, built locally)
│   └── XenonRecomp/               # Patched XenonRecomp with Altivec/VMX extensions
├── src/                           # Runtime implementation source code
├── ppc/                           # Generated C++ output (gitignored, reproducible)
├── .gitignore
├── PROGRESS.md                    # Detailed progress log
└── README.md                      # This file
```

## Prerequisites

- **CMake 3.20+**
- **Clang 18+** (required by XenonRecomp)
- **Ninja** (recommended build system)
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

# 4. Run analysis (generates switch tables)
cd config
../tools/XenonRecomp/build/XenonAnalyse/XenonAnalyse ../extracted/default.xex vig8_switch_tables.toml

# 5. Run recompilation
../tools/XenonRecomp/build/XenonRecomp/XenonRecomp vig8.toml ../tools/XenonRecomp/XenonUtils/ppc_context.h
cd ..
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

## Toolchain

- [XenonRecomp](https://github.com/hedge-dev/XenonRecomp) - Xbox 360 static recompiler
- [XenosRecomp](https://github.com/hedge-dev/XenosRecomp) - Xenos GPU shader recompiler (future)

## References

- [UnleashedRecomp](https://github.com/hedge-dev/UnleashedRecomp) - Reference project (Sonic Unleashed recomp)
- [N64: Recompiled](https://github.com/N64Recomp/N64Recomp) - Inspiration for the approach

## License

This project contains no copyrighted game assets. You must provide your own legally obtained copy of Vigilante 8 Arcade.
