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
| 5b. Game Boot | Fix CRT init, get game loop running | DONE |
| 5c. Window | Win32 window with message pump | DONE |
| 5d. File I/O | Handle table, path translation, directory enumeration | DONE |
| 5e. Threading | Fiber-based cooperative threading (ExCreateThread, yield) | DONE |
| 5f. GPU Init | Ring buffer, EDRAM, render threads, XConfig | DONE |
| 6. ReXGlue Migration | Migrate from raw XenonRecomp to ReXGlue SDK | DONE |
| 7. Vtable & Crash Fixes | Fix indirect calls, missing functions, null page | DONE |
| 8. Graphics | D3D12 GPU pipeline, shader translation | DONE |
| 9. Audio | XMA audio decoding & playback | DONE (via ReXGlue) |
| 10. Input | Controller input + rumble | DONE (via ReXGlue) |
| 11. Gameplay | Game loads into rounds, full 3D rendering | DONE |
| 12. Polish | Performance, RTV path fix, multiplayer | IN PROGRESS |

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
- File I/O: NtOpenFile, NtCreateFile, NtReadFile, NtWriteFile, NtQueryInformationFile, NtQueryDirectoryFile, NtClose (real implementations with handle table & path translation)
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

---

### 2026-02-19 - Game Boots & Main Loop Running

**Completed:**
- [x] **CRITICAL FIX:** FPSCR cached MXCSR initialization (`ctx.fpscr.csr = 0x1F80`)
  - Root cause: `memset(&ctx, 0, ...)` left `fpscr.csr = 0`. When `enableFlushModeUnconditional()` called `setcsr(csr | FlushMask)`, it wrote `0x8040` to MXCSR — which has flush bits but **no exception mask bits**. Every SSE floating-point operation triggered a hardware exception caught by VEH, making execution ~1000x slower.
  - Fix: Initialize `ctx.fpscr.csr = 0x1F80` (all exceptions masked) after memset.
- [x] **Dynamic import resolution:** XexGetProcedureAddress now writes a universal dynamic stub address instead of returning error. Game resolves kernel function pointers at runtime via XexGetModuleHandle + XexGetProcedureAddress (like GetProcAddress on Windows).
- [x] **Graceful NULL indirect call handling:** COM-style vtable calls on uninitialized D3D/XAudio objects no longer crash — first 5 are logged, rest silently return 0.
- [x] **VdGetSystemCommandBuffer single-allocation fix:** Was allocating 64KB on every call in a tight loop, exhausting the heap. Now allocates once and reuses.
- [x] **Frame limiter:** Sleep(16) in VdSwap stub prevents 100% CPU usage.
- [x] Cleaned up all debug traces from generated PPC code.
- [x] Removed verbose indirect call logging (was generating millions of log lines/sec).

**Game Boot Sequence (fully working):**
1. `_xstart` entry point (CRT initialization)
2. Heap init (`sub_8232B560`)
3. atexit registration (`sub_82329AC0`)
4. Privilege/AV check (`sub_823251C8`) — returns 0 with stub
5. TLS initialization (`sub_8231A370`)
6. Constructor table 1 (`sub_8232B410`) — 3 entries
7. Constructor table 2 (`sub_8232B330`) — 140 entries (including vector math, audio, GPU init)
8. Command line parsing
9. `sub_820C11D8` (game WinMain) → `sub_820C0FC0` (game init)
10. `sub_820AC5B0` (game main loop) — **running continuously**

**Game Main Loop (sub_820AC5B0):**
- Init controllers x4 (`sub_82130710`)
- Call vtable[0] (Update) via indirect call
- Call `sub_82130FC8` (sync)
- Call vtable[4] (Render) via indirect call
- Call `sub_82131E80` (present/VdSwap)
- Repeat

**Stubs Called During Boot:**
NtAllocateVirtualMemory, KeGetCurrentProcessType, RtlInitializeCriticalSection,
XexCheckExecutablePrivilege, KeTlsAlloc, ExCreateThread (x4),
MmAllocatePhysicalMemoryEx (64MB GPU memory), XGetVideoMode,
VdInitializeEngines, VdSetGraphicsInterruptCallback,
XexGetModuleHandle/XexGetProcedureAddress (xam.xex, xboxkrnl.exe),
XAudioRegisterRenderDriverClient, XamGetSystemVersion,
XamNotifyCreateListener, NtOpenFile (data dir), NtQueryDirectoryFile (38 entries),
VdGetSystemCommandBuffer, VdSwap

