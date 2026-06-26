#pragma once

#include "LogbookSync.h"
#include "Qso.h"
#include "SyncProtocol.h"

#include <functional>
#include <string>

class LogPagePresenter;

// Drives the logbook-sync protocol on the UI thread. The transport
// (LogbookSync) only moves bytes; this class owns the anti-entropy state
// machine and is the *only* place that touches the synced LogPagePresenter,
// so all logbook access stays single-threaded.
//
// Handshake: HELLO/HELLO_ACK agree a shared sync_id (and authenticate via the
// pre-shared secret). Reconcile: the higher-node-id peer initiates with a
// DIGEST; on mismatch the other sends its MANIFEST; the initiator pulls what it
// lacks (REQUEST -> RECORDS) and pushes what it has newer (RECORDS), applying
// tombstones from the manifest. Steady state: single local changes are pushed
// in real time (PUSH); the periodic/on-connect anti-entropy repairs anything
// missed while disconnected. Every merge is last-write-wins and idempotent, so
// PUSH and the anti-entropy pass compose safely regardless of ordering.
class SyncCoordinator {
public:
    explicit SyncCoordinator(LogbookSync& transport) : transport_(transport) {}

    std::function<void(const std::string&)> onStatus;

    // node id breaks last-write-wins ties and decides who initiates; secret is
    // the pre-shared HMAC key (empty disables auth).
    void configure(const std::string& nodeId, const std::string& secret);

    // Bind to the synced logbook tab (the persistent default log). Pass nullptr
    // to detach (e.g. the tab closed or was Saved-As elsewhere).
    void attach(LogPagePresenter* synced);
    void detach() { attach(nullptr); }
    LogPagePresenter* synced() const { return synced_; }

    // --- transport callbacks (UI thread) ---
    void onConnected();
    void onDisconnected();
    void onMessage(const syncproto::Message& m);

    // --- local-mutation hooks (UI thread), wired from the synced presenter ---
    void onLocalUpsert(const Qso& q);
    void onLocalDelete(const std::string& uuid, const std::string& deletedAt);

    // Force an anti-entropy pass (the "Sync now" action).
    void syncNow();

private:
    void sendHello();
    void beginAntiEntropy();              // initiator: send DIGEST
    void handleHello(const syncproto::Message&);
    void handleHelloAck(const syncproto::Message&);
    void handleDigest(const syncproto::Message&);
    void handleManifest(const syncproto::Message&);
    void handleRequest(const syncproto::Message&);
    void handleRecords(const syncproto::Message&);
    void handleTombstones(const syncproto::Message&);
    void handlePush(const syncproto::Message&);
    void status(const std::string& s) { if (onStatus) onStatus(s); }
    bool initiator() const { return nodeId_ > peerNodeId_; }

    LogbookSync&      transport_;
    LogPagePresenter* synced_ = nullptr;
    std::string       nodeId_;
    std::string       secret_;

    std::string peerNodeId_;
    bool        helloSent_  = false;
    bool        peerHello_  = false;  // received peer HELLO
    bool        ackOk_      = false;  // received an accepting HELLO_ACK
    bool        ready_      = false;  // handshake complete, anti-entropy may run
    bool        refused_    = false;  // syncId/auth mismatch: ignore data
};
