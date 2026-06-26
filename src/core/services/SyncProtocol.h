#pragma once

#include "LogBook.h"  // SyncEntry, SyncManifest
#include "Qso.h"

#include <cstdint>
#include <string>
#include <vector>

// Wire format for logbook sync. Socket-free and dependency-free (a small
// SHA-256 lives in the .cpp), so it can be unit-tested without a transport.
//
// Every message is framed: magic(uint32) | version(uint16) | type(uint16) |
// length(uint32) | payload[length]. Integers are big-endian; payload strings
// are length-prefixed (uint32 + bytes). The framing layer carries no ADIF/JSON.
namespace syncproto {

constexpr std::uint32_t kMagic   = 0x584C5347u;  // 'XLSG'
constexpr std::uint16_t kVersion = 1;

enum class Type : std::uint16_t {
    Hello      = 1,
    HelloAck   = 2,
    Digest     = 3,
    Manifest   = 4,
    Request    = 5,
    Records    = 6,
    Tombstones = 7,
    Push       = 8,
    Ping       = 9,
    Pong       = 10,
};

struct Message {
    Type        type{};
    std::string payload;  // already-serialized body bytes
};

// Serialize a message to full wire bytes (frame header + payload).
std::string encodeFrame(const Message& m);

// Incremental frame reassembler: TCP gives no message boundaries, so bytes are
// fed in as they arrive and complete messages are pulled out one at a time.
class Decoder {
public:
    void feed(const char* data, std::size_t n) { buf_.append(data, n); }
    // Pops the next complete message; false if more bytes are needed. Sets
    // failed() on a bad magic / version / oversize frame (caller should drop
    // the connection).
    bool next(Message& out);
    bool failed() const { return failed_; }

private:
    std::string buf_;
    bool        failed_ = false;
};

// The mesh group name for a given shared secret: "xlog2" when empty, else
// "xlog2-" + a short hash of the secret. Same-secret instances share a mesh;
// different-secret instances never even connect (segregation in addition to the
// HELLO authentication gate).
std::string meshGroup(const std::string& secret);

// --- payload builders / parsers (return false on a malformed payload) ---

// HELLO carries identity plus an HMAC over (nodeId|syncId|nonce) keyed by the
// shared secret. decodeHello recomputes it and reports authOk (always true when
// secret is empty on both sides).
std::string encodeHello(const std::string& nodeId, const std::string& syncId,
                        const std::string& nonce, const std::string& secret);
struct Hello { std::string nodeId, syncId, nonce; bool authOk = false; };
bool decodeHello(const std::string& payload, const std::string& secret, Hello& out);

std::string encodeHelloAck(bool ok, const std::string& syncId,
                           const std::string& reason);
struct HelloAck { bool ok = false; std::string syncId, reason; };
bool decodeHelloAck(const std::string& payload, HelloAck& out);

// Order-independent hex digest of a manifest, for the steady-state fast path.
std::string manifestDigest(const SyncManifest& m);
std::string encodeDigest(const std::string& hex);
bool decodeDigest(const std::string& payload, std::string& hex);

std::string encodeManifest(const SyncManifest& m);
bool decodeManifest(const std::string& payload, SyncManifest& out);

std::string encodeRequest(const std::vector<std::string>& uuids);
bool decodeRequest(const std::string& payload, std::vector<std::string>& out);

std::string encodeRecords(const std::vector<Qso>& qsos);
bool decodeRecords(const std::string& payload, std::vector<Qso>& out);

std::string encodeTombstones(const std::vector<SyncEntry>& tombs);
bool decodeTombstones(const std::string& payload, std::vector<SyncEntry>& out);

// A single incremental change pushed in real time: a live record or a deletion.
std::string encodePushRecord(const Qso& q);
std::string encodePushTombstone(const SyncEntry& t);
struct Push { bool isTombstone = false; Qso record; SyncEntry tomb; };
bool decodePush(const std::string& payload, Push& out);

}  // namespace syncproto
