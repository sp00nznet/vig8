#include "ppc_config.h"
#include "ppc_context.h"
#include "memory.h"
#include "xex_loader.h"

#include <cstdio>
#include <cstring>
#include <cfenv>
#include <xmmintrin.h>
#include <float.h>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

static uint8_t* g_ppc_base = nullptr;

// Counter for NULL indirect calls (COM vtable entries on uninitialized objects)
uint64_t g_null_icall_count = 0;

// Global window handle
HWND g_hwnd = nullptr;

// Window procedure
static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg)
    {
    case WM_CLOSE:
        PostQuitMessage(0);
        return 0;
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProc(hwnd, msg, wp, lp);
}

// Vectored exception handler: catch and silence FP exceptions
static LONG WINAPI fp_exception_handler(EXCEPTION_POINTERS* ep)
{
    DWORD code = ep->ExceptionRecord->ExceptionCode;
    // Handle all floating-point exceptions by masking them and continuing
    if (code >= 0xC000008D && code <= 0xC0000093) // FLT_DENORMAL through FLT_UNDERFLOW
    {
        // Clear the x87 FPU status and re-mask exceptions
        _clearfp();
        _controlfp(_MCW_EM, _MCW_EM);
        // Also reset MXCSR
        _mm_setcsr(0x1F80);
        return EXCEPTION_CONTINUE_EXECUTION;
    }
    return EXCEPTION_CONTINUE_SEARCH;
}

