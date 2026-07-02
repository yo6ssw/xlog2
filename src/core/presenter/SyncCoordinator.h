// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2025 Adrian Scripcă (YO6SSW)

#pragma once

#include "LogbookSync.h"
#include "Qso.h"
#include "SyncProtocol.h"

#include <functional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

class LogPagePresenter;

// Drives the logbook-sync protocol on the UI thread. The transport
// (LogbookSync) only moves bytes across the mesh; this class owns the
// anti-entropy state machine and is the *only* place that touches the synced
// LogPagePresenter, so all logbook access stays single-threaded.
//
// The mesh is symmetric and may hold more than one peer, so the protocol runs
// independently per peer. For each peer: HELLO/HELLO_ACK authenticate (pre-shared
// secret HMAC) and agree a shared sync_id; then the higher-node-id side initiates
// a DIGEST; on mismatch the other returns its MANIFEST; the initiator pulls what
// it lacks (REQUEST -> RECORDS) and pushes what it has newer (RECORDS), applying
// the manifest's tombstones. Steady state: a local change is pushed (PUSH) to
// every authenticated peer; the per-peer anti-entropy pass repairs anything
// missed. Every merge is last-write-wins and idempotent, so PUSH and anti-entropy
// compose safely across any number of peers and any message ordering.
class SyncCoordinator {
public:
    explicit SyncCoordinator(LogbookSync& transport) : transport_(transport) {}

    std::function<void(const std::string&)> onStatus;
    // Fired (UI thread) whenever the peer set or its trust/readiness changes, so
    // a "Trusted peers" view can refresh. Never fired during destruction.
    std::function<void()> onPeersChanged;

    // node id breaks last-write-wins ties and decides who initiates; secret is
    // the pre-shared HMAC key (empty disables auth).
    void configure(const std::string& nodeId, const std::string& secret);

    // --- per-node trust (app-layer allowlist) ---
    // When enforced, only peers whose mesh id is trusted exchange logbook data;
    // others still connect (identity-verified by the mesh) and are surfaced via
    // onPeersChanged so the operator can trust them. Enforcement is meaningful
    // only on an identity-enabled mesh; pass enforce=false for legacy/plaintext.
    void setTrust(bool enforce, const std::vector<std::string>& trustedIds);
    bool isTrusted(const LogbookSync::PeerKey& peer) const;
    void trustPeer(const LogbookSync::PeerKey& peer);   // admit + begin syncing
    void revokePeer(const LogbookSync::PeerKey& peer);  // stop syncing with peer

    // A snapshot of every known peer (online now, or trusted-but-offline) for the
    // trusted-peers UI. `name` is the peer's signed/labelled display name.
    struct PeerInfo {
        std::string id;
        std::string name;
        bool        trusted = false;
        bool        online  = false;  // currently a reachable mesh member
        bool        ready   = false;  // handshake complete (actively syncing)
    };
    std::vector<PeerInfo> peerSnapshot() const;

    // Bind to the synced logbook tab (the persistent default log). Pass nullptr
    // to detach (e.g. the tab closed or was Saved-As elsewhere).
    void attach(LogPagePresenter* synced);
    void detach() { attach(nullptr); }
    LogPagePresenter* synced() const { return synced_; }

    // --- transport callbacks (UI thread) ---
    void onPeerUp(const LogbookSync::PeerKey& peer);
    void onPeerDown(const LogbookSync::PeerKey& peer);
    void onMessage(const LogbookSync::PeerKey& peer, const syncproto::Message& m);

    // --- local-mutation hooks (UI thread), wired from the synced presenter ---
    void onLocalUpsert(const Qso& q);
    void onLocalDelete(const std::string& uuid, const std::string& deletedAt);

    // Force an anti-entropy pass with every ready peer (the "Sync now" action).
    void syncNow();

    // Number of peers that have completed the handshake.
    int readyPeerCount() const;

private:
    // Per-peer handshake + readiness state.
    struct PeerState {
        bool        helloSent = false;
        bool        peerHello = false;  // received their HELLO
        bool        ackOk     = false;  // received an accepting HELLO_ACK
        bool        ready     = false;  // handshake complete
        bool        refused   = false;  // auth / sync_id mismatch: ignore data
        bool        untrusted = false;  // connected but not in the allowlist
        std::string peerNodeId;         // their node id (LWW tiebreak)
    };

    void sendHello(const LogbookSync::PeerKey& peer);
    void beginAntiEntropy(const LogbookSync::PeerKey& peer);
    void maybeReady(const LogbookSync::PeerKey& peer, PeerState& st);
    void handleHello(const LogbookSync::PeerKey& peer, const syncproto::Message&);
    void handleHelloAck(const LogbookSync::PeerKey& peer, const syncproto::Message&);
    void handleDigest(const LogbookSync::PeerKey& peer, const syncproto::Message&);
    void handleManifest(const LogbookSync::PeerKey& peer, const syncproto::Message&);
    void handleRequest(const LogbookSync::PeerKey& peer, const syncproto::Message&);
    void handleRecords(const LogbookSync::PeerKey& peer, const syncproto::Message&);
    void handleTombstones(const LogbookSync::PeerKey& peer, const syncproto::Message&);
    void handlePush(const LogbookSync::PeerKey& peer, const syncproto::Message&);
    void status(const std::string& s) { if (onStatus) onStatus(s); }
    // The higher node id drives the anti-entropy pass for a given peer.
    bool initiator(const PeerState& st) const { return nodeId_ > st.peerNodeId; }

    void peersChanged() { if (onPeersChanged) onPeersChanged(); }

    LogbookSync&      transport_;
    LogPagePresenter* synced_ = nullptr;
    std::string       nodeId_;
    std::string       secret_;

    bool                                         trustEnforced_ = false;
    std::unordered_set<LogbookSync::PeerKey>     trusted_;
    std::unordered_map<LogbookSync::PeerKey, PeerState> peers_;
};
