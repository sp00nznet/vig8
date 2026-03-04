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
#include "xlive.h"

#include <rex/runtime/guest/context.h>
#include <rex/runtime/guest/memory.h>
#include <rex/kernel/kernel_state.h>
#include <rex/kernel/xsocket.h>
#include <rex/logging.h>

#include <algorithm>
#include <atomic>
#include <cstdlib>
#include <cstring>
#include <ctime>
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

// Guest memory base — stored from the first PPC_FUNC call so the discovery
// thread can write to guest memory for deferred QoS completion.
static uint8_t* g_base = nullptr;

// Deferred QoS completion: when XNetQosLookup finds sessions it writes
// cxnqosPending = N (pending) and schedules a completion here.  The
// discovery thread sets cxnqosPending = 0 after a short delay, giving the
// game time to finish initialising the session struct (specifically r31+8,
// the completion-callback object) before we trigger the callback path.
struct PendingQosCompletion {
    uint32_t qos_addr;  // guest address of XNQOS struct
    std::chrono::steady_clock::time_point ready_at;
};
static std::vector<PendingQosCompletion> g_pending_qos;
static std::mutex                        g_pending_qos_mutex;

// Discovery thread
static SOCKET             g_disc_socket = (SOCKET)INVALID_SOCKET;
static std::thread        g_disc_thread;
static std::atomic<bool>  g_disc_running{false};

// Step tracker for crash diagnostics: updated at each major step in DiscoveryThreadFunc
// so the crash handler can report exactly where the thread was.
std::atomic<int> g_disc_step{0};

// Pending overlapped completions — for XGI SEARCH we delay the completion
// so the game has time to store the callback object at overlapped+8 before
// sub_8218A068 tries to dereference it.
struct PendingOverlappedCompletion {
    uint32_t overlapped_ptr;
    uint32_t result;
    std::chrono::steady_clock::time_point fire_at;
};
static std::vector<PendingOverlappedCompletion> g_pending_ov;
static std::mutex                               g_pending_ov_mutex;

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
// XGI session intercept — overrides __imp__XMsgStartIORequest to handle
// session create/search/delete via xlive relay.
//
// The SDK's XgiApp is already registered at app_id=251 and handles many XGI
// messages. We cannot replace it via RegisterApp (key already exists in map).
// Instead we override the XMsgStartIORequest PPC import directly. For session
// messages we care about we handle them ourselves; everything else is forwarded
// to the AppManager as normal.
//
// Message IDs (determined from vig8_recomp.9.cpp PPC disassembly):
//   0x000B0010  XGISessionCreateImpl  (28 bytes) — host creates a session
//   0x000B001C  XGISessionSearch      (36 bytes) — search for sessions
//   0x000B0011  XGISessionDelete      (16 bytes) — delete/end session
//   0x000B0012  XGISessionJoinRemote  (20 bytes) — join from search result
// ============================================================================

// V8 Arcade title ID
static constexpr uint32_t VIG8_TITLE_ID = 0x584109A8;

// Currently hosted session XNKID
static uint8_t  g_session_xnkid[8] = {};
static bool     g_session_active   = false;
static std::mutex g_session_mutex;

// Forward declaration (defined below in peer-table section)
static void AddOrUpdatePeer(const XNADDR_LAN& addr, const uint8_t* xnkid);

// Big-endian read/write helpers for guest memory
static inline uint32_t GuestReadU32(uint8_t* base, uint32_t addr) {
    uint32_t v;
    std::memcpy(&v, base + addr, 4);
    return ((v & 0xFF000000u) >> 24) | ((v & 0x00FF0000u) >> 8)
         | ((v & 0x0000FF00u) << 8)  | ((v & 0x000000FFu) << 24);
}
static inline void GuestWriteU32(uint8_t* base, uint32_t addr, uint32_t val) {
    uint32_t be = ((val & 0xFF000000u) >> 24) | ((val & 0x00FF0000u) >> 8)
                | ((val & 0x0000FF00u) << 8)  | ((val & 0x000000FFu) << 24);
    std::memcpy(base + addr, &be, 4);
}

