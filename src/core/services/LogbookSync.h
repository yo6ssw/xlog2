// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2025 Adrian Scripcă (YO6SSW)

#pragma once

#include <functional>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "IUiDispatcher.h"
#include "SyncProtocol.h"

namespace mm {
class mesh;
}  // namespace mm

// Peer-to-peer transport for logbook sync, backed by a multimaster LAN mesh.
//
// Every node is symmetric: it both listens and dials, auto-discovers LAN peers
// via UDP multicast, and resolves simultaneous-dial races itself — there is no
// listener/connector role to configure. Optional static peers cover the WAN
// case (the internet), where multicast can't reach. The mesh owns a single IO
// thread; this class marshals every event onto the UI thread via the injected
// dispatcher, so the coordinator's callbacks always run UI-side. All logbook
// access lives in SyncCoordinator on the UI thread.
class LogbookSync {
 public:
  using PeerKey = std::string;  // a peer's mesh id, lowercase hex

  // Each callback fires on the UI thread. onPeerUp/Down track mesh-wide
  // membership (a reachable node, possibly via a relay); onMessage delivers a
  // decoded frame tagged with its sender.
  std::function<void(const PeerKey&)> onPeerUp;
  std::function<void(const PeerKey&)> onPeerDown;
  std::function<void(const PeerKey&, const syncproto::Message&)> onMessage;
  std::function<void(const std::string&)> onStatus;

  explicit LogbookSync(
      IUiDispatcher& ui);  // out-of-line: mm::mesh is incomplete here
  ~LogbookSync();
  LogbookSync(const LogbookSync&) = delete;
  LogbookSync& operator=(const LogbookSync&) = delete;

  struct Config {
    std::string nodeId;  // our mesh id (hex); empty => the mesh generates one
    std::string group;   // mesh group name (only same-group nodes connect)
    int port = 0;        // mesh TCP listen port (0 = ephemeral)
    // Optional persistent WAN peers (host, port), dialed and kept connected.
    std::vector<std::pair<std::string, int>> staticPeers;

    // --- transport security (all optional) ---
    // Pre-shared key. When non-empty the mesh is secured: peer links are
    // mutually authenticated + encrypted, and (with identityFile set) nodes
    // carry self-certifying Ed25519 identities. Empty => legacy plaintext.
    std::string psk;
    // Path to this node's persistent identity seed (mesh-managed, mode 0600).
    // Non-empty enables identity; requires a non-empty psk. When set, the
    // mesh derives the node id from the identity key (nodeId is ignored).
    std::string identityFile;
    // This node's signed, gossiped display name (advisory; not unique).
    std::string nodeName;
    // Reject peers that present no identity (only meaningful with identity on).
    bool requireIdentity = true;
  };

  void start(const Config& cfg);
  void stop();
  bool isRunning() const { return static_cast<bool>(mesh_); }

  // The resolved local mesh id (hex). Empty until start(). When Config::nodeId
  // was empty the mesh generated one; the shell persists this so the id (and
  // thus the last-write-wins tiebreak) is stable across restarts.
  std::string localId() const { return localId_; }

  // Count of reachable mesh members (excludes self).
  int memberCount() const;

  // This node's Ed25519 identity public key (hex), or "" when no identity is
  // configured. Stable across restarts; peers add it to their allowlist.
  std::string identityKey() const;

  // The signed/operator-assigned display name for a connected peer, or "" if
  // unknown. Only meaningful on an identity-enabled mesh.
  std::string peerName(const PeerKey& peer) const;

  // Flood a message to every mesh member. Used for anti-entropy fan-out is
  // avoided; prefer sendTo for anything that must respect the auth gate.
  void broadcast(const syncproto::Message& m);

  // Deliver a message to one peer (relayed across the mesh if not a direct
  // neighbor). No-op if not running.
  void sendTo(const PeerKey& peer, const syncproto::Message& m);

  // Parse a WAN-peer field "host" or "host:port" into a (host, port) pair,
  // falling back to defaultPort when no ":port" is given. Empty host => {} so
  // the caller skips it.
  static std::pair<std::string, int> parsePeer(const std::string& s,
                                               int defaultPort);

 private:
  IUiDispatcher& ui_;
  std::unique_ptr<mm::mesh> mesh_;
  std::string localId_;
  std::shared_ptr<bool> alive_ = std::make_shared<bool>(true);
};
