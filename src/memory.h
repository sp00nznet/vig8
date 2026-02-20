#pragma once

#include <cstdint>
#include <cstddef>

// PPC memory layout constants (from ppc_config.h)
constexpr uint64_t PPC_MEM_IMAGE_BASE = 0x82000000ULL;
constexpr uint64_t PPC_MEM_IMAGE_SIZE = 0x4E0000ULL;
constexpr uint64_t PPC_MEM_CODE_BASE  = 0x82090000ULL;
constexpr uint64_t PPC_MEM_CODE_SIZE  = 0x2FD8F8ULL;
constexpr uint64_t PPC_MEM_TOTAL_SIZE = 0x100000000ULL; // 4 GB address space

// XEX entry point
constexpr uint32_t PPC_ENTRY_POINT = 0x823253B0;

// Stack configuration
constexpr uint32_t PPC_STACK_SIZE = 1 * 1024 * 1024;  // 1 MB stack
constexpr uint32_t PPC_STACK_BASE = 0x90000000;         // Stack top (grows down)

// Heap region for kernel stub allocations (NtAllocateVirtualMemory, etc.)
constexpr uint32_t PPC_HEAP_BASE = 0xA0000000;
constexpr uint32_t PPC_HEAP_SIZE = 0x10000000;          // 256 MB

// Fake Xbox 360 kernel structures (KPCR / KTHREAD)
// r13 points to KPCR on Xbox 360. The game code accesses:
//   r13+0x100 = pointer to current KTHREAD
//   r13+0x10C = per-processor flag byte
//   r13+0x150 = error suppression flag
constexpr uint32_t PPC_KPCR_BASE    = 0x92000000;
constexpr uint32_t PPC_KPCR_SIZE    = 0x1000;           // 4 KB
constexpr uint32_t PPC_KTHREAD_BASE = 0x92001000;
constexpr uint32_t PPC_KTHREAD_SIZE = 0x1000;           // 4 KB

// Function lookup table lives right after the image
constexpr uint64_t PPC_FUNC_TABLE_OFFSET = PPC_MEM_IMAGE_BASE + PPC_MEM_IMAGE_SIZE;
constexpr uint64_t PPC_FUNC_TABLE_SIZE   = PPC_MEM_CODE_SIZE * 2; // 8 bytes per 4-byte instruction

// Allocate the PPC memory space using platform virtual memory.
// Returns nullptr on failure.
uint8_t* ppc_memory_alloc();

// Free the PPC memory space.
void ppc_memory_free(uint8_t* base);

// Populate the function lookup table from PPCFuncMappings[].
// This fills base[PPC_FUNC_TABLE_OFFSET..] with function pointers
// so PPC_LOOKUP_FUNC / PPC_CALL_INDIRECT_FUNC work at runtime.
void ppc_populate_func_table(uint8_t* base);

// Register a dynamic stub at a specific PPC address in the function table.
// Used by XexGetProcedureAddress to return callable function pointers.
void ppc_register_dynamic_stub(uint8_t* base, uint32_t ppc_addr);

// Address reserved for the universal dynamic stub function.
// Must be within PPC_MEM_CODE_BASE..PPC_MEM_CODE_BASE+PPC_MEM_CODE_SIZE.
// Using last aligned address in code range.
constexpr uint32_t PPC_DYNAMIC_STUB_ADDR = 0x8238D8F0;

// Global window handle (created in main.cpp, used by kernel_stubs.cpp)
#ifdef _WIN32
#include <windows.h>
extern HWND g_hwnd;
#endif
