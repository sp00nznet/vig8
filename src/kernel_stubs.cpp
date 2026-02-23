#include "ppc_config.h"
#include "ppc_context.h"
#include "memory.h"

#include <cstdio>
#include <cstdarg>
#include <cstring>
#include <cstdlib>
#include <string>
#include <vector>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#else
#include <chrono>
#include <thread>
#endif

// ============================================================================
// Xbox 360 Kernel / XAM / System Import Stubs
// ============================================================================
//
// The recompiled PPC code references these as __imp__FunctionName.
// Each function has the standard PPC recomp signature:
//   void name(PPCContext& __restrict ctx, uint8_t* base)
//
// Xbox 360 calling convention (PPC):
//   r3-r10 = arguments, r3 = return value
//   f1-f13 = float arguments, f1 = float return
//
// Most stubs log the call and return success (r3 = 0 / STATUS_SUCCESS).
// Critical functions (memory, TLS, printf) have real implementations.
// ============================================================================

// Stub logging - can be disabled for performance
#ifndef STUB_LOG_ENABLED
#define STUB_LOG_ENABLED 1
#endif

// Verbose mode: log every call, not just first
#ifndef STUB_VERBOSE
#define STUB_VERBOSE 1
#endif

#if STUB_LOG_ENABLED
#define STUB_LOG(name) \
    fprintf(stderr, "[STUB] %s(r3=0x%08X, r4=0x%08X, r5=0x%08X, r6=0x%08X)\n", \
            name, ctx.r3.u32, ctx.r4.u32, ctx.r5.u32, ctx.r6.u32)
#if STUB_VERBOSE
#define STUB_LOG_ONCE(name) STUB_LOG(name)
#else
#define STUB_LOG_ONCE(name) do { \
    static bool logged = false; \
    if (!logged) { \
        fprintf(stderr, "[STUB] %s (first call, further calls suppressed)\n", name); \
        logged = true; \
    } \
} while(0)
#endif
#else
#define STUB_LOG(name)
#define STUB_LOG_ONCE(name)
#endif

// Heartbeat: track total stub calls to show the game is alive
static uint64_t g_stub_call_count = 0;
static uint64_t g_last_heartbeat = 0;
#define STUB_HEARTBEAT() do { \
    g_stub_call_count++; \
    if (g_stub_call_count - g_last_heartbeat >= 10000) { \
        fprintf(stderr, "[HEARTBEAT] %llu stub calls\n", (unsigned long long)g_stub_call_count); \
        g_last_heartbeat = g_stub_call_count; \
    } \
} while(0)

// Helper: read a big-endian uint32 from PPC memory
static inline uint32_t ppc_read_u32(uint8_t* base, uint32_t addr)
{
    uint8_t* p = base + addr;
    return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) |
           (uint32_t(p[2]) << 8) | uint32_t(p[3]);
}

// Helper: write a big-endian uint32 to PPC memory
static inline void ppc_write_u32(uint8_t* base, uint32_t addr, uint32_t val)
{
    uint8_t* p = base + addr;
    p[0] = (val >> 24) & 0xFF;
    p[1] = (val >> 16) & 0xFF;
    p[2] = (val >> 8) & 0xFF;
    p[3] = val & 0xFF;
}

// Helper: read a null-terminated string from PPC memory
static inline const char* ppc_string(uint8_t* base, uint32_t addr)
{
    return reinterpret_cast<const char*>(base + addr);
}

// Helper: read a big-endian uint16 from PPC memory
static inline uint16_t ppc_read_u16(uint8_t* base, uint32_t addr)
{
    uint8_t* p = base + addr;
    return (uint16_t(p[0]) << 8) | uint16_t(p[1]);
}

// Helper: write a big-endian uint64 to PPC memory
static inline void ppc_write_u64(uint8_t* base, uint32_t addr, uint64_t val)
{
    ppc_write_u32(base, addr, (uint32_t)(val >> 32));
    ppc_write_u32(base, addr + 4, (uint32_t)(val & 0xFFFFFFFF));
}

// ============================================================================
// Thread management (cooperative fiber-based model)
// ============================================================================

// PPC thread state — each thread has its own PPCContext and PPC stack
struct PendingThread
{
    uint32_t handle;        // handle assigned to this thread
    uint32_t start_routine; // PPC address
    uint32_t start_context; // PPC address passed as r3
    uint32_t api_startup;   // PPC address of ApiThreadStartup wrapper
    bool     suspended;     // created in suspended state
    bool     finished;      // thread function has returned
    bool     started;       // fiber has been created and started
    uint32_t ppc_stack_top; // PPC stack address for this thread
    LPVOID   fiber;         // Windows fiber handle
    PPCContext thread_ctx;  // thread's own PPC register state
    uint8_t* base;          // shared PPC memory base
};

static constexpr int MAX_PENDING_THREADS = 16;
static PendingThread g_pending_threads[MAX_PENDING_THREADS] = {};
static int g_pending_thread_count = 0;

// Fiber globals
LPVOID g_main_fiber = nullptr;                 // main thread's fiber (set in main.cpp)
static PendingThread* g_current_thread = nullptr; // currently running PPC thread (NULL = main)
static int g_current_thread_idx = -1;

// Allocate a PPC stack for a child thread (from the heap region)
static uint32_t g_thread_stack_next = 0x8E000000; // separate region for thread stacks
static constexpr uint32_t THREAD_STACK_SIZE = 256 * 1024; // 256 KB per thread

static uint32_t alloc_thread_stack()
{
    uint32_t top = g_thread_stack_next;
    g_thread_stack_next -= THREAD_STACK_SIZE;
    return top;
}

// Fiber entry point for PPC threads
static void CALLBACK ppc_thread_fiber_proc(LPVOID param)
{
    PendingThread* pt = (PendingThread*)param;
    PPCContext& ctx = pt->thread_ctx;
    uint8_t* base = pt->base;
    int idx = (int)(pt - g_pending_threads);

    uint32_t func_addr = pt->start_routine;
    typedef void (*PPCFuncPtr)(PPCContext& __restrict, uint8_t*);
    PPCFuncPtr fn = PPC_LOOKUP_FUNC(base, func_addr);
    if (fn)
    {
        fprintf(stderr, "[THREAD] Fiber %d starting: routine=0x%08X, context=0x%08X, r1=0x%08X\n",
                idx, func_addr, pt->start_context, ctx.r1.u32);
        fn(ctx, base);
        fprintf(stderr, "[THREAD] Fiber %d returned normally\n", idx);
    }
    else
    {
        fprintf(stderr, "[THREAD] Fiber %d: no function at 0x%08X\n", idx, func_addr);
    }

    pt->finished = true;
    // Switch back to main fiber (thread is done)
    SwitchToFiber(g_main_fiber);
}

// Initialize a thread's PPCContext from the main context template
static void init_thread_ctx(PendingThread& pt, PPCContext& main_ctx)
{
    memset(&pt.thread_ctx, 0, sizeof(PPCContext));
    // Copy key registers from main context
    pt.thread_ctx.r13 = main_ctx.r13;   // KPCR pointer
    pt.thread_ctx.r2 = main_ctx.r2;     // TOC (unused but copy anyway)
    pt.thread_ctx.fpscr.csr = 0x1F80;   // mask FP exceptions
    // Set thread-specific registers
    pt.thread_ctx.r1.u32 = pt.ppc_stack_top - 16; // stack pointer with headroom
    pt.thread_ctx.r3.u32 = pt.start_context;       // first argument
}

// Give a thread a time slice by switching to its fiber
static void thread_give_timeslice(PendingThread& pt, int idx)
{
    if (pt.finished || pt.suspended) return;

    if (!pt.started)
    {
        // First run: create the fiber (context already initialized in ExCreateThread)
        pt.fiber = CreateFiber(0, ppc_thread_fiber_proc, &pt);
        if (!pt.fiber)
        {
            fprintf(stderr, "[THREAD] Failed to create fiber for thread %d\n", idx);
            pt.finished = true;
            return;
        }
        pt.started = true;
        fprintf(stderr, "[THREAD] Created fiber for thread %d (handle=0x%X, stack=0x%08X)\n",
                idx, pt.handle, pt.ppc_stack_top);
    }

    g_current_thread = &pt;
    g_current_thread_idx = idx;
    SwitchToFiber(pt.fiber);
    g_current_thread = nullptr;
    g_current_thread_idx = -1;
}

// Called from yield stubs (KeDelayExecutionThread, etc.) to return to main
static void thread_yield()
{
    if (g_current_thread && g_main_fiber)
    {
        SwitchToFiber(g_main_fiber);
    }
}


// ============================================================================
// C Runtime Functions (sprintf, vsnprintf, DbgPrint)
// ============================================================================

// NOTE: These are simplified stubs. Full implementations would need to handle
// PPC memory pointers in format arguments (e.g., %s with PPC addresses).
// For now they operate on host memory at the PPC addresses.

PPC_FUNC(__imp__sprintf)
{
    // r3 = dest buffer (PPC addr), r4 = format string (PPC addr), r5+ = args
    STUB_LOG("sprintf");
    // Can't easily forward varargs from PPC context to host sprintf.
    // For now, just copy the format string as-is.
    uint32_t dest_addr = ctx.r3.u32;
    uint32_t fmt_addr = ctx.r4.u32;
    const char* fmt = ppc_string(base, fmt_addr);
    char* dest = reinterpret_cast<char*>(base + dest_addr);
    // Simple passthrough - won't handle format specifiers with PPC args correctly
    int len = snprintf(dest, 1024, "%s", fmt);
    ctx.r3.s32 = len;
}

PPC_FUNC(__imp___vsnprintf)
{
    // r3 = dest, r4 = count, r5 = format, r6 = va_list
    STUB_LOG("_vsnprintf");
    uint32_t dest_addr = ctx.r3.u32;
    uint32_t count = ctx.r4.u32;
    uint32_t fmt_addr = ctx.r5.u32;
    const char* fmt = ppc_string(base, fmt_addr);
    char* dest = reinterpret_cast<char*>(base + dest_addr);
    int len = snprintf(dest, count, "%s", fmt);
    ctx.r3.s32 = len;
}

PPC_FUNC(__imp__DbgPrint)
{
    // r3 = format string (PPC addr)
    uint32_t fmt_addr = ctx.r3.u32;
    const char* fmt = ppc_string(base, fmt_addr);
    fprintf(stderr, "[DbgPrint] %s\n", fmt);
    ctx.r3.u32 = 0; // STATUS_SUCCESS
}


// ============================================================================
// Thread-Local Storage (KeTls*)
// ============================================================================

// Simple TLS emulation using a fixed-size array.
// Xbox 360 TLS slots are per-thread; we only support single-threaded for now.
static constexpr int MAX_TLS_SLOTS = 64;
static void* g_tls_slots[MAX_TLS_SLOTS] = {};
static bool g_tls_used[MAX_TLS_SLOTS] = {};

PPC_FUNC(__imp__KeTlsAlloc)
{
    STUB_LOG_ONCE("KeTlsAlloc");
    for (int i = 0; i < MAX_TLS_SLOTS; i++)
    {
        if (!g_tls_used[i])
        {
            g_tls_used[i] = true;
            g_tls_slots[i] = nullptr;
            ctx.r3.u32 = i;
            return;
        }
    }
    ctx.r3.u32 = 0xFFFFFFFF; // TLS_OUT_OF_INDEXES
}

PPC_FUNC(__imp__KeTlsSetValue)
{
    // r3 = index, r4 = value
    uint32_t index = ctx.r3.u32;
    if (index < MAX_TLS_SLOTS)
    {
        g_tls_slots[index] = reinterpret_cast<void*>(static_cast<uintptr_t>(ctx.r4.u32));
        ctx.r3.u32 = 1; // TRUE
    }
    else
    {
        ctx.r3.u32 = 0; // FALSE
    }
}

PPC_FUNC(__imp__KeTlsGetValue)
{
    // r3 = index, returns value in r3
    uint32_t index = ctx.r3.u32;
    if (index < MAX_TLS_SLOTS)
        ctx.r3.u32 = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(g_tls_slots[index]));
    else
        ctx.r3.u32 = 0;
}

