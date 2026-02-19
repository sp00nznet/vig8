# Vigilante 8 Arcade Recomp - Progress Log

## Phase Overview

| Phase | Description | Status |
|-------|-------------|--------|
| 1. Setup | Repo, toolchain, documentation | DONE |
| 2. Analysis | XenonAnalyse, ABI address hunting | DONE |
| 3. Configuration | TOML config, function definitions | DONE |
| 4. Initial Recomp | First XenonRecomp pass, fix errors | DONE |
| 4b. Instruction Support | Add 30+ missing Altivec/VMX to XenonRecomp | DONE |
| 5. Runtime Skeleton | Minimal runtime to link & boot | NOT STARTED |
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

## Next Steps

1. Add 3 remaining switch table entries (computed jump tables at 0x8212DD34, 0x8219F5B4, 0x821A16B0)
2. Begin runtime skeleton (PPCContext init, memory mapping, host entry point)
3. Create CMakeLists.txt for building recompiled C++ into native executable
4. Study UnleashedRecomp for runtime architecture reference
5. Investigate the .ib/.ibz file format (game's custom asset format)
6. Contribute instruction patches upstream to XenonRecomp

---

## Open Questions

1. What engine does Vigilante 8 Arcade use? (custom engine likely given .ib format)
2. Does it have any title update patches (XEXP files)?
3. What graphics API complexity do we expect? (Xenos shader count, render pipeline)
4. Are there any known modding/reverse engineering resources for this game?