// ---- XGISessionCreateImpl (0x000B0010, 28 bytes) ----
// Buffer layout (from XgiApp.cpp + vig8_recomp.9.cpp disasm):
//   [0x00] session_ptr        (IN  — XamSessionRefObjByHandle result)
//   [0x04] flags              (IN)
//   [0x08] num_public_slots   (IN)
//   [0x0C] num_private_slots  (IN)
//   [0x10] user_xuid          (IN)
//   [0x14] session_info_ptr   (OUT — write XNKID(8)+XNADDR(36) here)
//   [0x18] nonce_ptr          (IN)
static rex::X_HRESULT HandleXgiCreate(uint8_t* base, uint32_t buf, uint32_t len) {
    if (!buf || len < 0x14 + 4) {
        REXLOG_WARN("[XGI] CREATE: buffer too small (len={})", len);
        fprintf(stderr, "[XGI] CREATE: buffer too small (len=%u)\n", len);
        fflush(stderr);
        return 0x80004005;
    }
    uint32_t session_info_ptr = GuestReadU32(base, buf + 0x14);
    uint32_t num_public = GuestReadU32(base, buf + 0x08);
    REXLOG_INFO("[XGI] CREATE session_info_ptr=0x{:08X} pub_slots={}", session_info_ptr, num_public);
    fprintf(stderr, "[XGI] CREATE session_info_ptr=0x%08X pub_slots=%u\n", session_info_ptr, num_public);
    fflush(stderr);

    // Generate XNKID from local IP + time
    uint8_t xnkid[8];
    uint32_t seed = g_local_ip_net ^ (uint32_t)std::time(nullptr);
    for (int i = 0; i < 8; i++) {
        seed = seed * 1664525u + 1013904223u;
        xnkid[i] = (uint8_t)(seed >> 24);
    }

    {
        std::lock_guard<std::mutex> lk(g_session_mutex);
        std::memcpy(g_session_xnkid, xnkid, 8);
        g_session_active = true;
    }

    // Write XSESSION_INFO = XNKID(8) + XNADDR(36)
    if (session_info_ptr) {
        std::memcpy(base + session_info_ptr,      xnkid,           8);
        std::memcpy(base + session_info_ptr + 8,  &g_local_xnaddr, sizeof(XNADDR_LAN));
    }

    if (xlive::IsConnected()) {
        xlive::RegisterSession(xnkid, nullptr, 0, (uint8_t)num_public);
        fprintf(stderr, "[XGI] CREATE: registered session with relay\n");
    } else {
        fprintf(stderr, "[XGI] CREATE: relay not connected, session is LAN-only\n");
    }
    fflush(stderr);

    REXLOG_INFO("[XGI] CREATE ok xnkid={:02X}{:02X}{:02X}{:02X}...",
                xnkid[0], xnkid[1], xnkid[2], xnkid[3]);
    return 0;
}