PPC_FUNC(__imp__KeTlsFree)
{
    // r3 = index
    uint32_t index = ctx.r3.u32;
    if (index < MAX_TLS_SLOTS)
    {
        g_tls_used[index] = false;
        g_tls_slots[index] = nullptr;
        ctx.r3.u32 = 1; // TRUE
    }
    else
    {
        ctx.r3.u32 = 0; // FALSE
    }
}


// ============================================================================
// Kernel Core (Ke*)
// ============================================================================

PPC_FUNC(__imp__KeGetCurrentProcessType)
{
    STUB_LOG_ONCE("KeGetCurrentProcessType");
    ctx.r3.u32 = 2; // PROC_USER (user-mode process)
}

PPC_FUNC(__imp__KeBugCheck)
{
    fprintf(stderr, "[FATAL] KeBugCheck called! Code: 0x%08X\n", ctx.r3.u32);
    exit(1);
}

PPC_FUNC(__imp__KeBugCheckEx)
{
    fprintf(stderr, "[FATAL] KeBugCheckEx called! Code: 0x%08X (0x%08X, 0x%08X, 0x%08X, 0x%08X)\n",
            ctx.r3.u32, ctx.r4.u32, ctx.r5.u32, ctx.r6.u32, ctx.r7.u32);
    exit(1);
}

PPC_FUNC(__imp__KeQueryPerformanceFrequency)
{
    STUB_LOG_ONCE("KeQueryPerformanceFrequency");
    // Xbox 360 uses 50MHz timebase
    ctx.r3.u32 = 50000000;
}

PPC_FUNC(__imp__KeDelayExecutionThread)
{
    // r3 = processor mode, r4 = alertable, r5 = interval (ptr to LARGE_INTEGER, 100ns units)
    STUB_LOG_ONCE("KeDelayExecutionThread");

    // If we're running in a fiber thread, yield back to main
    if (g_current_thread)
    {
        fprintf(stderr, "[THREAD] Fiber %d yielding via KeDelayExecutionThread\n", g_current_thread_idx);
        thread_yield();
        fprintf(stderr, "[THREAD] Fiber %d resumed from KeDelayExecutionThread\n", g_current_thread_idx);
        ctx.r3.u32 = 0;
        return;
    }

    // Main thread: actually sleep
    uint32_t interval_addr = ctx.r5.u32;
    int64_t interval = 0;
    if (interval_addr)
    {
        uint32_t hi = ppc_read_u32(base, interval_addr);
        uint32_t lo = ppc_read_u32(base, interval_addr + 4);
        interval = (int64_t(hi) << 32) | lo;
    }
    if (interval < 0)
    {
        int ms = (int)(-interval / 10000);
        if (ms > 0)
        {
#ifdef _WIN32
            Sleep(ms);
#else
            usleep(ms * 1000);
#endif
        }
    }
    ctx.r3.u32 = 0; // STATUS_SUCCESS
}

PPC_FUNC(__imp__KeSetAffinityThread)
{
    STUB_LOG_ONCE("KeSetAffinityThread");
    ctx.r3.u32 = 0;
}

PPC_FUNC(__imp__KeSetBasePriorityThread)
{
    STUB_LOG_ONCE("KeSetBasePriorityThread");
    ctx.r3.u32 = 0;
}

PPC_FUNC(__imp__KeResumeThread)
{
    // r3 = PKTHREAD (thread object pointer)
    // On Xbox 360, this is a kernel thread object, not a handle.
    // We treat it as a handle since we assigned handles in ExCreateThread.
    uint32_t thread_ref = ctx.r3.u32;
    fprintf(stderr, "[THREAD] KeResumeThread: ref=0x%08X\n", thread_ref);

    // Search pending threads - try matching by handle
    for (int i = 0; i < g_pending_thread_count; i++)
    {
        PendingThread& pt = g_pending_threads[i];
        if (pt.handle == thread_ref && pt.suspended && !pt.finished)
        {
            fprintf(stderr, "[THREAD] Resuming thread %d (handle=0x%X) via KeResumeThread — giving timeslice\n", i, pt.handle);
            pt.suspended = false;
            if (g_main_fiber)
                thread_give_timeslice(pt, i);
            ctx.r3.u32 = 1; // previous suspend count
            return;
        }
    }

    // Not found by handle - maybe the game passed something else.
    // Log and succeed silently.
    fprintf(stderr, "[THREAD] KeResumeThread: no matching suspended thread for ref=0x%08X\n", thread_ref);
    ctx.r3.u32 = 0;
}

PPC_FUNC(__imp__KeSetEvent)
{
    STUB_LOG_ONCE("KeSetEvent");
    ctx.r3.u32 = 0;
}

PPC_FUNC(__imp__KeResetEvent)
{
    STUB_LOG_ONCE("KeResetEvent");
    ctx.r3.u32 = 0;
}

PPC_FUNC(__imp__KeWaitForSingleObject)
{
    STUB_LOG_ONCE("KeWaitForSingleObject");
    STUB_HEARTBEAT();
    if (g_current_thread) thread_yield();
    ctx.r3.u32 = 0; // STATUS_WAIT_0
}

PPC_FUNC(__imp__KeWaitForMultipleObjects)
{
    STUB_LOG_ONCE("KeWaitForMultipleObjects");
    if (g_current_thread) thread_yield();
    ctx.r3.u32 = 0; // STATUS_WAIT_0
}

PPC_FUNC(__imp__KeInitializeSemaphore)
{
    STUB_LOG_ONCE("KeInitializeSemaphore");
}

PPC_FUNC(__imp__KeReleaseSemaphore)
{
    STUB_LOG_ONCE("KeReleaseSemaphore");
    if (g_current_thread) thread_yield();
    ctx.r3.u32 = 0;
}

PPC_FUNC(__imp__KeInitializeApc)
{
    STUB_LOG_ONCE("KeInitializeApc");
}

PPC_FUNC(__imp__KeInsertQueueApc)
{
    STUB_LOG_ONCE("KeInsertQueueApc");
    ctx.r3.u32 = 1; // TRUE
}

PPC_FUNC(__imp__KeEnterCriticalRegion)
{
    // No-op in single-threaded mode
}

PPC_FUNC(__imp__KeLeaveCriticalRegion)
{
    // No-op in single-threaded mode
}

PPC_FUNC(__imp__KeRaiseIrqlToDpcLevel)
{
    STUB_LOG_ONCE("KeRaiseIrqlToDpcLevel");
    ctx.r3.u32 = 0; // Old IRQL
}

PPC_FUNC(__imp__KfLowerIrql)
{
    // No-op
}

PPC_FUNC(__imp__KeLockL2)
{
    STUB_LOG_ONCE("KeLockL2");
}

PPC_FUNC(__imp__KeUnlockL2)
{
    STUB_LOG_ONCE("KeUnlockL2");
}


// ============================================================================
// Spinlocks (Kf*/Ke* spinlock)
// ============================================================================

PPC_FUNC(__imp__KfAcquireSpinLock)
{
    // r3 = spinlock addr, returns old IRQL in r3
    ctx.r3.u32 = 0; // Old IRQL = PASSIVE_LEVEL
}

PPC_FUNC(__imp__KfReleaseSpinLock)
{
    // r3 = spinlock addr, r4 = old IRQL
    // No-op in single-threaded mode
}

PPC_FUNC(__imp__KeAcquireSpinLockAtRaisedIrql)
{
    // No-op in single-threaded mode
}

PPC_FUNC(__imp__KeReleaseSpinLockFromRaisedIrql)
{
    // No-op in single-threaded mode
}

PPC_FUNC(__imp__KiApcNormalRoutineNop)
{
    // No-op APC routine
}


// ============================================================================
// Critical Sections (Rtl*)
// ============================================================================

PPC_FUNC(__imp__RtlInitializeCriticalSection)
{
    STUB_LOG_ONCE("RtlInitializeCriticalSection");
    // r3 = pointer to CRITICAL_SECTION in PPC memory
    // Zero-init is enough for our single-threaded stub
    ctx.r3.u32 = 0;
}

PPC_FUNC(__imp__RtlInitializeCriticalSectionAndSpinCount)
{
    STUB_LOG_ONCE("RtlInitializeCriticalSectionAndSpinCount");
    ctx.r3.u32 = 0;
}

PPC_FUNC(__imp__RtlEnterCriticalSection)
{
    STUB_HEARTBEAT();
    // No-op in single-threaded mode
    ctx.r3.u32 = 0;
}

PPC_FUNC(__imp__RtlLeaveCriticalSection)
{
    // No-op in single-threaded mode
    ctx.r3.u32 = 0;
}

PPC_FUNC(__imp__RtlTryEnterCriticalSection)
{
    ctx.r3.u32 = 1; // TRUE = acquired
}


// ============================================================================
// RTL Utility Functions
// ============================================================================

PPC_FUNC(__imp__RtlFillMemoryUlong)
{
    // r3 = dest, r4 = length (in bytes), r5 = pattern (ULONG)
    uint32_t dest = ctx.r3.u32;
    uint32_t length = ctx.r4.u32;
    uint32_t pattern = ctx.r5.u32;
    for (uint32_t i = 0; i + 4 <= length; i += 4)
        ppc_write_u32(base, dest + i, pattern);
}

PPC_FUNC(__imp__RtlCompareMemoryUlong)
{
    // r3 = source, r4 = length, r5 = pattern
    // Returns number of bytes that matched
    uint32_t src = ctx.r3.u32;
    uint32_t length = ctx.r4.u32;
    uint32_t pattern = ctx.r5.u32;
    uint32_t matched = 0;
    for (uint32_t i = 0; i + 4 <= length; i += 4)
    {
        if (ppc_read_u32(base, src + i) == pattern)
            matched += 4;
        else
            break;
    }
    ctx.r3.u32 = matched;
}

PPC_FUNC(__imp__RtlInitAnsiString)
{
    // r3 = ANSI_STRING* dest, r4 = source PCSTR
    STUB_LOG_ONCE("RtlInitAnsiString");
    uint32_t str_ptr = ctx.r4.u32;
    uint16_t len = 0;
    if (str_ptr)
    {
        const char* s = ppc_string(base, str_ptr);
        len = (uint16_t)strlen(s);
    }
    // ANSI_STRING: Length (2), MaximumLength (2), Buffer (4)
    uint32_t dest = ctx.r3.u32;
    // Write in big-endian
    base[dest] = (len >> 8) & 0xFF;
    base[dest + 1] = len & 0xFF;
    uint16_t maxlen = len + 1;
    base[dest + 2] = (maxlen >> 8) & 0xFF;
    base[dest + 3] = maxlen & 0xFF;
    ppc_write_u32(base, dest + 4, str_ptr);
}

PPC_FUNC(__imp__RtlMultiByteToUnicodeN)
{
    // r3 = UnicodeString, r4 = MaxBytesInUnicodeString, r5 = BytesInUnicodeString (out ptr),
    // r6 = MultiByteString, r7 = BytesInMultiByteString
    STUB_LOG_ONCE("RtlMultiByteToUnicodeN");
    uint32_t uni_addr = ctx.r3.u32;
    uint32_t max_bytes = ctx.r4.u32;
    uint32_t out_size_addr = ctx.r5.u32;
    uint32_t mb_addr = ctx.r6.u32;
    uint32_t mb_len = ctx.r7.u32;

    // Simple ASCII→UTF-16LE conversion (one byte → two bytes)
    uint32_t chars = mb_len;
    if (chars * 2 > max_bytes) chars = max_bytes / 2;
    for (uint32_t i = 0; i < chars; i++)
    {
        base[uni_addr + i * 2] = base[mb_addr + i];
        base[uni_addr + i * 2 + 1] = 0;
    }
    if (out_size_addr)
        ppc_write_u32(base, out_size_addr, chars * 2);
    ctx.r3.u32 = 0;
}

PPC_FUNC(__imp__RtlUnicodeToMultiByteN)
{
    STUB_LOG_ONCE("RtlUnicodeToMultiByteN");
    uint32_t mb_addr = ctx.r3.u32;
    uint32_t max_bytes = ctx.r4.u32;
    uint32_t out_size_addr = ctx.r5.u32;
    uint32_t uni_addr = ctx.r6.u32;
    uint32_t uni_len = ctx.r7.u32;

    uint32_t chars = uni_len / 2;
    if (chars > max_bytes) chars = max_bytes;
    for (uint32_t i = 0; i < chars; i++)
        base[mb_addr + i] = base[uni_addr + i * 2]; // Take low byte
    if (out_size_addr)
        ppc_write_u32(base, out_size_addr, chars);
    ctx.r3.u32 = 0;
}

