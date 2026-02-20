#include "ppc_config.h"
#include "ppc_context.h"
#include "memory.h"

#include <cstdio>
#include <cstdarg>
#include <cstring>
#include <cstdlib>

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
#define STUB_VERBOSE 0
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
    uint32_t interval_addr = ctx.r5.u32;
    int64_t interval = 0;
    if (interval_addr)
    {
        uint32_t hi = ppc_read_u32(base, interval_addr);
        uint32_t lo = ppc_read_u32(base, interval_addr + 4);
        interval = (int64_t(hi) << 32) | lo;
    }
    // Negative = relative time in 100ns units
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
    STUB_LOG_ONCE("KeResumeThread");
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
    ctx.r3.u32 = 0; // STATUS_WAIT_0
}

PPC_FUNC(__imp__KeWaitForMultipleObjects)
{
    STUB_LOG_ONCE("KeWaitForMultipleObjects");
    ctx.r3.u32 = 0; // STATUS_WAIT_0
}

PPC_FUNC(__imp__KeInitializeSemaphore)
{
    STUB_LOG_ONCE("KeInitializeSemaphore");
}

PPC_FUNC(__imp__KeReleaseSemaphore)
{
    STUB_LOG_ONCE("KeReleaseSemaphore");
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
    STUB_LOG_ONCE("RtlNtStatusToDosError");
    // Simple mapping: 0 → 0, anything else → generic error
    if (ctx.r3.u32 == 0)
        ctx.r3.u32 = 0; // ERROR_SUCCESS
    else
        ctx.r3.u32 = 1; // ERROR_INVALID_FUNCTION
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

// Simple bump allocator for NtAllocateVirtualMemory
// Allocations start at 0xA0000000 in the PPC address space
static uint32_t g_heap_next = 0xA0000000;
static constexpr uint32_t g_heap_end = 0xB0000000; // 256 MB heap space

PPC_FUNC(__imp__NtAllocateVirtualMemory)
{
    // r3 = BaseAddress* (in/out), r4 = RegionSize* (in/out), r5 = AllocationType, r6 = Protect
    STUB_LOG("NtAllocateVirtualMemory");
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
// NT Kernel - File I/O
// ============================================================================

PPC_FUNC(__imp__NtOpenFile)
{
    STUB_LOG("NtOpenFile");
    ctx.r3.u32 = 0xC0000034; // STATUS_OBJECT_NAME_NOT_FOUND
}

PPC_FUNC(__imp__NtCreateFile)
{
    STUB_LOG("NtCreateFile");
    ctx.r3.u32 = 0xC0000034; // STATUS_OBJECT_NAME_NOT_FOUND
}

PPC_FUNC(__imp__NtReadFile)
{
    STUB_LOG("NtReadFile");
    ctx.r3.u32 = 0xC0000008; // STATUS_INVALID_HANDLE
}

PPC_FUNC(__imp__NtReadFileScatter)
{
    STUB_LOG("NtReadFileScatter");
    ctx.r3.u32 = 0xC0000008;
}

PPC_FUNC(__imp__NtWriteFile)
{
    STUB_LOG("NtWriteFile");
    ctx.r3.u32 = 0xC0000008;
}

PPC_FUNC(__imp__NtQueryInformationFile)
{
    STUB_LOG("NtQueryInformationFile");
    ctx.r3.u32 = 0xC0000008;
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
    STUB_LOG("NtQueryDirectoryFile");
    ctx.r3.u32 = 0xC0000008;
}

PPC_FUNC(__imp__NtFlushBuffersFile)
{
    STUB_LOG("NtFlushBuffersFile");
    ctx.r3.u32 = 0;
}

PPC_FUNC(__imp__NtClose)
{
    STUB_LOG_ONCE("NtClose");
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
    ctx.r3.u32 = 0;
}

PPC_FUNC(__imp__NtWaitForMultipleObjectsEx)
{
    STUB_LOG_ONCE("NtWaitForMultipleObjectsEx");
    ctx.r3.u32 = 0;
}

PPC_FUNC(__imp__NtResumeThread)
{
    STUB_LOG_ONCE("NtResumeThread");
    ctx.r3.u32 = 0;
}


// ============================================================================
// Executive Functions (Ex*)
// ============================================================================

PPC_FUNC(__imp__ExCreateThread)
{
    // r3 = ThreadHandle*, r4 = StackSize, r5 = ThreadId*, r6 = ApiThreadStartup,
    // r7 = StartRoutine, r8 = StartContext, r9 = CreateSuspended
    STUB_LOG("ExCreateThread");
    uint32_t handle_ptr = ctx.r3.u32;
    if (handle_ptr)
        ppc_write_u32(base, handle_ptr, g_next_handle++);
    // TODO: Actually create a thread for multi-threaded games
    ctx.r3.u32 = 0;
}

PPC_FUNC(__imp__ExTerminateThread)
{
    STUB_LOG("ExTerminateThread");
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
    STUB_LOG("ExGetXConfigSetting");
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
    STUB_LOG_ONCE("ObReferenceObjectByHandle");
    ctx.r3.u32 = 0;
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

PPC_FUNC(__imp__VdInitializeRingBuffer)
{
    STUB_LOG("VdInitializeRingBuffer");
    ctx.r3.u32 = 0;
}

PPC_FUNC(__imp__VdEnableRingBufferRPtrWriteBack)
{
    STUB_LOG("VdEnableRingBufferRPtrWriteBack");
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
    // Pump Win32 messages and sleep to prevent 100% CPU usage.
    STUB_LOG_ONCE("VdSwap");
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
    STUB_LOG_ONCE("VdRetrainEDRAM");
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
    // r3 = buffer, r4 = length
    uint32_t buf = ctx.r3.u32;
    uint32_t len = ctx.r4.u32;
    for (uint32_t i = 0; i < len; i++)
        base[buf + i] = (uint8_t)(rand() & 0xFF);
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
