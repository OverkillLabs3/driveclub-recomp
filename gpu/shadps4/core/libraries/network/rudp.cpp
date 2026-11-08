// SPDX-License-Identifier: GPL-2.0-or-later
// bbport co-op: libSceRudp, the reliable UDP the game sends its P2P lobby and race data over.
//
// Not Sony's wire format: both ends run this runtime. Each context is one message channel to one
// peer, keyed by its bound virtual port and the peer's P2P transport address (from signaling).
// Packets go over the P2P transport on their own virtual port (see P2PRudpSendTo). Both ends call
// sceRudpInitiate (a simultaneous open, as on the console); the game polls the context status and
// reads with sceRudpGetSizeReadable/sceRudpRead, so no guest callbacks are needed.
// Reliable contexts (the default) get acks, retransmission and in-order delivery; contexts with
// DELIVERY_CRITICAL off are plain datagrams.
// shortcut: no congestion control and no aggregation; fine on a LAN or a decent connection.

#include <chrono>
#include <cstring>
#include <deque>
#include <map>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <vector>

#include "common/logging/log.h"
#include "core/libraries/error_codes.h"
#include "core/libraries/libs.h"
#include "core/libraries/network/net.h"
#include "core/libraries/network/sockets.h"

namespace Libraries::Rudp {

namespace {

constexpr s32 ORBIS_RUDP_ERROR_NOT_INITIALIZED = 0x80770001;
constexpr s32 ORBIS_RUDP_ERROR_INVALID_CONTEXT_ID = 0x80770003;
constexpr s32 ORBIS_RUDP_ERROR_INVALID_ARGUMENT = 0x80770004;
constexpr s32 ORBIS_RUDP_ERROR_MSG_TOO_LARGE = 0x80770012;
constexpr s32 ORBIS_RUDP_ERROR_WOULDBLOCK = 0x80770018;
constexpr s32 ORBIS_RUDP_ERROR_CONN_TIMEOUT = 0x8077000b;

enum State : s32 { Idle = 0, Closed = 1, SynSent = 2, SynRcvd = 3, Established = 4 };

enum Option : s32 {
    OptMaxPayload = 1,
    OptSndBuf = 2,
    OptRcvBuf = 3,
    OptNoDelay = 4,
    OptDeliveryCritical = 5,
    OptOrderCritical = 6,
    OptNonBlock = 7,
    OptStream = 8,
    OptConnectionTimeout = 9,
    OptKeepaliveInterval = 18,
    OptKeepaliveTimeout = 19,
};

enum PacketType : u8 { Syn = 1, SynAck = 2, Data = 3, Datagram = 4, Ack = 5, Ping = 6, Fin = 7 };

#pragma pack(push, 1)
struct Header {
    u8 magic; // 'R'
    u8 type;
    u16 vport;
    u32 seq;
};
#pragma pack(pop)

using Clock = std::chrono::steady_clock;

struct Unacked {
    std::vector<u8> packet;
    Clock::time_point sent;
};

struct Context {
    u16 vport{};
    u32 peer_addr{};
    u16 peer_port{};
    s32 state{Idle};
    bool reliable{true};
    std::map<s32, s32> options;
    u32 next_send_seq{1};
    std::map<u32, Unacked> unacked;
    u32 next_recv_seq{1};
    std::map<u32, std::vector<u8>> early; // reliable packets ahead of next_recv_seq
    std::deque<std::vector<u8>> readable;
    Clock::time_point last_rx{};
    Clock::time_point last_tx{};
    Clock::time_point initiated{};
    u64 bytes_sent{}, bytes_received{}, retransmits{};
};

std::mutex g_mutex;
bool g_initialized = false;
bool g_thread_started = false;
s32 g_next_id = 1;
std::unordered_map<s32, Context> g_contexts;

void SendLocked(Context& c, u8 type, u32 seq, const void* data = nullptr, u32 len = 0,
                std::vector<u8>* keep = nullptr) {
    std::vector<u8> packet(sizeof(Header) + len);
    Header h{'R', type, c.vport, seq};
    std::memcpy(packet.data(), &h, sizeof(h));
    if (len) {
        std::memcpy(packet.data() + sizeof(h), data, len);
    }
    Net::P2PRudpSendTo(packet.data(), static_cast<u32>(packet.size()), c.peer_addr, c.peer_port);
    c.last_tx = Clock::now();
    if (keep) {
        *keep = std::move(packet);
    }
}

void EstablishLocked(s32 id, Context& c) {
    if (c.state != Established) {
        c.state = Established;
        c.last_rx = Clock::now();
        LOG_INFO(Lib_Net, "RUDP context {} (vport {}) established with {:#x}:{}", id, c.vport,
                 c.peer_addr, Net::sceNetNtohs(c.peer_port));
    }
}

void DeliverLocked(Context& c, const u8* data, size_t len) {
    c.readable.emplace_back(data, data + len);
    c.bytes_received += len;
}

void Receive(u32 from_addr, u16 from_port, const u8* data, size_t len) {
    if (len < sizeof(Header)) {
        return;
    }
    Header h;
    std::memcpy(&h, data, sizeof(h));
    if (h.magic != 'R') {
        return;
    }
    const u8* payload = data + sizeof(h);
    const size_t payload_len = len - sizeof(h);

    std::lock_guard lock(g_mutex);
    for (auto& [id, c] : g_contexts) {
        if (c.vport != h.vport || c.peer_addr != from_addr || c.peer_port != from_port ||
            c.state == Idle || c.state == Closed) {
            continue;
        }
        c.last_rx = Clock::now();
        switch (h.type) {
        case Syn:
            SendLocked(c, SynAck, 0);
            EstablishLocked(id, c);
            break;
        case SynAck:
            EstablishLocked(id, c);
            break;
        case Data:
            EstablishLocked(id, c);
            SendLocked(c, Ack, h.seq);
            if (h.seq == c.next_recv_seq) {
                DeliverLocked(c, payload, payload_len);
                ++c.next_recv_seq;
                for (auto it = c.early.find(c.next_recv_seq); it != c.early.end();
                     it = c.early.find(c.next_recv_seq)) {
                    DeliverLocked(c, it->second.data(), it->second.size());
                    c.early.erase(it);
                    ++c.next_recv_seq;
                }
            } else if (h.seq > c.next_recv_seq) {
                c.early.emplace(h.seq, std::vector<u8>(payload, payload + payload_len));
            }
            break;
        case Datagram:
            EstablishLocked(id, c);
            DeliverLocked(c, payload, payload_len);
            break;
        case Ack:
            c.unacked.erase(h.seq);
            break;
        case Ping:
            break;
        case Fin:
            LOG_INFO(Lib_Net, "RUDP context {} closed by the peer", id);
            c.state = Closed;
            break;
        }
        return;
    }
}

// Retransmission, handshake retries, keepalive and timeouts (the "internal IO thread").
void TimerMain() {
    using namespace std::chrono_literals;
    for (;;) {
        std::this_thread::sleep_for(20ms);
        std::lock_guard lock(g_mutex);
        const auto now = Clock::now();
        for (auto& [id, c] : g_contexts) {
            if (c.state == SynSent) {
                if (now - c.last_tx > 250ms) {
                    SendLocked(c, Syn, 0);
                }
                if (now - c.initiated > 30s) {
                    LOG_WARNING(Lib_Net, "RUDP context {} handshake timed out", id);
                    c.state = Closed;
                }
            } else if (c.state == Established) {
                for (auto& [seq, u] : c.unacked) {
                    if (now - u.sent > 200ms) {
                        Net::P2PRudpSendTo(u.packet.data(), static_cast<u32>(u.packet.size()),
                                           c.peer_addr, c.peer_port);
                        u.sent = now;
                        ++c.retransmits;
                    }
                }
                if (now - c.last_tx > 1s) {
                    SendLocked(c, Ping, 0);
                }
                if (now - c.last_rx > 20s) {
                    LOG_WARNING(Lib_Net, "RUDP context {} timed out (nothing from the peer)", id);
                    c.state = Closed;
                }
            }
        }
    }
}

Context* Find(s32 id) {
    const auto it = g_contexts.find(id);
    return it == g_contexts.end() ? nullptr : &it->second;
}

} // namespace

s32 PS4_SYSV_ABI sceRudpInit(void* pool, s32 pool_size) {
    std::lock_guard lock(g_mutex);
    LOG_INFO(Lib_Net, "pool={} size={}", pool, pool_size);
    g_initialized = true;
    Net::SetP2PRudpReceiver(Receive);
    if (!g_thread_started) {
        g_thread_started = true;
        std::thread(TimerMain).detach();
    }
    return ORBIS_OK;
}

s32 PS4_SYSV_ABI sceRudpEnd() {
    // Two parts of the game init the library on their own; keep it up.
    return ORBIS_OK;
}

s32 PS4_SYSV_ABI sceRudpEnableInternalIOThread(u32 stack_size, u32 priority) {
    return ORBIS_OK;
}

s32 PS4_SYSV_ABI sceRudpSetEventHandler(void* handler, void* arg) {
    return ORBIS_OK;
}

s32 PS4_SYSV_ABI sceRudpSetMaxSegmentSize(u16 mss) {
    return ORBIS_OK;
}

s32 PS4_SYSV_ABI sceRudpProcessEvents(s64 timeout) {
    return ORBIS_OK;
}

s32 PS4_SYSV_ABI sceRudpNetReceived(s32 soc, const u8* data, u64 len, const void* from,
                                    u32 from_len) {
    // Packets arrive on the P2P transport's RUDP port, never through the game's own socket.
    return ORBIS_OK;
}

s32 PS4_SYSV_ABI sceRudpCreateContext(void* handler, void* arg, s32* ctx_id) {
    std::lock_guard lock(g_mutex);
    if (!g_initialized) {
        return ORBIS_RUDP_ERROR_NOT_INITIALIZED;
    }
    if (!ctx_id) {
        return ORBIS_RUDP_ERROR_INVALID_ARGUMENT;
    }
    const s32 id = g_next_id++;
    g_contexts[id] = Context{};
    *ctx_id = id;
    return ORBIS_OK;
}

s32 PS4_SYSV_ABI sceRudpSetOption(s32 ctx_id, s32 option, const void* value, u32 len) {
    std::lock_guard lock(g_mutex);
    Context* c = Find(ctx_id);
    if (!c) {
        return ORBIS_RUDP_ERROR_INVALID_CONTEXT_ID;
    }
    if (!value || len < 1) {
        return ORBIS_RUDP_ERROR_INVALID_ARGUMENT;
    }
    s32 v = 0;
    std::memcpy(&v, value, std::min<u32>(len, sizeof(v)));
    c->options[option] = v;
    if (option == OptDeliveryCritical) {
        c->reliable = v != 0;
    }
    return ORBIS_OK;
}

s32 PS4_SYSV_ABI sceRudpGetOption(s32 ctx_id, s32 option, void* value, u32 len) {
    std::lock_guard lock(g_mutex);
    Context* c = Find(ctx_id);
    if (!c) {
        return ORBIS_RUDP_ERROR_INVALID_CONTEXT_ID;
    }
    if (!value || len < 1) {
        return ORBIS_RUDP_ERROR_INVALID_ARGUMENT;
    }
    s32 v = 0;
    if (const auto it = c->options.find(option); it != c->options.end()) {
        v = it->second;
    } else if (option == OptMaxPayload) {
        v = 0x7d0;
    } else if (option == OptSndBuf || option == OptRcvBuf) {
        v = 0x10000;
    } else if (option == OptDeliveryCritical || option == OptOrderCritical) {
        v = 1;
    }
    std::memset(value, 0, len);
    std::memcpy(value, &v, std::min<u32>(len, sizeof(v)));
    return ORBIS_OK;
}

s32 PS4_SYSV_ABI sceRudpBind(s32 ctx_id, s32 soc, u16 vport, u8 mux_mode) {
    std::lock_guard lock(g_mutex);
    Context* c = Find(ctx_id);
    if (!c) {
        return ORBIS_RUDP_ERROR_INVALID_CONTEXT_ID;
    }
    c->vport = vport;
    LOG_INFO(Lib_Net, "ctx={} soc={} vport={} mux={}", ctx_id, soc, vport, mux_mode);
    return ORBIS_OK;
}

s32 PS4_SYSV_ABI sceRudpInitiate(s32 ctx_id, const Net::OrbisNetSockaddrIn* to, u32 to_len,
                                 u16 vport) {
    std::lock_guard lock(g_mutex);
    Context* c = Find(ctx_id);
    if (!c) {
        return ORBIS_RUDP_ERROR_INVALID_CONTEXT_ID;
    }
    if (!to || to_len < 8) {
        return ORBIS_RUDP_ERROR_INVALID_ARGUMENT;
    }
    if (vport != 0) {
        c->vport = vport;
    }
    c->peer_addr = to->sin_addr;
    c->peer_port = to->sin_port;
    LOG_INFO(Lib_Net, "ctx={} vport={} -> {:#x}:{} (reliable={})", ctx_id, c->vport, c->peer_addr,
             Net::sceNetNtohs(c->peer_port), c->reliable);
    if (c->state != Established) {
        c->state = SynSent;
        c->initiated = Clock::now();
        SendLocked(*c, Syn, 0);
    }
    return ORBIS_OK;
}

s32 PS4_SYSV_ABI sceRudpTerminate(s32 ctx_id) {
    std::lock_guard lock(g_mutex);
    Context* c = Find(ctx_id);
    if (!c) {
        return ORBIS_RUDP_ERROR_INVALID_CONTEXT_ID;
    }
    if (c->state == Established || c->state == SynSent) {
        SendLocked(*c, Fin, 0);
    }
    g_contexts.erase(ctx_id);
    return ORBIS_OK;
}

s32 PS4_SYSV_ABI sceRudpWrite(s32 ctx_id, const void* data, u32 len, u8 flags) {
    std::lock_guard lock(g_mutex);
    Context* c = Find(ctx_id);
    if (!c) {
        return ORBIS_RUDP_ERROR_INVALID_CONTEXT_ID;
    }
    if (c->state == Closed) {
        return ORBIS_RUDP_ERROR_CONN_TIMEOUT;
    }
    if (c->state != Established) {
        return ORBIS_RUDP_ERROR_WOULDBLOCK;
    }
    if (len > 0x8000) {
        return ORBIS_RUDP_ERROR_MSG_TOO_LARGE;
    }
    if (c->reliable) {
        const u32 seq = c->next_send_seq++;
        Unacked u;
        SendLocked(*c, Data, seq, data, len, &u.packet);
        u.sent = Clock::now();
        c->unacked.emplace(seq, std::move(u));
    } else {
        SendLocked(*c, Datagram, 0, data, len);
    }
    c->bytes_sent += len;
    return static_cast<s32>(len);
}

s32 PS4_SYSV_ABI sceRudpRead(s32 ctx_id, void* data, u32 len, u8 flags, void* info) {
    std::lock_guard lock(g_mutex);
    Context* c = Find(ctx_id);
    if (!c) {
        return ORBIS_RUDP_ERROR_INVALID_CONTEXT_ID;
    }
    if (c->readable.empty()) {
        return c->state == Closed ? ORBIS_RUDP_ERROR_CONN_TIMEOUT : ORBIS_RUDP_ERROR_WOULDBLOCK;
    }
    std::vector<u8> msg = std::move(c->readable.front());
    c->readable.pop_front();
    const u32 n = std::min<u32>(len, static_cast<u32>(msg.size()));
    if (data && n) {
        std::memcpy(data, msg.data(), n);
    }
    return static_cast<s32>(n);
}

s32 PS4_SYSV_ABI sceRudpGetSizeReadable(s32 ctx_id) {
    std::lock_guard lock(g_mutex);
    Context* c = Find(ctx_id);
    if (!c) {
        return ORBIS_RUDP_ERROR_INVALID_CONTEXT_ID;
    }
    return c->readable.empty() ? 0 : static_cast<s32>(c->readable.front().size());
}

s32 PS4_SYSV_ABI sceRudpGetSizeWritable(s32 ctx_id) {
    std::lock_guard lock(g_mutex);
    Context* c = Find(ctx_id);
    if (!c) {
        return ORBIS_RUDP_ERROR_INVALID_CONTEXT_ID;
    }
    return c->state == Established ? 0x10000 : 0;
}

s32 PS4_SYSV_ABI sceRudpGetContextStatus(s32 ctx_id, void* status, u32 len) {
    std::lock_guard lock(g_mutex);
    Context* c = Find(ctx_id);
    if (!c) {
        return ORBIS_RUDP_ERROR_INVALID_CONTEXT_ID;
    }
    if (!status) {
        return ORBIS_RUDP_ERROR_INVALID_ARGUMENT;
    }
    // { state, parentId, children, lostPackets, sentPackets, receivedPackets, ... }
    std::memset(status, 0, len);
    std::memcpy(status, &c->state, std::min<u32>(len, sizeof(c->state)));
    return ORBIS_OK;
}

s32 PS4_SYSV_ABI sceRudpGetStatus(void* status, u32 len) {
    if (!status) {
        return ORBIS_RUDP_ERROR_INVALID_ARGUMENT;
    }
    std::memset(status, 0, len);
    return ORBIS_OK;
}

void RegisterLib(Core::Loader::SymbolsResolver* sym) {
    LIB_FUNCTION("amuBfI-AQc4", "libSceRudp", 1, "libSceRudp", sceRudpInit);
    LIB_FUNCTION("3hBvwqEwqj8", "libSceRudp", 1, "libSceRudp", sceRudpEnd);
    LIB_FUNCTION("6PBNpsgyaxw", "libSceRudp", 1, "libSceRudp", sceRudpEnableInternalIOThread);
    LIB_FUNCTION("SUEVes8gvmw", "libSceRudp", 1, "libSceRudp", sceRudpSetEventHandler);
    LIB_FUNCTION("beAsSTVWVPQ", "libSceRudp", 1, "libSceRudp", sceRudpSetMaxSegmentSize);
    LIB_FUNCTION("9U9m1YH0ScQ", "libSceRudp", 1, "libSceRudp", sceRudpProcessEvents);
    LIB_FUNCTION("vPzJldDSxXc", "libSceRudp", 1, "libSceRudp", sceRudpNetReceived);
    LIB_FUNCTION("CAbbX6BuQZ0", "libSceRudp", 1, "libSceRudp", sceRudpCreateContext);
    LIB_FUNCTION("0yzYdZf0IwE", "libSceRudp", 1, "libSceRudp", sceRudpSetOption);
    LIB_FUNCTION("mCQIhSmCP6o", "libSceRudp", 1, "libSceRudp", sceRudpGetOption);
    LIB_FUNCTION("l4SLBpKUDK4", "libSceRudp", 1, "libSceRudp", sceRudpBind);
    LIB_FUNCTION("szEVu+edXV4", "libSceRudp", 1, "libSceRudp", sceRudpInitiate);
    LIB_FUNCTION("OMYRTU0uc4w", "libSceRudp", 1, "libSceRudp", sceRudpTerminate);
    LIB_FUNCTION("KaPL3fbTLCA", "libSceRudp", 1, "libSceRudp", sceRudpWrite);
    LIB_FUNCTION("rZqWV3eXgOA", "libSceRudp", 1, "libSceRudp", sceRudpRead);
    LIB_FUNCTION("sAZqO2+5Qqo", "libSceRudp", 1, "libSceRudp", sceRudpGetSizeReadable);
    LIB_FUNCTION("fRc1ahQppR4", "libSceRudp", 1, "libSceRudp", sceRudpGetSizeWritable);
    LIB_FUNCTION("wIJsiqY+BMk", "libSceRudp", 1, "libSceRudp", sceRudpGetContextStatus);
    LIB_FUNCTION("i3STzxuwPx0", "libSceRudp", 1, "libSceRudp", sceRudpGetStatus);
}

} // namespace Libraries::Rudp
