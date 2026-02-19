#include "xex_loader.h"
#include "memory.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

// Load data sections from a pre-extracted PE image file.
//
// The PE image should be extracted from the XEX using tools/dump_pe.exe:
//   dump_pe.exe extracted/default.xex extracted/pe_image.bin
//
// We skip code sections (.text) since they're statically recompiled.
// We copy data sections (.rdata, .data, .embsec_*, etc.) into the PPC
// memory space so the recompiled code can access global variables,
// string constants, and other data correctly.

bool xex_load_data_sections(uint8_t* base, const char* pe_path)
{
    // Read entire PE image
    FILE* f = fopen(pe_path, "rb");
    if (!f)
    {
        fprintf(stderr, "Failed to open PE image: %s\n", pe_path);
        return false;
    }

    fseek(f, 0, SEEK_END);
    size_t file_size = ftell(f);
    fseek(f, 0, SEEK_SET);

    std::vector<uint8_t> pe_image(file_size);
    if (fread(pe_image.data(), 1, file_size, f) != file_size)
    {
        fprintf(stderr, "Failed to read PE image\n");
        fclose(f);
        return false;
    }
    fclose(f);

    printf("PE image loaded: %zu bytes\n", file_size);

    // Validate - check for MZ header or PE signature
    if (file_size < 0x200)
    {
        fprintf(stderr, "  PE image too small\n");
        return false;
    }

    uint32_t pe_off = 0;
    if (pe_image[0] == 'M' && pe_image[1] == 'Z')
    {
        pe_off = *(uint32_t*)(pe_image.data() + 0x3C);
        printf("  MZ header found, PE at offset 0x%X\n", pe_off);
    }

    // Validate PE signature
    if (pe_off + 4 > file_size ||
        memcmp(pe_image.data() + pe_off, "PE\0\0", 4) != 0)
    {
        fprintf(stderr, "  Invalid PE signature\n");
        return false;
    }

    // Parse COFF header
    uint32_t coff_off = pe_off + 4;
    uint16_t machine     = *(uint16_t*)(pe_image.data() + coff_off);
    uint16_t num_sections = *(uint16_t*)(pe_image.data() + coff_off + 2);
    uint16_t opt_hdr_size = *(uint16_t*)(pe_image.data() + coff_off + 16);
    uint32_t section_table = coff_off + 20 + opt_hdr_size;

    printf("  Machine: 0x%04X, Sections: %u\n", machine, num_sections);

    // Copy each data section into PPC memory
    size_t total_loaded = 0;
    size_t sections_loaded = 0;

    for (uint16_t i = 0; i < num_sections && section_table + 40 <= file_size; i++)
    {
        const uint8_t* sec_hdr = pe_image.data() + section_table + (i * 40);
        char name[9] = {};
        memcpy(name, sec_hdr, 8);

        uint32_t virt_size   = *(uint32_t*)(sec_hdr + 8);
        uint32_t virt_addr   = *(uint32_t*)(sec_hdr + 12);
        uint32_t raw_size    = *(uint32_t*)(sec_hdr + 16);
        uint32_t raw_offset  = *(uint32_t*)(sec_hdr + 20);
        uint32_t chars       = *(uint32_t*)(sec_hdr + 36);

        uint64_t dest_addr = PPC_MEM_IMAGE_BASE + virt_addr;
        bool is_code = (chars & 0x20) != 0;  // IMAGE_SCN_CNT_CODE

        printf("  %-8s VA=0x%08llX VSize=0x%06X Raw=0x%06X %s\n",
               name, (unsigned long long)dest_addr, virt_size, raw_size,
               is_code ? "(code, skip)" : "(data, load)");

        // Skip code sections (statically recompiled)
        if (is_code) continue;

        // Skip sections with no raw data
        if (raw_size == 0 && virt_size == 0) continue;

        // Validate bounds
        uint32_t copy_size = raw_size < virt_size ? raw_size : virt_size;
        if (raw_offset + copy_size > pe_image.size())
        {
            fprintf(stderr, "    WARNING: section data extends past file end, truncating\n");
            copy_size = (raw_offset < pe_image.size()) ? (uint32_t)(pe_image.size() - raw_offset) : 0;
        }

        if (dest_addr + virt_size > PPC_MEM_IMAGE_BASE + PPC_MEM_IMAGE_SIZE)
        {
            fprintf(stderr, "    WARNING: section extends past image region, skipping\n");
            continue;
        }

        // Copy section data
        if (copy_size > 0)
        {
            memcpy(base + dest_addr, pe_image.data() + raw_offset, copy_size);
        }

        // Zero-fill BSS portion (virt_size > raw_size)
        if (virt_size > copy_size)
        {
            uint32_t zero_fill = virt_size - copy_size;
            memset(base + dest_addr + copy_size, 0, zero_fill);
        }

        total_loaded += copy_size;
        sections_loaded++;
    }

    printf("  Loaded %zu data sections, %zu bytes total\n", sections_loaded, total_loaded);
    return true;
}