// ---- XGISessionSearch (0x000B001C, 36 bytes) ----
// Buffer layout (from vig8_recomp.9.cpp line ~49340):
//   [0x00] user_index          (IN)
//   [0x04] search_filter_id    (IN)
//   [0x08] max_results         (IN — max sessions to return)
//   [0x0C] num_properties      (IN — uint16)
//   [0x0E] num_contexts        (IN — uint16)
//   [0x10] properties_ptr      (IN)
//   [0x14] contexts_ptr        (IN)
//   [0x18] buf_size            (IN — size of output buffer in bytes)
//   [0x1C] output_buf_ptr      (IN — pointer to output buffer)
//   [0x20] ???
//
// Output buffer layout (at output_buf_ptr):
//   [0]    count of results found  (uint32 big-endian)
//   [4]    reserved (zeroed)
//   [8 + i*1326]  XSESSION_SEARCHRESULT entry i
//
// XSESSION_SEARCHRESULT (base 64 bytes, padded to 1326 with property data):
//   [0..7]  XNKID
//   [8..43] XNADDR
//   [44]    cOpenPublicSlots
//   [45]    cOpenPrivateSlots
//   [46]    cFilledPublicSlots
//   [47]    cFilledPrivateSlots
//   [48..63] dwNumProperties, dwNumContexts, pProperties, pContexts (all 0)
static rex::X_HRESULT HandleXgiSearch(uint8_t* base, uint32_t buf, uint32_t len) {
    if (!buf || len < 0x20) {
        REXLOG_WARN("[XGI] SEARCH: buffer too small (len={})", len);
        return 0x80004005;
    }
    uint32_t max_results  = GuestReadU32(base, buf + 0x08);
    uint32_t buf_size     = GuestReadU32(base, buf + 0x18);
    uint32_t output_ptr   = GuestReadU32(base, buf + 0x1C);

    REXLOG_INFO("[XGI] SEARCH max={} buf_size={} output=0x{:08X}",
                max_results, buf_size, output_ptr);
    fprintf(stderr, "[XGI] SEARCH max=%u buf_size=%u output=0x%08X relay=%s\n",
            max_results, buf_size, output_ptr,
            xlive::IsConnected() ? "connected" : "NOT CONNECTED");
    fflush(stderr);

    // Log all buffer dwords for debugging
    for (int i = 0; i < 9; i++) {
        REXLOG_INFO("[XGI] SEARCH buf[{}]=0x{:08X}", i*4, GuestReadU32(base, buf + i*4));
    }

    if (!output_ptr || max_results == 0) {
        return 0;
    }

    xlive::Session sessions[8];
    int cap = (int)std::min(max_results, 8u);
    int count = xlive::IsConnected() ? xlive::SearchSessions(sessions, cap) : 0;
    if (count < 0) count = 0;

    constexpr uint32_t ENTRY_STRIDE = 1326; // bytes per XSESSION_SEARCHRESULT

    for (int i = 0; i < count; i++) {
        uint32_t entry = output_ptr + 8 + (uint32_t)i * ENTRY_STRIDE;
        std::memset(base + entry, 0, ENTRY_STRIDE);

        // XSESSION_INFO: XNKID(8) + XNADDR(36)
        std::memcpy(base + entry,      sessions[i].xnkid,       8);
        std::memcpy(base + entry + 8,  sessions[i].host_xnaddr, 36);

        uint8_t filled = sessions[i].current_players;
        uint8_t maxp   = sessions[i].max_players;
        uint8_t open   = (maxp > filled) ? (maxp - filled) : 0;
        *(base + entry + 44) = open;
        *(base + entry + 46) = filled;

        XNADDR_LAN host_addr;
        std::memcpy(&host_addr, sessions[i].host_xnaddr, sizeof(XNADDR_LAN));
        AddOrUpdatePeer(host_addr, sessions[i].xnkid);

        REXLOG_INFO("[XGI] SEARCH result[{}]: xnkid={:02X}{:02X}.. ip={:08X}",
                    i, sessions[i].xnkid[0], sessions[i].xnkid[1], ntohl(host_addr.ina));
    }

    // Write result count at output_ptr[0] (big-endian)
    GuestWriteU32(base, output_ptr, (uint32_t)count);
    REXLOG_INFO("[XGI] SEARCH found {} sessions", count);
    fprintf(stderr, "[XGI] SEARCH found %d sessions\n", count);
    fflush(stderr);
    return 0;
}

// ---- XGISessionDelete (0x000B0011) ----
static rex::X_HRESULT HandleXgiDelete(uint8_t* /*base*/, uint32_t /*buf*/, uint32_t /*len*/) {
    {
        std::lock_guard<std::mutex> lk(g_session_mutex);
        g_session_active = false;
    }
    // DO NOT unregister from relay here.
    //
    // The game calls XGISessionDelete as part of normal Live session lifecycle —
    // it fires immediately after JoinLocal during hosting setup, while the host
    // is still in the lobby waiting for players. Unregistering here removes the
    // session from the relay before any client can discover it.
    //
    // The relay session stays alive until the TCP connection drops (app exit or
    // disconnect), which is the correct cleanup point for a hosted game.
    fprintf(stderr, "[XGI] DELETE: keeping relay session alive until exit\n");
    fflush(stderr);
    REXLOG_INFO("[XGI] DELETE ok (relay session kept)");
    return 0;
}

