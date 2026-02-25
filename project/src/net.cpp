// vig8 - LAN Multiplayer Networking Implementation
//
// Overrides the stubbed XNet/QoS/async-receive functions with real UDP
// broadcast networking for system-link (LAN) multiplayer.
//
// Architecture:
//   Game code (recompiled PPC)
//     -> XNet/QoS wrapper thunks (generated)
//       -> Our overrides here (via /force:multiple)
//         -> Real native sockets (already working in SDK)
//         -> Background discovery thread (for QoS beacons)

#include "net.h"
#include "vig8_config.h"

#include <rex/runtime/guest/context.h>
#include <rex/runtime/guest/memory.h>
#include <rex/kernel/kernel_state.h>
#include <rex/kernel/xsocket.h>
#include <rex/logging.h>

#include <atomic>
#include <cstring>
#include <mutex>
#include <shared_mutex>
#include <thread>
#include <unordered_map>
#include <vector>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#pragma comment(lib, "ws2_32.lib")
#else
#include <arpa/inet.h>
#include <netdb.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/select.h>
#include <unistd.h>
#define INVALID_SOCKET (~0ULL)
#define SOCKET_ERROR (-1)
#define closesocket close
typedef int SOCKET;
#endif

using namespace rex::runtime::guest;

// ============================================================================
// Local state
// ============================================================================

static XNADDR_LAN g_local_xnaddr;        // Our XNADDR with real LAN IP
static uint32_t    g_local_ip_net = 0;    // Our IP in network byte order
static int         g_lan_port = 3074;     // Discovery port (configurable)

// Peer table: maps IP (network order) -> peer info
struct PeerEntry {
    XNADDR_LAN xnaddr;
    uint8_t     xnkid[8];
    bool        connected;
};
static std::vector<PeerEntry> g_peers;
static std::shared_mutex      g_peers_mutex;

// QoS listener state
struct QosListenerState {
    bool     active;
    uint8_t  xnkid[8];
    uint8_t  data[DISC_MAX_QOS];
    uint16_t data_len;
};
static QosListenerState g_qos_listener = {};
static std::mutex       g_qos_mutex;

// Discovery thread
static SOCKET             g_disc_socket = (SOCKET)INVALID_SOCKET;
static std::thread        g_disc_thread;
static std::atomic<bool>  g_disc_running{false};

// Pending async recv operations
struct PendingRecv {
    uint32_t socket_handle;   // guest XSocket handle
    uint32_t buf_guest;       // guest buffer pointer (WSABUF.buf)
    uint32_t buf_len;         // buffer length
    uint32_t bytes_ptr;       // guest ptr for bytes received
    uint32_t flags_ptr;       // guest ptr for flags
    uint32_t from_ptr;        // guest ptr for sockaddr_in
    uint32_t fromlen_ptr;     // guest ptr for fromlen
};
static std::unordered_map<uint32_t, PendingRecv> g_pending_recvs;
static std::mutex g_pending_mutex;

// System-link port (game-set via XNetSetSystemLinkPort)
static uint16_t g_system_link_port = 0;

// ============================================================================
// LAN IP detection
// ============================================================================

static uint32_t GetLocalLanIP() {
    char hostname[256] = {};
    if (gethostname(hostname, sizeof(hostname)) != 0) {
        REXLOG_ERROR("[NET] gethostname failed");
        return htonl(INADDR_LOOPBACK);
    }

    struct addrinfo hints = {};
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_DGRAM;

    struct addrinfo* result = nullptr;
    if (getaddrinfo(hostname, nullptr, &hints, &result) != 0 || !result) {
        REXLOG_ERROR("[NET] getaddrinfo failed for '{}'", hostname);
        return htonl(INADDR_LOOPBACK);
    }

    uint32_t chosen_ip = htonl(INADDR_LOOPBACK);
    for (auto* ai = result; ai; ai = ai->ai_next) {
        if (ai->ai_family == AF_INET) {
            auto* sin = (struct sockaddr_in*)ai->ai_addr;
            uint32_t ip = sin->sin_addr.s_addr;
            // Skip loopback
            if ((ntohl(ip) & 0xFF000000) == 0x7F000000) continue;
            chosen_ip = ip;
            break;
        }
    }
    freeaddrinfo(result);
    return chosen_ip;
}

// ============================================================================
// XNADDR construction
// ============================================================================

static void BuildLocalXnAddr(uint32_t ip_net) {
    std::memset(&g_local_xnaddr, 0, sizeof(g_local_xnaddr));
    g_local_xnaddr.ina = ip_net;
    g_local_xnaddr.inaOnline = ip_net;
    g_local_xnaddr.wPortOnline = htons((uint16_t)g_lan_port);

    // Derive MAC from IP for uniqueness
    uint8_t* ip_bytes = (uint8_t*)&ip_net;
    g_local_xnaddr.abEnet[0] = 0x00;
    g_local_xnaddr.abEnet[1] = 0x50;
    g_local_xnaddr.abEnet[2] = ip_bytes[0];
    g_local_xnaddr.abEnet[3] = ip_bytes[1];
    g_local_xnaddr.abEnet[4] = ip_bytes[2];
    g_local_xnaddr.abEnet[5] = ip_bytes[3];
}