---

### 2026-02-19 - Win32 Window Creation

**Completed:**
- [x] Created 1280x720 Win32 window ("Vigilante 8 Arcade") in `main.cpp`
- [x] Window creation happens after function table setup, before PPC context init
- [x] `WndProc` handles `WM_CLOSE`/`WM_DESTROY` → `PostQuitMessage(0)`
- [x] `VdSwap` stub now pumps Win32 messages (`PeekMessage`/`TranslateMessage`/`DispatchMessage`)
- [x] `WM_QUIT` in message pump → `ExitProcess(0)` for clean shutdown
- [x] `g_hwnd` global declared in `memory.h`, defined in `main.cpp`
- [x] CMakeLists.txt links `user32`/`gdi32` explicitly
- [x] Black background brush (window starts black, ready for future D3D surface)
- [x] **Fix:** Use ANSI Win32 APIs (`CreateWindowExA`) instead of wide-char — `UNICODE` not defined, causing `L"Vigilante 8 Arcade"` to truncate to `"V"`

**What This Enables:**
- Visible window for future D3D rendering surface
- Proper process lifecycle (close window = exit)
- Responsive window (draggable, resizable, closable)
- Console output still shows normal boot sequence

---

### 2026-02-19 - File I/O: Handle Table, Path Translation & Directory Enumeration

**Completed:**
- [x] **Handle table system:** 128-slot handle table (handles start at 0x1000 to avoid collision with event/thread handles at 0x100+). `HandleEntry` struct tracks type (file/directory), FILE*, host path, file size, and directory enumeration state.
- [x] **Path translation:** `xbox_path_to_host()` maps `game:\...` to `extracted/...` with backslash normalization. `parse_object_name()` reads ANSI_STRING from Xbox 360 OBJECT_ATTRIBUTES structs.
- [x] **NtOpenFile / NtCreateFile:** Real implementation. Detects directories (trailing slash, `GetFileAttributesA`), opens files with `fopen("rb")`, caches file size via `_ftelli64`. Both share `NtOpenFile_impl()`.
- [x] **NtReadFile:** Seeks to `ByteOffset` (big-endian LARGE_INTEGER) if provided, reads directly into PPC memory via `fread()`. Fills IO_STATUS_BLOCK.
- [x] **NtWriteFile:** Stub returning success (claims all bytes written).
- [x] **NtQueryInformationFile:** Supports class 5 (FileStandardInformation: file size, is-directory), class 14 (FilePositionInformation: `_ftelli64`), class 34 (FileNetworkOpenInformation: size + attributes).
- [x] **NtQueryDirectoryFile:** Full directory enumeration. Enumerates at open time via `FindFirstFileA`/`FindNextFileA`. Packs multiple entries per call with NextEntryOffset linking. Always uses info class 1 (FileDirectoryInformation, 0x40 header + ANSI filename). Returns `STATUS_NO_MORE_FILES` when exhausted.
- [x] **NtClose:** Handle-aware — closes FILE* and frees handle slot for file handles, silently succeeds for event/thread handles.
- [x] **Big-endian helpers:** Added `ppc_read_u16`, `ppc_write_u16`, `ppc_write_u64`.