PPC_FUNC(__imp__RtlNtStatusToDosError)
{
    // r3 = NTSTATUS, returns DOS error
    uint32_t status = ctx.r3.u32;
    uint32_t dos_err;
    switch (status)
    {
    case 0x00000000: dos_err = 0; break;    // STATUS_SUCCESS -> ERROR_SUCCESS
    case 0x80000005: dos_err = 234; break;  // STATUS_BUFFER_OVERFLOW -> ERROR_MORE_DATA
    case 0x80000006: dos_err = 18; break;   // STATUS_NO_MORE_FILES -> ERROR_NO_MORE_FILES
    case 0xC0000008: dos_err = 6; break;    // STATUS_INVALID_HANDLE -> ERROR_INVALID_HANDLE
    case 0xC0000017: dos_err = 8; break;    // STATUS_NO_MEMORY -> ERROR_NOT_ENOUGH_MEMORY
    case 0xC0000034: dos_err = 2; break;    // STATUS_OBJECT_NAME_NOT_FOUND -> ERROR_FILE_NOT_FOUND
    case 0xC000003A: dos_err = 3; break;    // STATUS_OBJECT_PATH_NOT_FOUND -> ERROR_PATH_NOT_FOUND
    case 0xC0000035: dos_err = 183; break;  // STATUS_OBJECT_NAME_COLLISION -> ERROR_ALREADY_EXISTS
    case 0xC000009A: dos_err = 8; break;    // STATUS_INSUFFICIENT_RESOURCES -> ERROR_NOT_ENOUGH_MEMORY
    case 0xC0000003: dos_err = 87; break;   // STATUS_INVALID_INFO_CLASS -> ERROR_INVALID_PARAMETER
    default:
        fprintf(stderr, "[STUB] RtlNtStatusToDosError: unmapped NTSTATUS 0x%08X\n", status);
        dos_err = 1; // ERROR_INVALID_FUNCTION (fallback)
        break;
    }
    ctx.r3.u32 = dos_err;
}

PPC_FUNC(__imp__RtlUnwind)
{
    STUB_LOG("RtlUnwind");
    // Exception handling - log and do nothing for now
}

PPC_FUNC(__imp__RtlCaptureContext)
{
    STUB_LOG("RtlCaptureContext");
    // Exception handling - log and do nothing for now
}

PPC_FUNC(__imp__RtlRaiseException)
{
    fprintf(stderr, "[STUB] RtlRaiseException called! ExceptionCode unknown\n");
    // Don't abort - some games use SEH for flow control
    ctx.r3.u32 = 0;
}

PPC_FUNC(__imp__RtlImageXexHeaderField)
{
    // r3 = XEX header base, r4 = field key
    STUB_LOG("RtlImageXexHeaderField");
    ctx.r3.u32 = 0; // Field not found
}

PPC_FUNC(__imp____C_specific_handler)
{
    // C structured exception handler - used by MSVC exception handling
    STUB_LOG("__C_specific_handler");
    ctx.r3.u32 = 0;
}


// ============================================================================
// NT Kernel - Memory Management
// ============================================================================

// Watchpoint: track when 0x8200185C changes
static uint32_t g_watch_last = 0;
static bool g_watch_init = false;
static void check_watchpoint(uint8_t* base, const char* where)
{
    uint32_t val = ppc_read_u32(base, 0x8200185C);
    if (!g_watch_init)
    {
        g_watch_init = true;
        g_watch_last = val;
        fprintf(stderr, "[WATCH] Initial value at 0x8200185C = 0x%08X (%s)\n", val, where);
    }
    else if (val != g_watch_last)
    {
        fprintf(stderr, "[WATCH] 0x8200185C CHANGED: 0x%08X -> 0x%08X at %s\n",
                g_watch_last, val, where);
        g_watch_last = val;
    }
}

// Simple bump allocator for NtAllocateVirtualMemory
// Allocations start at 0xA0000000 in the PPC address space
static uint32_t g_heap_next = 0xA0000000;
static constexpr uint32_t g_heap_end = 0xB0000000; // 256 MB heap space

PPC_FUNC(__imp__NtAllocateVirtualMemory)
{
    // r3 = BaseAddress* (in/out), r4 = RegionSize* (in/out), r5 = AllocationType, r6 = Protect
    STUB_LOG("NtAllocateVirtualMemory");
    check_watchpoint(base, "NtAllocateVirtualMemory:entry");
    uint32_t base_ptr = ctx.r3.u32;
    uint32_t size_ptr = ctx.r4.u32;
    uint32_t size = ppc_read_u32(base, size_ptr);
    // Align to 4K page
    size = (size + 0xFFF) & ~0xFFFu;

    if (g_heap_next + size <= g_heap_end)
    {
        uint32_t addr = g_heap_next;
        g_heap_next += size;
        ppc_write_u32(base, base_ptr, addr);
        ppc_write_u32(base, size_ptr, size);
        // Zero the allocated memory (it's already committed via VirtualAlloc on first touch)
        memset(base + addr, 0, size);
        fprintf(stderr, "[MEM] NtAllocateVirtualMemory: 0x%08X (%u bytes)\n", addr, size);
        ctx.r3.u32 = 0; // STATUS_SUCCESS
    }
    else
    {
        fprintf(stderr, "[MEM] NtAllocateVirtualMemory: FAILED (out of heap space)\n");
        ctx.r3.u32 = 0xC0000017; // STATUS_NO_MEMORY
    }
}

PPC_FUNC(__imp__NtFreeVirtualMemory)
{
    STUB_LOG_ONCE("NtFreeVirtualMemory");
    // Can't easily free from bump allocator - just succeed
    ctx.r3.u32 = 0;
}

PPC_FUNC(__imp__NtQueryVirtualMemory)
{
    STUB_LOG("NtQueryVirtualMemory");
    ctx.r3.u32 = 0;
}

PPC_FUNC(__imp__MmAllocatePhysicalMemoryEx)
{
    // r3 = type, r4 = size, r5 = protect, r6 = min addr, r7 = max addr, r8 = alignment
    STUB_LOG("MmAllocatePhysicalMemoryEx");
    check_watchpoint(base, "MmAllocatePhysicalMemoryEx:entry");
    uint32_t size = ctx.r4.u32;
    size = (size + 0xFFF) & ~0xFFFu;
    if (g_heap_next + size <= g_heap_end)
    {
        uint32_t addr = g_heap_next;
        g_heap_next += size;
        memset(base + addr, 0, size);
        fprintf(stderr, "[MEM] MmAllocatePhysicalMemoryEx: 0x%08X (%u bytes)\n", addr, size);
        ctx.r3.u32 = addr;
    }
    else
    {
        ctx.r3.u32 = 0; // NULL = failure
    }
}

PPC_FUNC(__imp__MmFreePhysicalMemory)
{
    STUB_LOG_ONCE("MmFreePhysicalMemory");
    // Can't reclaim from bump allocator
}

PPC_FUNC(__imp__MmGetPhysicalAddress)
{
    // r3 = virtual address, returns physical address
    // In our flat model, physical = virtual
    STUB_LOG_ONCE("MmGetPhysicalAddress");
    // Return low 32 bits of address as the "physical" address
    // Return value is LARGE_INTEGER (64-bit): r3 = high, r4 = low on PPC?
    // Actually Xbox 360 returns it in r3 as a 32-bit physical address
    // ctx.r3 already contains the virtual address, leave as-is
}

PPC_FUNC(__imp__MmQueryAddressProtect)
{
    STUB_LOG_ONCE("MmQueryAddressProtect");
    ctx.r3.u32 = 0x04; // PAGE_READWRITE
}


// ============================================================================
// NT Kernel - File I/O: Handle Table & Path Translation
// ============================================================================

enum HandleType { HANDLE_NONE = 0, HANDLE_FILE, HANDLE_DIRECTORY };

struct DirEntry
{
    std::string name;
    int64_t     size;
    bool        is_directory;
};

struct HandleEntry
{
    HandleType type;
    FILE*      fp;
    std::string host_path;
    int64_t    file_size;
    // Directory enumeration state
    std::vector<DirEntry> dir_entries;
    size_t dir_index;
};

static constexpr int    MAX_FILE_HANDLES = 128;
static constexpr uint32_t FILE_HANDLE_BASE = 0x1000;
static HandleEntry g_file_handles[MAX_FILE_HANDLES] = {};

static int handle_alloc(HandleType type, FILE* fp, const std::string& path, int64_t size)
{
    for (int i = 0; i < MAX_FILE_HANDLES; i++)
    {
        if (g_file_handles[i].type == HANDLE_NONE)
        {
            g_file_handles[i].type = type;
            g_file_handles[i].fp = fp;
            g_file_handles[i].host_path = path;
            g_file_handles[i].file_size = size;
            g_file_handles[i].dir_entries.clear();
            g_file_handles[i].dir_index = 0;
            return i;
        }
    }
    return -1; // no free slots
}

static HandleEntry* handle_lookup(uint32_t handle)
{
    if (handle < FILE_HANDLE_BASE) return nullptr;
    int idx = (int)(handle - FILE_HANDLE_BASE);
    if (idx < 0 || idx >= MAX_FILE_HANDLES) return nullptr;
    if (g_file_handles[idx].type == HANDLE_NONE) return nullptr;
    return &g_file_handles[idx];
}

static void handle_free(uint32_t handle)
{
    if (handle < FILE_HANDLE_BASE) return;
    int idx = (int)(handle - FILE_HANDLE_BASE);
    if (idx < 0 || idx >= MAX_FILE_HANDLES) return;
    g_file_handles[idx].type = HANDLE_NONE;
    g_file_handles[idx].fp = nullptr;
    g_file_handles[idx].host_path.clear();
    g_file_handles[idx].file_size = 0;
    g_file_handles[idx].dir_entries.clear();
    g_file_handles[idx].dir_index = 0;
}

// Parse ANSI_STRING from Xbox 360 OBJECT_ATTRIBUTES
// X_OBJECT_ATTRIBUTES: +0x00 RootDirectory(u32), +0x04 ObjectName*(u32), +0x08 Attributes(u32)
// ANSI_STRING: +0x00 Length(u16), +0x02 MaxLength(u16), +0x04 Buffer(u32)
static std::string parse_object_name(uint8_t* base, uint32_t oa_addr)
{
    if (!oa_addr) return "";
    uint32_t name_str_ptr = ppc_read_u32(base, oa_addr + 0x04);
    if (!name_str_ptr) return "";
    uint16_t name_len = ppc_read_u16(base, name_str_ptr);
    uint32_t buf_addr = ppc_read_u32(base, name_str_ptr + 0x04);
    if (!buf_addr || name_len == 0 || name_len >= 512) return "";
    return std::string(reinterpret_cast<const char*>(base + buf_addr), name_len);
}

// Map Xbox 360 paths to host filesystem paths
// "game:\..." -> "extracted/..."
// "game:..." -> "extracted/..."
static std::string xbox_path_to_host(const std::string& xbox_path)
{
    std::string path = xbox_path;
    // Normalize backslashes to forward slashes
    for (auto& c : path)
        if (c == '\\') c = '/';

    // Map "game:" prefix to "extracted/"
    if (path.size() >= 5 && (path.substr(0, 5) == "game:" || path.substr(0, 5) == "GAME:"))
    {
        path = path.substr(5);
        // Remove leading slash if present
        if (!path.empty() && path[0] == '/')
            path = path.substr(1);
        path = "extracted/" + path;
    }

    return path;
}


// ============================================================================
// NT Kernel - File I/O
// ============================================================================

