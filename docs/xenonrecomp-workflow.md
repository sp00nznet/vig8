# XenonRecomp Workflow for Vigilante 8 Arcade

## Tool Overview

**XenonRecomp** (https://github.com/hedge-dev/XenonRecomp) is a static recompiler that converts Xbox 360 PowerPC executables into C++ source code compilable for x86-64.

### What it produces
- C++ source files with 1:1 translated PowerPC functions
- Each function operates on a `ppc_context` struct (32 GPRs, 32 FPRs, 128 vector regs, CR, CTR, XER, LR, MSR, FPSCR)
- Functions use weak linking for easy hooking (`PPC_FUNC` / `PPC_FUNC_IMPL`)
- Handles big-endian to little-endian byte-swapping automatically
- Converts jump tables to C++ switch statements
- Supports mid-ASM hooks for injecting custom code at specific addresses

### What it does NOT provide
- No runtime implementation (graphics, audio, input, I/O, memory, threading)
- No MMIO operations
- No exception handling support

## Step-by-Step Workflow

### Step 1: Build XenonRecomp

```bash
git clone --recursive https://github.com/hedge-dev/XenonRecomp.git tools/XenonRecomp
cd tools/XenonRecomp
mkdir build && cd build
cmake ..
cmake --build . --config Release
```

**Requirements:** CMake 3.20+, Clang 18+, C++17

**Bundled dependencies (submodules):**
- xxHash, fmt, tomlplusplus, libmspack (LZX decompression), tiny-AES-c (AES decryption)

### Step 2: Run XenonAnalyse

```bash
XenonAnalyse "Vigilante 8 Arcade" config/vig8_switch_tables.toml
```

This scans the XEX to detect jump tables and outputs a TOML file listing them.

### Step 3: Locate ABI Addresses

Using a hex editor (HxD, 010 Editor, etc.), search the XEX binary for the PowerPC ABI register save/restore functions. These are standard Xbox 360 ABI routines.

**Byte patterns to search for:**

- **savegprlr** / **restgprlr** - General purpose register save/restore with link register
- **savefpr** / **restfpr** - Floating point register save/restore
- **savevmx** / **restvmx** - VMX (Altivec/vector) register save/restore
- **setjmp** / **longjmp** - C runtime jump functions

These addresses vary per game build. Record them for the TOML config.

### Step 4: Create TOML Configuration

```toml
[main]
file_path = "../Vigilante 8 Arcade"
out_directory_path = "../ppc"
switch_table_file_path = "vig8_switch_tables.toml"

# ABI addresses (MUST be found in the actual binary)
restgprlr_14_address = 0x00000000  # TODO: find
savegprlr_14_address = 0x00000000  # TODO: find
restfpr_14_address = 0x00000000    # TODO: find
savefpr_14_address = 0x00000000    # TODO: find
restvmx_14_address = 0x00000000   # TODO: find
savevmx_14_address = 0x00000000   # TODO: find
restvmx_64_address = 0x00000000   # TODO: find
savevmx_64_address = 0x00000000   # TODO: find

# Optional
longjmp_address = 0x00000000      # TODO: find
setjmp_address = 0x00000000       # TODO: find

[optimizations]
# Leave all disabled for initial pass
skip_lr = false
skip_msr = false
ctr_as_local = false
xer_as_local = false
reserved_as_local = false
cr_as_local = false
non_argument_as_local = false
non_volatile_as_local = false
```

### Step 5: Run XenonRecomp

```bash
mkdir -p ppc
XenonRecomp config/vig8.toml tools/XenonRecomp/XenonUtils/ppc_context.h
```

**Output files generated in `ppc/`:**

| File | Purpose |
|------|---------|
| `ppc_config.h` | Configuration defines and memory layout |
| `ppc_context.h` | PowerPC execution context structure |
| `ppc_recomp_shared.h` | Function declarations and shared definitions |
| `ppc_func_mapping.cpp` | Address-to-function resolution hash table |
| `ppc_recomp.*.cpp` | Implementation files (split every 256 functions) |

### Step 6: Build Runtime (The Hard Part)

The generated C++ is just the translated game logic. To actually run the game, we need:

1. **Memory management** - Map Xbox 360 virtual address space, handle big-endian memory
2. **Graphics** - Translate Xenos GPU draw calls to D3D12 or Vulkan
3. **Shaders** - Recompile Xenos shaders to HLSL/SPIR-V (XenosRecomp)
4. **Audio** - Implement XMA/PCM audio decoding and playback
5. **Input** - Map Xbox 360 controller calls to PC input
6. **File I/O** - Redirect Xbox 360 file system calls to PC paths
7. **Threading** - Map Xbox 360 threading primitives to Windows/POSIX threads
8. **System calls** - Stub or implement Xbox 360 kernel calls (XAM, XEX loader, etc.)

### Step 7: Iterate

- Fix recompilation errors (invalid instructions, missing function boundaries)
- Add `functions` entries in TOML for functions that couldn't be auto-detected
- Add `invalid_instructions` entries for data embedded in code sections
- Add mid-ASM hooks where needed

### Step 8: Optimize (After Everything Works)

Enable TOML optimization flags one by one:
1. `non_volatile_as_local = true` (biggest impact - ~20MB size reduction)
2. `cr_as_local = true`
3. `non_argument_as_local = true`
4. etc.

## Reference: UnleashedRecomp

The only completed XenonRecomp project is [Sonic Unleashed Recompiled](https://github.com/hedge-dev/UnleashedRecomp). Key takeaways:
- Took ~6 months of development
- Uses D3D12 + Vulkan rendering backends
- Custom Xenos shader -> HLSL translator
- Extensive mid-ASM hooking for graphics/audio/input
- Good reference architecture for our runtime
