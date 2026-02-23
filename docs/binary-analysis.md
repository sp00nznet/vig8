# Vigilante 8 Arcade - Binary Analysis Notes

## XEX File Information

- **Filename:** `default.xex` (extracted from STFS LIVE container)
- **Size:** 1,728,512 bytes (1.65 MB)
- **Format:** XEX2 (Xbox 360 executable)
- **Architecture:** PowerPC (Xenon, big-endian)
- **Platform:** Xbox Live Arcade (XBLA)
- **Title ID:** 0x584108A8
- **Base Address:** 0x82000000
- **PE Data Offset:** 0x2000
- **Module Flags:** 0x00000001
- **Game Engine:** IsopodEngine (custom)

## STFS Container

The game is distributed as a LIVE (STFS) container:
- **Magic:** LIVE (0x4C495645)
- **Content Type:** 0x000D0000 (Xbox Live Arcade)
- **Total Container Size:** 123,797,504 bytes (118 MB)
- 59 files extracted: default.xex + data/ + achievement PNGs + rating PNGs

## ABI Addresses

All 8 required ABI functions located via PowerPC instruction pattern scanning.

| Function | Address | Verified |
|----------|---------|----------|
| savegprlr_14 | `0x82310F00` | YES |
| restgprlr_14 | `0x82310F50` | YES |
| savefpr_14 | `0x82311720` | YES |
| restfpr_14 | `0x8231176C` | YES |
| savevmx_14 | `0x823117C0` | YES |
| restvmx_14 | `0x82311A58` | YES |
| savevmx_64 | `0x82311854` | YES |
| restvmx_64 | `0x82311AEC` | YES |
| setjmp | Not present | N/A |
| longjmp | Not present | N/A |

## Jump Tables

81 jump tables (80 detected by XenonAnalyse + 1 manual):
- Absolute, computed, and offset types
- Manual addition: zlib inflate state machine at 0x8212DD34 (30 entries, u16 relative offsets)
- Saved to `config/vig8_switch_tables.toml`

## Recompilation Statistics

### Legacy (raw XenonRecomp)

| Metric | Value |
|--------|-------|
| Generated C++ files | 48 implementation + 3 headers + 1 mapping |
| Total lines of C++ | 1,833,509 |
| Total generated size | 58.36 MB |
| Functions | ~12,000 |

### Current (ReXGlue SDK)

| Metric | Value |
|--------|-------|
| Generated C++ files | 17 implementation + 3 headers |
| Total generated size | ~60 MB |
| Functions recompiled | 8,059 |
| Function overrides | 31 (vtable entries, thunks, merged functions) |
| Unimplemented instructions | 1 (cctph — handled as no-op) |

## Function Overrides in Config

31 manual function definitions in `vig8_rexglue.toml`:

| Category | Count | Purpose |
|----------|-------|---------|
| Original analysis fixes | 11 | Switch case boundary corrections |
| C++ this-adjustor thunks | 14 | 2-instruction vtable wrappers (`addi r3; b target`) |
| Game logic vtable functions | 5 | Functions only called via vtable dispatch |
| Cross-function goto merge | 1 | sub_8219F570 + sub_8219F6C0 → single function |

### Missing Vtable Functions

Automated vtable scanner (`find_missing_vtable_funcs.py`) identified 129 total vtable entries missing from the function table:
- 17 high-priority (added to config)
- 112 library/CRT functions (lower priority, some may not be reached)

## Game Asset Format

Custom `.ib` / `.ibz` format (IsopodEngine):
- `.ib` — Uncompressed data bundles
- `.ibz` — zlib-compressed bundles
- 38 data files total

| Category | Files |
|----------|-------|
| Config | Config.ib, default.ib, microflakemap.ib, weaponparams.ibz |
| Characters | boogie.ib, chassey.ib, dave.ib, molo.ib, torque.ib |
| Vehicles | GrooVan.ibz, Incarcerator.ibz, Leprechaun.ibz, Mammoth.ibz, Manta.ibz, Piranha.ibz, Saucer.ibz, Stag.ibz |
| Levels | Farmland.ibz, FartyDog.ibz, HooverDam.ibz, Jefferson.ibz, MeteorCrater.ibz, OilFields.ibz, SkiResort.ibz |
| UI/Effects | hud.ibz, menu.ibz, Debris.ibz, Particles.ibz, Surface.ibz, Weapons.ibz |
| Audio/Text | sounds.ib, v8theme.ib, Text_ENG.ibz (+ FRE, GER, ITA, JAP, SPA) |

## Kernel Import Usage

The game imports from two Xbox 360 modules:

**xboxkrnl.exe** — Core kernel APIs:
- Threading: ExCreateThread, KeWaitForSingleObject, KeSetEvent, KeDelayExecutionThread
- Memory: NtAllocateVirtualMemory, MmAllocatePhysicalMemoryEx
- File I/O: NtOpenFile, NtReadFile, NtQueryDirectoryFile, NtClose
- GPU: VdInitializeEngines, VdInitializeRingBuffer, VdSwap, VdSetGraphicsInterruptCallback
- Sync: RtlInitializeCriticalSection, KfAcquireSpinLock
- Audio: XMACreateContext, XAudioRegisterRenderDriverClient

**xam.xex** — Xbox Application Manager:
- Input: XamInputGetState, XamInputGetCapabilities
- Profiles: XamUserGetSigninState, XamUserGetName
- UI: XamShowGamerCardUI, XamShowMarketplaceUI
- Network: XamNotifyCreateListener, XNetConnect, XNetGetConnectStatus
- Content: XamContentCreateEnumerator, XamContentCreate

## Xbox 360 Technical Notes

### NetDll Calling Convention
Xbox 360 NetDll functions take an xnet handle as the first parameter (r3), shifting all other parameters by 1 register. This is unlike standard kernel APIs.

### NtQueryDirectoryFile
On Xbox 360, always uses FileDirectoryInformation (class 1, 0x40 header). The r10 parameter is a FileName ANSI_STRING* filter, NOT FileInformationClass (unlike desktop Windows).

### Thread Priority Hints
`cctph`/`cctpm`/`cctpl` are PowerPC thread scheduling hints encoded as `or rN,rN,rN`. They're no-ops on x86-64.

### Guest Memory Model
- 4GB guest space allocated at host address 0x100000000
- Guest address 0x82000000 = host address 0x182000000
- Guest address 0x00000000 = host address 0x100000000 (null page)
