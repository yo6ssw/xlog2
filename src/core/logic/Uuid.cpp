// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2025 Adrian Scripcă (YO6SSW)

#include "Uuid.h"

#include <array>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <random>

namespace {

// --- Minimal SHA-1 (RFC 3174), enough for deterministic v5 UUIDs. ----------

struct Sha1 {
    std::uint32_t h[5] = {0x67452301u, 0xEFCDAB89u, 0x98BADCFEu, 0x10325476u,
                          0xC3D2E1F0u};
    std::uint64_t len = 0;  // total bytes
    std::uint8_t  buf[64];
    std::size_t   fill = 0;

    static std::uint32_t rol(std::uint32_t v, int b) {
        return (v << b) | (v >> (32 - b));
    }

    void block(const std::uint8_t* p) {
        std::uint32_t w[80];
        for (int i = 0; i < 16; ++i)
            w[i] = (std::uint32_t(p[i * 4]) << 24) | (std::uint32_t(p[i * 4 + 1]) << 16) |
                   (std::uint32_t(p[i * 4 + 2]) << 8) | std::uint32_t(p[i * 4 + 3]);
        for (int i = 16; i < 80; ++i)
            w[i] = rol(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);

        std::uint32_t a = h[0], b = h[1], c = h[2], d = h[3], e = h[4];
        for (int i = 0; i < 80; ++i) {
            std::uint32_t f, k;
            if (i < 20)      { f = (b & c) | (~b & d);           k = 0x5A827999u; }
            else if (i < 40) { f = b ^ c ^ d;                    k = 0x6ED9EBA1u; }
            else if (i < 60) { f = (b & c) | (b & d) | (c & d);  k = 0x8F1BBCDCu; }
            else             { f = b ^ c ^ d;                    k = 0xCA62C1D6u; }
            const std::uint32_t t = rol(a, 5) + f + e + k + w[i];
            e = d; d = c; c = rol(b, 30); b = a; a = t;
        }
        h[0] += a; h[1] += b; h[2] += c; h[3] += d; h[4] += e;
    }

    void update(const std::uint8_t* p, std::size_t n) {
        len += n;
        while (n) {
            const std::size_t take = std::min(n, sizeof(buf) - fill);
            std::memcpy(buf + fill, p, take);
            fill += take; p += take; n -= take;
            if (fill == sizeof(buf)) { block(buf); fill = 0; }
        }
    }

    void finish(std::uint8_t out[20]) {
        const std::uint64_t bits = len * 8;
        const std::uint8_t pad = 0x80;
        update(&pad, 1);
        const std::uint8_t zero = 0;
        while (fill != 56) update(&zero, 1);
        std::uint8_t lb[8];
        for (int i = 0; i < 8; ++i) lb[i] = std::uint8_t(bits >> (56 - i * 8));
        update(lb, 8);
        for (int i = 0; i < 5; ++i) {
            out[i * 4]     = std::uint8_t(h[i] >> 24);
            out[i * 4 + 1] = std::uint8_t(h[i] >> 16);
            out[i * 4 + 2] = std::uint8_t(h[i] >> 8);
            out[i * 4 + 3] = std::uint8_t(h[i]);
        }
    }
};

// Format 16 bytes as the canonical 8-4-4-4-12 hex string.
std::string format(const std::uint8_t b[16]) {
    char s[37];
    std::snprintf(s, sizeof(s),
                  "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-"
                  "%02x%02x%02x%02x%02x%02x",
                  b[0], b[1], b[2], b[3], b[4], b[5], b[6], b[7], b[8], b[9],
                  b[10], b[11], b[12], b[13], b[14], b[15]);
    return s;
}

// Fixed namespace UUID for xlog2 sync (a random constant, generated once).
constexpr std::uint8_t kNamespace[16] = {0x6b, 0x2e, 0x8a, 0x14, 0x9d, 0x37,
                                         0x4c, 0x55, 0xb1, 0x0e, 0xa2, 0x7f,
                                         0x3d, 0xc9, 0x18, 0x40};

}  // namespace

namespace uuidutil {

std::string newUuid() {
    std::uint8_t b[16];
    bool got = false;
    if (std::ifstream f("/dev/urandom", std::ios::binary); f) {
        f.read(reinterpret_cast<char*>(b), sizeof(b));
        got = static_cast<bool>(f);
    }
    if (!got) {
        std::random_device rd;
        for (auto& byte : b) byte = static_cast<std::uint8_t>(rd() & 0xFF);
    }
    b[6] = static_cast<std::uint8_t>((b[6] & 0x0F) | 0x40);  // version 4
    b[8] = static_cast<std::uint8_t>((b[8] & 0x3F) | 0x80);  // variant RFC 4122
    return format(b);
}

std::string uuidV5(const std::string& content) {
    Sha1 sha;
    sha.update(kNamespace, sizeof(kNamespace));
    sha.update(reinterpret_cast<const std::uint8_t*>(content.data()), content.size());
    std::uint8_t digest[20];
    sha.finish(digest);

    std::uint8_t b[16];
    std::memcpy(b, digest, 16);
    b[6] = static_cast<std::uint8_t>((b[6] & 0x0F) | 0x50);  // version 5
    b[8] = static_cast<std::uint8_t>((b[8] & 0x3F) | 0x80);  // variant RFC 4122
    return format(b);
}

}  // namespace uuidutil