// ============================================================================
// Peer table helpers
// ============================================================================

static PeerEntry* FindPeerByIP(uint32_t ip_net) {
    for (auto& p : g_peers) {
        if (p.xnaddr.ina == ip_net) return &p;
    }
    return nullptr;
}

static void AddOrUpdatePeer(const XNADDR_LAN& addr, const uint8_t* xnkid) {
    std::unique_lock lock(g_peers_mutex);
    auto* existing = FindPeerByIP(addr.ina);
    if (existing) {
        existing->xnaddr = addr;
        if (xnkid) std::memcpy(existing->xnkid, xnkid, 8);
    } else {
        PeerEntry entry = {};
        entry.xnaddr = addr;
        if (xnkid) std::memcpy(entry.xnkid, xnkid, 8);
        entry.connected = false;
        g_peers.push_back(entry);
    }
}

// ============================================================================
// Discovery protocol
// ============================================================================

static void SendBeacon(SOCKET sock, const struct sockaddr_in& dest) {
    std::lock_guard lock(g_qos_mutex);
    if (!g_qos_listener.active) return;

    uint8_t buf[DISC_HEADER_LEN + 2 + DISC_MAX_QOS];
    buf[0] = DISC_MAGIC;
    buf[1] = DISC_BEACON;
    std::memcpy(buf + 2, g_qos_listener.xnkid, 8);
    std::memcpy(buf + 10, &g_local_xnaddr, 36);
    // data_len in big-endian
    uint16_t dlen = g_qos_listener.data_len;
    buf[46] = (uint8_t)(dlen >> 8);
    buf[47] = (uint8_t)(dlen & 0xFF);
    if (dlen > 0) {
        std::memcpy(buf + 48, g_qos_listener.data, dlen);
    }

    int total = 48 + (int)dlen;
    sendto(sock, (const char*)buf, total, 0,
           (const struct sockaddr*)&dest, sizeof(dest));
}

static void SendProbe(SOCKET sock, const uint8_t* target_kid) {
    uint8_t buf[DISC_HEADER_LEN];
    buf[0] = DISC_MAGIC;
    buf[1] = DISC_PROBE;
    std::memcpy(buf + 2, target_kid, 8);
    std::memcpy(buf + 10, &g_local_xnaddr, 36);

    struct sockaddr_in bcast = {};
    bcast.sin_family = AF_INET;
    bcast.sin_port = htons((uint16_t)g_lan_port);
    bcast.sin_addr.s_addr = INADDR_BROADCAST;

    sendto(sock, (const char*)buf, DISC_HEADER_LEN, 0,
           (const struct sockaddr*)&bcast, sizeof(bcast));
}

// Beacon response data returned by CollectBeaconResponses
struct BeaconResponse {
    XNADDR_LAN host_addr;
    uint8_t     xnkid[8];
    uint8_t     qos_data[DISC_MAX_QOS];
    uint16_t    qos_data_len;
};

static std::vector<BeaconResponse> CollectBeaconResponses(
    SOCKET sock, int timeout_ms) {
    std::vector<BeaconResponse> responses;
    uint8_t buf[2048];

    auto start = std::chrono::steady_clock::now();
    while (true) {
        auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - start);
        int remaining = timeout_ms - (int)elapsed.count();
        if (remaining <= 0) break;

        fd_set fds;
        FD_ZERO(&fds);
        FD_SET(sock, &fds);
        struct timeval tv;
        tv.tv_sec = remaining / 1000;
        tv.tv_usec = (remaining % 1000) * 1000;

        int sel = select((int)sock + 1, &fds, nullptr, nullptr, &tv);
        if (sel <= 0) break;

        struct sockaddr_in from = {};
        int fromlen = sizeof(from);
        int n = recvfrom(sock, (char*)buf, sizeof(buf), 0,
                         (struct sockaddr*)&from, &fromlen);
        if (n < DISC_HEADER_LEN + 2) continue;
        if (buf[0] != DISC_MAGIC || buf[1] != DISC_BEACON) continue;

        // Skip our own beacons
        XNADDR_LAN* addr = (XNADDR_LAN*)(buf + 10);
        if (addr->ina == g_local_ip_net) continue;

        BeaconResponse resp = {};
        std::memcpy(resp.xnkid, buf + 2, 8);
        std::memcpy(&resp.host_addr, buf + 10, 36);
        resp.qos_data_len = ((uint16_t)buf[46] << 8) | buf[47];
        if (resp.qos_data_len > DISC_MAX_QOS)
            resp.qos_data_len = DISC_MAX_QOS;
        if (resp.qos_data_len > 0 && n >= 48 + (int)resp.qos_data_len) {
            std::memcpy(resp.qos_data, buf + 48, resp.qos_data_len);
        }

        // Add peer
        AddOrUpdatePeer(resp.host_addr, resp.xnkid);
        responses.push_back(resp);
    }
    return responses;
}

