// vig8 - LAN Multiplayer Networking
// Overrides XNet/QoS/async-receive stubs with real UDP broadcast networking
// for system-link (LAN) multiplayer.

#pragma once

#include <cstdint>

// Initialize LAN networking: detect local IP, start discovery thread.
// Call after runtime initialization.
void NetInit(int lan_port);

// Stop discovery thread and clean up sockets.
// Call before runtime shutdown.
void NetShutdown();

// ============================================================================
// XNADDR layout (36 bytes, matches Xbox 360 structure)
// ============================================================================
#pragma pack(push, 1)
struct XNADDR_LAN {
    uint32_t ina;           // +0   IPv4 address (network byte order)
    uint32_t inaOnline;     // +4   IPv4 address (network byte order)
    uint16_t wPortOnline;   // +8   port (network byte order)
    uint8_t  abEnet[6];    // +10  MAC address
    uint8_t  abOnline[20]; // +16  online address (zeroed for LAN)
};
static_assert(sizeof(XNADDR_LAN) == 36, "XNADDR_LAN must be 36 bytes");
#pragma pack(pop)

// ============================================================================
// Discovery protocol constants
// ============================================================================
constexpr uint8_t  DISC_MAGIC      = 0xD8;
constexpr uint8_t  DISC_BEACON     = 0x01;
constexpr uint8_t  DISC_PROBE      = 0x02;
constexpr int      DISC_HEADER_LEN = 46;  // magic(1) + type(1) + xnkid(8) + xnaddr(36)
constexpr int      DISC_MAX_QOS    = 512; // max QoS data blob size

// ============================================================================
// XNet status constants
// ============================================================================
constexpr uint32_t XNET_GET_XNADDR_STATIC         = 4;
constexpr uint32_t XNET_CONNECT_STATUS_IDLE        = 0;
constexpr uint32_t XNET_CONNECT_STATUS_PENDING     = 1;
constexpr uint32_t XNET_CONNECT_STATUS_CONNECTED   = 4;
constexpr uint32_t XNET_ETHERNET_LINK_ACTIVE       = 0x01;
constexpr uint32_t XNET_ETHERNET_LINK_100MBPS      = 0x04;
constexpr uint32_t XNET_ETHERNET_LINK_FULL_DUPLEX  = 0x08;
