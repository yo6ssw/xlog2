#include "SyncProtocol.h"

#include <algorithm>
#include <array>
#include <cstring>

namespace syncproto {
namespace {

// --- big-endian primitives -------------------------------------------------

void putU16(std::string& s, std::uint16_t v) {
    s.push_back(static_cast<char>((v >> 8) & 0xFF));
    s.push_back(static_cast<char>(v & 0xFF));
}
void putU32(std::string& s, std::uint32_t v) {
    s.push_back(static_cast<char>((v >> 24) & 0xFF));
    s.push_back(static_cast<char>((v >> 16) & 0xFF));
    s.push_back(static_cast<char>((v >> 8) & 0xFF));
    s.push_back(static_cast<char>(v & 0xFF));
}
void putStr(std::string& s, const std::string& v) {
    putU32(s, static_cast<std::uint32_t>(v.size()));
    s += v;
}

// A bounds-checked reader over a payload buffer.
struct Reader {
    const char* p;
    std::size_t n;
    std::size_t off = 0;
    bool ok = true;

    std::uint16_t u16() {
        if (off + 2 > n) { ok = false; return 0; }
        std::uint16_t v = (std::uint8_t(p[off]) << 8) | std::uint8_t(p[off + 1]);
        off += 2;
        return v;
    }
    std::uint32_t u32() {
        if (off + 4 > n) { ok = false; return 0; }
        std::uint32_t v = (std::uint32_t(std::uint8_t(p[off])) << 24) |
                          (std::uint32_t(std::uint8_t(p[off + 1])) << 16) |
                          (std::uint32_t(std::uint8_t(p[off + 2])) << 8) |
                          std::uint32_t(std::uint8_t(p[off + 3]));
        off += 4;
        return v;
    }
    std::string str() {
        const std::uint32_t len = u32();
        if (!ok || off + len > n) { ok = false; return {}; }
        std::string v(p + off, len);
        off += len;
        return v;
    }
    bool done() const { return off == n; }
};

// --- SHA-256 (RFC 6234) for HMAC + manifest digest -------------------------

struct Sha256 {
    std::uint32_t h[8] = {0x6a09e667u, 0xbb67ae85u, 0x3c6ef372u, 0xa54ff53au,
                          0x510e527fu, 0x9b05688cu, 0x1f83d9abu, 0x5be0cd19u};
    std::uint64_t len = 0;
    std::uint8_t  buf[64];
    std::size_t   fill = 0;

