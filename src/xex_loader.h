#pragma once

#include <cstdint>

// Load data sections from a pre-extracted PE image into PPC memory.
//
// The PE image should be extracted from the XEX beforehand using:
//   tools/dump_pe.exe extracted/default.xex extracted/pe_image.bin
//
// This copies .rdata, .data, .embsec_*, and other data sections to
// the correct offsets in base[]. Code sections are skipped (they're
// statically recompiled).
//
// Returns true on success.
bool xex_load_data_sections(uint8_t* base, const char* pe_path);