// ============================================================================
// Discovery thread
// ============================================================================

static void DiscoveryThreadFunc() {
    REXLOG_INFO("[NET] Discovery thread started on port {}", g_lan_port);

    auto last_beacon = std::chrono::steady_clock::now() -
                       std::chrono::seconds(10); // send first beacon immediately

    while (g_disc_running.load(std::memory_order_relaxed)) {
        // Check for incoming packets
        fd_set fds;
        FD_ZERO(&fds);
        FD_SET(g_disc_socket, &fds);
        struct timeval tv = {0, 100000}; // 100ms

        int sel = select((int)g_disc_socket + 1, &fds, nullptr, nullptr, &tv);
        if (sel > 0) {
            uint8_t buf[2048];
            struct sockaddr_in from = {};
            int fromlen = sizeof(from);
            int n = recvfrom(g_disc_socket, (char*)buf, sizeof(buf), 0,
                             (struct sockaddr*)&from, &fromlen);

            if (n >= DISC_HEADER_LEN && buf[0] == DISC_MAGIC) {
                XNADDR_LAN* sender_addr = (XNADDR_LAN*)(buf + 10);

                // Skip our own packets
                if (sender_addr->ina != g_local_ip_net) {
                    if (buf[1] == DISC_PROBE) {
                        // Someone is looking for us — respond with beacon
                        // directly to them
                        struct sockaddr_in reply = {};
                        reply.sin_family = AF_INET;
                        reply.sin_port = htons((uint16_t)g_lan_port);
                        reply.sin_addr.s_addr = from.sin_addr.s_addr;
                        SendBeacon(g_disc_socket, reply);
                    }
                    else if (buf[1] == DISC_BEACON && n >= DISC_HEADER_LEN + 2) {
                        // Received a beacon — add/update peer
                        uint8_t* xnkid = buf + 2;
                        AddOrUpdatePeer(*sender_addr, xnkid);
                    }
                }
            }
        }

        // Broadcast beacons periodically if hosting (QoS listener active)
        auto now = std::chrono::steady_clock::now();
        auto since_beacon = std::chrono::duration_cast<std::chrono::milliseconds>(
            now - last_beacon);
        if (since_beacon.count() >= 500) {
            struct sockaddr_in bcast = {};
            bcast.sin_family = AF_INET;
            bcast.sin_port = htons((uint16_t)g_lan_port);
            bcast.sin_addr.s_addr = INADDR_BROADCAST;
            SendBeacon(g_disc_socket, bcast);
            last_beacon = now;
        }
    }

    REXLOG_INFO("[NET] Discovery thread stopped");
}

// ============================================================================
// Init / Shutdown
// ============================================================================

void NetInit(int lan_port) {
    g_lan_port = lan_port;

#ifdef _WIN32
    WSADATA wsa;
    WSAStartup(MAKEWORD(2, 2), &wsa);
#endif

    // Detect LAN IP
    g_local_ip_net = GetLocalLanIP();
    BuildLocalXnAddr(g_local_ip_net);

    {
        char ip_str[INET_ADDRSTRLEN] = {};
        struct in_addr a;
        a.s_addr = g_local_ip_net;
        inet_ntop(AF_INET, &a, ip_str, sizeof(ip_str));
        REXLOG_INFO("[NET] Local LAN IP: {}, port: {}", ip_str, g_lan_port);
    }

    // Create discovery socket
    g_disc_socket = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (g_disc_socket == (SOCKET)INVALID_SOCKET) {
        REXLOG_ERROR("[NET] Failed to create discovery socket");
        return;
    }

    // Enable broadcast + address reuse
    int opt = 1;
    setsockopt(g_disc_socket, SOL_SOCKET, SO_BROADCAST,
               (const char*)&opt, sizeof(opt));
    setsockopt(g_disc_socket, SOL_SOCKET, SO_REUSEADDR,
               (const char*)&opt, sizeof(opt));

    // Bind to discovery port
    struct sockaddr_in bind_addr = {};
    bind_addr.sin_family = AF_INET;
    bind_addr.sin_port = htons((uint16_t)g_lan_port);
    bind_addr.sin_addr.s_addr = INADDR_ANY;

    if (bind(g_disc_socket, (struct sockaddr*)&bind_addr, sizeof(bind_addr)) < 0) {
        REXLOG_ERROR("[NET] Failed to bind discovery socket to port {}", g_lan_port);
        closesocket(g_disc_socket);
        g_disc_socket = (SOCKET)INVALID_SOCKET;
        return;
    }

    // Start discovery thread
    g_disc_running.store(true, std::memory_order_release);
    g_disc_thread = std::thread(DiscoveryThreadFunc);

    REXLOG_INFO("[NET] LAN networking initialized");
}