    static std::uint32_t ror(std::uint32_t v, int b) {
        return (v >> b) | (v << (32 - b));
    }
    void block(const std::uint8_t* d) {
        static const std::uint32_t k[64] = {
            0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b,
            0x59f111f1, 0x923f82a4, 0xab1c5ed5, 0xd807aa98, 0x12835b01,
            0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7,
            0xc19bf174, 0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc,
            0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da, 0x983e5152,
            0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147,
            0x06ca6351, 0x14292967, 0x27b70a85, 0x2e1b2138, 0x4d2c6dfc,
            0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
            0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819,
            0xd6990624, 0xf40e3585, 0x106aa070, 0x19a4c116, 0x1e376c08,
            0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f,
            0x682e6ff3, 0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208,
            0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2};
        std::uint32_t w[64];
        for (int i = 0; i < 16; ++i)
            w[i] = (std::uint32_t(d[i * 4]) << 24) | (std::uint32_t(d[i * 4 + 1]) << 16) |
                   (std::uint32_t(d[i * 4 + 2]) << 8) | std::uint32_t(d[i * 4 + 3]);
        for (int i = 16; i < 64; ++i) {
            std::uint32_t s0 = ror(w[i - 15], 7) ^ ror(w[i - 15], 18) ^ (w[i - 15] >> 3);
            std::uint32_t s1 = ror(w[i - 2], 17) ^ ror(w[i - 2], 19) ^ (w[i - 2] >> 10);
            w[i] = w[i - 16] + s0 + w[i - 7] + s1;
        }
        std::uint32_t a = h[0], b = h[1], c = h[2], dd = h[3], e = h[4], f = h[5],
                      g = h[6], hh = h[7];
        for (int i = 0; i < 64; ++i) {
            std::uint32_t S1 = ror(e, 6) ^ ror(e, 11) ^ ror(e, 25);
            std::uint32_t ch = (e & f) ^ (~e & g);
            std::uint32_t t1 = hh + S1 + ch + k[i] + w[i];
            std::uint32_t S0 = ror(a, 2) ^ ror(a, 13) ^ ror(a, 22);
            std::uint32_t maj = (a & b) ^ (a & c) ^ (b & c);
            std::uint32_t t2 = S0 + maj;
            hh = g; g = f; f = e; e = dd + t1; dd = c; c = b; b = a; a = t1 + t2;
        }
        h[0] += a; h[1] += b; h[2] += c; h[3] += dd;
        h[4] += e; h[5] += f; h[6] += g; h[7] += hh;
    }
    void update(const std::uint8_t* d, std::size_t n) {
        len += n;
        while (n) {
            std::size_t take = std::min(n, sizeof(buf) - fill);
            std::memcpy(buf + fill, d, take);
            fill += take; d += take; n -= take;
            if (fill == sizeof(buf)) { block(buf); fill = 0; }
        }
    }
    void finish(std::uint8_t out[32]) {
        std::uint64_t bits = len * 8;
        std::uint8_t pad = 0x80;
        update(&pad, 1);
        std::uint8_t zero = 0;
        while (fill != 56) update(&zero, 1);
        std::uint8_t lb[8];
        for (int i = 0; i < 8; ++i) lb[i] = std::uint8_t(bits >> (56 - i * 8));
        update(lb, 8);
        for (int i = 0; i < 8; ++i) {
            out[i * 4]     = std::uint8_t(h[i] >> 24);
            out[i * 4 + 1] = std::uint8_t(h[i] >> 16);
            out[i * 4 + 2] = std::uint8_t(h[i] >> 8);
            out[i * 4 + 3] = std::uint8_t(h[i]);
        }
    }
};

std::string toHex(const std::uint8_t* b, std::size_t n) {
    static const char* hx = "0123456789abcdef";
    std::string s;
    s.reserve(n * 2);
    for (std::size_t i = 0; i < n; ++i) {
        s.push_back(hx[b[i] >> 4]);
        s.push_back(hx[b[i] & 0xF]);
    }
    return s;
}

std::string sha256Hex(const std::string& data) {
    Sha256 sh;
    sh.update(reinterpret_cast<const std::uint8_t*>(data.data()), data.size());
    std::uint8_t d[32];
    sh.finish(d);
    return toHex(d, 32);
}

std::string hmacSha256Hex(const std::string& key, const std::string& msg) {
    std::array<std::uint8_t, 64> k{};
    if (key.size() > 64) {
        Sha256 sh;
        sh.update(reinterpret_cast<const std::uint8_t*>(key.data()), key.size());
        std::uint8_t d[32];
        sh.finish(d);
        std::memcpy(k.data(), d, 32);
    } else {
        std::memcpy(k.data(), key.data(), key.size());
    }
    std::array<std::uint8_t, 64> ipad{}, opad{};
    for (int i = 0; i < 64; ++i) {
        ipad[i] = k[i] ^ 0x36;
        opad[i] = k[i] ^ 0x5c;
    }
    Sha256 inner;
    inner.update(ipad.data(), 64);
    inner.update(reinterpret_cast<const std::uint8_t*>(msg.data()), msg.size());
    std::uint8_t id[32];
    inner.finish(id);
    Sha256 outer;
    outer.update(opad.data(), 64);
    outer.update(id, 32);
    std::uint8_t od[32];
    outer.finish(od);
    return toHex(od, 32);
}

// --- Qso (de)serialization (lossless, all fields incl. uuid/updated_at) ----

void putQso(std::string& s, const Qso& q) {
    putStr(s, q.uuid);       putStr(s, q.updated_at);
    putStr(s, q.date);       putStr(s, q.time_on);   putStr(s, q.time_off);
    putStr(s, q.call);       putStr(s, q.band);      putStr(s, q.mode);
    putStr(s, q.freq);       putStr(s, q.rst_sent);  putStr(s, q.rst_rcvd);
    putStr(s, q.name);       putStr(s, q.qth);       putStr(s, q.locator);
    putStr(s, q.power);      putStr(s, q.qsl_sent);  putStr(s, q.qsl_rcvd);
    putStr(s, q.comment);    putStr(s, q.country);   putStr(s, q.cq_zone);
    putStr(s, q.itu_zone);   putStr(s, q.continent); putStr(s, q.lotw_sent);
    putStr(s, q.lotw_sent_date); putStr(s, q.lotw_rcvd); putStr(s, q.lotw_rcvd_date);
}

Qso getQso(Reader& r) {
    Qso q;
    q.uuid = r.str();       q.updated_at = r.str();
    q.date = r.str();       q.time_on = r.str();    q.time_off = r.str();
    q.call = r.str();       q.band = r.str();       q.mode = r.str();
    q.freq = r.str();       q.rst_sent = r.str();   q.rst_rcvd = r.str();
    q.name = r.str();       q.qth = r.str();        q.locator = r.str();
    q.power = r.str();      q.qsl_sent = r.str();   q.qsl_rcvd = r.str();
    q.comment = r.str();    q.country = r.str();    q.cq_zone = r.str();
    q.itu_zone = r.str();   q.continent = r.str();  q.lotw_sent = r.str();
    q.lotw_sent_date = r.str(); q.lotw_rcvd = r.str(); q.lotw_rcvd_date = r.str();
    return q;
}

}  // namespace

std::string meshGroup(const std::string& secret) {
    if (secret.empty())
        return "xlog2";
    return "xlog2-" + sha256Hex(secret).substr(0, 16);
}

// --- framing ---------------------------------------------------------------

std::string encodeFrame(const Message& m) {
    std::string s;
    putU32(s, kMagic);
    putU16(s, kVersion);
    putU16(s, static_cast<std::uint16_t>(m.type));
    putU32(s, static_cast<std::uint32_t>(m.payload.size()));
    s += m.payload;
    return s;
}

bool Decoder::next(Message& out) {
    if (failed_)
        return false;
    constexpr std::size_t kHeader = 12;
    if (buf_.size() < kHeader)
        return false;

    Reader r{buf_.data(), buf_.size()};
    const std::uint32_t magic = r.u32();
    const std::uint16_t ver   = r.u16();
    const std::uint16_t type  = r.u16();
    const std::uint32_t len   = r.u32();
    if (magic != kMagic || ver != kVersion) { failed_ = true; return false; }
    if (len > 64u * 1024 * 1024)            { failed_ = true; return false; }
    if (buf_.size() < kHeader + len)
        return false;  // need more bytes

    out.type = static_cast<Type>(type);
    out.payload.assign(buf_.data() + kHeader, len);
    buf_.erase(0, kHeader + len);
    return true;
}

// --- HELLO -----------------------------------------------------------------

std::string encodeHello(const std::string& nodeId, const std::string& syncId,
                        const std::string& nonce, const std::string& secret) {
    std::string s;
    putStr(s, nodeId);
    putStr(s, syncId);
    putStr(s, nonce);
    putStr(s, hmacSha256Hex(secret, nodeId + "\x1f" + syncId + "\x1f" + nonce));
    return s;
}

bool decodeHello(const std::string& payload, const std::string& secret, Hello& out) {
    Reader r{payload.data(), payload.size()};
    out.nodeId = r.str();
    out.syncId = r.str();
    out.nonce  = r.str();
    const std::string mac = r.str();
    if (!r.ok)
        return false;
    const std::string expect =
        hmacSha256Hex(secret, out.nodeId + "\x1f" + out.syncId + "\x1f" + out.nonce);
    out.authOk = (mac == expect);
    return true;
}

std::string encodeHelloAck(bool ok, const std::string& syncId,
                           const std::string& reason) {
    std::string s;
    s.push_back(ok ? 1 : 0);
    putStr(s, syncId);
    putStr(s, reason);
    return s;
}

bool decodeHelloAck(const std::string& payload, HelloAck& out) {
    Reader r{payload.data(), payload.size()};
    if (payload.empty()) return false;
    out.ok = payload[0] != 0;
    r.off = 1;
    out.syncId = r.str();
    out.reason = r.str();
    return r.ok;
}

// --- DIGEST ----------------------------------------------------------------

std::string manifestDigest(const SyncManifest& m) {
    std::vector<std::string> lines;
    lines.reserve(m.records.size() + m.tombstones.size());
    for (const auto& e : m.records)
        lines.push_back("R\x1f" + e.uuid + "\x1f" + e.stamp);
    for (const auto& e : m.tombstones)
        lines.push_back("T\x1f" + e.uuid + "\x1f" + e.stamp);
    std::sort(lines.begin(), lines.end());
    std::string joined;
    for (const auto& l : lines) { joined += l; joined.push_back('\n'); }
    return sha256Hex(joined);
}

std::string encodeDigest(const std::string& hex) {
    std::string s;
    putStr(s, hex);
    return s;
}
bool decodeDigest(const std::string& payload, std::string& hex) {
    Reader r{payload.data(), payload.size()};
    hex = r.str();
    return r.ok;
}

// --- MANIFEST --------------------------------------------------------------

namespace {
void putEntries(std::string& s, const std::vector<SyncEntry>& v) {
    putU32(s, static_cast<std::uint32_t>(v.size()));
    for (const auto& e : v) { putStr(s, e.uuid); putStr(s, e.stamp); }
}
bool getEntries(Reader& r, std::vector<SyncEntry>& v) {
    const std::uint32_t n = r.u32();
    if (!r.ok) return false;
    v.reserve(n);
    for (std::uint32_t i = 0; i < n && r.ok; ++i) {
        SyncEntry e;
        e.uuid = r.str();
        e.stamp = r.str();
        v.push_back(std::move(e));
    }
    return r.ok;
}
}  // namespace

std::string encodeManifest(const SyncManifest& m) {
    std::string s;
    putEntries(s, m.records);
    putEntries(s, m.tombstones);
    return s;
}

bool decodeManifest(const std::string& payload, SyncManifest& out) {
    Reader r{payload.data(), payload.size()};
    return getEntries(r, out.records) && getEntries(r, out.tombstones);
}

// --- REQUEST ---------------------------------------------------------------

std::string encodeRequest(const std::vector<std::string>& uuids) {
    std::string s;
    putU32(s, static_cast<std::uint32_t>(uuids.size()));
    for (const auto& u : uuids) putStr(s, u);
    return s;
}

bool decodeRequest(const std::string& payload, std::vector<std::string>& out) {
    Reader r{payload.data(), payload.size()};
    const std::uint32_t n = r.u32();
    if (!r.ok) return false;
    out.reserve(n);
    for (std::uint32_t i = 0; i < n && r.ok; ++i) out.push_back(r.str());
    return r.ok;
}

// --- RECORDS ---------------------------------------------------------------

std::string encodeRecords(const std::vector<Qso>& qsos) {
    std::string s;
    putU32(s, static_cast<std::uint32_t>(qsos.size()));
    for (const auto& q : qsos) putQso(s, q);
    return s;
}

bool decodeRecords(const std::string& payload, std::vector<Qso>& out) {
    Reader r{payload.data(), payload.size()};
    const std::uint32_t n = r.u32();
    if (!r.ok) return false;
    out.reserve(n);
    for (std::uint32_t i = 0; i < n && r.ok; ++i) out.push_back(getQso(r));
    return r.ok;
}

// --- TOMBSTONES ------------------------------------------------------------

std::string encodeTombstones(const std::vector<SyncEntry>& tombs) {
    std::string s;
    putEntries(s, tombs);
    return s;
}

bool decodeTombstones(const std::string& payload, std::vector<SyncEntry>& out) {
    Reader r{payload.data(), payload.size()};
    return getEntries(r, out);
}

// --- PUSH ------------------------------------------------------------------

std::string encodePushRecord(const Qso& q) {
    std::string s;
    s.push_back(0);  // kind: record
    putQso(s, q);
    return s;
}

std::string encodePushTombstone(const SyncEntry& t) {
    std::string s;
    s.push_back(1);  // kind: tombstone
    putStr(s, t.uuid);
    putStr(s, t.stamp);
    return s;
}

bool decodePush(const std::string& payload, Push& out) {
    if (payload.empty()) return false;
    Reader r{payload.data(), payload.size()};
    r.off = 1;
    if (payload[0] == 0) {
        out.isTombstone = false;
        out.record = getQso(r);
    } else {
        out.isTombstone = true;
        out.tomb.uuid = r.str();
        out.tomb.stamp = r.str();
    }
    return r.ok;
}

}  // namespace syncproto
