#include "ppc_config.h"
#include "ppc_context.h"
#include "memory.h"
#include "xex_loader.h"

#include <cstdio>
#include <cstring>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

static LONG WINAPI crash_handler(EXCEPTION_POINTERS* ep)
{
    fprintf(stderr, "\n[CRASH] Exception 0x%08lX at address %p\n",
            ep->ExceptionRecord->ExceptionCode,
            ep->ExceptionRecord->ExceptionAddress);
    if (ep->ExceptionRecord->ExceptionCode == 0xC0000005)
    {
        fprintf(stderr, "[CRASH] Access violation: %s address %p\n",
                ep->ExceptionRecord->ExceptionInformation[0] ? "writing" : "reading",
                (void*)ep->ExceptionRecord->ExceptionInformation[1]);
    }
    fflush(stderr);
    return EXCEPTION_EXECUTE_HANDLER;
}
#endif

// Forward-declare the XEX entry point (generated in ppc_recomp.43.cpp)
PPC_EXTERN_FUNC(_xstart);

int main(int argc, char* argv[])
{
#ifdef _WIN32
    SetUnhandledExceptionFilter(crash_handler);
#endif
    printf("=== Vigilante 8 Arcade - Static Recompilation ===\n\n");

    // Default PE image path (extracted from XEX using tools/dump_pe.exe)
    const char* pe_path = "extracted/pe_image.bin";
    if (argc > 1)
        pe_path = argv[1];

    // Step 1: Allocate PPC memory space (4 GB committed)
    printf("[1/4] Allocating PPC memory space...\n");
    uint8_t* base = ppc_memory_alloc();
    if (!base)
    {
        fprintf(stderr, "FATAL: Failed to allocate PPC memory\n");
        return 1;
    }

    // Step 2: Load PE data sections into memory
    printf("\n[2/4] Loading PE data sections...\n");
    if (!xex_load_data_sections(base, pe_path))
    {
        fprintf(stderr, "WARNING: PE data loading failed, data sections will be zeroed\n");
    }

    // Step 3: Populate function lookup table
    printf("\n[3/4] Building function lookup table...\n");
    ppc_populate_func_table(base);

    // Step 4: Initialize PPC context and launch
    printf("\n[4/4] Initializing PPC context...\n");
    PPCContext ctx{};
    memset(&ctx, 0, sizeof(ctx));

    // Set up stack pointer (r1) - stack grows downward from PPC_STACK_BASE
    // Leave 16 bytes of headroom at the top for the stack frame header
    ctx.r1.u32 = PPC_STACK_BASE - 16;

    // r2 = Table of Contents (TOC) pointer
    // TODO: Extract from XEX header or find via analysis
    // For now, leave as 0 - will need to be set correctly for global data access
    ctx.r2.u32 = 0;

    // r13 = Small Data Area (SDA) base
    // TODO: Extract from XEX or determine via analysis
    ctx.r13.u32 = 0;

    printf("  r1  (SP)    = 0x%08X\n", ctx.r1.u32);
    printf("  r2  (TOC)   = 0x%08X\n", ctx.r2.u32);
    printf("  r13 (SDA)   = 0x%08X\n", ctx.r13.u32);
    printf("  Entry point = 0x%08X (_xstart)\n\n", PPC_ENTRY_POINT);

    printf("=== Launching _xstart ===\n");
    fflush(stdout);

    // Call the XEX entry point
    // The generated _xstart function will initialize the C runtime
    // and eventually call main() / WinMain equivalent.
    _xstart(ctx, base);

    printf("\n=== _xstart returned ===\n");

    // Cleanup
    ppc_memory_free(base);
    return 0;
}