void NetShutdown() {
    // Stop discovery thread
    g_disc_running.store(false, std::memory_order_release);
    if (g_disc_thread.joinable()) {
        g_disc_thread.join();
    }

    // Close discovery socket
    if (g_disc_socket != (SOCKET)INVALID_SOCKET) {
        closesocket(g_disc_socket);
        g_disc_socket = (SOCKET)INVALID_SOCKET;
    }

    // Clear state
    {
        std::unique_lock lock(g_peers_mutex);
        g_peers.clear();
    }
    {
        std::lock_guard lock(g_qos_mutex);
        g_qos_listener.active = false;
    }
    {
        std::lock_guard lock(g_pending_mutex);
        g_pending_recvs.clear();
    }

#ifdef _WIN32
    WSACleanup();
#endif

    REXLOG_INFO("[NET] LAN networking shut down");
}

// ============================================================================
// XNet Address Overrides
// ============================================================================

// XNetGetTitleXnAddr(r4=xnaddr_ptr) -> XNET_GET_XNADDR_STATIC
extern "C" PPC_FUNC(__imp__NetDll_XNetGetTitleXnAddr) {
    uint32_t xnaddr_ptr = ctx.r4.u32;

    if (xnaddr_ptr) {
        // Write our real LAN XNADDR to guest memory (raw bytes, no swap)
        std::memcpy(base + xnaddr_ptr, &g_local_xnaddr, sizeof(XNADDR_LAN));
    }

    ctx.r3.u64 = XNET_GET_XNADDR_STATIC;
}

// XNetXnAddrToInAddr(r4=xnaddr_ptr, r5=xnkid_ptr, r6=in_addr_ptr) -> 0
extern "C" PPC_FUNC(__imp__NetDll_XNetXnAddrToInAddr) {
    uint32_t xnaddr_ptr = ctx.r4.u32;
    uint32_t xnkid_ptr  = ctx.r5.u32;
    uint32_t inaddr_ptr  = ctx.r6.u32;

    if (!xnaddr_ptr || !inaddr_ptr) {
        ctx.r3.u64 = 0x80070057; // E_INVALIDARG
        return;
    }

    // Read XNADDR from guest (raw bytes — it's already in our struct layout)
    XNADDR_LAN addr;
    std::memcpy(&addr, base + xnaddr_ptr, sizeof(XNADDR_LAN));

    // Read XNKID if present
    uint8_t xnkid[8] = {};
    if (xnkid_ptr) {
        std::memcpy(xnkid, base + xnkid_ptr, 8);
    }

    // Add to peer table
    AddOrUpdatePeer(addr, xnkid);

    // Write the IP to guest memory (raw 4 bytes, network byte order — no swap)
    std::memcpy(base + inaddr_ptr, &addr.ina, 4);

    ctx.r3.u64 = 0; // success
}

// XNetInAddrToXnAddr(r4=in_addr_ptr, r5=xnaddr_out, r6=xnkid_out) -> 0
extern "C" PPC_FUNC(__imp__NetDll_XNetInAddrToXnAddr) {
    uint32_t inaddr_ptr  = ctx.r4.u32;
    uint32_t xnaddr_out  = ctx.r5.u32;
    uint32_t xnkid_out   = ctx.r6.u32;

    if (!inaddr_ptr) {
        ctx.r3.u64 = 0x80070057;
        return;
    }

    // Read IP from guest (raw 4 bytes, network byte order)
    uint32_t ip_net;
    std::memcpy(&ip_net, base + inaddr_ptr, 4);

    std::shared_lock lock(g_peers_mutex);
    auto* peer = FindPeerByIP(ip_net);

    if (peer) {
        if (xnaddr_out) {
            std::memcpy(base + xnaddr_out, &peer->xnaddr, sizeof(XNADDR_LAN));
        }
        if (xnkid_out) {
            std::memcpy(base + xnkid_out, peer->xnkid, 8);
        }
        ctx.r3.u64 = 0;
    } else {
        // Peer not found — synthesize from IP
        XNADDR_LAN synth = {};
        synth.ina = ip_net;
        synth.inaOnline = ip_net;
        uint8_t* ip_bytes = (uint8_t*)&ip_net;
        synth.abEnet[0] = 0x00;
        synth.abEnet[1] = 0x50;
        synth.abEnet[2] = ip_bytes[0];
        synth.abEnet[3] = ip_bytes[1];
        synth.abEnet[4] = ip_bytes[2];
        synth.abEnet[5] = ip_bytes[3];

        if (xnaddr_out) {
            std::memcpy(base + xnaddr_out, &synth, sizeof(XNADDR_LAN));
        }
        if (xnkid_out) {
            std::memset(base + xnkid_out, 0, 8);
        }
        ctx.r3.u64 = 0;
    }
}

