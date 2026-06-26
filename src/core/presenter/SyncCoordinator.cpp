#include "SyncCoordinator.h"

#include "LogPagePresenter.h"
#include "Uuid.h"

#include <unordered_map>

using syncproto::Message;
using syncproto::Type;

void SyncCoordinator::configure(const std::string& nodeId,
                                const std::string& secret) {
    nodeId_ = nodeId;
    secret_ = secret;
}

void SyncCoordinator::attach(LogPagePresenter* synced) {
    synced_ = synced;
}

void SyncCoordinator::onConnected() {
    peerNodeId_.clear();
    helloSent_ = peerHello_ = ackOk_ = ready_ = false;
    refused_   = false;
    sendHello();
}

void SyncCoordinator::onDisconnected() {
    ready_ = false;
}

void SyncCoordinator::sendHello() {
    if (!synced_)
        return;
    const std::string nonce = uuidutil::newUuid();
    transport_.sendMessage(
        {Type::Hello, syncproto::encodeHello(nodeId_, synced_->syncId(), nonce, secret_)});
    helloSent_ = true;
}

void SyncCoordinator::onMessage(const Message& m) {
    if (!synced_)
        return;
    switch (m.type) {
        case Type::Hello:      handleHello(m);      break;
        case Type::HelloAck:   handleHelloAck(m);   break;
        case Type::Digest:     handleDigest(m);     break;
        case Type::Manifest:   handleManifest(m);   break;
        case Type::Request:    handleRequest(m);    break;
        case Type::Records:    handleRecords(m);    break;
        case Type::Tombstones: handleTombstones(m); break;
        case Type::Push:       handlePush(m);       break;
        default: break;  // Ping/Pong handled in the transport
    }
}

void SyncCoordinator::handleHello(const Message& m) {
    syncproto::Hello h;
    if (!syncproto::decodeHello(m.payload, secret_, h))
        return;
    if (!h.authOk) {
        refused_ = true;
        transport_.sendMessage(
            {Type::HelloAck, syncproto::encodeHelloAck(false, "", "authentication failed")});
        status("Sync: peer authentication failed (check the shared secret).");
        return;
    }

    peerNodeId_ = h.nodeId;
    peerHello_  = true;

    const std::string localId = synced_->syncId();
    const std::string peerId  = h.syncId;

    if (!localId.empty() && !peerId.empty() && localId != peerId) {
        refused_ = true;
        transport_.sendMessage(
            {Type::HelloAck, syncproto::encodeHelloAck(false, localId, "different logbook")});
        status("Sync: peer is a different logbook; refusing to merge.");
        return;
    }

    std::string resolved;
    if (!localId.empty()) {
        resolved = localId;
    } else if (!peerId.empty()) {
        resolved = peerId;
        synced_->setSyncId(peerId);  // adopt the peer's id
    } else if (initiator()) {
        resolved = synced_->ensureSyncId();  // both fresh: higher node mints
    }
    transport_.sendMessage(
        {Type::HelloAck, syncproto::encodeHelloAck(true, resolved, "")});

    // handshake may now be complete
    if (peerHello_ && ackOk_ && !ready_) {
        ready_ = true;
        status("Sync: connected to peer.");
        if (initiator()) beginAntiEntropy();
    }
}

void SyncCoordinator::handleHelloAck(const Message& m) {
    syncproto::HelloAck a;
    if (!syncproto::decodeHelloAck(m.payload, a))
        return;
    if (!a.ok) {
        refused_ = true;
        status("Sync: peer refused — " + a.reason + ".");
        return;
    }
    if (synced_->syncId().empty() && !a.syncId.empty())
        synced_->setSyncId(a.syncId);
    ackOk_ = true;

    if (peerHello_ && ackOk_ && !ready_) {
        ready_ = true;
        status("Sync: connected to peer.");
        if (initiator()) beginAntiEntropy();
    }
}

void SyncCoordinator::beginAntiEntropy() {
    if (refused_ || !synced_)
        return;
    const std::string digest = syncproto::manifestDigest(synced_->syncManifest());
    transport_.sendMessage({Type::Digest, syncproto::encodeDigest(digest)});
}

void SyncCoordinator::handleDigest(const Message& m) {
    if (refused_)
        return;
    std::string peerHex;
    if (!syncproto::decodeDigest(m.payload, peerHex))
        return;
    const SyncManifest local = synced_->syncManifest();
    if (syncproto::manifestDigest(local) == peerHex) {
        status("Sync: logbooks already in sync.");
        return;
    }
    // We differ: hand the peer our full manifest so it can reconcile both ways.
    transport_.sendMessage({Type::Manifest, syncproto::encodeManifest(local)});
}

