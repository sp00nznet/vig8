#include "memory.h"
#include "ppc_config.h"
#include "ppc_context.h"

#include <cstdio>
#include <cstring>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#else
#include <sys/mman.h>
#endif

uint8_t* ppc_memory_alloc()
{
    uint8_t* base = nullptr;

#ifdef _WIN32
    // Reserve and commit 4 GB of virtual address space.
    // Using MEM_RESERVE|MEM_COMMIT together; Windows will only allocate physical
    // pages on first access, so this doesn't actually use 4 GB of RAM.
    // This is necessary because the PPC code can access any address in the 32-bit
    // space (globals, heap, stack, etc.) and we need all of it to be accessible.
    base = static_cast<uint8_t*>(
        VirtualAlloc(nullptr, PPC_MEM_TOTAL_SIZE, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
    if (!base)
    {
        fprintf(stderr, "Failed to allocate 4 GB virtual address space (error %lu)\n", GetLastError());
        return nullptr;
    }

#else
    // POSIX: mmap with MAP_ANONYMOUS
    base = static_cast<uint8_t*>(
        mmap(nullptr, PPC_MEM_TOTAL_SIZE,
             PROT_READ | PROT_WRITE,
             MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE,
             -1, 0));
    if (base == MAP_FAILED)
    {
        perror("mmap failed");
        return nullptr;
    }
#endif

    printf("PPC memory allocated at %p (4 GB)\n", base);
    printf("  Image:   0x%08llX - 0x%08llX\n",
        (unsigned long long)PPC_MEM_IMAGE_BASE,
        (unsigned long long)(PPC_MEM_IMAGE_BASE + PPC_MEM_IMAGE_SIZE));
    printf("  Stack:   0x%08X - 0x%08X\n",
        PPC_STACK_BASE - PPC_STACK_SIZE, PPC_STACK_BASE);
    printf("  Heap:    0x%08X - 0x%08X\n",
        PPC_HEAP_BASE, PPC_HEAP_BASE + PPC_HEAP_SIZE);

    return base;
}

void ppc_memory_free(uint8_t* base)
{
    if (!base) return;

#ifdef _WIN32
    VirtualFree(base, 0, MEM_RELEASE);
#else
    munmap(base, PPC_MEM_TOTAL_SIZE);
#endif
}

void ppc_populate_func_table(uint8_t* base)
{
    // PPCFuncMappings is defined in the generated ppc_func_mapping.cpp
    // Each entry maps a guest address to a host function pointer.
    // The lookup table is stored at:
    //   base + PPC_IMAGE_BASE + PPC_IMAGE_SIZE + (guest_addr - PPC_CODE_BASE) * 2
    // This gives 8 bytes per 4-byte PPC instruction (one function pointer slot).

    size_t count = 0;
    for (const PPCFuncMapping* m = PPCFuncMappings; m->host != nullptr; ++m)
    {
        uint32_t guest_addr = static_cast<uint32_t>(m->guest);
        if (guest_addr >= PPC_CODE_BASE && guest_addr < PPC_CODE_BASE + PPC_CODE_SIZE)
        {
            uint64_t table_offset = PPC_FUNC_TABLE_OFFSET +
                (static_cast<uint64_t>(guest_addr - PPC_CODE_BASE) * 2);
            auto* slot = reinterpret_cast<PPCFunc**>(base + table_offset);
            *slot = m->host;
            ++count;
        }
    }

    printf("  Populated %zu function table entries\n", count);
}