// XNetConnect(r4=in_addr_ptr, r5=xnkid_ptr) -> 0
extern "C" PPC_FUNC(__imp__NetDll_XNetConnect) {
    uint32_t inaddr_ptr = ctx.r4.u32;

    if (inaddr_ptr) {
        uint32_t ip_net;
        std::memcpy(&ip_net, base + inaddr_ptr, 4);

        std::unique_lock lock(g_peers_mutex);
        auto* peer = FindPeerByIP(ip_net);
        if (peer) {
            peer->connected = true;
        } else {
            // Create peer entry for this IP
            PeerEntry entry = {};
            entry.xnaddr.ina = ip_net;
            entry.xnaddr.inaOnline = ip_net;
            entry.connected = true;
            g_peers.push_back(entry);
        }
    }

    ctx.r3.u64 = 0;
}

// XNetGetConnectStatus(r4=in_addr_ptr) -> status
extern "C" PPC_FUNC(__imp__NetDll_XNetGetConnectStatus) {
    uint32_t inaddr_ptr = ctx.r4.u32;

    if (inaddr_ptr) {
        uint32_t ip_net;
        std::memcpy(&ip_net, base + inaddr_ptr, 4);

        std::shared_lock lock(g_peers_mutex);
        auto* peer = FindPeerByIP(ip_net);
        if (peer && peer->connected) {
            ctx.r3.u64 = XNET_CONNECT_STATUS_CONNECTED;
            return;
        }
    }

    ctx.r3.u64 = XNET_CONNECT_STATUS_IDLE;
}

// XNetUnregisterInAddr(r4=in_addr_ptr) -> 0
extern "C" PPC_FUNC(__imp__NetDll_XNetUnregisterInAddr) {
    uint32_t inaddr_ptr = ctx.r4.u32;

    if (inaddr_ptr) {
        uint32_t ip_net;
        std::memcpy(&ip_net, base + inaddr_ptr, 4);

        std::unique_lock lock(g_peers_mutex);
        for (auto it = g_peers.begin(); it != g_peers.end(); ++it) {
            if (it->xnaddr.ina == ip_net) {
                g_peers.erase(it);
                break;
            }
        }
    }

    ctx.r3.u64 = 0;
}

// XNetGetEthernetLinkStatus() -> flags
extern "C" PPC_FUNC(__imp__NetDll_XNetGetEthernetLinkStatus) {
    (void)base;
    ctx.r3.u64 = XNET_ETHERNET_LINK_ACTIVE |
                 XNET_ETHERNET_LINK_100MBPS |
                 XNET_ETHERNET_LINK_FULL_DUPLEX;
}

// XNetSetSystemLinkPort(r4=port) -> 0
extern "C" PPC_FUNC(__imp__NetDll_XNetSetSystemLinkPort) {
    g_system_link_port = (uint16_t)ctx.r4.u32;
    ctx.r3.u64 = 0;
}

// ============================================================================
// QoS Discovery Overrides
// ============================================================================

// XNetQosListen(r4=xnkid_ptr, r5=data_ptr, r6=data_len, r7=bits, r8=flags)
extern "C" PPC_FUNC(__imp__NetDll_XNetQosListen) {
    uint32_t xnkid_ptr = ctx.r4.u32;
    uint32_t data_ptr  = ctx.r5.u32;
    uint32_t data_len  = ctx.r6.u32;
    // r7 = dwBitsPerSec (unused)
    uint32_t flags     = ctx.r8.u32;

    std::lock_guard lock(g_qos_mutex);

    if (flags & 1) {
        // RELEASE: stop listening
        g_qos_listener.active = false;
        REXLOG_INFO("[NET] QoS listener released");
    } else if ((flags & 4) || (flags & 2)) {
        // SET_DATA or LISTEN: activate listener
        g_qos_listener.active = true;

        if (xnkid_ptr) {
            std::memcpy(g_qos_listener.xnkid, base + xnkid_ptr, 8);
        }

        if (data_ptr && data_len > 0) {
            if (data_len > DISC_MAX_QOS) data_len = DISC_MAX_QOS;
            std::memcpy(g_qos_listener.data, base + data_ptr, data_len);
            g_qos_listener.data_len = (uint16_t)data_len;
        }

        REXLOG_INFO("[NET] QoS listener active (data_len={})", g_qos_listener.data_len);
    }

    ctx.r3.u64 = 0;
}

