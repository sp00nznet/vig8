// Missing kernel stubs for Vigilante 8 Arcade
// These are Xbox 360 APIs not yet implemented in the ReXGlue SDK.
// Most are non-essential: networking, USB camera, and UI dialogs.

#include "vig8_config.h"
#include "settings.h"
#include <rex/runtime/guest/context.h>
#include <rex/runtime/guest/memory.h>

using namespace rex::runtime::guest;

// Global debug flags (set from ApplySettings in main.cpp)
bool g_vig8_invulnerable = false;
bool g_vig8_unlock_all_cars = false;

// Simple stub macro that returns a value
#define VIG8_STUB_RETURN(name, val) \
    extern "C" PPC_FUNC(name) { (void)base; ctx.r3.u64 = (val); }

#define VIG8_STUB(name) VIG8_STUB_RETURN(name, 0)

// Networking stubs (NetDll_*)
VIG8_STUB(__imp__NetDll_XNetUnregisterInAddr)
VIG8_STUB(__imp__NetDll_XNetConnect)
VIG8_STUB(__imp__NetDll_XNetGetConnectStatus)
VIG8_STUB(__imp__NetDll_XNetQosLookup)
VIG8_STUB(__imp__NetDll_WSAGetOverlappedResult)

// XAM UI stubs
VIG8_STUB(__imp__XamShowGamerCardUIForXUID)
VIG8_STUB(__imp__XamShowAchievementsUI)
VIG8_STUB(__imp__XamShowMarketplaceUI)
VIG8_STUB_RETURN(__imp__XamUserCreateStatsEnumerator, 1)  // fail = no stats
VIG8_STUB(__imp__XamVoiceSubmitPacket)

// Content license — override the weak wrapper sub_823245B0 to return full
// license mask, bypassing the SDK's XamContentGetLicenseMask (which returns
// no-license). The generated code declares sub_823245B0 as PPC_WEAK_FUNC,
// so our strong definition takes precedence.
// Calling convention: r3 = output pointer for mask, r4 = overlapped (ignored)
// Returns 0 (success) in r3.
extern "C" PPC_FUNC(sub_823245B0) {
    uint32_t mask_ptr = ctx.r3.u32;
    if (mask_ptr)
        PPC_STORE_U32(mask_ptr, 0xFFFFFFFF);  // full license
    ctx.r3.u32 = 0;  // ERROR_SUCCESS
}

// Kernel memory allocation
VIG8_STUB(__imp__ExAllocatePoolWithTag)  // NULL = allocation failed

// USB Camera stubs (XUsbcam*)
VIG8_STUB_RETURN(__imp__XUsbcamCreate, 1)  // fail
VIG8_STUB(__imp__XUsbcamDestroy)
VIG8_STUB(__imp__XUsbcamGetState)
VIG8_STUB_RETURN(__imp__XUsbcamSetConfig, 1)
VIG8_STUB_RETURN(__imp__XUsbcamSetView, 1)
VIG8_STUB_RETURN(__imp__XUsbcamSetCaptureMode, 1)
VIG8_STUB_RETURN(__imp__XUsbcamReadFrame, 1)

// ObReferenceObject - reference counting stub
VIG8_STUB(__imp__ObReferenceObject)

// ============================================================================
// Vehicle unlock override
// ============================================================================
// sub_821B80F0 reads vehicle data structures. At offset 196, bits 12-15
// (mask 0xF0000) indicate the vehicle is unlocked. If those bits are 0,
// the vehicle is locked (skipped). We override the weak symbol to force
// those bits set before calling the original implementation.

// Forward-declare the generated implementation
extern "C" void __imp__sub_821B80F0(PPCContext& ctx, uint8_t* base);

extern "C" PPC_FUNC(sub_821B80F0) {
    if (g_vig8_unlock_all_cars) {
        uint32_t struct_ptr = ctx.r3.u32;
        if (struct_ptr) {
            uint32_t val = PPC_LOAD_U32(struct_ptr + 196);
            val |= 0xF0000;  // force bits 12-15 set = unlocked
            PPC_STORE_U32(struct_ptr + 196, val);
        }
    }
    // Call original implementation
    __imp__sub_821B80F0(ctx, base);
}
