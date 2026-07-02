// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2025 Adrian Scripcă (YO6SSW)

#include "LogbookSync.h"

#include <cstddef>
#include <cstdlib>
#include <exception>
#include <multimaster/multimaster.hpp>

namespace {

// A syncproto frame (a std::string of bytes) viewed as mm::bytes.
mm::bytes asBytes(const std::string& s) {
  return mm::bytes(reinterpret_cast<const std::byte*>(s.data()), s.size());
}

// Decode the single complete frame carried by one mesh message. multimaster
// delivers whole messages, so the streaming Decoder yields exactly one.
bool decodeOne(mm::bytes data, syncproto::Message& out) {
  syncproto::Decoder dec;
  dec.feed(reinterpret_cast<const char*>(data.data()), data.size());
  return dec.next(out);
}

}  // namespace

LogbookSync::LogbookSync(IUiDispatcher& ui) : ui_(ui) {}

LogbookSync::~LogbookSync() {
  onPeerUp = nullptr;
  onPeerDown = nullptr;
  onMessage = nullptr;
  onStatus = nullptr;
  stop();
}

void LogbookSync::start(const Config& cfg) {
  stop();
  alive_ = std::make_shared<bool>(true);

  mm::mesh_config mc;
  if (auto pinned = mm::peer_id::from_string(cfg.nodeId)) mc.nodeId = *pinned;
  if (!cfg.group.empty()) mc.groupName = cfg.group;
  mc.listenPort = static_cast<uint16_t>(cfg.port);
  for (const auto& [host, port] : cfg.staticPeers)
    if (!host.empty() && port > 0)
      mc.staticPeers.push_back({host, static_cast<uint16_t>(port)});

  // Transport security: the shared secret doubles as the mesh PSK (encryption
  // + authentication). With an identity file, each node gets a persistent
  // self-certifying Ed25519 key — its id then provably hashes from that key,
  // so peers can't impersonate one another. The trust allowlist is enforced
  // one layer up (SyncCoordinator), so we leave mesh trustedKeys empty: every
  // valid identity connects and is surfaced for the operator to trust.
  if (!cfg.psk.empty()) {
    mc.psk = cfg.psk;
    if (!cfg.identityFile.empty()) {
      mc.identityFile = cfg.identityFile;
      mc.requireIdentity = cfg.requireIdentity;
      if (!cfg.nodeName.empty()) mc.nodeName = cfg.nodeName;
    }
  }

  auto mesh = std::make_unique<mm::mesh>(std::move(mc));

  mm::callbacks cb;
  cb.onMemberJoined = [this, w = std::weak_ptr<bool>(alive_)](mm::peer_id p) {
    const std::string key = p.to_string();
    ui_.post([this, w, key] {
      if (!w.expired() && onPeerUp) onPeerUp(key);
    });
  };
  cb.onMemberLeft = [this, w = std::weak_ptr<bool>(alive_)](mm::peer_id p) {
    const std::string key = p.to_string();
    ui_.post([this, w, key] {
      if (!w.expired() && onPeerDown) onPeerDown(key);
    });
  };
  cb.onMessage = [this, w = std::weak_ptr<bool>(alive_)](mm::peer_id from,
                                                         mm::bytes data) {
    syncproto::Message m;
    if (!decodeOne(data, m)) return;
    const std::string key = from.to_string();
    ui_.post([this, w, key, m = std::move(m)] {
      if (!w.expired() && onMessage) onMessage(key, m);
    });
  };
  cb.onError = [this, w = std::weak_ptr<bool>(alive_)](const mm::error& e) {
    const std::string what = "Sync: " + e.what;
    ui_.post([this, w, what] {
      if (!w.expired() && onStatus) onStatus(what);
    });
  };
  mesh->set_callbacks(std::move(cb));
  // start() can throw std::system_error (socket bind / multicast setup) — e.g.
  // a network with no multicast, or the port in use. Never let that crash the
  // app; report it and leave sync disabled.
  try {
    mesh->start();
  } catch (const std::exception& e) {
    if (onStatus)
      onStatus(std::string("Sync: could not start mesh — ") + e.what());
    return;
  }

  localId_ = mesh->id().to_string();
  mesh_ = std::move(mesh);
  if (onStatus) onStatus("Sync: mesh started (discovering peers)…");
}

void LogbookSync::stop() {
  if (mesh_) {
    mesh_->stop();
    mesh_.reset();
  }
  // Invalidate any in-flight posted callbacks from the previous session.
  alive_ = std::make_shared<bool>(true);
  localId_.clear();
}

int LogbookSync::memberCount() const {
  return mesh_ ? static_cast<int>(mesh_->members().size()) : 0;
}

std::string LogbookSync::identityKey() const {
  return mesh_ ? mesh_->identity_public_key() : std::string{};
}

std::string LogbookSync::peerName(const PeerKey& peer) const {
  if (!mesh_) return {};
  auto pid = mm::peer_id::from_string(peer);
  return pid ? mesh_->node_name(*pid) : std::string{};
}

void LogbookSync::broadcast(const syncproto::Message& m) {
  if (!mesh_) return;
  mesh_->broadcast(asBytes(syncproto::encodeFrame(m)));
}

void LogbookSync::sendTo(const PeerKey& peer, const syncproto::Message& m) {
  if (!mesh_) return;
  auto pid = mm::peer_id::from_string(peer);
  if (!pid) return;
  mesh_->send(*pid, asBytes(syncproto::encodeFrame(m)));
}

std::pair<std::string, int> LogbookSync::parsePeer(const std::string& s,
                                                   int defaultPort) {
  if (s.empty()) return {};
  const auto colon = s.rfind(':');
  // Treat as host:port only when the suffix is all digits (so a bare IPv6 or a
  // plain hostname isn't mis-split).
  if (colon != std::string::npos && colon + 1 < s.size()) {
    const std::string portStr = s.substr(colon + 1);
    bool digits = !portStr.empty();
    for (char ch : portStr) digits = digits && (ch >= '0' && ch <= '9');
    if (digits) {
      const int p = std::atoi(portStr.c_str());
      if (p > 0 && p <= 65535) return {s.substr(0, colon), p};
    }
  }
  return {s, defaultPort};
}