// XNetQosLookup: complex function with stack parameters
// r4=cxna, r5=apxna, r6=apxnkid, r7=apxnkey, r8=cina, r9=aina, r10=aProbeFlags
// stack+84=dwBitsPerSec, stack+92=cProbes, stack+100=dwTimeout, stack+108=ppxnqos
extern "C" PPC_FUNC(__imp__NetDll_XNetQosLookup) {
    uint32_t cxna        = ctx.r4.u32;
    uint32_t apxna       = ctx.r5.u32;
    uint32_t apxnkid     = ctx.r6.u32;
    // r7=apxnkey, r8=cina, r9=aina, r10=aProbeFlags (unused for LAN)
    // Stack parameters
    // uint32_t dwBitsPerSec = PPC_LOAD_U32(ctx.r1.u32 + 84);
    // uint32_t cProbes      = PPC_LOAD_U32(ctx.r1.u32 + 92);
    uint32_t dwTimeout    = PPC_LOAD_U32(ctx.r1.u32 + 100);
    uint32_t ppxnqos      = PPC_LOAD_U32(ctx.r1.u32 + 108);

    REXLOG_INFO("[NET] XNetQosLookup: cxna={}, timeout={}, ppxnqos=0x{:08X}",
                cxna, dwTimeout, ppxnqos);

    // Read target XNKID(s) for probe
    uint8_t target_kid[8] = {};
    if (apxnkid && cxna > 0) {
        std::memcpy(target_kid, base + apxnkid, 8);
    }

    // Send probe(s) on discovery socket
    if (g_disc_socket != (SOCKET)INVALID_SOCKET) {
        SendProbe(g_disc_socket, target_kid);
    }

    // Collect responses (use short timeout — LAN latency is <1ms)
    int collect_timeout = (dwTimeout > 0 && dwTimeout < 500) ? (int)dwTimeout : 200;
    std::vector<BeaconResponse> responses;
    if (g_disc_socket != (SOCKET)INVALID_SOCKET) {
        responses = CollectBeaconResponses(g_disc_socket, collect_timeout);
    }

    // Also check for responses matching specific target addresses
    if (apxna && cxna > 0 && responses.empty()) {
        // If we got no broadcast responses, try to find matching peers
        std::shared_lock lock(g_peers_mutex);
        for (uint32_t i = 0; i < cxna; i++) {
            XNADDR_LAN addr;
            std::memcpy(&addr, base + apxna + i * sizeof(XNADDR_LAN),
                        sizeof(XNADDR_LAN));
            auto* peer = FindPeerByIP(addr.ina);
            if (peer) {
                // Peer known but no beacon received — still include in results
                // with empty QoS data (the game may handle this)
            }
        }
    }

    uint32_t count = (uint32_t)responses.size();

    // Allocate XNQOS result structure in guest memory
    // Layout:
    //   +0  uint32_t count
    //   +4  uint32_t count_pending (0 = complete)
    //   +8  XNQOSINFO[count] (24 bytes each)
    //   After entries: QoS data blobs
    //
    // XNQOSINFO (24 bytes):
    //   +0  uint8_t  bFlags
    //   +1  uint8_t  bReserved
    //   +2  uint16_t cProbesXmit
    //   +4  uint16_t cProbesRecv
    //   +6  uint16_t cbData
    //   +8  uint32_t pbData (guest ptr)
    //   +12 uint16_t wRttMedian
    //   +14 uint16_t wRttMinimum
    //   +16 uint32_t dwUpBitsPerSec
    //   +20 uint32_t dwDnBitsPerSec

    auto* mem = rex::kernel::kernel_state()->memory();
    uint32_t header_size = 8;
    uint32_t entry_size = 24;
    uint32_t entries_size = count * entry_size;

    // Calculate total data blob sizes
    uint32_t total_data = 0;
    for (auto& r : responses) total_data += r.qos_data_len;

    uint32_t alloc_size = header_size + entries_size + total_data;
    if (alloc_size < 8) alloc_size = 8; // minimum allocation

    uint32_t qos_addr = mem->SystemHeapAlloc(alloc_size, 0x10);
    if (!qos_addr) {
        REXLOG_ERROR("[NET] Failed to allocate XNQOS ({} bytes)", alloc_size);
        ctx.r3.u64 = 0x8007000E; // E_OUTOFMEMORY
        return;
    }

    // Zero the entire allocation
    std::memset(base + qos_addr, 0, alloc_size);

    // Write header
    PPC_STORE_U32(qos_addr + 0, count);       // cxnqos
    PPC_STORE_U32(qos_addr + 4, 0);           // cxnqosPending = 0 (complete)

    // Write entries and copy data blobs
    uint32_t data_offset = header_size + entries_size;
    for (uint32_t i = 0; i < count; i++) {
        auto& r = responses[i];
        uint32_t entry_addr = qos_addr + header_size + i * entry_size;
        uint32_t data_addr = qos_addr + data_offset;

        // bFlags: 0x01 (COMPLETE) | 0x02 (TARGET)
        PPC_STORE_U8(entry_addr + 0, 0x03);
        // bReserved
        PPC_STORE_U8(entry_addr + 1, 0);
        // cProbesXmit / cProbesRecv
        PPC_STORE_U16(entry_addr + 2, 1);
        PPC_STORE_U16(entry_addr + 4, 1);
        // cbData
        PPC_STORE_U16(entry_addr + 6, r.qos_data_len);
        // pbData (guest pointer)
        if (r.qos_data_len > 0) {
            PPC_STORE_U32(entry_addr + 8, data_addr);
            std::memcpy(base + data_addr, r.qos_data, r.qos_data_len);
            data_offset += r.qos_data_len;
        } else {
            PPC_STORE_U32(entry_addr + 8, 0);
        }
        // wRttMedian / wRttMinimum (1ms — LAN)
        PPC_STORE_U16(entry_addr + 12, 1);
        PPC_STORE_U16(entry_addr + 14, 1);
        // dwUpBitsPerSec / dwDnBitsPerSec (10 Mbps)
        PPC_STORE_U32(entry_addr + 16, 10000000);
        PPC_STORE_U32(entry_addr + 20, 10000000);
    }

    // Write XNQOS pointer to guest output
    if (ppxnqos) {
        PPC_STORE_U32(ppxnqos, qos_addr);
    }

    REXLOG_INFO("[NET] QoS lookup complete: {} results, alloc=0x{:08X}",
                count, qos_addr);
    ctx.r3.u64 = 0;
}

