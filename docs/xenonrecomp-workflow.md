# Static Recompilation Workflow for Xbox 360 Titles

## Overview

This document describes the workflow for statically recompiling an Xbox 360 (XEX) title to run natively on PC. The approach has two generations:

- **Legacy (raw XenonRecomp)** — Manual codegen + hand-built runtime (used for initial bring-up)
- **Current (ReXGlue SDK)** — Integrated codegen + full runtime SDK with GPU, audio, and input

This project uses the **ReXGlue SDK** approach.

## Tool Overview

### XenonRecomp
[XenonRecomp](https://github.com/hedge-dev/XenonRecomp) is a static recompiler that converts Xbox 360 PowerPC executables into C++ source code compilable for x86-64. Each PowerPC function maps to a C++ function operating on a `PPCContext` struct (32 GPRs, 32 FPRs, 128 vector regs, CR, CTR, XER, LR, MSR, FPSCR).

### ReXGlue SDK
The ReXGlue SDK wraps XenonRecomp's codegen with a complete Xbox 360 runtime:
- **rex::kernel** — Threading, sync, memory, file I/O, kernel APIs
- **rex::graphics** — D3D12-based Xenos GPU emulation, shader translation
- **rex::ui** — Windowed app framework, ImGui integration
- **Audio** — XMA decoding and playback
- **Input** — XInput controller support

## Step-by-Step Workflow (ReXGlue)

### Step 1: Set Up the ReXGlue SDK

```bash
# Clone and build the SDK
cd tools/rexglue-sdk
cmake --preset win-amd64
cmake --build out/build/win-amd64 --config Release --target install
```

### Step 2: Create the Codegen Config

Create a TOML config (e.g., `config/vig8_rexglue.toml`) with:

```toml
[main]
file_path = "../extracted/default.xex"
out_directory_path = "../generated"

# ABI addresses (found via hex search in XEX binary)
restgprlr_14_address = 0x82310F50
savegprlr_14_address = 0x82310F00
# ... etc

[functions]
# Manual function boundary overrides
# Vtable functions missed by static analysis:
0x821A17D0 = { end = 0x821A17E8 }
# Cross-function goto fixes (merge split functions):
0x8219F570 = { end = 0x8219F950 }
# C++ this-adjustor thunks:
0x8216B558 = { end = 0x8216B560 }
```

### Step 3: Run Codegen

```bash
tools/rexglue-sdk/out/install/win-amd64/bin/rexglue.exe codegen
```

This generates:
- `generated/vig8_config.h` — Address constants
- `generated/vig8_init.h` — Function declarations + table
- `generated/vig8_init.cpp` — Function table mappings
- `generated/vig8_recomp.*.cpp` — Recompiled C++ source files
- `generated/sources.cmake` — CMake source list

### Step 4: Apply Post-Codegen Fixes

**CRITICAL:** Codegen overwrites `vig8_init.h` every run. You must re-apply:

1. **Safe PPC_CALL_INDIRECT_FUNC** — The SDK's default macro does raw lookup+call with no NULL check. Override with a version that checks for NULL targets, out-of-range addresses, and missing function slots.

2. **PPC_UNIMPLEMENTED override** — Change from `throw` to warn-and-skip for instructions like `cctph` (thread priority hints that are safe no-ops).

### Step 5: Build

```bash
cd project
cmake --preset win-amd64
cmake --build out/build/win-amd64 --config Release
```

### Step 6: Run and Iterate

```bash
project/out/build/win-amd64/Release/vig8.exe extracted/
```

Common issues to fix iteratively:
- **Missing vtable functions** — Use `find_missing_vtable_funcs.py` to scan data section
- **Cross-function gotos** — Merge incorrectly-split functions in config
- **Missing kernel APIs** — Add stub implementations in `stubs.cpp`
- **Unimplemented instructions** — Override `PPC_UNIMPLEMENTED` or fix in codegen

## Finding ABI Addresses

Using a hex editor, search the XEX binary for PowerPC ABI register save/restore functions:

| Function | Purpose |
|----------|---------|
| savegprlr_14 / restgprlr_14 | General purpose register save/restore with link register |
| savefpr_14 / restfpr_14 | Floating point register save/restore |
| savevmx_14 / restvmx_14 | VMX (Altivec/vector) register save/restore |
| savevmx_64 / restvmx_64 | VMX 64-bit variant save/restore |

These addresses vary per game build and must be found in the actual binary.

## Common Patterns and Fixes

### Vtable Dispatch Safety
Xbox 360 games use C++ virtual method tables extensively. In recompiled code, vtable calls go through `PPC_CALL_INDIRECT_FUNC`. The SDK's default implementation will crash on any NULL or unresolved entry. Always override with null-safe version.

### Guest Null Page Handler
Games often dereference uninitialized pointers. A VEH handler can catch access violations in the guest null page (address 0x0-0xFFFF in guest space), decode the x86-64 load instruction, zero the destination register, and skip the instruction.

### Thread Priority Hints
`cctph`/`cctpm`/`cctpl` are PowerPC thread scheduling hints (`or rN,rN,rN` encodings). They're safe no-ops on x86-64 — override `PPC_UNIMPLEMENTED` to handle them.

## Reference Projects

- [UnleashedRecomp](https://github.com/hedge-dev/UnleashedRecomp) — Sonic Unleashed static recomp (most complete reference)
- [N64: Recompiled](https://github.com/N64Recomp/N64Recomp) — N64 static recomp (different architecture, same concept)