// Shared implementation for NtOpenFile and NtCreateFile
// r3=FileHandle*, r4=DesiredAccess, r5=ObjectAttributes*, r6=IoStatusBlock*
static void NtOpenFile_impl(PPCContext& __restrict ctx, uint8_t* base)
{
    check_watchpoint(base, "NtOpenFile:entry");
    uint32_t handle_out_addr = ctx.r3.u32;
    uint32_t oa_addr = ctx.r5.u32;
    uint32_t iosb_addr = ctx.r6.u32;

    std::string xbox_name = parse_object_name(base, oa_addr);
    if (xbox_name.empty())
    {
        fprintf(stderr, "[FILE] NtOpenFile: (empty name)\n");
        ctx.r3.u32 = 0xC0000034; // STATUS_OBJECT_NAME_NOT_FOUND
        return;
    }

    std::string host_path = xbox_path_to_host(xbox_name);
    fprintf(stderr, "[FILE] NtOpenFile: \"%s\" -> \"%s\"\n", xbox_name.c_str(), host_path.c_str());

    // Detect directory open: trailing slash or host path is a directory
    bool is_dir = false;
    if (!host_path.empty() && host_path.back() == '/')
        is_dir = true;

#ifdef _WIN32
    DWORD attrs = GetFileAttributesA(host_path.c_str());
    if (attrs != INVALID_FILE_ATTRIBUTES && (attrs & FILE_ATTRIBUTE_DIRECTORY))
        is_dir = true;
    bool exists = (attrs != INVALID_FILE_ATTRIBUTES);
#else
    struct stat st;
    bool exists = (stat(host_path.c_str(), &st) == 0);
    if (exists && S_ISDIR(st.st_mode))
        is_dir = true;
#endif

    if (!exists)
    {
        // Try without trailing slash for directories
        std::string trimmed = host_path;
        while (!trimmed.empty() && trimmed.back() == '/')
            trimmed.pop_back();
        if (!trimmed.empty() && trimmed != host_path)
        {
#ifdef _WIN32
            attrs = GetFileAttributesA(trimmed.c_str());
            exists = (attrs != INVALID_FILE_ATTRIBUTES);
            if (exists && (attrs & FILE_ATTRIBUTE_DIRECTORY))
                is_dir = true;
#else
            exists = (stat(trimmed.c_str(), &st) == 0);
            if (exists && S_ISDIR(st.st_mode))
                is_dir = true;
#endif
            if (exists) host_path = trimmed;
        }
    }

    if (!exists)
    {
        fprintf(stderr, "[FILE]   -> NOT FOUND\n");
        ctx.r3.u32 = 0xC0000034; // STATUS_OBJECT_NAME_NOT_FOUND
        return;
    }

    if (is_dir)
    {
        int slot = handle_alloc(HANDLE_DIRECTORY, nullptr, host_path, 0);
        if (slot < 0)
        {
            fprintf(stderr, "[FILE]   -> no free handle slots!\n");
            ctx.r3.u32 = 0xC000009A; // STATUS_INSUFFICIENT_RESOURCES
            return;
        }

        // Enumerate directory contents at open time
        HandleEntry& he = g_file_handles[slot];
#ifdef _WIN32
        // Ensure path doesn't end with slash for FindFirstFile
        std::string search = host_path;
        while (!search.empty() && search.back() == '/')
            search.pop_back();
        search += "/*";
        WIN32_FIND_DATAA fd;
        HANDLE hFind = FindFirstFileA(search.c_str(), &fd);
        if (hFind != INVALID_HANDLE_VALUE)
        {
            do {
                if (strcmp(fd.cFileName, ".") == 0 || strcmp(fd.cFileName, "..") == 0)
                    continue;
                DirEntry de;
                de.name = fd.cFileName;
                de.size = ((int64_t)fd.nFileSizeHigh << 32) | fd.nFileSizeLow;
                de.is_directory = (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
                he.dir_entries.push_back(de);
            } while (FindNextFileA(hFind, &fd));
            FindClose(hFind);
        }
#else
        DIR* d = opendir(host_path.c_str());
        if (d)
        {
            struct dirent* ent;
            while ((ent = readdir(d)) != nullptr)
            {
                if (strcmp(ent->d_name, ".") == 0 || strcmp(ent->d_name, "..") == 0)
                    continue;
                DirEntry de;
                de.name = ent->d_name;
                de.is_directory = (ent->d_type == DT_DIR);
                // Get file size
                std::string full = host_path + "/" + de.name;
                struct stat st;
                if (stat(full.c_str(), &st) == 0)
                    de.size = st.st_size;
                else
                    de.size = 0;
                he.dir_entries.push_back(de);
            }
            closedir(d);
        }
#endif
        fprintf(stderr, "[FILE]   -> directory handle 0x%X (%zu entries)\n",
                FILE_HANDLE_BASE + (uint32_t)slot, he.dir_entries.size());

        uint32_t handle = FILE_HANDLE_BASE + (uint32_t)slot;
        ppc_write_u32(base, handle_out_addr, handle);
        if (iosb_addr)
        {
            ppc_write_u32(base, iosb_addr, 0);
            ppc_write_u32(base, iosb_addr + 4, 1); // FILE_OPENED
        }
        ctx.r3.u32 = 0; // STATUS_SUCCESS
        return;
    }

    // Regular file
    FILE* fp = fopen(host_path.c_str(), "rb");
    if (!fp)
    {
        fprintf(stderr, "[FILE]   -> fopen failed\n");
        ctx.r3.u32 = 0xC0000034;
        return;
    }

    // Get file size
    _fseeki64(fp, 0, SEEK_END);
    int64_t fsize = _ftelli64(fp);
    _fseeki64(fp, 0, SEEK_SET);

    int slot = handle_alloc(HANDLE_FILE, fp, host_path, fsize);
    if (slot < 0)
    {
        fclose(fp);
        fprintf(stderr, "[FILE]   -> no free handle slots!\n");
        ctx.r3.u32 = 0xC000009A;
        return;
    }
    uint32_t handle = FILE_HANDLE_BASE + (uint32_t)slot;
    ppc_write_u32(base, handle_out_addr, handle);
    if (iosb_addr)
    {
        ppc_write_u32(base, iosb_addr, 0);
        ppc_write_u32(base, iosb_addr + 4, 1); // FILE_OPENED
    }
    fprintf(stderr, "[FILE]   -> file handle 0x%X (size=%lld)\n", handle, (long long)fsize);
    ctx.r3.u32 = 0; // STATUS_SUCCESS
}

PPC_FUNC(__imp__NtOpenFile)
{
    NtOpenFile_impl(ctx, base);
}

PPC_FUNC(__imp__NtCreateFile)
{
    // NtCreateFile has same first 4 args as NtOpenFile (r3-r6)
    NtOpenFile_impl(ctx, base);
}

PPC_FUNC(__imp__NtReadFile)
{
    // r3=FileHandle, r4=Event, r5=ApcRoutine, r6=ApcContext,
    // r7=IoStatusBlock*, r8=Buffer(PPC addr), r9=Length, r10=ByteOffset*
    uint32_t handle_val = ctx.r3.u32;
    uint32_t iosb_addr = ctx.r7.u32;
    uint32_t buf_addr = ctx.r8.u32;
    uint32_t length = ctx.r9.u32;
    uint32_t offset_ptr = ctx.r10.u32;

    HandleEntry* entry = handle_lookup(handle_val);
    if (!entry || entry->type != HANDLE_FILE || !entry->fp)
    {
        fprintf(stderr, "[FILE] NtReadFile: invalid handle 0x%X\n", handle_val);
        ctx.r3.u32 = 0xC0000008; // STATUS_INVALID_HANDLE
        return;
    }

    // Seek if ByteOffset provided
    if (offset_ptr)
    {
        uint32_t off_hi = ppc_read_u32(base, offset_ptr);
        uint32_t off_lo = ppc_read_u32(base, offset_ptr + 4);
        int64_t offset = ((int64_t)off_hi << 32) | off_lo;
        _fseeki64(entry->fp, offset, SEEK_SET);
    }

    // Check if the read would overwrite .rdata or other PE sections
    if (buf_addr >= 0x82000000 && buf_addr < 0x82090000)
    {
        fprintf(stderr, "[FILE] NtReadFile: WARNING: buffer 0x%08X is in PE data section!\n", buf_addr);
    }

    // Read directly into PPC memory (raw bytes, no endian swap needed for file data)
    // Snapshot the watchpoint before the read
    uint32_t watch_addr = 0x8200185C;
    uint32_t watch_before = ppc_read_u32(base, watch_addr);
    size_t bytes_read = fread(base + buf_addr, 1, length, entry->fp);
    uint32_t watch_after = ppc_read_u32(base, watch_addr);
    if (watch_before != watch_after)
    {
        fprintf(stderr, "[WATCHPOINT] 0x%08X changed from 0x%08X to 0x%08X during NtReadFile "
                "(buf=0x%08X, len=%u, file=%s)\n",
                watch_addr, watch_before, watch_after, buf_addr, length, entry->host_path.c_str());
    }

    // Fill IO_STATUS_BLOCK
    if (iosb_addr)
    {
        ppc_write_u32(base, iosb_addr, 0); // Status = SUCCESS
        ppc_write_u32(base, iosb_addr + 4, (uint32_t)bytes_read);
    }

    fprintf(stderr, "[FILE] NtReadFile: handle=0x%X, buf=0x%08X, requested=%u, read=%zu\n",
            handle_val, buf_addr, length, bytes_read);
    ctx.r3.u32 = 0; // STATUS_SUCCESS
}

PPC_FUNC(__imp__NtReadFileScatter)
{
    STUB_LOG("NtReadFileScatter");
    ctx.r3.u32 = 0xC0000008;
}

PPC_FUNC(__imp__NtWriteFile)
{
    // Stub: claim all bytes written successfully
    uint32_t iosb_addr = ctx.r7.u32;
    uint32_t length = ctx.r9.u32;
    if (iosb_addr)
    {
        ppc_write_u32(base, iosb_addr, 0); // STATUS_SUCCESS
        ppc_write_u32(base, iosb_addr + 4, length);
    }
    STUB_LOG_ONCE("NtWriteFile");
    ctx.r3.u32 = 0; // STATUS_SUCCESS
}

PPC_FUNC(__imp__NtQueryInformationFile)
{
    // r3=FileHandle, r4=IoStatusBlock*, r5=FileInformation*, r6=Length, r7=FileInformationClass
    uint32_t handle_val = ctx.r3.u32;
    uint32_t iosb_addr = ctx.r4.u32;
    uint32_t info_addr = ctx.r5.u32;
    uint32_t info_len = ctx.r6.u32;
    uint32_t info_class = ctx.r7.u32;

    HandleEntry* entry = handle_lookup(handle_val);
    if (!entry)
    {
        fprintf(stderr, "[FILE] NtQueryInformationFile: invalid handle 0x%X\n", handle_val);
        ctx.r3.u32 = 0xC0000008; // STATUS_INVALID_HANDLE
        return;
    }

    fprintf(stderr, "[FILE] NtQueryInformationFile: handle=0x%X, class=%u\n", handle_val, info_class);

    switch (info_class)
    {
    case 5: // FileStandardInformation
    {
        // AllocationSize(u64), EndOfFile(u64), NumberOfLinks(u32), DeletePending(u8), Directory(u8)
        if (info_len >= 24)
        {
            memset(base + info_addr, 0, 24);
            int64_t sz = entry->file_size;
            ppc_write_u64(base, info_addr + 0, (uint64_t)sz); // AllocationSize
            ppc_write_u64(base, info_addr + 8, (uint64_t)sz); // EndOfFile
            ppc_write_u32(base, info_addr + 16, 1);           // NumberOfLinks
            base[info_addr + 20] = 0;                         // DeletePending
            base[info_addr + 21] = (entry->type == HANDLE_DIRECTORY) ? 1 : 0;
        }
        if (iosb_addr)
        {
            ppc_write_u32(base, iosb_addr, 0);
            ppc_write_u32(base, iosb_addr + 4, 24);
        }
        ctx.r3.u32 = 0;
        break;
    }
    case 14: // FilePositionInformation
    {
        // CurrentByteOffset(u64)
        int64_t pos = 0;
        if (entry->fp)
            pos = _ftelli64(entry->fp);
        if (info_len >= 8)
            ppc_write_u64(base, info_addr, (uint64_t)pos);
        if (iosb_addr)
        {
            ppc_write_u32(base, iosb_addr, 0);
            ppc_write_u32(base, iosb_addr + 4, 8);
        }
        ctx.r3.u32 = 0;
        break;
    }
    case 34: // FileNetworkOpenInformation
    {
        // CreationTime(u64), LastAccessTime(u64), LastWriteTime(u64), ChangeTime(u64),
        // AllocationSize(u64), EndOfFile(u64), FileAttributes(u32)
        if (info_len >= 56)
        {
            memset(base + info_addr, 0, 56);
            int64_t sz = entry->file_size;
            ppc_write_u64(base, info_addr + 32, (uint64_t)sz); // AllocationSize
            ppc_write_u64(base, info_addr + 40, (uint64_t)sz); // EndOfFile
            uint32_t attrs = (entry->type == HANDLE_DIRECTORY) ? 0x10 : 0x80; // DIR or NORMAL
            ppc_write_u32(base, info_addr + 48, attrs);
        }
        if (iosb_addr)
        {
            ppc_write_u32(base, iosb_addr, 0);
            ppc_write_u32(base, iosb_addr + 4, 56);
        }
        ctx.r3.u32 = 0;
        break;
    }
    default:
        fprintf(stderr, "[FILE]   -> unsupported info class %u\n", info_class);
        ctx.r3.u32 = 0xC0000003; // STATUS_INVALID_INFO_CLASS
        break;
    }
}

PPC_FUNC(__imp__NtSetInformationFile)
{
    STUB_LOG("NtSetInformationFile");
    ctx.r3.u32 = 0xC0000008;
}

PPC_FUNC(__imp__NtQueryVolumeInformationFile)
{
    STUB_LOG("NtQueryVolumeInformationFile");
    ctx.r3.u32 = 0xC0000008;
}

PPC_FUNC(__imp__NtQueryDirectoryFile)
{
    // Xbox 360 NtQueryDirectoryFile parameter layout (determined by register probing):
    // r3=FileHandle, r4=Event(NULL), r5=ApcRoutine(NULL), r6=ApcContext(NULL),
    // r7=IoStatusBlock*, r8=FileInformation*, r9=Length,
    // r10=pointer whose first u32 is FileInformationClass
    uint32_t handle_val = ctx.r3.u32;
    uint32_t iosb_addr = ctx.r7.u32;
    uint32_t info_buf = ctx.r8.u32;
    uint32_t info_len = ctx.r9.u32;

    // Xbox 360 NtQueryDirectoryFile parameter layout (from PPC code analysis):
    //   r3=FileHandle, r4=Event(0), r5=ApcRoutine(0), r6=ApcContext(0),
    //   r7=IoStatusBlock*, r8=FileInformation*, r9=Length,
    //   r10=FileName (ANSI_STRING* filter, or NULL for continuation)
    //
    // Xbox 360 always uses FileDirectoryInformation (class 1) with 0x40 header.
    // r10 is the filename filter, NOT the FileInformationClass.
    // (Confirmed: Xenia only implements X_FILE_DIRECTORY_INFORMATION for class 1)
    uint32_t info_class = 1; // Always class 1 on Xbox 360

    HandleEntry* entry = handle_lookup(handle_val);
    if (!entry || entry->type != HANDLE_DIRECTORY)
    {
        fprintf(stderr, "[FILE] NtQueryDirectoryFile: invalid dir handle 0x%X\n", handle_val);
        ctx.r3.u32 = 0xC0000008; // STATUS_INVALID_HANDLE
        return;
    }

    if (entry->dir_index >= entry->dir_entries.size())
    {
        if (iosb_addr)
        {
            ppc_write_u32(base, iosb_addr, 0x80000006);
            ppc_write_u32(base, iosb_addr + 4, 0);
        }
        ctx.r3.u32 = 0x80000006; // STATUS_NO_MORE_FILES
        return;
    }

    // FileDirectoryInformation (class 1): 0x40 header, then ANSI filename
    uint32_t header_size = 0x40;

    // Pack as many entries as fit in the output buffer
    uint32_t offset = 0;
    uint32_t prev_entry_offset_pos = 0; // position of last NextEntryOffset field
    bool first = true;
    uint32_t entries_written = 0;

    while (entry->dir_index < entry->dir_entries.size())
    {
        const DirEntry& de = entry->dir_entries[entry->dir_index];
        uint32_t name_len = (uint32_t)de.name.size();
        uint32_t entry_size = header_size + name_len;
        // Align to 8 bytes for next entry
        uint32_t aligned_size = (entry_size + 7) & ~7u;

        if (offset + entry_size > info_len)
        {
            if (first)
            {
                // Buffer too small for even one entry
                ctx.r3.u32 = 0x80000005; // STATUS_BUFFER_OVERFLOW
                return;
            }
            break; // no room for more entries
        }

        uint32_t entry_addr = info_buf + offset;
        // Zero the entry area
        memset(base + entry_addr, 0, (offset + aligned_size <= info_len) ? aligned_size : entry_size);

        // Link previous entry to this one
        if (!first)
            ppc_write_u32(base, prev_entry_offset_pos, offset - (prev_entry_offset_pos - info_buf));

        prev_entry_offset_pos = entry_addr; // NextEntryOffset is at +0x00

        // X_FILE_DIRECTORY_INFORMATION (class 1, Xbox 360 layout):
        // +0x00 NextEntryOffset (u32) - filled later or 0
        ppc_write_u32(base, entry_addr + 0x00, 0);
        // +0x04 FileIndex (u32)
        ppc_write_u32(base, entry_addr + 0x04, (uint32_t)entry->dir_index);
        // +0x08 CreationTime (u64) - zeroed
        // +0x10 LastAccessTime (u64) - zeroed
        // +0x18 LastWriteTime (u64) - zeroed
        // +0x20 ChangeTime (u64) - zeroed
        // +0x28 EndOfFile (u64)
        ppc_write_u64(base, entry_addr + 0x28, (uint64_t)de.size);
        // +0x30 AllocationSize (u64)
        ppc_write_u64(base, entry_addr + 0x30, (uint64_t)de.size);
        // +0x38 FileAttributes (u32)
        ppc_write_u32(base, entry_addr + 0x38, de.is_directory ? 0x10 : 0x80);
        // +0x3C FileNameLength (u32)
        ppc_write_u32(base, entry_addr + 0x3C, name_len);
        // +0x40 FileName (ANSI, not null-terminated)
        memcpy(base + entry_addr + 0x40, de.name.c_str(), name_len);

        entry->dir_index++;
        entries_written++;
        first = false;
        offset += aligned_size;
    }

    // Last entry has NextEntryOffset = 0 (already written)

    if (iosb_addr)
    {
        ppc_write_u32(base, iosb_addr, 0); // STATUS_SUCCESS
        ppc_write_u32(base, iosb_addr + 4, offset); // bytes written
    }

    fprintf(stderr, "[FILE] NtQueryDirectoryFile: handle=0x%X, %u entries returned (%u/%zu)\n",
            handle_val, entries_written,
            (uint32_t)entry->dir_index, entry->dir_entries.size());
    ctx.r3.u32 = 0; // STATUS_SUCCESS
}

PPC_FUNC(__imp__NtFlushBuffersFile)
{
    STUB_LOG("NtFlushBuffersFile");
    ctx.r3.u32 = 0;
}

PPC_FUNC(__imp__NtClose)
{
    uint32_t handle_val = ctx.r3.u32;
    HandleEntry* entry = handle_lookup(handle_val);
    if (entry)
    {
        fprintf(stderr, "[FILE] NtClose: handle=0x%X (\"%s\")\n", handle_val, entry->host_path.c_str());
        if (entry->fp)
            fclose(entry->fp);
        handle_free(handle_val);
    }
    // Silently succeed for non-file handles (events, threads, etc.)
    ctx.r3.u32 = 0;
}


// ============================================================================
// NT Kernel - Events, Timers, Threads, Objects
// ============================================================================

static uint32_t g_next_handle = 0x100;

PPC_FUNC(__imp__NtCreateEvent)
{
    // r3 = EventHandle* (out), r4 = ObjectAttributes*, r5 = EventType, r6 = InitialState
    STUB_LOG_ONCE("NtCreateEvent");
    uint32_t handle_ptr = ctx.r3.u32;
    ppc_write_u32(base, handle_ptr, g_next_handle++);
    ctx.r3.u32 = 0;
}

PPC_FUNC(__imp__NtSetEvent)
{
    STUB_LOG_ONCE("NtSetEvent");
    ctx.r3.u32 = 0;
}

PPC_FUNC(__imp__NtClearEvent)
{
    STUB_LOG_ONCE("NtClearEvent");
    ctx.r3.u32 = 0;
}

PPC_FUNC(__imp__NtCreateTimer)
{
    STUB_LOG_ONCE("NtCreateTimer");
    uint32_t handle_ptr = ctx.r3.u32;
    ppc_write_u32(base, handle_ptr, g_next_handle++);
    ctx.r3.u32 = 0;
}

PPC_FUNC(__imp__NtSetTimerEx)
{
    STUB_LOG_ONCE("NtSetTimerEx");
    ctx.r3.u32 = 0;
}

PPC_FUNC(__imp__NtCancelTimer)
{
    STUB_LOG_ONCE("NtCancelTimer");
    ctx.r3.u32 = 0;
}

PPC_FUNC(__imp__NtDuplicateObject)
{
    STUB_LOG("NtDuplicateObject");
    ctx.r3.u32 = 0;
}

PPC_FUNC(__imp__NtWaitForSingleObjectEx)
{
    STUB_LOG_ONCE("NtWaitForSingleObjectEx");
    if (g_current_thread) thread_yield();
    ctx.r3.u32 = 0;
}

PPC_FUNC(__imp__NtWaitForMultipleObjectsEx)
{
    STUB_LOG_ONCE("NtWaitForMultipleObjectsEx");
    if (g_current_thread) thread_yield();
    ctx.r3.u32 = 0;
}

PPC_FUNC(__imp__NtResumeThread)
{
    // r3 = ThreadHandle, r4 = PreviousSuspendCount*
    check_watchpoint(base, "NtResumeThread:entry");
    uint32_t thread_handle = ctx.r3.u32;
    uint32_t prev_count_ptr = ctx.r4.u32;
    fprintf(stderr, "[THREAD] NtResumeThread: handle=0x%08X\n", thread_handle);

    for (int i = 0; i < g_pending_thread_count; i++)
    {
        PendingThread& pt = g_pending_threads[i];
        if (pt.handle == thread_handle && pt.suspended && !pt.finished)
        {
            fprintf(stderr, "[THREAD] Resuming thread %d (handle=0x%X) — giving immediate timeslice\n", i, pt.handle);
            pt.suspended = false;
            if (prev_count_ptr)
                ppc_write_u32(base, prev_count_ptr, 1);
            // Give the thread an immediate timeslice via fiber
            // It will run until it yields (KeDelayExecutionThread etc.)
            if (g_main_fiber)
                thread_give_timeslice(pt, i);
            ctx.r3.u32 = 0; // STATUS_SUCCESS
            return;
        }
    }

    fprintf(stderr, "[THREAD] NtResumeThread: no matching suspended thread for handle=0x%08X\n", thread_handle);
    if (prev_count_ptr)
        ppc_write_u32(base, prev_count_ptr, 0);
    ctx.r3.u32 = 0;
}


// ============================================================================
// Executive Functions (Ex*)
// ============================================================================

PPC_FUNC(__imp__ExCreateThread)
{
    // r3 = ThreadHandle*, r4 = StackSize, r5 = ThreadId*, r6 = ApiThreadStartup,
    // r7 = StartRoutine, r8 = StartContext, r9 = CreateSuspended
    check_watchpoint(base, "ExCreateThread:entry");
    uint32_t handle_ptr = ctx.r3.u32;
    uint32_t api_startup = ctx.r6.u32;
    uint32_t start_routine = ctx.r7.u32;
    uint32_t start_context = ctx.r8.u32;
    uint32_t suspended = ctx.r9.u32;
    fprintf(stderr, "[THREAD] ExCreateThread: routine=0x%08X, context=0x%08X, suspended=%u\n",
            start_routine, start_context, suspended);
    uint32_t thread_handle = g_next_handle++;
    if (handle_ptr)
        ppc_write_u32(base, handle_ptr, thread_handle);

    // Queue the thread for later execution
    if (start_routine && g_pending_thread_count < MAX_PENDING_THREADS)
    {
        PendingThread& pt = g_pending_threads[g_pending_thread_count];
        pt.handle = thread_handle;
        pt.start_routine = start_routine;
        pt.start_context = start_context;
        pt.api_startup = api_startup;
        pt.suspended = (suspended != 0);
        pt.finished = false;
        pt.started = false;
        pt.fiber = nullptr;
        pt.base = base;
        // Initialize thread PPC context from main context
        pt.ppc_stack_top = alloc_thread_stack();
        init_thread_ctx(pt, ctx);
        int idx = g_pending_thread_count;
        fprintf(stderr, "[THREAD]   -> thread %d, PPC stack=0x%08X\n",
                idx, pt.ppc_stack_top);
        g_pending_thread_count++;

        // Non-suspended threads start immediately on Xbox 360.
        // Give them a fiber timeslice right away.
        if (!pt.suspended && g_main_fiber)
        {
            fprintf(stderr, "[THREAD] Non-suspended thread %d — giving immediate timeslice\n", idx);
            thread_give_timeslice(pt, idx);
        }
    }
    ctx.r3.u32 = 0;
}

PPC_FUNC(__imp__ExTerminateThread)
{
    STUB_LOG("ExTerminateThread");
    // If called from a fiber thread, mark finished and switch back to main
    if (g_current_thread)
    {
        g_current_thread->finished = true;
        SwitchToFiber(g_main_fiber);
    }
    ctx.r3.u32 = 0;
}

PPC_FUNC(__imp__ExAllocatePoolWithTag)
{
    // r3 = pool type, r4 = size, r5 = tag
    uint32_t size = ctx.r4.u32;
    size = (size + 0xF) & ~0xFu; // 16-byte align
    if (g_heap_next + size <= g_heap_end)
    {
        uint32_t addr = g_heap_next;
        g_heap_next += size;
        memset(base + addr, 0, size);
        ctx.r3.u32 = addr;
    }
    else
    {
        ctx.r3.u32 = 0;
    }
}

PPC_FUNC(__imp__ExAllocatePoolTypeWithTag)
{
    // Same as above with different argument order
    uint32_t size = ctx.r4.u32;
    size = (size + 0xF) & ~0xFu;
    if (g_heap_next + size <= g_heap_end)
    {
        uint32_t addr = g_heap_next;
        g_heap_next += size;
        memset(base + addr, 0, size);
        ctx.r3.u32 = addr;
    }
    else
    {
        ctx.r3.u32 = 0;
    }
}

PPC_FUNC(__imp__ExFreePool)
{
    STUB_LOG_ONCE("ExFreePool");
    // Can't reclaim from bump allocator
}

PPC_FUNC(__imp__ExRegisterTitleTerminateNotification)
{
    STUB_LOG_ONCE("ExRegisterTitleTerminateNotification");
}

PPC_FUNC(__imp__ExGetXConfigSetting)
{
    // r3 = category, r4 = setting, r5 = buffer, r6 = buffer_size, r7 = required_size*
    uint32_t category = ctx.r3.u32;
    uint32_t setting = ctx.r4.u32;
    uint32_t buffer = ctx.r5.u32;
    uint32_t buf_size = ctx.r6.u32;
    uint32_t req_size_ptr = ctx.r7.u32;

    // Handle known settings
    if (category == 0x03 && setting == 0x09)
    {
        // XCONFIG_SECURED_AV_REGION: 0x00001000 = NTSC-U (North America)
        uint32_t val = 0x00001000;
        if (buf_size >= 4 && buffer)
            ppc_write_u32(base, buffer, val);
        if (req_size_ptr)
            ppc_write_u32(base, req_size_ptr, 4);
        fprintf(stderr, "[STUB] ExGetXConfigSetting(cat=%u, set=%u) -> AV_REGION=0x%X\n",
                category, setting, val);
        ctx.r3.u32 = 0; // STATUS_SUCCESS
        return;
    }
    if (category == 0x03 && setting == 0x0A)
    {
        // XCONFIG_SECURED_GAME_REGION: 0x000000FF = all regions
        uint32_t val = 0x000000FF;
        if (buf_size >= 4 && buffer)
            ppc_write_u32(base, buffer, val);
        if (req_size_ptr)
            ppc_write_u32(base, req_size_ptr, 4);
        fprintf(stderr, "[STUB] ExGetXConfigSetting(cat=%u, set=%u) -> GAME_REGION=0x%X\n",
                category, setting, val);
        ctx.r3.u32 = 0;
        return;
    }

    fprintf(stderr, "[STUB] ExGetXConfigSetting(cat=%u, set=%u) -> NOT_FOUND\n", category, setting);
    ctx.r3.u32 = 0xC0000225; // STATUS_NOT_FOUND
}

PPC_FUNC(__imp__ExInitializeReadWriteLock)
{
    STUB_LOG_ONCE("ExInitializeReadWriteLock");
}

PPC_FUNC(__imp__ExAcquireReadWriteLockShared)
{
    // No-op in single-threaded mode
}

PPC_FUNC(__imp__ExAcquireReadWriteLockExclusive)
{
    // No-op in single-threaded mode
}

PPC_FUNC(__imp__ExReleaseReadWriteLock)
{
    // No-op in single-threaded mode
}


// ============================================================================
// Object Manager (Ob*)
// ============================================================================

PPC_FUNC(__imp__ObReferenceObject)
{
    STUB_LOG_ONCE("ObReferenceObject");
}

PPC_FUNC(__imp__ObDereferenceObject)
{
    STUB_LOG_ONCE("ObDereferenceObject");
}

PPC_FUNC(__imp__ObReferenceObjectByHandle)
{
    // r3 = Handle, r4 = ObjectType, r5 = Object** (output pointer)
    uint32_t handle = ctx.r3.u32;
    uint32_t out_ptr = ctx.r5.u32;
    fprintf(stderr, "[OBJ] ObReferenceObjectByHandle: handle=0x%X, out=0x%08X\n", handle, out_ptr);

    // Write the handle value as the "object pointer" so KeResumeThread can match it
    if (out_ptr)
        ppc_write_u32(base, out_ptr, handle);

    ctx.r3.u32 = 0; // STATUS_SUCCESS
}

PPC_FUNC(__imp__ObCreateSymbolicLink)
{
    STUB_LOG("ObCreateSymbolicLink");
    ctx.r3.u32 = 0;
}

PPC_FUNC(__imp__ObDeleteSymbolicLink)
{
    STUB_LOG("ObDeleteSymbolicLink");
    ctx.r3.u32 = 0;
}


// ============================================================================
// Video / Display (Vd*)
// ============================================================================

PPC_FUNC(__imp__VdInitializeEngines)
{
    STUB_LOG("VdInitializeEngines");
    ctx.r3.u32 = 0;
}

PPC_FUNC(__imp__VdShutdownEngines)
{
    STUB_LOG("VdShutdownEngines");
}

PPC_FUNC(__imp__VdSetDisplayMode)
{
    STUB_LOG("VdSetDisplayMode");
    ctx.r3.u32 = 0;
}

PPC_FUNC(__imp__VdQueryVideoMode)
{
    // r3 = VIDEO_MODE* (output struct)
    STUB_LOG("VdQueryVideoMode");
    uint32_t mode_addr = ctx.r3.u32;
    if (mode_addr)
    {
        // Fill in a default 1280x720 mode
        ppc_write_u32(base, mode_addr + 0, 1280); // width
        ppc_write_u32(base, mode_addr + 4, 720);  // height
    }
}

PPC_FUNC(__imp__VdQueryVideoFlags)
{
    STUB_LOG_ONCE("VdQueryVideoFlags");
    ctx.r3.u32 = 0;
}

PPC_FUNC(__imp__VdGetCurrentDisplayInformation)
{
    STUB_LOG("VdGetCurrentDisplayInformation");
    ctx.r3.u32 = 0;
}

PPC_FUNC(__imp__VdGetCurrentDisplayGamma)
{
    STUB_LOG_ONCE("VdGetCurrentDisplayGamma");
    ctx.r3.u32 = 0;
}

PPC_FUNC(__imp__VdGetSystemCommandBuffer)
{
    // r3 = command_buffer* (out), r4 = buffer_size* (out)
    STUB_LOG_ONCE("VdGetSystemCommandBuffer");
    // Allocate a command buffer on first call, reuse on subsequent calls
    static uint32_t s_cmd_buf = 0;
    static uint32_t s_cmd_size = 0x10000; // 64KB
    if (!s_cmd_buf)
    {
        s_cmd_buf = g_heap_next;
        g_heap_next += s_cmd_size;
        memset(base + s_cmd_buf, 0, s_cmd_size);
        fprintf(stderr, "[MEM] VdGetSystemCommandBuffer: allocated 0x%08X (%u bytes)\n",
                s_cmd_buf, s_cmd_size);
    }
    ppc_write_u32(base, ctx.r3.u32, s_cmd_buf);
    ppc_write_u32(base, ctx.r4.u32, s_cmd_size);
}

// GPU ring buffer state for fake "instant GPU processing"
static uint8_t* g_gpu_base = nullptr;
static uint32_t g_gpu_ring_base = 0;       // PPC addr of ring buffer
static uint32_t g_gpu_ring_size = 0;       // Size in DWORDs
static uint32_t g_gpu_wptr_addr = 0;       // PPC addr where game stores write pointer
static uint32_t g_gpu_rptr_wb_phys = 0;    // Physical addr for read pointer writeback
static uint32_t g_gpu_rptr_wb_virt = 0;    // Virtual addr for read pointer writeback
static volatile bool g_gpu_thread_running = false;

// Background thread: sync GPU read pointer to write pointer
// This makes the game think the GPU instantly processes all commands
static DWORD WINAPI gpu_sync_thread(LPVOID param)
{
    (void)param;
    fprintf(stderr, "[GPU] Ring buffer sync thread started\n");
    fprintf(stderr, "[GPU]   wptr_addr=0x%08X, rptr_wb_virt=0x%08X, rptr_wb_phys=0x%08X\n",
            g_gpu_wptr_addr, g_gpu_rptr_wb_virt, g_gpu_rptr_wb_phys);
    fflush(stderr);
    while (g_gpu_thread_running)
    {
        if (g_gpu_base && g_gpu_wptr_addr && g_gpu_rptr_wb_virt)
        {
            // Read current write pointer (big-endian)
            uint32_t wptr = ppc_read_u32(g_gpu_base, g_gpu_wptr_addr);
            // Write it to read pointer writeback (both virtual and physical addresses)
            ppc_write_u32(g_gpu_base, g_gpu_rptr_wb_virt, wptr);
            if (g_gpu_rptr_wb_phys && g_gpu_rptr_wb_phys != g_gpu_rptr_wb_virt)
                ppc_write_u32(g_gpu_base, g_gpu_rptr_wb_phys, wptr);
        }
        Sleep(1); // 1ms sync interval
    }
    return 0;
}

PPC_FUNC(__imp__VdInitializeRingBuffer)
{
    STUB_LOG("VdInitializeRingBuffer");
    // r3 = ring buffer base, r4 = size (log2 DWORDs), r5 = initial wptr, r6 = wptr addr
    g_gpu_base = base;
    g_gpu_ring_base = ctx.r3.u32;
    g_gpu_ring_size = 1 << ctx.r4.u32;
    g_gpu_wptr_addr = ctx.r6.u32;
    fprintf(stderr, "[GPU] Ring buffer: base=0x%08X, size=%u DW, wptr_addr=0x%08X, init_wptr=0x%08X\n",
            g_gpu_ring_base, g_gpu_ring_size, g_gpu_wptr_addr, ctx.r5.u32);
    fflush(stderr);
    ctx.r3.u32 = 0;
}

PPC_FUNC(__imp__VdEnableRingBufferRPtrWriteBack)
{
    STUB_LOG("VdEnableRingBufferRPtrWriteBack");
    // r3 = physical address for GPU read pointer writeback
    g_gpu_rptr_wb_phys = ctx.r3.u32;
    // Virtual address: on Xbox 360, phys 0x0XXXXXXX maps to virt 0xA0000000 + phys
    g_gpu_rptr_wb_virt = 0xA0000000 + ctx.r3.u32;
    fprintf(stderr, "[GPU] Read pointer writeback: phys=0x%08X, virt=0x%08X\n",
            g_gpu_rptr_wb_phys, g_gpu_rptr_wb_virt);
    fflush(stderr);
    // Initialize read pointer to current write pointer
    if (g_gpu_base && g_gpu_wptr_addr)
    {
        uint32_t wptr = ppc_read_u32(g_gpu_base, g_gpu_wptr_addr);
        ppc_write_u32(g_gpu_base, g_gpu_rptr_wb_virt, wptr);
        ppc_write_u32(g_gpu_base, g_gpu_rptr_wb_phys, wptr);
        fprintf(stderr, "[GPU] Initial rptr = wptr = 0x%08X\n", wptr);
    }
    // Start background sync thread
    if (!g_gpu_thread_running)
    {
        g_gpu_thread_running = true;
        CreateThread(nullptr, 0, gpu_sync_thread, nullptr, 0, nullptr);
    }
}

PPC_FUNC(__imp__VdSetSystemCommandBufferGpuIdentifierAddress)
{
    STUB_LOG("VdSetSystemCommandBufferGpuIdentifierAddress");
}

PPC_FUNC(__imp__VdSetGraphicsInterruptCallback)
{
    STUB_LOG("VdSetGraphicsInterruptCallback");
}

PPC_FUNC(__imp__VdInitializeScalerCommandBuffer)
{
    STUB_LOG("VdInitializeScalerCommandBuffer");
}

PPC_FUNC(__imp__VdCallGraphicsNotificationRoutines)
{
    STUB_LOG_ONCE("VdCallGraphicsNotificationRoutines");
}

PPC_FUNC(__imp__VdPersistDisplay)
{
    STUB_LOG_ONCE("VdPersistDisplay");
    ctx.r3.u32 = 0;
}

PPC_FUNC(__imp__VdSwap)
{
    // Frame swap - this is where we'd present the frame.
    STUB_LOG_ONCE("VdSwap");

    // Give each ready thread a time slice via fibers
    for (int i = 0; i < g_pending_thread_count; i++)
    {
        PendingThread& pt = g_pending_threads[i];
        if (!pt.suspended && !pt.finished)
            thread_give_timeslice(pt, i);
    }

    // Pump Win32 messages and sleep to prevent 100% CPU usage.
#ifdef _WIN32
    MSG msg;
    while (PeekMessage(&msg, NULL, 0, 0, PM_REMOVE))
    {
        if (msg.message == WM_QUIT)
            ExitProcess(0);
        TranslateMessage(&msg);
        DispatchMessage(&msg);
    }
    Sleep(16); // ~60 FPS cap
#else
    std::this_thread::sleep_for(std::chrono::milliseconds(16));
#endif
}

PPC_FUNC(__imp__VdEnableDisableClockGating)
{
    STUB_LOG_ONCE("VdEnableDisableClockGating");
}

PPC_FUNC(__imp__VdIsHSIOTrainingSucceeded)
{
    STUB_LOG_ONCE("VdIsHSIOTrainingSucceeded");
    ctx.r3.u32 = 1; // TRUE
}

PPC_FUNC(__imp__VdRetrainEDRAM)
{
    STUB_LOG("VdRetrainEDRAM");
    fprintf(stderr, "  LR=0x%08X\n", (uint32_t)ctx.lr);
    fflush(stderr);
    ctx.r3.u32 = 0; // STATUS_SUCCESS
}

PPC_FUNC(__imp__VdRetrainEDRAMWorker)
{
    STUB_LOG_ONCE("VdRetrainEDRAMWorker");
}


// ============================================================================
// Audio (XAudio*, XMA*)
// ============================================================================

PPC_FUNC(__imp__XAudioRegisterRenderDriverClient)
{
    STUB_LOG("XAudioRegisterRenderDriverClient");
    ctx.r3.u32 = 0;
}

PPC_FUNC(__imp__XAudioUnregisterRenderDriverClient)
{
    STUB_LOG("XAudioUnregisterRenderDriverClient");
    ctx.r3.u32 = 0;
}

PPC_FUNC(__imp__XAudioSubmitRenderDriverFrame)
{
    STUB_LOG_ONCE("XAudioSubmitRenderDriverFrame");
    ctx.r3.u32 = 0;
}

PPC_FUNC(__imp__XAudioGetVoiceCategoryVolume)
{
    STUB_LOG_ONCE("XAudioGetVoiceCategoryVolume");
    ctx.r3.u32 = 0;
}

PPC_FUNC(__imp__XAudioGetVoiceCategoryVolumeChangeMask)
{
    STUB_LOG_ONCE("XAudioGetVoiceCategoryVolumeChangeMask");
    ctx.r3.u32 = 0;
}

PPC_FUNC(__imp__XMACreateContext)
{
    STUB_LOG("XMACreateContext");
    ctx.r3.u32 = 0;
}

PPC_FUNC(__imp__XMAReleaseContext)
{
    STUB_LOG("XMAReleaseContext");
}


// ============================================================================
// XAM - Application Manager
// ============================================================================

PPC_FUNC(__imp__XamAlloc)
{
    // r3 = flags, r4 = size, r5 = ptr* (out)
    uint32_t size = ctx.r4.u32;
    uint32_t out_ptr = ctx.r5.u32;
    size = (size + 0xF) & ~0xFu;
    if (g_heap_next + size <= g_heap_end)
    {
        uint32_t addr = g_heap_next;
        g_heap_next += size;
        memset(base + addr, 0, size);
        ppc_write_u32(base, out_ptr, addr);
        ctx.r3.u32 = 0;
    }
    else
    {
        ctx.r3.u32 = 0x8007000E; // E_OUTOFMEMORY
    }
}

PPC_FUNC(__imp__XamFree)
{
    // Can't reclaim from bump allocator
    ctx.r3.u32 = 0;
}

PPC_FUNC(__imp__XamGetExecutionId)
{
    STUB_LOG("XamGetExecutionId");
    // r3 = EXECUTION_ID** (out)
    // Allocate a fake execution ID struct
    uint32_t exec_id = g_heap_next;
    g_heap_next += 0x18;
    memset(base + exec_id, 0, 0x18);
    ppc_write_u32(base, ctx.r3.u32, exec_id);
    ctx.r3.u32 = 0;
}

PPC_FUNC(__imp__XamGetSystemVersion)
{
    STUB_LOG_ONCE("XamGetSystemVersion");
    ctx.r3.u32 = 0x20B10024; // Dashboard version ~2.0.17511.0
}


// ============================================================================
// XAM - User / Profile
// ============================================================================

PPC_FUNC(__imp__XamUserGetXUID)
{
    STUB_LOG_ONCE("XamUserGetXUID");
    ctx.r3.u32 = 0;
}

PPC_FUNC(__imp__XamUserGetName)
{
    // r3 = user index, r4 = buffer, r5 = buffer size
    STUB_LOG_ONCE("XamUserGetName");
    uint32_t buf = ctx.r4.u32;
    const char* name = "Player1";
    memcpy(base + buf, name, strlen(name) + 1);
    ctx.r3.u32 = 0;
}

PPC_FUNC(__imp__XamUserGetSigninState)
{
    // r3 = user index
    STUB_LOG_ONCE("XamUserGetSigninState");
    ctx.r3.u32 = 1; // eXUserSigninState_SignedInLocally
}

PPC_FUNC(__imp__XamUserGetSigninInfo)
{
    STUB_LOG_ONCE("XamUserGetSigninInfo");
    ctx.r3.u32 = 0;
}

PPC_FUNC(__imp__XamUserCheckPrivilege)
{
    STUB_LOG_ONCE("XamUserCheckPrivilege");
    // r3 = user, r4 = privilege, r5 = result*
    uint32_t result_ptr = ctx.r5.u32;
    if (result_ptr)
        ppc_write_u32(base, result_ptr, 1); // Has privilege
    ctx.r3.u32 = 0;
}

PPC_FUNC(__imp__XamUserAreUsersFriends)
{
    STUB_LOG_ONCE("XamUserAreUsersFriends");
    ctx.r3.u32 = 0;
}

PPC_FUNC(__imp__XamUserReadProfileSettings)
{
    STUB_LOG("XamUserReadProfileSettings");
    ctx.r3.u32 = 0x80070057; // E_INVALIDARG - not implemented
}

PPC_FUNC(__imp__XamUserWriteProfileSettings)
{
    STUB_LOG("XamUserWriteProfileSettings");
    ctx.r3.u32 = 0;
}

PPC_FUNC(__imp__XamUserCreateStatsEnumerator)
{
    STUB_LOG("XamUserCreateStatsEnumerator");
    ctx.r3.u32 = 0x80070057;
}


// ============================================================================
// XAM - Input
// ============================================================================

PPC_FUNC(__imp__XamInputGetState)
{
    // r3 = user index, r4 = flags, r5 = XINPUT_STATE*
    // Return ERROR_DEVICE_NOT_CONNECTED for now
    ctx.r3.u32 = 0x48F; // ERROR_DEVICE_NOT_CONNECTED
}

PPC_FUNC(__imp__XamInputSetState)
{
    ctx.r3.u32 = 0x48F; // ERROR_DEVICE_NOT_CONNECTED
}

PPC_FUNC(__imp__XamInputGetCapabilities)
{
    STUB_LOG_ONCE("XamInputGetCapabilities");
    ctx.r3.u32 = 0x48F; // ERROR_DEVICE_NOT_CONNECTED
}


// ============================================================================
// XAM - UI
// ============================================================================

PPC_FUNC(__imp__XamShowSigninUI)
{
    STUB_LOG("XamShowSigninUI");
    ctx.r3.u32 = 0;
}

PPC_FUNC(__imp__XamShowGamerCardUIForXUID)
{
    STUB_LOG("XamShowGamerCardUIForXUID");
    ctx.r3.u32 = 0;
}

PPC_FUNC(__imp__XamShowAchievementsUI)
{
    STUB_LOG("XamShowAchievementsUI");
    ctx.r3.u32 = 0;
}

PPC_FUNC(__imp__XamShowMarketplaceUI)
{
    STUB_LOG("XamShowMarketplaceUI");
    ctx.r3.u32 = 0;
}

PPC_FUNC(__imp__XamShowDirtyDiscErrorUI)
{
    STUB_LOG("XamShowDirtyDiscErrorUI");
    ctx.r3.u32 = 0;
}

PPC_FUNC(__imp__XamShowMessageBoxUIEx)
{
    STUB_LOG("XamShowMessageBoxUIEx");
    ctx.r3.u32 = 0;
}

PPC_FUNC(__imp__XamReadTileToTexture)
{
    STUB_LOG("XamReadTileToTexture");
    ctx.r3.u32 = 0;
}

PPC_FUNC(__imp__XamParseGamerTileKey)
{
    STUB_LOG("XamParseGamerTileKey");
    ctx.r3.u32 = 0;
}


// ============================================================================
// XAM - Content / Enumerator
// ============================================================================

PPC_FUNC(__imp__XamContentCreateEx)
{
    STUB_LOG("XamContentCreateEx");
    ctx.r3.u32 = 0x80070057; // E_INVALIDARG
}

PPC_FUNC(__imp__XamContentGetLicenseMask)
{
    STUB_LOG("XamContentGetLicenseMask");
    // r3 = mask* (out), r4 = overlapped*
    uint32_t mask_ptr = ctx.r3.u32;
    if (mask_ptr)
        ppc_write_u32(base, mask_ptr, 0xFFFFFFFF); // Full license
    ctx.r3.u32 = 0;
}

PPC_FUNC(__imp__XamContentCreateEnumerator)
{
    STUB_LOG("XamContentCreateEnumerator");
    ctx.r3.u32 = 0x80070057;
}

PPC_FUNC(__imp__XamEnumerate)
{
    STUB_LOG("XamEnumerate");
    ctx.r3.u32 = 0x80070057;
}

PPC_FUNC(__imp__XamLoaderLaunchTitle)
{
    STUB_LOG("XamLoaderLaunchTitle");
    ctx.r3.u32 = 0;
}

PPC_FUNC(__imp__XamLoaderTerminateTitle)
{
    fprintf(stderr, "[STUB] XamLoaderTerminateTitle - game requested exit\n");
    exit(0);
}


// ============================================================================
// XAM - Voice / Headset
// ============================================================================

PPC_FUNC(__imp__XamVoiceCreate)
{
    STUB_LOG("XamVoiceCreate");
    ctx.r3.u32 = 0x80070057;
}

PPC_FUNC(__imp__XamVoiceClose)
{
    STUB_LOG_ONCE("XamVoiceClose");
    ctx.r3.u32 = 0;
}

PPC_FUNC(__imp__XamVoiceHeadsetPresent)
{
    ctx.r3.u32 = 0; // No headset
}

PPC_FUNC(__imp__XamVoiceSubmitPacket)
{
    STUB_LOG_ONCE("XamVoiceSubmitPacket");
    ctx.r3.u32 = 0;
}


// ============================================================================
// XAM - Messaging / Notifications
// ============================================================================

PPC_FUNC(__imp__XMsgStartIORequest)
{
    STUB_LOG("XMsgStartIORequest");
    ctx.r3.u32 = 0x80070057;
}

PPC_FUNC(__imp__XMsgInProcessCall)
{
    STUB_LOG("XMsgInProcessCall");
    ctx.r3.u32 = 0;
}

PPC_FUNC(__imp__XMsgCancelIORequest)
{
    STUB_LOG_ONCE("XMsgCancelIORequest");
    ctx.r3.u32 = 0;
}

PPC_FUNC(__imp__XNotifyGetNext)
{
    // r3 = handle, r4 = notification_id*, r5 = param*
    ctx.r3.u32 = 0; // FALSE - no notifications
}

PPC_FUNC(__imp__XamNotifyCreateListener)
{
    STUB_LOG_ONCE("XamNotifyCreateListener");
    ctx.r3.u32 = g_next_handle++; // Return a fake handle
}

PPC_FUNC(__imp__XamSessionRefObjByHandle)
{
    STUB_LOG("XamSessionRefObjByHandle");
    ctx.r3.u32 = 0x80070057;
}

PPC_FUNC(__imp__XamSessionCreateHandle)
{
    STUB_LOG("XamSessionCreateHandle");
    ctx.r3.u32 = 0x80070057;
}


// ============================================================================
// Xbox System Info
// ============================================================================

PPC_FUNC(__imp__XGetVideoMode)
{
    // r3 = XVIDEO_MODE* (output)
    STUB_LOG("XGetVideoMode");
    uint32_t addr = ctx.r3.u32;
    if (addr)
    {
        memset(base + addr, 0, 48); // Zero the struct
        ppc_write_u32(base, addr + 0, 1280);  // dwDisplayWidth
        ppc_write_u32(base, addr + 4, 720);   // dwDisplayHeight
        ppc_write_u32(base, addr + 8, 1);     // fIsInterlaced = false
        ppc_write_u32(base, addr + 12, 1);    // fIsWideScreen = true
    }
}

PPC_FUNC(__imp__XGetLanguage)
{
    ctx.r3.u32 = 1; // English
}

PPC_FUNC(__imp__XGetGameRegion)
{
    ctx.r3.u32 = 0xFF; // All regions
}

PPC_FUNC(__imp__XGetAVPack)
{
    ctx.r3.u32 = 0x16; // XC_AV_PACK_HDMI
}


// ============================================================================
// Networking (NetDll_*)
// ============================================================================

PPC_FUNC(__imp__NetDll_XNetStartup)
{
    STUB_LOG("NetDll_XNetStartup");
    ctx.r3.u32 = 0; // Success
}

PPC_FUNC(__imp__NetDll_XNetCleanup)
{
    STUB_LOG_ONCE("NetDll_XNetCleanup");
    ctx.r3.u32 = 0;
}

PPC_FUNC(__imp__NetDll_XNetRandom)
{
    STUB_LOG_ONCE("NetDll_XNetRandom");
    check_watchpoint(base, "NetDll_XNetRandom:entry");
    // r3 = xnet handle (ignored), r4 = buffer, r5 = length
    // NOTE: Xbox 360 NetDll functions take a handle in r3
    uint32_t buf = ctx.r4.u32;
    uint32_t len = ctx.r5.u32;
    fprintf(stderr, "[NET] XNetRandom: buf=0x%08X, len=%u\n", buf, len);
    if (buf >= 0x82000000 && buf < 0x82090000)
    {
        fprintf(stderr, "[NET] WARNING: XNetRandom writing to PE data section!\n");
    }
    for (uint32_t i = 0; i < len; i++)
        base[buf + i] = (uint8_t)(rand() & 0xFF);
    check_watchpoint(base, "NetDll_XNetRandom:exit");
    ctx.r3.u32 = 0;
}

PPC_FUNC(__imp__NetDll_XNetXnAddrToInAddr)
{
    STUB_LOG("NetDll_XNetXnAddrToInAddr");
    ctx.r3.u32 = 0x80070057; // E_INVALIDARG
}

PPC_FUNC(__imp__NetDll_XNetInAddrToXnAddr)
{
    STUB_LOG("NetDll_XNetInAddrToXnAddr");
    ctx.r3.u32 = 0x80070057;
}

PPC_FUNC(__imp__NetDll_XNetUnregisterInAddr)
{
    STUB_LOG_ONCE("NetDll_XNetUnregisterInAddr");
    ctx.r3.u32 = 0;
}

PPC_FUNC(__imp__NetDll_XNetConnect)
{
    STUB_LOG("NetDll_XNetConnect");
    ctx.r3.u32 = 0x80070057;
}

PPC_FUNC(__imp__NetDll_XNetGetConnectStatus)
{
    STUB_LOG_ONCE("NetDll_XNetGetConnectStatus");
    ctx.r3.u32 = 2; // XNET_CONNECT_STATUS_LOST
}

PPC_FUNC(__imp__NetDll_XNetQosListen)
{
    STUB_LOG("NetDll_XNetQosListen");
    ctx.r3.u32 = 0x80070057;
}

PPC_FUNC(__imp__NetDll_XNetQosLookup)
{
    STUB_LOG("NetDll_XNetQosLookup");
    ctx.r3.u32 = 0x80070057;
}

PPC_FUNC(__imp__NetDll_XNetQosRelease)
{
    STUB_LOG_ONCE("NetDll_XNetQosRelease");
    ctx.r3.u32 = 0;
}

PPC_FUNC(__imp__NetDll_XNetGetTitleXnAddr)
{
    STUB_LOG("NetDll_XNetGetTitleXnAddr");
    ctx.r3.u32 = 0; // XNET_GET_XNADDR_NONE
}

PPC_FUNC(__imp__NetDll_XNetGetOpt)
{
    STUB_LOG("NetDll_XNetGetOpt");
    ctx.r3.u32 = 0x80070057;
}


// ============================================================================
// Networking - Winsock (NetDll_WSA*, NetDll_socket*, etc.)
// ============================================================================

PPC_FUNC(__imp__NetDll_WSAStartup)
{
    STUB_LOG("NetDll_WSAStartup");
    ctx.r3.u32 = 0; // Success
}

PPC_FUNC(__imp__NetDll_WSACleanup)
{
    STUB_LOG_ONCE("NetDll_WSACleanup");
    ctx.r3.u32 = 0;
}

PPC_FUNC(__imp__NetDll_socket)
{
    STUB_LOG("NetDll_socket");
    ctx.r3.u32 = 0xFFFFFFFF; // INVALID_SOCKET
}

PPC_FUNC(__imp__NetDll_closesocket)
{
    STUB_LOG_ONCE("NetDll_closesocket");
    ctx.r3.u32 = 0;
}

PPC_FUNC(__imp__NetDll_ioctlsocket)
{
    STUB_LOG_ONCE("NetDll_ioctlsocket");
    ctx.r3.u32 = 0;
}

PPC_FUNC(__imp__NetDll_setsockopt)
{
    STUB_LOG_ONCE("NetDll_setsockopt");
    ctx.r3.u32 = 0;
}

PPC_FUNC(__imp__NetDll_bind)
{
    STUB_LOG("NetDll_bind");
    ctx.r3.u32 = 0xFFFFFFFF; // SOCKET_ERROR
}

PPC_FUNC(__imp__NetDll_select)
{
    STUB_LOG_ONCE("NetDll_select");
    ctx.r3.u32 = 0; // No sockets ready
}

PPC_FUNC(__imp__NetDll_WSAGetOverlappedResult)
{
    STUB_LOG("NetDll_WSAGetOverlappedResult");
    ctx.r3.u32 = 0; // FALSE
}

PPC_FUNC(__imp__NetDll_recvfrom)
{
    STUB_LOG_ONCE("NetDll_recvfrom");
    ctx.r3.u32 = 0xFFFFFFFF; // SOCKET_ERROR
}

PPC_FUNC(__imp__NetDll_WSARecvFrom)
{
    STUB_LOG_ONCE("NetDll_WSARecvFrom");
    ctx.r3.u32 = 0xFFFFFFFF;
}

PPC_FUNC(__imp__NetDll_WSASendTo)
{
    STUB_LOG_ONCE("NetDll_WSASendTo");
    ctx.r3.u32 = 0xFFFFFFFF;
}

PPC_FUNC(__imp__NetDll_WSAGetLastError)
{
    ctx.r3.u32 = 10093; // WSANOTINITIALISED
}


// ============================================================================
// USB Camera (XUsbcam*)
// ============================================================================

PPC_FUNC(__imp__XUsbcamCreate)
{
    STUB_LOG("XUsbcamCreate");
    ctx.r3.u32 = 1; // Error - no camera
}

PPC_FUNC(__imp__XUsbcamDestroy)
{
    STUB_LOG_ONCE("XUsbcamDestroy");
    ctx.r3.u32 = 0;
}

PPC_FUNC(__imp__XUsbcamGetState)
{
    ctx.r3.u32 = 0; // Not connected
}

PPC_FUNC(__imp__XUsbcamSetConfig)
{
    STUB_LOG("XUsbcamSetConfig");
    ctx.r3.u32 = 1; // Error
}

PPC_FUNC(__imp__XUsbcamSetView)
{
    STUB_LOG("XUsbcamSetView");
    ctx.r3.u32 = 1;
}

PPC_FUNC(__imp__XUsbcamSetCaptureMode)
{
    STUB_LOG("XUsbcamSetCaptureMode");
    ctx.r3.u32 = 1;
}

PPC_FUNC(__imp__XUsbcamReadFrame)
{
    STUB_LOG("XUsbcamReadFrame");
    ctx.r3.u32 = 1;
}


// ============================================================================
// XEX Loader (Xex*)
// ============================================================================

PPC_FUNC(__imp__XexGetModuleHandle)
{
    // r3 = module name (string ptr), r4 = handle* (out)
    STUB_LOG("XexGetModuleHandle");
    uint32_t name_addr = ctx.r3.u32;
    if (name_addr)
        fprintf(stderr, "  Module: %s\n", ppc_string(base, name_addr));
    // Return a fake handle
    uint32_t handle_ptr = ctx.r4.u32;
    if (handle_ptr)
        ppc_write_u32(base, handle_ptr, 0xDEAD0001);
    ctx.r3.u32 = 0;
}

PPC_FUNC(__imp__XexGetProcedureAddress)
{
    // r3 = module handle, r4 = ordinal, r5 = proc address* (out)
    STUB_LOG("XexGetProcedureAddress");
    uint32_t ordinal = ctx.r4.u32;
    uint32_t out_ptr = ctx.r5.u32;
    fprintf(stderr, "  Handle=0x%08X, Ordinal=%u\n", ctx.r3.u32, ordinal);
    // Write a dynamic stub address so the game can call through the pointer
    if (out_ptr)
        ppc_write_u32(base, out_ptr, PPC_DYNAMIC_STUB_ADDR);
    ctx.r3.u32 = 0; // Success
}

PPC_FUNC(__imp__XexCheckExecutablePrivilege)
{
    STUB_LOG_ONCE("XexCheckExecutablePrivilege");
    ctx.r3.u32 = 0; // Has privilege
}

PPC_FUNC(__imp__XexLoadImage)
{
    STUB_LOG("XexLoadImage");
    ctx.r3.u32 = 0xC0000034; // STATUS_OBJECT_NAME_NOT_FOUND
}

PPC_FUNC(__imp__XexUnloadImage)
{
    STUB_LOG("XexUnloadImage");
    ctx.r3.u32 = 0;
}


// ============================================================================
// HAL (Hardware Abstraction Layer)
// ============================================================================

PPC_FUNC(__imp__HalReturnToFirmware)
{
    fprintf(stderr, "[STUB] HalReturnToFirmware(%u) - game requested reboot/poweroff\n", ctx.r3.u32);
    exit(0);
}