// XNetQosRelease(r4=xnqos_ptr) -> 0
extern "C" PPC_FUNC(__imp__NetDll_XNetQosRelease) {
    uint32_t qos_ptr = ctx.r4.u32;

    if (qos_ptr) {
        auto* mem = rex::kernel::kernel_state()->memory();
        mem->SystemHeapFree(qos_ptr);
    }

    ctx.r3.u64 = 0;
}

// ============================================================================
// Async Receive Overrides
// ============================================================================

// WSARecvFrom(r4=socket, r5=bufs, r6=buf_count, r7=bytes_ptr,
//             r8=flags_ptr, r9=from, r10=fromlen, stack+84=overlapped)
extern "C" PPC_FUNC(__imp__NetDll_WSARecvFrom) {
    uint32_t socket_handle = ctx.r4.u32;
    uint32_t bufs_ptr      = ctx.r5.u32;
    uint32_t buf_count     = ctx.r6.u32;
    uint32_t bytes_ptr     = ctx.r7.u32;
    uint32_t flags_ptr     = ctx.r8.u32;
    uint32_t from_ptr      = ctx.r9.u32;
    uint32_t fromlen_ptr   = ctx.r10.u32;
    uint32_t overlapped    = PPC_LOAD_U32(ctx.r1.u32 + 84);

    (void)buf_count; // We handle the first buffer only

    // Read WSABUF from guest memory
    // WSABUF layout (big-endian): +0 uint32_t len, +4 uint32_t buf_ptr
    uint32_t buf_len = 0;
    uint32_t buf_guest = 0;
    if (bufs_ptr) {
        buf_len = PPC_LOAD_U32(bufs_ptr + 0);
        buf_guest = PPC_LOAD_U32(bufs_ptr + 4);
    }

    // Look up the XSocket
    auto* ks = rex::kernel::kernel_state();
    auto socket_obj = ks->object_table()->LookupObject<rex::kernel::XSocket>(
        socket_handle);
    if (!socket_obj) {
        // Invalid socket handle — return error
        ctx.r3.u64 = (uint32_t)-1; // SOCKET_ERROR
        return;
    }

    SOCKET native = (SOCKET)socket_obj->native_handle();

    // Try non-blocking receive
    struct sockaddr_in from_addr = {};
    int from_len = sizeof(from_addr);
    uint8_t temp_buf[65536];
    uint32_t recv_len = (buf_len < sizeof(temp_buf)) ? buf_len : sizeof(temp_buf);

    // Set non-blocking for this attempt
#ifdef _WIN32
    u_long nonblock = 1;
    ioctlsocket(native, FIONBIO, &nonblock);
#else
    int flags_val = fcntl(native, F_GETFL, 0);
    fcntl(native, F_SETFL, flags_val | O_NONBLOCK);
#endif

    int n = recvfrom(native, (char*)temp_buf, recv_len, 0,
                     (struct sockaddr*)&from_addr, &from_len);

    if (n > 0) {
        // Data received — copy to guest buffer
        if (buf_guest && (uint32_t)n <= buf_len) {
            std::memcpy(base + buf_guest, temp_buf, n);
        }
        if (bytes_ptr) {
            PPC_STORE_U32(bytes_ptr, (uint32_t)n);
        }
        if (flags_ptr) {
            PPC_STORE_U32(flags_ptr, 0);
        }
        // Write source address to guest
        if (from_ptr) {
            // sockaddr_in layout for Xbox (big-endian):
            // +0 uint16_t sin_family, +2 uint16_t sin_port,
            // +4 uint32_t sin_addr, +8 zero[8]
            PPC_STORE_U16(from_ptr + 0, AF_INET);
            // Port and addr are already in network byte order
            std::memcpy(base + from_ptr + 2, &from_addr.sin_port, 2);
            std::memcpy(base + from_ptr + 4, &from_addr.sin_addr, 4);
            std::memset(base + from_ptr + 8, 0, 8);
        }
        if (fromlen_ptr) {
            PPC_STORE_U32(fromlen_ptr, 16);
        }
        // Mark overlapped as complete if present
        if (overlapped) {
            // XOVERLAPPED: +0 InternalLow, +4 InternalHigh, +8 Offset, +12 OffsetHigh,
            //              +16 hEvent, +20 extended
            PPC_STORE_U32(overlapped + 0, 0); // status = complete
            PPC_STORE_U32(overlapped + 4, (uint32_t)n); // bytes transferred
        }
        ctx.r3.u64 = 0; // success
    } else {
        // No data yet — store as pending operation
        if (overlapped) {
            std::lock_guard lock(g_pending_mutex);
            PendingRecv pr;
            pr.socket_handle = socket_handle;
            pr.buf_guest = buf_guest;
            pr.buf_len = buf_len;
            pr.bytes_ptr = bytes_ptr;
            pr.flags_ptr = flags_ptr;
            pr.from_ptr = from_ptr;
            pr.fromlen_ptr = fromlen_ptr;
            g_pending_recvs[overlapped] = pr;

            // Mark overlapped as pending
            PPC_STORE_U32(overlapped + 0, 0x00000103); // STATUS_PENDING
        }

        // Return -1 with WSA_IO_PENDING
        ctx.r3.u64 = (uint32_t)-1;
        // The game should check WSAGetLastError which will return IO_PENDING
    }
}

