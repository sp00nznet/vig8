// Missing kernel stubs for Vigilante 8 Arcade
// These are Xbox 360 APIs not yet implemented in the ReXGlue SDK.
// Most are non-essential: networking, USB camera, and UI dialogs.

#include "vig8_config.h"
#include <rex/runtime/guest/context.h>
#include <rex/runtime/guest/memory.h>

using namespace rex::runtime::guest;

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