void SyncCoordinator::handleManifest(const Message& m) {
    if (refused_)
        return;
    SyncManifest peer;
    if (!syncproto::decodeManifest(m.payload, peer))
        return;

    const SyncManifest local = synced_->syncManifest();

    std::unordered_map<std::string, std::string> localLive, peerLive;
    for (const auto& e : local.records) localLive[e.uuid] = e.stamp;
    for (const auto& e : peer.records)  peerLive[e.uuid]  = e.stamp;

    // Pull: peer records we lack or that are newer on the peer.
    std::vector<std::string> pull;
    for (const auto& e : peer.records) {
        const auto it = localLive.find(e.uuid);
        if (it == localLive.end() || e.stamp > it->second)
            pull.push_back(e.uuid);
    }
    // Push: local records the peer lacks or that are newer locally.
    std::vector<std::string> pushUuids;
    for (const auto& e : local.records) {
        const auto it = peerLive.find(e.uuid);
        if (it == peerLive.end() || e.stamp > it->second)
            pushUuids.push_back(e.uuid);
    }

    // Apply the peer's tombstones immediately (they're complete in the manifest).
    if (!peer.tombstones.empty())
        synced_->applyRemoteDelta({}, peer.tombstones, nodeId_, peerNodeId_);

    if (!pull.empty())
        transport_.sendMessage({Type::Request, syncproto::encodeRequest(pull)});
    if (!pushUuids.empty())
        transport_.sendMessage(
            {Type::Records, syncproto::encodeRecords(synced_->recordsByUuids(pushUuids))});
    if (!local.tombstones.empty())
        transport_.sendMessage(
            {Type::Tombstones, syncproto::encodeTombstones(local.tombstones)});

    status("Sync: reconciling (" + std::to_string(pull.size()) + " in, " +
           std::to_string(pushUuids.size()) + " out).");
}

void SyncCoordinator::handleRequest(const Message& m) {
    if (refused_)
        return;
    std::vector<std::string> uuids;
    if (!syncproto::decodeRequest(m.payload, uuids) || uuids.empty())
        return;
    transport_.sendMessage(
        {Type::Records, syncproto::encodeRecords(synced_->recordsByUuids(uuids))});
}

void SyncCoordinator::handleRecords(const Message& m) {
    if (refused_)
        return;
    std::vector<Qso> recs;
    if (!syncproto::decodeRecords(m.payload, recs) || recs.empty())
        return;
    const MergeResult r = synced_->applyRemoteDelta(recs, {}, nodeId_, peerNodeId_);
    if (r.inserted || r.updated)
        status("Sync: received " + std::to_string(r.inserted) + " new, " +
               std::to_string(r.updated) + " updated QSO(s).");
}

void SyncCoordinator::handleTombstones(const Message& m) {
    if (refused_)
        return;
    std::vector<SyncEntry> tombs;
    if (!syncproto::decodeTombstones(m.payload, tombs) || tombs.empty())
        return;
    const MergeResult r = synced_->applyRemoteDelta({}, tombs, nodeId_, peerNodeId_);
    if (r.deleted)
        status("Sync: removed " + std::to_string(r.deleted) + " QSO(s) deleted on peer.");
}

void SyncCoordinator::handlePush(const Message& m) {
    if (refused_)
        return;
    syncproto::Push p;
    if (!syncproto::decodePush(m.payload, p))
        return;
    if (p.isTombstone)
        synced_->applyRemoteDelta({}, {p.tomb}, nodeId_, peerNodeId_);
    else
        synced_->applyRemoteDelta({p.record}, {}, nodeId_, peerNodeId_);
}

void SyncCoordinator::onLocalUpsert(const Qso& q) {
    if (!ready_ || refused_ || !transport_.isConnected())
        return;
    transport_.sendMessage({Type::Push, syncproto::encodePushRecord(q)});
}

void SyncCoordinator::onLocalDelete(const std::string& uuid,
                                    const std::string& deletedAt) {
    if (!ready_ || refused_ || !transport_.isConnected())
        return;
    transport_.sendMessage(
        {Type::Push, syncproto::encodePushTombstone({uuid, deletedAt})});
}

void SyncCoordinator::syncNow() {
    if (!ready_) {
        status("Sync: not connected to a peer.");
        return;
    }
    beginAntiEntropy();
}