// WSAGetOverlappedResult(r4=socket, r5=overlapped, r6=bytes_ptr,
//                        r7=wait, r8=flags_ptr)
extern "C" PPC_FUNC(__imp__NetDll_WSAGetOverlappedResult) {
    uint32_t socket_handle = ctx.r4.u32;
    uint32_t overlapped    = ctx.r5.u32;
    uint32_t bytes_ptr     = ctx.r6.u32;
    // r7=wait (ignored — we poll)
    uint32_t flags_ptr     = ctx.r8.u32;

    (void)socket_handle;

    std::lock_guard lock(g_pending_mutex);
    auto it = g_pending_recvs.find(overlapped);
    if (it == g_pending_recvs.end()) {
        // Check if the overlapped was already completed (InternalLow == 0)
        if (overlapped) {
            uint32_t status = PPC_LOAD_U32(overlapped + 0);
            if (status == 0) {
                // Already complete
                uint32_t transferred = PPC_LOAD_U32(overlapped + 4);
                if (bytes_ptr) PPC_STORE_U32(bytes_ptr, transferred);
                if (flags_ptr) PPC_STORE_U32(flags_ptr, 0);
                ctx.r3.u64 = 1; // TRUE
                return;
            }
        }
        // No pending operation found
        ctx.r3.u64 = 0; // FALSE
        return;
    }

    auto& pr = it->second;

    // Try to receive data now
    auto* ks = rex::kernel::kernel_state();
    auto socket_obj = ks->object_table()->LookupObject<rex::kernel::XSocket>(
        pr.socket_handle);
    if (!socket_obj) {
        g_pending_recvs.erase(it);
        ctx.r3.u64 = 0;
        return;
    }

    SOCKET native = (SOCKET)socket_obj->native_handle();

    struct sockaddr_in from_addr = {};
    int from_len = sizeof(from_addr);
    uint8_t temp_buf[65536];
    uint32_t recv_len = (pr.buf_len < sizeof(temp_buf)) ? pr.buf_len
                                                        : sizeof(temp_buf);

#ifdef _WIN32
    u_long nonblock = 1;
    ioctlsocket(native, FIONBIO, &nonblock);
#else
    int flags_val = fcntl(native, F_GETFL, 0);
    fcntl(native, F_SETFL, flags_val | O_NONBLOCK);
#endif

    int n = recvfrom(native, (char*)temp_buf, recv_len, 0,
                     (struct sockaddr*)&from_addr, &from_len);

    if (n > 0) {
        // Copy to guest buffers
        if (pr.buf_guest && (uint32_t)n <= pr.buf_len) {
            std::memcpy(base + pr.buf_guest, temp_buf, n);
        }
        if (pr.bytes_ptr) {
            PPC_STORE_U32(pr.bytes_ptr, (uint32_t)n);
        }
        if (pr.flags_ptr) {
            PPC_STORE_U32(pr.flags_ptr, 0);
        }
        if (pr.from_ptr) {
            PPC_STORE_U16(pr.from_ptr + 0, AF_INET);
            std::memcpy(base + pr.from_ptr + 2, &from_addr.sin_port, 2);
            std::memcpy(base + pr.from_ptr + 4, &from_addr.sin_addr, 4);
            std::memset(base + pr.from_ptr + 8, 0, 8);
        }
        if (pr.fromlen_ptr) {
            PPC_STORE_U32(pr.fromlen_ptr, 16);
        }

        // Mark overlapped complete
        if (overlapped) {
            PPC_STORE_U32(overlapped + 0, 0); // complete
            PPC_STORE_U32(overlapped + 4, (uint32_t)n);
        }

        // Write to output params
        if (bytes_ptr) PPC_STORE_U32(bytes_ptr, (uint32_t)n);
        if (flags_ptr) PPC_STORE_U32(flags_ptr, 0);

        g_pending_recvs.erase(it);
        ctx.r3.u64 = 1; // TRUE = success
    } else {
        // Still no data
        ctx.r3.u64 = 0; // FALSE = incomplete
    }
}