**Xbox 360 ABI Discovery:**
- NtQueryDirectoryFile on Xbox 360 always uses FileDirectoryInformation (class 1, 0x40 header) — the r10 parameter is a FileName ANSI_STRING* filter, NOT FileInformationClass. This is unlike desktop Windows which supports multiple info classes.
- The game opens `game:\data\` once, enumerates all 38 data files (.ib/.ibz), builds an internal file list, then enters the main render loop.

**Game Data Files (38 in extracted/data/):**
- Config/metadata: `Config.ib`, `default.ib`, `microflakemap.ib`, `weaponparams.ibz`
- Characters: `boogie.ib`, `chassey.ib`, `dave.ib`, `molo.ib`, `torque.ib`
- Vehicles: `GrooVan.ibz`, `Incarcerator.ibz`, `Leprechaun.ibz`, `Mammoth.ibz`, `Manta.ibz`, `Piranha.ibz`, `Saucer.ibz`, `Stag.ibz`
- Levels: `Farmland.ibz`, `FartyDog.ibz`, `HooverDam.ibz`, `Jefferson.ibz`, `MeteorCrater.ibz`, `OilFields.ibz`, `SkiResort.ibz`
- UI/effects: `hud.ibz`, `menu.ibz`, `Debris.ibz`, `Particles.ibz`, `Surface.ibz`, `Weapons.ibz`
- Audio/text: `sounds.ib`, `v8theme.ib`, `Text_ENG.ibz`, `Text_FRE.ibz`, `Text_GER.ibz`, `Text_ITA.ibz`, `Text_JAP.ibz`, `Text_SPA.ibz`

---

### 2026-02-19 - File Loading & Decompression

**Completed:**
- [x] **NtQueryDirectoryFile class 1 fix:** Xbox 360 always uses FileDirectoryInformation (class 1, 0x40 header). Previously misinterpreted r10 (FileName filter pointer) as FileInformationClass, causing class 3 (0x5E header) responses. Game read filenames at wrong offset → "dirty disc" error.
- [x] **Zlib inflate switch table:** Added 81st switch table to `vig8_switch_tables.toml` — 30-entry state machine at 0x8212DD34 (jump table at 0x82007E38, u16 relative offsets). Without this, bctr was treated as indirect call → crash.
- [x] **Thread management restructure:** Moved PendingThread struct, globals, and `run_thread_inline()` to top of kernel_stubs.cpp. Implemented real KeResumeThread/NtResumeThread (find by handle, run inline).
- [x] **ObReferenceObjectByHandle fix:** Now writes handle value to output pointer (needed for KeResumeThread chain to work).
- [x] **ppc_config.h protection:** Restored custom PPC_CALL_INDIRECT_FUNC macro after XenonRecomp overwrote it. Must restore after every recompiler run.

**Game Progress:**
- Successfully enumerates all 38 data files in `game:\data\`
- Opens and fully reads `Text_ENG.ibz` (17KB) and `menu.ibz`
- Zlib decompression runs (inflate state machine works with new switch table)
- Crashes after menu.ibz with corrupted function pointer `0xF5582BB1` — likely memory corruption from decompression output or pointer endianness issue

**Key Technical Discoveries:**
- Xbox 360 NtQueryDirectoryFile r10 = FileName (ANSI_STRING* filter), NOT FileInformationClass
- XenonRecomp overwrites `ppc/ppc_config.h` on every run — custom macros must be restored
- The game's zlib inflate uses a computed switch table XenonAnalyse couldn't detect

---

### 2026-02-20 - NetDll Fix, Fiber Threading & GPU Init

**Completed:**
- [x] **ROOT CAUSE: Crash at 0xF5582BB1 was NetDll_XNetRandom corrupting .rdata**
  - Xbox 360 NetDll functions take an xnet handle in r3, shifting all other params by 1
  - Old code: `buf = r3` (handle), `len = r4` (buffer ptr) — wrote random bytes at handle address
  - Fix: `buf = r4`, `len = r5` — correct Xbox 360 NetDll calling convention
  - Memory watchpoint system traced corruption to NetDll_XNetRandom call
- [x] **Fiber-based cooperative threading** replacing single-threaded model
  - `ConvertThreadToFiber` in main.cpp converts main thread to fiber
  - `PendingThread` struct extended: fiber handle, own PPCContext, PPC stack (256KB each)
  - `ppc_thread_fiber_proc()`: fiber entry point for PPC threads
  - `init_thread_ctx()`: initializes thread's PPCContext (r13, fpscr, stack, context arg)
  - `thread_give_timeslice()`: creates fiber if needed, switches via `SwitchToFiber`
  - `thread_yield()`: called from blocking stubs, switches back to `g_main_fiber`
  - KeDelayExecutionThread, KeWaitForSingleObject yield from fiber threads
  - NtResumeThread gives immediate timeslice to resumed threads
  - ExCreateThread gives immediate timeslice to non-suspended threads
  - VdSwap gives each ready thread a timeslice per frame
- [x] **Non-fatal indirect calls:** PPC_CALL_INDIRECT_FUNC changed from fatal abort to warning+skip for out-of-range targets (logs first 20, then silently returns r3=0)
- [x] **ExGetXConfigSetting real values:**
  - Category 3, Setting 9: AV_REGION = 0x1000 (NTSC-U)
  - Category 3, Setting 10: GAME_REGION = 0xFF (all regions)
  - This fixed the game taking a completely different (and correct) initialization path
- [x] **GPU initialization now working:**
  - VdInitializeEngines, VdSetGraphicsInterruptCallback
  - MmAllocatePhysicalMemoryEx (64MB GPU, ring buffer, EDRAM)
  - VdInitializeRingBuffer, VdEnableRingBufferRPtrWriteBack
  - VdQueryVideoMode, VdRetrainEDRAM, VdCallGraphicsNotificationRoutines
  - GPU render threads 3 & 4 (routine 0x821517E8) created and running
- [x] **Full rebuild of all 49 PPC source files** after ppc_config.h macro change

**Game Progress:**
- Boots through full initialization (CRT, heap, constructors, game init)
- GPU subsystem initializes (ring buffer, EDRAM, video mode, render threads)
- Loads Text_ENG.ibz (17KB) and menu.ibz (2.4MB) with zlib decompression
- Creates 6 threads (0-5): 3 game threads, 2 GPU render threads, 1 game logic thread
- Queries input for 4 controllers (all return ERROR_DEVICE_NOT_CONNECTED)
- Queries ExGetXConfigSetting for AV_REGION and GAME_REGION
- Currently stalls after input initialization in pure PPC code (likely GPU command buffer polling)

**Key Technical Discoveries:**
- Xbox 360 NetDll calling convention: r3 = xnet handle (ignored), r4+ = actual params
- ExGetXConfigSetting AV_REGION response changes the entire game initialization flow
- Non-suspended threads need immediate fiber timeslices (can't wait for VdSwap)
- GPU render threads wait on events (KeWaitForSingleObject) for work signaling

---

### 2026-02-23 - ReXGlue SDK Migration & Full GPU Pipeline

**Completed:**
- [x] **Migrated from raw XenonRecomp to ReXGlue SDK**
  - ReXGlue provides: rex::Runtime, rex::kernel (threads, sync, file I/O, memory), rex::graphics (D3D12 GPU emulation, Xenos shader translation), rex::ui (windowed app, ImGui), audio (XMA decode)
  - New project structure: `project/CMakeLists.txt` linking rex::core, rex::runtime, rex::kernel, rex::graphics, rex::ui
  - Codegen via `rexglue.exe codegen` (replaces manual XenonRecomp + XenonAnalyse)
  - 8,059 functions recompiled across 17 source files
- [x] **Safe PPC_CALL_INDIRECT_FUNC macro override**
  - SDK's default macro does raw lookup+call with NO null check — instant crash on any unresolved vtable call
  - Override adds: NULL target check, out-of-range check, NULL function slot check
  - All gracefully return r3=0 and skip instead of crashing
  - Must re-apply after every `rexglue codegen` run (it overwrites vig8_init.h)
- [x] **PPC_UNIMPLEMENTED override** — warn-and-skip instead of throwing
  - `cctph` (thread priority hint) was throwing std::runtime_error, crashing on level load
  - Override logs warning and continues as no-op
- [x] **20 missing vtable function entries added to config**
  - 14 C++ this-adjustor thunks (2-instruction: `addi r3,r3,offset; b target`)
  - 3 game-logic functions missed by static analysis (only called via vtable)
  - 3 additional functions discovered during runtime
  - Automated vtable scanner (`find_missing_vtable_funcs.py`) found 129 total missing entries
- [x] **Cross-function goto fix**: sub_8219F570 and sub_8219F6C0 were one function incorrectly split by analysis. Merged via `0x8219F570 = { end = 0x8219F950 }` in config
- [x] **VEH null page handler**: Guest null pointer dereferences (reads from host 0x100000000) handled by decoding x86-64 MOV/MOVZX/MOVSX/MOVSXD instructions, zeroing destination register, and skipping
- [x] **VEH crash diagnostics**: Vectored exception handler logs all crashes with register dumps, PPC context extraction, stack traces, and C++ exception message decoding (MSVC 0xE06D7363)
- [x] **Windowed app** (Vig8App) with ImGui debug overlay (FPS counter)
- [x] **Console test harness** (vig8_test.exe) for headless crash debugging

**Game Progress — GAMEPLAY REACHED:**
- Main menu renders fully (V8 logo, menu text, vehicle artwork, background) at 56 FPS
- Audio plays (XMA decoding active)
- Controller input works (navigate menus, select options, rumble feedback)
- Game loads into rounds: HUD visible (minimap with moving enemies, targeting reticles, health bars)
- Controller rumble active during combat (being shot triggers vibration)
- **3D world renders white** — HUD overlay works perfectly but terrain/vehicles/skybox not visible
  - GPU pipeline is active (D3D12 command processor running, shaders translating)
  - Investigating shader translation or render state issue for 3D geometry

**Key Technical Discoveries:**
- ReXGlue SDK's PPC_CALL_INDIRECT_FUNC has no safety checks — must override for any game with C++ vtable dispatch
- `cctph`/`cctpl`/`cctpm` are PowerPC thread priority hints (`or rN,rN,rN` encodings) — safe no-ops on x86-64
- MSVC C++ exception code 0xE06D7363 can be decoded in VEH by reading ExceptionInformation[1] as std::exception*
- vig8_init.h gets overwritten by codegen — must maintain a post-codegen fixup step

---

### 2026-02-23 - 3D Rendering Fixed: ROV Path Discovery

**Completed:**
- [x] **ROOT CAUSE: White 3D world was an RTV resolve path issue with k_2_10_10_10_FLOAT + 4xMSAA**
  - The default RTV (Render Target View) D3D12 path fails to correctly resolve the game's 3D scene render targets
  - The 3D scene renders to EDRAM in k_2_10_10_10_FLOAT format with 4xMSAA, then resolves to k_16_16_16_16_FLOAT intermediate textures
  - RTV path produces white output during the compositing pass; HUD (k_8_8_8_8, no MSAA) renders correctly
  - **Fix:** `--render_target_path_d3d12=rov` — ROV (Rasterizer Ordered Views) path uses pixel shader interlock for EDRAM emulation and handles the resolve correctly
- [x] **GPU diagnostic instrumentation** added to command_processor.cpp
  - Per-frame counters: draw calls, resolve operations, resolve failures
  - Resolve destination logging: src_select, dest_base, dest_format, pitch
  - Swap texture fetch constant logging (texture fetch 0 = frontbuffer source)
  - Logging triggers for first N frames and every Nth frame during gameplay
- [x] **Full GPU pipeline characterized** during gameplay:

| Metric | Menu | Gameplay |
|--------|------|----------|
| Draw calls/frame | 4-8 | ~2,800 |
| Resolves/frame | 1 | 15 |
| Resolve failures | 0 | 0 |

**Per-Frame Resolve Breakdown (Gameplay):**

| Resolves | Type | Format | Description |
|----------|------|--------|-------------|
| 1-2 | Depth (src_select=4) | — | Shadow map depth renders |
| 3,5,7,9 | Color (src_select=0) | k_2_10_10_10_FLOAT → k_16_16_16_16_FLOAT (fmt 32) | 3D scene color (4xMSAA) |
| 4,6,8,10 | Depth (src_select=4) | — | 3D scene depth |
| 11-12 | Color | k_16_16_16_16 (fmt 26) | Bloom / post-processing |
| 13-14 | Color | k_8_8_8_8 (fmt 6) | Small textures |
| 15 | Color | k_8_8_8_8 (fmt 6), pitch=1280 | Frontbuffer composite (1280x720) |

- Swap always reads from texture fetch constant 0 → guest physical address 0x1BCA7000 (frontbuffer)

**Hypotheses Eliminated:**
- `gpu_allow_invalid_fetch_constants` — no invalid fetch constant warnings in GPU log; draws not being skipped
- Memexport — game uses vf95 instruction but no eM_ (memexport writes)
- Texture cache coherency — `--d3d12_readback_resolve` did not fix the issue
- GPU inactivity — confirmed ~2,800 draws and 15 successful resolves per frame

**Game Status — FULLY PLAYABLE:**
- 3D world renders correctly: terrain, vehicles, sky, buildings, particles
- Weapon effects and pickups visible
- Full HUD: minimap with enemy tracking, targeting reticles, health bars
- Running at ~90 FPS during gameplay
- 79 shaders translated, 58+ pipelines active

---

## Next Steps

1. **Investigate RTV path fix** — the default D3D12 RTV resolve path produces white output for k_2_10_10_10_FLOAT + 4xMSAA; ROV workaround is functional but a proper fix would benefit NVIDIA GPUs
2. **Expand null page handler** — handle additional x86-64 instruction encodings for guest null pointer reads
3. **Add remaining vtable functions** — 112 library/CRT entries still missing from function table
4. **Multiplayer stubs** — currently returns offline; could stub enough for local play
5. Contribute instruction patches upstream to XenonRecomp/ReXGlue

---

## Open Questions

1. ~~What engine does Vigilante 8 Arcade use?~~ Custom engine (IsopodEngine) with .ib/.ibz asset format
2. ~~Why does 3D scene render white while HUD renders correctly?~~ RTV resolve path issue with k_2_10_10_10_FLOAT + 4xMSAA → fixed by ROV path
3. Is the RTV resolve issue specific to this game's render target format, or a general Xenia-derived bug with k_2_10_10_10_FLOAT?
