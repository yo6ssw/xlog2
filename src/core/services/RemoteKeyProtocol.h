#pragma once

// Operator-side wire format for cwsd's "remote_key" service (real paddle keying
// over the internet). This is the encoder half of cwsd's src/remote_key_protocol.h
// — kept self-contained here so xlog2 has no build dependency on the cwsd tree.
//
// The keyer forms elements locally (jitter-free paddle input) and streams each
// key transition as a timestamped edge; cwsd replays them behind a fixed playout
// delay so the original element spacing survives network jitter. Every datagram
// carries the most recent edges as loss-recovery history (cwsd dedups by source
// timestamp), so a lost packet is recovered by the next one.
//
// Multi-byte fields are big-endian.

#include <cstdint>
#include <vector>

namespace remotekey {

constexpr std::uint8_t  kMagic   = 0xC7;
constexpr std::uint8_t  kVersion = 1;

// packet flags
constexpr std::uint8_t  kFlagSessionReset = 0x01;  // re-anchor cwsd (sent on the first packet)

// edge.state bits
constexpr std::uint8_t  kKeyDown = 0x01;           // key closed (transmitting)
constexpr std::uint8_t  kPttReq  = 0x02;           // optional explicit PTT (cwsd derives its own)

// Keep history short enough for one datagram while still bridging a few losses.
constexpr std::size_t   kMaxEdges = 32;

struct Edge {
    std::uint64_t tsUs;    // microseconds since the operator's session start
    std::uint8_t  state;   // kKeyDown / kPttReq
};

// Serialize a packet: header { magic(1) version(1) flags(1) session_id(2) count(1) }
// followed by count * { ts_us(8) state(1) }. At most kMaxEdges (the most recent)
// edges are emitted.
inline std::vector<std::uint8_t> encode(std::uint16_t sessionId, std::uint8_t flags,
                                        const std::vector<Edge>& edges) {
    const std::size_t n = edges.size() > kMaxEdges ? kMaxEdges : edges.size();
    const std::size_t first = edges.size() - n;

    std::vector<std::uint8_t> out;
    out.reserve(6 + n * 9);
    out.push_back(kMagic);
    out.push_back(kVersion);
    out.push_back(flags);
    out.push_back(static_cast<std::uint8_t>(sessionId >> 8));
    out.push_back(static_cast<std::uint8_t>(sessionId));
    out.push_back(static_cast<std::uint8_t>(n));
    for (std::size_t i = 0; i < n; ++i) {
        const std::uint64_t t = edges[first + i].tsUs;
        for (int b = 0; b < 8; ++b)
            out.push_back(static_cast<std::uint8_t>(t >> (56 - 8 * b)));
        out.push_back(edges[first + i].state);
    }
    return out;
}

}  // namespace remotekey