static LONG WINAPI crash_handler(EXCEPTION_POINTERS* ep)
{
    fprintf(stderr, "\n[CRASH] Exception 0x%08lX at address %p\n",
            ep->ExceptionRecord->ExceptionCode,
            ep->ExceptionRecord->ExceptionAddress);
    if (ep->ExceptionRecord->ExceptionCode == 0xC0000005)
    {
        auto fault_addr = (uintptr_t)ep->ExceptionRecord->ExceptionInformation[1];
        fprintf(stderr, "[CRASH] Access violation: %s address 0x%llX\n",
                ep->ExceptionRecord->ExceptionInformation[0] ? "writing" : "reading",
                (unsigned long long)fault_addr);
        if (g_ppc_base)
        {
            auto base_addr = (uintptr_t)g_ppc_base;
            int64_t offset = (int64_t)(fault_addr - base_addr);
            fprintf(stderr, "[CRASH] PPC base = 0x%llX, offset = 0x%llX (%lld)\n",
                    (unsigned long long)base_addr, (unsigned long long)offset, (long long)offset);
            if (offset >= 0 && offset < 0x100000000LL)
                fprintf(stderr, "[CRASH] PPC address = 0x%08X\n", (uint32_t)offset);
            else
                fprintf(stderr, "[CRASH] PPC address = OUT OF RANGE (negative or > 4GB)\n");
        }
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
    AddVectoredExceptionHandler(1, fp_exception_handler);
    SetUnhandledExceptionFilter(crash_handler);
#endif
    // Force line-buffered stderr so crash messages aren't lost
    setvbuf(stderr, nullptr, _IONBF, 0);
    setvbuf(stdout, nullptr, _IONBF, 0);
    printf("=== Vigilante 8 Arcade - Static Recompilation ===\n\n");

    // Default PE image path (extracted from XEX using tools/dump_pe.exe)
    const char* pe_path = "extracted/pe_image.bin";
    if (argc > 1)
        pe_path = argv[1];

    // Step 1: Allocate PPC memory space (4 GB committed)
    printf("[1/4] Allocating PPC memory space...\n");
    uint8_t* base = ppc_memory_alloc();
    g_ppc_base = base;
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
    printf("\n[3/5] Building function lookup table...\n");
    ppc_populate_func_table(base);

    // Step 4: Create Win32 window
    printf("\n[4/5] Creating window...\n");
    {
        WNDCLASSEXA wc = {};
        wc.cbSize = sizeof(wc);
        wc.style = CS_HREDRAW | CS_VREDRAW;
        wc.lpfnWndProc = WndProc;
        wc.hInstance = GetModuleHandle(nullptr);
        wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
        wc.hbrBackground = (HBRUSH)GetStockObject(BLACK_BRUSH);
        wc.lpszClassName = "Vig8WndClass";
        RegisterClassExA(&wc);

        RECT rc = {0, 0, 1280, 720};
        AdjustWindowRect(&rc, WS_OVERLAPPEDWINDOW, FALSE);

        g_hwnd = CreateWindowExA(
            0, "Vig8WndClass", "Vigilante 8 Arcade",
            WS_OVERLAPPEDWINDOW,
            CW_USEDEFAULT, CW_USEDEFAULT,
            rc.right - rc.left, rc.bottom - rc.top,
            nullptr, nullptr, GetModuleHandle(nullptr), nullptr);

        if (!g_hwnd)
        {
            fprintf(stderr, "FATAL: Failed to create window (error %lu)\n", GetLastError());
            ppc_memory_free(base);
            return 1;
        }

        ShowWindow(g_hwnd, SW_SHOW);
        UpdateWindow(g_hwnd);
        printf("  Window created: 1280x720\n");
    }

    // Step 5: Initialize PPC context and launch
    printf("\n[5/5] Initializing PPC context...\n");
    PPCContext ctx{};
    memset(&ctx, 0, sizeof(ctx));

    // CRITICAL: Initialize FPSCR's cached MXCSR to 0x1F80 (all exceptions masked).
    // The generated code calls ctx.fpscr.setcsr(csr) which writes the cached value
    // to MXCSR. If csr=0, this unmasks all SSE exceptions, causing every FP operation
    // to trigger a hardware exception (making execution ~1000x slower).
    ctx.fpscr.csr = 0x1F80;

    // Set up stack pointer (r1) - stack grows downward from PPC_STACK_BASE
    // Leave 16 bytes of headroom at the top for the stack frame header
    ctx.r1.u32 = PPC_STACK_BASE - 16;

    // r2 is NOT used as a TOC pointer in this binary (analysis confirmed all
    // TOC-style r2 accesses are dead code in switch table data). r2 is used
    // only as a scratch register. Leave at 0.
    ctx.r2.u32 = 0;

    // r13 = KPCR (Kernel Processor Control Region) pointer on Xbox 360.
    // The game code accesses:
    //   r13+0x100 = pointer to current KTHREAD
    //   r13+0x10C = per-processor flag byte
    //   r13+0x150 = error suppression flag
    ctx.r13.u32 = PPC_KPCR_BASE;

    // Set up fake KPCR structure
    // KPCR+0x100: pointer to current KTHREAD
    PPC_STORE_U32(PPC_KPCR_BASE + 0x100, PPC_KTHREAD_BASE);
    // KPCR+0x10C: per-processor flag byte (0 = normal)
    PPC_STORE_U32(PPC_KPCR_BASE + 0x10C, 0);
    // KPCR+0x150: error suppression flag (0 = not suppressed)
    PPC_STORE_U32(PPC_KPCR_BASE + 0x150, 0);

    // KTHREAD+0x160: last error code (0 = no error)
    PPC_STORE_U32(PPC_KTHREAD_BASE + 0x160, 0);

    printf("  r1  (SP)    = 0x%08X\n", ctx.r1.u32);
    printf("  r13 (KPCR)  = 0x%08X\n", ctx.r13.u32);
    printf("  KTHREAD     = 0x%08X\n", PPC_KTHREAD_BASE);
    printf("  Entry point = 0x%08X (_xstart)\n\n", PPC_ENTRY_POINT);

    // Mask all floating-point exceptions. The recompiled PPC code may trigger
    // FP inexact/overflow/underflow which are normally masked on Xbox 360.
    // Set both x87 FPU and SSE/AVX (MXCSR) exception masks.
    {
        // x87 FPU: mask all exceptions
        _controlfp(_MCW_EM, _MCW_EM);
        // SSE/AVX: set MXCSR to mask all exceptions (bits 7-12 = exception masks)
        // Default MXCSR = 0x1F80 (all masked). Force it.
        unsigned int mxcsr = 0x1F80; // all exceptions masked, round to nearest
        _mm_setcsr(mxcsr);
        printf("  FP exceptions masked (x87 + SSE/MXCSR=0x%04X)\n", mxcsr);
    }

    // Convert main thread to fiber for cooperative threading with PPC threads
    extern LPVOID g_main_fiber;
    g_main_fiber = ConvertThreadToFiber(nullptr);
    if (!g_main_fiber)
    {
        fprintf(stderr, "WARNING: ConvertThreadToFiber failed (error %lu), threads will not work\n",
                GetLastError());
    }
    else
    {
        printf("  Main thread converted to fiber\n");
    }

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
