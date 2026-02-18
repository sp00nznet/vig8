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
- **Optional Header Count:** 15

## STFS Container

The game is distributed as a LIVE (STFS) container:
- **Magic:** LIVE (0x4C495645)
- **Content Type:** 0x000D0000 (Xbox Live Arcade)
- **Block Separation:** 1
- **Total Container Size:** 123,797,504 bytes (118 MB)
- **Extracted using:** Custom Python STFS extractor (`tools/extract_stfs.py`)

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

### Verification

Address gaps match expected function sizes:
- savegprlr_14 -> restgprlr_14: 0x50 bytes (80 = (32-14)*4 + 8)
- savevmx_14 -> savevmx_64: 0x94 bytes (148 = (32-14)*8 + 4)
- restvmx_14 -> restvmx_64: 0x94 bytes
- savevmx_64 -> restvmx_14: 0x204 bytes (516 = (128-64)*8 + 4)

## Jump Tables

80 jump tables detected by XenonAnalyse:
- **Absolute jump tables:** ~50
- **Computed jump tables:** ~20
- **Offset jump tables:** ~10

Saved to `config/vig8_switch_tables.toml`.

## Recompilation Statistics (Initial Pass)

| Metric | Value |
|--------|-------|
| Generated C++ files | 48 implementation + 3 headers + 1 mapping |
| Total lines of C++ | 1,833,509 |
| Total generated size | 58.36 MB |
| Recompiled functions | ~12,000+ |
| Switch case errors | 72 |
| Unrecognized instructions | ~10,299 |
| RC bit warnings | 53 |

## Unrecognized Instructions

Instruction types not handled by XenonRecomp:

| Instruction | Category | Count (approx) |
|-------------|----------|-----------------|
| vnor128 | VMX128 | ~5 |
| cctph | Unknown | ~1 |
| vsubshs | Altivec | Many |
| vaddsbs | Altivec | Several |
| vsrah | Altivec | Several |
| vcmpgtsh | Altivec | Several |
| vspltish | Altivec | Many |
| vmaxsh | Altivec | Many |
| vminsh | Altivec | Several |
| vavguh | Altivec | Many |
| eqv | PPC Integer | Several |

Most of these are **standard Altivec SIMD instructions** that XenonRecomp doesn't implement yet. They are commonly used for:
- Audio processing (IDCT, sample manipulation)
- Texture/image processing
- Physics calculations
- General vectorized math

## Game Asset Format

The game uses a custom `.ib` / `.ibz` file format:
- `.ib` files appear to be uncompressed game data bundles
- `.ibz` files are likely compressed variants (zlib?)
- Files include: levels (Farmland, HooverDam, etc.), vehicles (boogie, chassey, dave, etc.), UI (menu, hud), audio (sounds, v8theme), text localizations

## Functions Needing Manual Boundaries

Switch case errors indicate these functions need manual `functions` entries in the TOML:

| Function Base | Error Type |
|---------------|------------|
| 0x8211FBD0 | Switch cases jump outside detected boundary |
| 0x821368BC | Switch cases jump outside detected boundary |
| 0x821A4628 | Switch cases jump outside detected boundary |
| Others TBD | Need to analyze remaining 72 errors |