// Override XMsgStartIORequest to intercept XGI session management messages.
// All other messages are forwarded to the AppManager as normal.
extern "C" PPC_FUNC(__imp__XMsgStartIORequest) {
    uint32_t app_id         = ctx.r3.u32;
    uint32_t message        = ctx.r4.u32;
    uint32_t overlapped_ptr = ctx.r5.u32;
    uint32_t buffer_ptr     = ctx.r6.u32;
    uint32_t buffer_length  = ctx.r7.u32;

    // Drain any deferred SEARCH completions that are ready.
    // MUST be done on a guest thread (this function) where kernel_state() is
    // valid.  The discovery thread is a raw std::thread where kernel_state()
    // returns null, so we cannot call CompleteOverlappedDeferred from there.
    {
        std::vector<PendingOverlappedCompletion> to_fire;
        {
            auto now = std::chrono::steady_clock::now();
            std::lock_guard<std::mutex> lk(g_pending_ov_mutex);
            for (auto it = g_pending_ov.begin(); it != g_pending_ov.end(); ) {
                if (now >= it->fire_at) {
                    to_fire.push_back(*it);
                    it = g_pending_ov.erase(it);
                } else {
                    ++it;
                }
            }
        }
        for (auto& pending : to_fire) {
            REXLOG_INFO("[XGI] SEARCH: completing overlapped 0x{:08X} result=0x{:08X} (immediate, 150ms delayed)",
                        pending.overlapped_ptr, pending.result);
            // Use CompleteOverlappedImmediate — NOT Deferred — because the SEARCH
            // overlapped was never registered with DispatchMessageAsync, so Deferred
            // crashes trying to look it up in internal state.  Immediate fires the
            // guest callback (sub_8218A068) directly on this thread.  The 150ms delay
            // above ensures overlapped+8 (context/callback object) is initialised
            // before we arrive here, so sub_8218A068 can safely dereference it.
            rex::kernel::kernel_state()->CompleteOverlappedImmediate(
                pending.overlapped_ptr, pending.result);
        }
    }

    REXLOG_INFO("[XMsg] app={} msg=0x{:08X} buf=0x{:08X} len={}",
                app_id, message, buffer_ptr, buffer_length);

    rex::X_HRESULT result = 0;

    if (app_id == 251) {
        switch (message) {
        case 0x000B0010:
            result = HandleXgiCreate(base, buffer_ptr, buffer_length);
            break;
        case 0x000B001C:
            result = HandleXgiSearch(base, buffer_ptr, buffer_length);
            // Delay the overlapped completion by 150 ms so the game's main
            // thread has time to store the callback object at overlapped+8
            // before sub_8218A068 tries to invoke it.  Without the delay the
            // kernel dispatch fires the completion almost immediately and the
            // callback pointer (overlapped+8) is still 0x44 (uninitialised),
            // causing a crash when sub_8218A068 dereferences it as a vtable.
            if (overlapped_ptr) {
                {
                    std::lock_guard<std::mutex> lk(g_pending_ov_mutex);
                    g_pending_ov.push_back({
                        overlapped_ptr,
                        (uint32_t)result,
                        std::chrono::steady_clock::now() +
                            std::chrono::milliseconds(150)
                    });
                }
                REXLOG_INFO("[XGI] SEARCH: deferred overlapped 0x{:08X} by 150ms",
                            overlapped_ptr);
                ctx.r3.u64 = 0x00000103; // X_ERROR_IO_PENDING
            } else {
                ctx.r3.u64 = (uint64_t)(uint32_t)result;
            }
            return; // Skip the common CompleteOverlappedDeferred below
        case 0x000B0011:
            result = HandleXgiDelete(base, buffer_ptr, buffer_length);
            break;
        default:
            // Forward to existing SDK XgiApp handler
            result = rex::kernel::kernel_state()->app_manager()->DispatchMessageAsync(
                app_id, message, buffer_ptr, buffer_length);
            break;
        }
    } else {
        result = rex::kernel::kernel_state()->app_manager()->DispatchMessageAsync(
            app_id, message, buffer_ptr, buffer_length);
    }

    // Complete the overlapped DEFERRED (not immediate).
    // CompleteOverlappedImmediate fires the game's callback synchronously from
    // inside this handler, before the caller has a chance to store its callback
    // object into overlapped+8 — causing a null-deref crash in sub_8218A068.
    // Deferred queues the completion to run after this call returns, by which
    // time the game has fully initialised the overlapped/session struct.
    if (overlapped_ptr) {
        rex::kernel::kernel_state()->CompleteOverlappedDeferred(
            []() {}, overlapped_ptr, result);
        ctx.r3.u64 = 0x00000103; // X_ERROR_IO_PENDING
    } else {
        ctx.r3.u64 = (uint64_t)(uint32_t)result;
    }
}

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

    // Prefer real private LAN addresses over VPN tunnels (Tailscale, etc.)
    // Priority: 192.168.x.x / 10.x.x.x / 172.16-31.x.x > anything else > loopback
    // Tailscale uses CGNAT 100.64.0.0/10 — skip it unless it's the only option.
    auto is_tailscale = [](uint32_t h) {
        // 100.64.0.0/10 = 100.64.0.0 – 100.127.255.255
        return (h & 0xFFC00000u) == 0x64400000u;
    };
    auto is_private = [](uint32_t h) {
        return ((h & 0xFF000000u) == 0x0A000000u) ||  // 10.x.x.x
               ((h & 0xFFF00000u) == 0xAC100000u) ||  // 172.16-31.x.x
               ((h & 0xFFFF0000u) == 0xC0A80000u);    // 192.168.x.x
    };

    uint32_t chosen_ip = htonl(INADDR_LOOPBACK);
    uint32_t fallback_ip = 0;  // non-loopback, non-private, non-tailscale

    for (auto* ai = result; ai; ai = ai->ai_next) {
        if (ai->ai_family != AF_INET) continue;
        auto* sin = (struct sockaddr_in*)ai->ai_addr;
        uint32_t ip_net = sin->sin_addr.s_addr;
        uint32_t h = ntohl(ip_net);
        if ((h & 0xFF000000u) == 0x7F000000u) continue;  // loopback
        if (is_private(h)) {
            chosen_ip = ip_net;
            break;  // preferred — take immediately
        }
        if (!is_tailscale(h) && !fallback_ip)
            fallback_ip = ip_net;
    }

    // If no private address found, fall back (non-Tailscale first, then Tailscale)
    if (ntohl(chosen_ip) == INADDR_LOOPBACK) {
        if (fallback_ip)
            chosen_ip = fallback_ip;
        else {
            // Last resort: accept Tailscale if that's all we have
            for (auto* ai = result; ai; ai = ai->ai_next) {
                if (ai->ai_family != AF_INET) continue;
                auto* sin = (struct sockaddr_in*)ai->ai_addr;
                uint32_t ip_net = sin->sin_addr.s_addr;
                uint32_t h = ntohl(ip_net);
                if ((h & 0xFF000000u) == 0x7F000000u) continue;
                chosen_ip = ip_net;
                break;
            }
        }
    }

    char ip_str[INET_ADDRSTRLEN] = {};
    inet_ntop(AF_INET, &chosen_ip, ip_str, sizeof(ip_str));
    fprintf(stderr, "[NET] Local LAN IP selected: %s\n", ip_str);
    fflush(stderr);

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
    g_disc_step.store(1, std::memory_order_relaxed);
    fprintf(stderr, "[DISC] step 1: thread entry\n"); fflush(stderr);
    REXLOG_INFO("[NET] Discovery thread started on port {}", g_lan_port);
    fprintf(stderr, "[DISC] step 1b: past REXLOG, entering loop\n"); fflush(stderr);

    auto last_beacon = std::chrono::steady_clock::now() -
                       std::chrono::seconds(10); // send first beacon immediately

    int iter = 0;
    while (g_disc_running.load(std::memory_order_relaxed)) {
        iter++;
        if (iter == 1 || iter % 50 == 0) {
            fprintf(stderr, "[DISC] iter=%d step=%d\n", iter,
                    g_disc_step.load(std::memory_order_relaxed));
            fflush(stderr);
        }

        // Step 2: Build fd_set and call select
        g_disc_step.store(2, std::memory_order_relaxed);
        fd_set fds;
        FD_ZERO(&fds);
        FD_SET(g_disc_socket, &fds);
        struct timeval tv = {0, 100000}; // 100ms
        int sel = select((int)g_disc_socket + 1, &fds, nullptr, nullptr, &tv);

        // Step 3: Process received packet if any
        g_disc_step.store(3, std::memory_order_relaxed);
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
                        g_disc_step.store(31, std::memory_order_relaxed);
                        SendBeacon(g_disc_socket, reply);
                    }
                    else if (buf[1] == DISC_BEACON && n >= DISC_HEADER_LEN + 2) {
                        // Received a beacon — add/update peer
                        uint8_t* xnkid = buf + 2;
                        g_disc_step.store(32, std::memory_order_relaxed);
                        AddOrUpdatePeer(*sender_addr, xnkid);
                    }
                }
            }
        }

        // Step 4: Fire any pending deferred QoS completions.
        g_disc_step.store(4, std::memory_order_relaxed);
        {
            auto now2 = std::chrono::steady_clock::now();
            std::lock_guard<std::mutex> lock(g_pending_qos_mutex);
            for (auto it = g_pending_qos.begin(); it != g_pending_qos.end(); ) {
                if (now2 >= it->ready_at && g_base) {
                    // Set cxnqosPending = 0 → game will fire completion callback
                    GuestWriteU32(g_base, it->qos_addr + 4, 0);
                    fprintf(stderr, "[NET] QoS deferred complete: XNQOS@0x%08X\n", it->qos_addr);
                    fflush(stderr);
                    it = g_pending_qos.erase(it);
                } else {
                    ++it;
                }
            }
        }

        // Step 5: Broadcast beacons periodically if hosting (QoS listener active)
        g_disc_step.store(5, std::memory_order_relaxed);
        auto now = std::chrono::steady_clock::now();
        auto since_beacon = std::chrono::duration_cast<std::chrono::milliseconds>(
            now - last_beacon);
        if (since_beacon.count() >= 500) {
            g_disc_step.store(51, std::memory_order_relaxed);
            struct sockaddr_in bcast = {};
            bcast.sin_family = AF_INET;
            bcast.sin_port = htons((uint16_t)g_lan_port);
            bcast.sin_addr.s_addr = INADDR_BROADCAST;
            SendBeacon(g_disc_socket, bcast);
            last_beacon = now;
        }

        g_disc_step.store(6, std::memory_order_relaxed);
    }

    REXLOG_INFO("[NET] Discovery thread stopped");
}

