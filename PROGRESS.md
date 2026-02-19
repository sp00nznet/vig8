# Vigilante 8 Arcade Recomp - Progress Log

## Phase Overview

| Phase | Description | Status |
|-------|-------------|--------|
| 1. Setup | Repo, toolchain, documentation | DONE |
| 2. Analysis | XenonAnalyse, ABI address hunting | DONE |
| 3. Configuration | TOML config, function definitions | DONE |
| 4. Initial Recomp | First XenonRecomp pass, fix errors | DONE |
| 4b. Instruction Support | Add 30+ missing Altivec/VMX to XenonRecomp | DONE |
| 5. Runtime Skeleton | Minimal runtime to link & boot | DONE |
| 6. Graphics | Xenos -> D3D12/Vulkan rendering | NOT STARTED |
| 7. Audio | Audio system implementation | NOT STARTED |
| 8. Input | Controller/keyboard input | NOT STARTED |
| 9. Integration | Full game loop, menus, gameplay | NOT STARTED |
| 10. Polish | Optimizations, bug fixes, testing | NOT STARTED |

---

## Detailed Log

### 2026-02-17 - Project Kickoff & Initial Recompilation

**Completed:**
- [x] Initialized repository structure (config/, docs/, tools/, src/)
- [x] Created .gitignore (excludes binaries, build artifacts, toolchain)
- [x] Created project documentation (README.md, PROGRESS.md, docs/)
- [x] Researched XenonRecomp thoroughly (see docs/xenonrecomp-workflow.md)
- [x] Extracted XEX from STFS/LIVE container (custom Python extractor)
- [x] Cloned and built XenonRecomp (Clang 21.1.8 + Ninja + CMake)
- [x] Ran XenonAnalyse - detected 80 jump tables
- [x] Located all 8 ABI register save/restore addresses
- [x] Created recomp TOML configuration (config/vig8.toml)
- [x] Ran first XenonRecomp pass - **SUCCESS**

**XEX Binary Details:**
- File: `default.xex` (1,728,512 bytes / 1.65 MB)
- Format: XEX2 (Xbox 360 executable)
- Title ID: 0x584108A8
- Base address: 0x82000000
- Content: XBLA vehicular combat game

**STFS Package Contents:**
- 59 files extracted from LIVE container
- `default.xex` - game executable
- `data/` - game assets (.ib, .ibz files - custom format)
- Achievement PNGs, rating PNGs, ArcadeInfo.xml

**ABI Addresses Found:**
| Function | Address |
|----------|---------|
| savegprlr_14 | 0x82310F00 |
| restgprlr_14 | 0x82310F50 |
| savefpr_14 | 0x82311720 |
| restfpr_14 | 0x8231176C |
| savevmx_14 | 0x823117C0 |
| restvmx_14 | 0x82311A58 |
| savevmx_64 | 0x82311854 |
| restvmx_64 | 0x82311AEC |
| setjmp | Not present |
| longjmp | Not present |

**XenonAnalyse Results:**
- 80 jump tables detected (absolute, computed, offset types)
- Saved to `config/vig8_switch_tables.toml`

**Recompilation Results:**
- **1,833,509 lines** of C++ generated (58.36 MB across 48 source files)
- 3 header files + 1 function mapping file + 48 implementation files
- 72 errors (switch cases jumping outside function boundaries)
- ~10,299 unrecognized instructions (mostly Altivec/VMX operations)
- 53 RC bit warnings (rldicl. instructions)

**Issues Found (all resolved in Phase 4b):**
1. ~~**Switch case boundary errors (72)**~~ - Fixed with manual function definitions in TOML
2. ~~**Unrecognized instructions (10,299)**~~ - Fixed by adding 30+ instruction handlers to XenonRecomp
3. ~~**RC bit warnings (53)**~~ - Fixed by adding CR update code to rldicl., vcmpgtub., vcmpgtuh.

**Build Environment:**
- Windows 11 Pro
- CMake 4.2.1
- Clang 21.1.8 (via LLVM)
- Ninja 1.13.2
- Python 3.11.9
- Visual Studio 2022 Community (MSVC 19.44)

### 2026-02-18 - Clean Recompilation Achieved

**Completed:**
- [x] Added 30+ missing Altivec/VMX instruction handlers to XenonRecomp
- [x] Fixed RC-bit (`.` suffix) handling for `rldicl.`, `vcmpgtub.`, `vcmpgtuh.`
- [x] Added CR bit manipulation instructions (`cror`, `crorc`)
- [x] Added integer logical instruction (`eqv`)
- [x] Added Cell SPE hint instruction (`cctph` as no-op)
- [x] Added `simde_mm_vctuxs()` helper for float->uint32 saturating conversion
- [x] Achieved **clean recompilation** - 0 unrecognized instructions, 0 RC-bit warnings
- [x] All 72 switch case boundary errors remain fixed from previous session

**Instructions Added to XenonRecomp:**

| Category | Instructions |
|----------|-------------|
| Vector saturating arithmetic | `vaddsbs`, `vaddsws`, `vsubsbs`, `vsubshs` |
| Vector modular arithmetic | `vsububm` |
| Vector shifts | `vslh`, `vsrh`, `vsrah`, `vsrab`, `vrlh` |
| Vector shift octet | `vslo`, `vslo128` |
| Vector compare | `vcmpequh`, `vcmpgtsh`, `vcmpgtsw` (all with RC bit) |
| Vector min/max | `vmaxsh`, `vminsh` |
| Vector average | `vavguh` |
| Vector splat | `vspltish` |
| Vector pack | `vpkshss`/128, `vpkswss`/128, `vpkswus`/128, `vpkuhus`/128 |
| Vector logical | `vnor`/128 |
| Float conversion | `vcfpuxws128` |
| Integer logical | `eqv` |
| CR bit ops | `cror`, `crorc` |
| Hints | `cctph` (no-op) |

**Bug Fixes:**
- `vcmpgtub`: Added missing RC-bit (`.`) CR6 update
- `vcmpgtuh`: Added missing RC-bit (`.`) CR6 update
- `rldicl`: Added missing RC-bit (`.`) CR0 comparison

**Recompilation Results (Pass 4 - Final):**
- **0** unrecognized instructions (was 10,299)
- **0** RC-bit warnings (was 53+14)
- **0** switch case boundary errors (was 72)
- **3** informational switch table notices (tables at 0x8212DD34, 0x8219F5B4, 0x821A16B0 need TOML entries)
- **49** C++ source files generated (63 MB total)

---

### 2026-02-18/19 - Runtime Skeleton & Successful Build

**Completed:**
- [x] Created CMakeLists.txt build system (CMake 3.20+, Clang, Ninja)
- [x] Implemented 4GB PPC memory space with VirtualAlloc (Windows) / mmap (POSIX)
- [x] Built PE image extraction tool (dump_pe.cpp) using XenonRecomp's XenonUtils
- [x] Extracted decrypted/decompressed PE image from encrypted XEX
- [x] Implemented PE data section loader (.rdata, .data, .embsec_*, etc.)
- [x] Populated function lookup table (12,282 entries for PPC_LOOKUP_FUNC)
- [x] Implemented 205 Xbox 360 kernel/system import stubs
- [x] Added C23 math polyfill (roundevenf) for MSVC CRT compatibility
- [x] **Successful full build: 19.7 MB native x86-64 executable**
- [x] **Entry point (_xstart) reached: game CRT begins initialization**

**Build System:**
- `ppc_recomp` static library: 48 generated .cpp files + function mapping
- `vig8` executable: runtime source (main, memory, xex_loader, kernel_stubs, math_polyfill)
- Clang flags: `-O2 -fno-strict-aliasing -Wno-everything` (for generated code)
- SIMDE headers from XenonRecomp thirdparty for SSE/AVX intrinsics

**PE Image Extraction:**
- XEX uses AES encryption (retail key) + LZX compression
- File key at security_info + 0x150, decrypted via AES-CBC
- Extracted 5,111,808 byte PE with 17 sections
- Data sections loaded: .rdata (488KB), .data (1.1MB), .pdata, .idata, .reloc, etc.

**Kernel Stubs (205 functions across 15 subsystems):**
- Memory: NtAllocateVirtualMemory (bump allocator), MmAllocatePhysicalMemoryEx
- Threading: ExCreateThread, KeWaitForSingleObject, KeTls* (single-threaded stubs)
- Sync: RtlInitializeCriticalSection, Kf*SpinLock (no-ops for single-threaded)
- Video: VdInitializeEngines, VdSwap, VdQueryVideoMode (1280x720 default)
- Audio: XAudioRegisterRenderDriverClient, XMACreateContext
- Input: XamInputGetState (ERROR_DEVICE_NOT_CONNECTED)
- Network: NetDll_* (all return errors/disconnected)
- File I/O: NtOpenFile, NtReadFile (return OBJECT_NAME_NOT_FOUND)
- C runtime: sprintf, _vsnprintf, DbgPrint (simplified stubs)

**Runtime Execution Progress:**
The recompiled executable successfully:
1. Allocates 4GB PPC address space
2. Loads PE data sections (989KB)
3. Populates 12,282 function table entries
4. Calls _xstart entry point
5. Executes NtAllocateVirtualMemory (1MB + 64KB allocations)
6. Calls KeGetCurrentProcessType, RtlInitializeCriticalSection
7. Crashes during CRT initialization (access violation in deeper init code)

**Known Issues:**
- r2 (TOC) and r13 (SDA) not yet set from XEX header
- CRT init crash needs investigation (likely related to missing TOC or import resolution)
- Python XEX decryptor only handles basic block compression; C++ dump_pe tool handles LZX
- NtAllocateVirtualMemory stub may need corrected parameter mapping

---

## Next Steps

1. Debug CRT initialization crash (likely TOC/r2 related)
2. Investigate and set correct r2 (TOC) and r13 (SDA) values
3. Improve NtAllocateVirtualMemory stub (handle requested base address, MEM_TOP_DOWN)
4. Add 3 remaining switch table entries
5. Implement basic file I/O stubs (for game data loading)
6. Begin Xenos GPU command buffer parsing for graphics
7. Contribute instruction patches upstream to XenonRecomp

---

## Open Questions

1. What engine does Vigilante 8 Arcade use? (custom engine likely given .ib format)
2. Does it have any title update patches (XEXP files)?
3. What graphics API complexity do we expect? (Xenos shader count, render pipeline)
4. Are there any known modding/reverse engineering resources for this game?