// ============================================================================
// Init / Shutdown
// ============================================================================

void NetInit(int lan_port, bool relay_enabled, const char* relay_host, int relay_port) {
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

    // Connect to relay server if enabled
    if (relay_enabled && relay_host && *relay_host) {
        const uint8_t* xnaddr_bytes = reinterpret_cast<const uint8_t*>(&g_local_xnaddr);
        if (xlive::Connect(relay_host, (uint16_t)relay_port,
                           VIG8_TITLE_ID, xnaddr_bytes, "Player")) {
            REXLOG_INFO("[NET] Connected to relay {}:{}", relay_host, relay_port);
            fprintf(stderr, "[NET] Connected to relay %s:%d\n", relay_host, relay_port);
        } else {
            REXLOG_ERROR("[NET] Failed to connect to relay {}:{}", relay_host, relay_port);
            fprintf(stderr, "[NET] FAILED to connect to relay %s:%d\n", relay_host, relay_port);
        }
        fflush(stderr);
    } else if (!relay_enabled) {
        fprintf(stderr, "[NET] Relay disabled (relay_enabled=false in settings)\n");
        fflush(stderr);
    }

    REXLOG_INFO("[NET] LAN networking initialized");
}

void NetShutdown() {
    // Disconnect from relay
    if (xlive::IsConnected()) {
        xlive::Disconnect();
    }

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

// XNetQosLookup (internal: __imp__NetDll_XNetQosLookup)
//
// Called via wrapper sub_821B3FD0 which prepends r3=1 and reshuffles args.
// At call time:
//   r3=1 (module), r4=cxna, r5=apxna, r6=apxnkid, r7=apxnkey
//   r8=cina, r9=aina, r10=adwBitsPerSec
//   sp+84=cProbes, sp+92=dwTimeout, sp+100=?, sp+108=pXoverlapped
//   sp+116=ppxnqos  ← game stores r27=r31+104 here (address of XNQOS* field)
//
// The game expects us to write an XNQOS* into *(ppxnqos). On the next
// iteration the game checks *(ppxnqos) != 0 && cxnqosPending == 0
// and then calls the completion callback.  We skip the UDP probe entirely
// and return fake-but-valid QoS data immediately.
extern "C" PPC_FUNC(__imp__NetDll_XNetQosLookup) {
    g_base = base;  // store for use by the deferred-completion thread

    uint32_t cxna    = ctx.r4.u32;
    // sp+116 = ppxnqos (address of the XNQOS* field in the session struct)
    uint32_t ppxnqos = PPC_LOAD_U32(ctx.r1.u32 + 116);

    fprintf(stderr, "[NET] XNetQosLookup: cxna=%u ppxnqos=0x%08X\n", cxna, ppxnqos);
    fflush(stderr);
    REXLOG_INFO("[NET] XNetQosLookup: cxna={}, ppxnqos=0x{:08X}", cxna, ppxnqos);

    if (cxna == 0 || !ppxnqos) {
        // No sessions to probe or no output pointer — nothing to do.
        ctx.r3.u64 = 0;
        return;
    }

    auto* mem = rex::kernel::kernel_state()->memory();

    // Free any previous XNQOS allocation at this slot.
    uint32_t prev = PPC_LOAD_U32(ppxnqos);
    if (prev) {
        mem->SystemHeapFree(prev);
        PPC_STORE_U32(ppxnqos, 0);
    }

    // Allocate XNQOS result structure in guest memory.
    // Layout:
    //   +0  uint32_t cxnqos        (number of results)
    //   +4  uint32_t cxnqosPending (N = still pending; 0 = complete)
    //   +8  XNQOSINFO[cxna] (24 bytes each):
    //     +0  uint8_t  bFlags
    //     +1  uint8_t  bReserved
    //     +2  uint16_t cProbesXmit
    //     +4  uint16_t cProbesRecv
    //     +6  uint16_t cbData
    //     +8  uint32_t pbData (guest ptr, NULL if no data)
    //     +12 uint16_t wRttMedian
    //     +14 uint16_t wRttMinimum
    //     +16 uint32_t dwUpBitsPerSec
    //     +20 uint32_t dwDnBitsPerSec
    uint32_t alloc_size = 8 + cxna * 24;
    uint32_t qos_addr = mem->SystemHeapAlloc(alloc_size, 0x10);
    if (!qos_addr) {
        REXLOG_ERROR("[NET] XNetQosLookup: alloc failed ({} bytes)", alloc_size);
        ctx.r3.u64 = 0x8007000E; // E_OUTOFMEMORY
        return;
    }
    std::memset(base + qos_addr, 0, alloc_size);

    // Header: cxnqos = cxna, cxnqosPending = cxna (NOT 0 yet).
    // The discovery thread will set cxnqosPending = 0 after a short delay,
    // giving the game time to fully initialise the session struct (including
    // the completion-callback object at session_struct+8) before we trigger
    // the callback path through sub_8218A068.
    PPC_STORE_U32(qos_addr + 0, cxna);   // cxnqos
    PPC_STORE_U32(qos_addr + 4, cxna);   // cxnqosPending = cxna (pending)

    // Fill each entry with good fake stats.
    // bFlags: XNET_XNQOSINFO_COMPLETE(0x01) | XNET_XNQOSINFO_TARGET_CONTACTED(0x02)
    for (uint32_t i = 0; i < cxna; i++) {
        uint32_t e = qos_addr + 8 + i * 24;
        PPC_STORE_U8 (e +  0, 0x03);      // bFlags
        PPC_STORE_U8 (e +  1, 0);         // bReserved
        PPC_STORE_U16(e +  2, 4);         // cProbesXmit
        PPC_STORE_U16(e +  4, 4);         // cProbesRecv
        PPC_STORE_U16(e +  6, 0);         // cbData (no QoS blob)
        PPC_STORE_U32(e +  8, 0);         // pbData = NULL
        PPC_STORE_U16(e + 12, 30);        // wRttMedian  (30 ms)
        PPC_STORE_U16(e + 14, 20);        // wRttMinimum (20 ms)
        PPC_STORE_U32(e + 16, 10000000);  // dwUpBitsPerSec (10 Mbps)
        PPC_STORE_U32(e + 20, 10000000);  // dwDnBitsPerSec (10 Mbps)
    }

    // Write XNQOS pointer into *ppxnqos (= session_struct+104).
    PPC_STORE_U32(ppxnqos, qos_addr);

    // Schedule deferred completion: set cxnqosPending = 0 after 300 ms.
    {
        std::lock_guard<std::mutex> lock(g_pending_qos_mutex);
        g_pending_qos.push_back({qos_addr,
            std::chrono::steady_clock::now() + std::chrono::milliseconds(300)});
    }

    fprintf(stderr, "[NET] XNetQosLookup: XNQOS@0x%08X pending (cxna=%u), completing in 300ms\n",
            qos_addr, cxna);
    fflush(stderr);
    REXLOG_INFO("[NET] XNetQosLookup: XNQOS@0x{:08X} pending -> *ppxnqos(0x{:08X})", qos_addr, ppxnqos);

    ctx.r3.u64 = 0;  // ERROR_SUCCESS
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

// ============================================================================
// sub_8218A068 guard — prevent vtable dispatch on invalid callback pointer
// ============================================================================
//
// sub_8218A068 fires a completion callback when r7=0 by doing a vtable
// dispatch through r3 (= *(session_struct+8) = callback object pointer).
// If r3 is a near-null value (0, 0x44, etc.), reading *(r3+12) crashes at
// base+0x50. This happens from sub_821969E8 when XNetQosLookup writes
// XNQOS with cxnqosPending=0 before the game has initialized the callback.
// Guard: if r3 < 0x10000, inhibit the vtable call (set r7=1).

extern "C" void __imp__sub_8218A068(PPCContext& ctx, uint8_t* base);
extern "C" PPC_FUNC(sub_8218A068) {
    fprintf(stderr, "[NET] sub_8218A068 called: r7=%u r3=0x%08X\n", ctx.r7.u32, ctx.r3.u32);
    fflush(stderr);
    if (ctx.r7.u32 == 0) {
        uint32_t cb_obj = ctx.r3.u32;
        if (cb_obj < 0x10000) {
            fprintf(stderr, "[NET] sub_8218A068: inhibit near-null cb_obj=0x%X\n", cb_obj);
            fflush(stderr);
            ctx.r7.u64 = 1;
        } else {
            uint32_t vtable_ptr = PPC_LOAD_U32(cb_obj + 12);
            fprintf(stderr, "[NET] sub_8218A068: cb_obj=0x%X vtable_ptr=0x%X\n", cb_obj, vtable_ptr);
            fflush(stderr);
            if (vtable_ptr < 0x10000) {
                fprintf(stderr, "[NET] sub_8218A068: inhibit bad vtable_ptr\n");
                fflush(stderr);
                ctx.r7.u64 = 1;
            }
        }
    }
    __imp__sub_8218A068(ctx, base);
}
