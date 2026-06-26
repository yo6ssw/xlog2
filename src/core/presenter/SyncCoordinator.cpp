#include "SyncCoordinator.h"

#include "LogPagePresenter.h"
#include "Uuid.h"

using syncproto::Message;
using syncproto::Type;
using PeerKey = LogbookSync::PeerKey;

void SyncCoordinator::configure(const std::string& nodeId,
                                const std::string& secret) {
    nodeId_ = nodeId;
    secret_ = secret;
}

void SyncCoordinator::attach(LogPagePresenter* synced) {
    synced_ = synced;
    if (!synced)
        peers_.clear();
}

int SyncCoordinator::readyPeerCount() const {
    int n = 0;
    for (const auto& [key, st] : peers_)
        if (st.ready) ++n;
    return n;
}

void SyncCoordinator::onPeerUp(const PeerKey& peer) {
    if (!synced_)
        return;
    peers_[peer] = PeerState{};  // fresh handshake state
    sendHello(peer);
}

void SyncCoordinator::onPeerDown(const PeerKey& peer) {
    peers_.erase(peer);
}

void SyncCoordinator::sendHello(const PeerKey& peer) {
    if (!synced_)
        return;
    const std::string nonce = uuidutil::newUuid();
    transport_.sendTo(
        peer, {Type::Hello, syncproto::encodeHello(nodeId_, synced_->syncId(), nonce, secret_)});
    peers_[peer].helloSent = true;
}

void SyncCoordinator::onMessage(const PeerKey& peer, const Message& m) {
    if (!synced_)
        return;
    switch (m.type) {
        case Type::Hello:      handleHello(peer, m);      break;
        case Type::HelloAck:   handleHelloAck(peer, m);   break;
        case Type::Digest:     handleDigest(peer, m);     break;
        case Type::Manifest:   handleManifest(peer, m);   break;
        case Type::Request:    handleRequest(peer, m);    break;
        case Type::Records:    handleRecords(peer, m);    break;
        case Type::Tombstones: handleTombstones(peer, m); break;
        case Type::Push:       handlePush(peer, m);       break;
        default: break;  // Ping/Pong unused (the mesh has its own heartbeats)
    }
}

void SyncCoordinator::maybeReady(const PeerKey& peer, PeerState& st) {
    if (st.peerHello && st.ackOk && !st.ready) {
        st.ready = true;
        status("Sync: connected to a peer.");
        if (initiator(st)) beginAntiEntropy(peer);
    }
}

void SyncCoordinator::handleHello(const PeerKey& peer, const Message& m) {
    syncproto::Hello h;
    if (!syncproto::decodeHello(m.payload, secret_, h))
        return;
    PeerState& st = peers_[peer];
    if (!h.authOk) {
        st.refused = true;
        transport_.sendTo(
            peer, {Type::HelloAck, syncproto::encodeHelloAck(false, "", "authentication failed")});
        status("Sync: peer authentication failed (check the shared secret).");
        return;
    }

    st.peerNodeId = h.nodeId;
    st.peerHello  = true;

    const std::string localId = synced_->syncId();
    const std::string peerId  = h.syncId;

    if (!localId.empty() && !peerId.empty() && localId != peerId) {
        st.refused = true;
        transport_.sendTo(
            peer, {Type::HelloAck, syncproto::encodeHelloAck(false, localId, "different logbook")});
        status("Sync: peer is a different logbook; refusing to merge.");
        return;
    }

    std::string resolved;
    if (!localId.empty()) {
        resolved = localId;
    } else if (!peerId.empty()) {
        resolved = peerId;
        synced_->setSyncId(peerId);  // adopt the peer's id
    } else if (initiator(st)) {
        resolved = synced_->ensureSyncId();  // both fresh: higher node mints
    }
    transport_.sendTo(peer, {Type::HelloAck, syncproto::encodeHelloAck(true, resolved, "")});

    maybeReady(peer, st);
}

void SyncCoordinator::handleHelloAck(const PeerKey& peer, const Message& m) {
    syncproto::HelloAck a;
    if (!syncproto::decodeHelloAck(m.payload, a))
        return;
    PeerState& st = peers_[peer];
    if (!a.ok) {
        st.refused = true;
        status("Sync: peer refused — " + a.reason + ".");
        return;
    }
    if (synced_->syncId().empty() && !a.syncId.empty())
        synced_->setSyncId(a.syncId);
    st.ackOk = true;
    maybeReady(peer, st);
}

void SyncCoordinator::beginAntiEntropy(const PeerKey& peer) {
    const PeerState& st = peers_[peer];
    if (st.refused || !synced_)
        return;
    const std::string digest = syncproto::manifestDigest(synced_->syncManifest());
    transport_.sendTo(peer, {Type::Digest, syncproto::encodeDigest(digest)});
}

void SyncCoordinator::handleDigest(const PeerKey& peer, const Message& m) {
    if (peers_[peer].refused)
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
    transport_.sendTo(peer, {Type::Manifest, syncproto::encodeManifest(local)});
}

void SyncCoordinator::handleManifest(const PeerKey& peer, const Message& m) {
    PeerState& st = peers_[peer];
    if (st.refused)
        return;
    SyncManifest peerMan;
    if (!syncproto::decodeManifest(m.payload, peerMan))
        return;

    const SyncManifest local = synced_->syncManifest();

    std::unordered_map<std::string, std::string> localLive, peerLive;
    for (const auto& e : local.records)   localLive[e.uuid] = e.stamp;
    for (const auto& e : peerMan.records) peerLive[e.uuid]  = e.stamp;

    // Pull: peer records we lack or that are newer on the peer.
    std::vector<std::string> pull;
    for (const auto& e : peerMan.records) {
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

    // Apply the peer's tombstones immediately (complete in the manifest).
    if (!peerMan.tombstones.empty())
        synced_->applyRemoteDelta({}, peerMan.tombstones, nodeId_, st.peerNodeId);

    if (!pull.empty())
        transport_.sendTo(peer, {Type::Request, syncproto::encodeRequest(pull)});
    if (!pushUuids.empty())
        transport_.sendTo(
            peer, {Type::Records, syncproto::encodeRecords(synced_->recordsByUuids(pushUuids))});
    if (!local.tombstones.empty())
        transport_.sendTo(
            peer, {Type::Tombstones, syncproto::encodeTombstones(local.tombstones)});

    status("Sync: reconciling (" + std::to_string(pull.size()) + " in, " +
           std::to_string(pushUuids.size()) + " out).");
}

void SyncCoordinator::handleRequest(const PeerKey& peer, const Message& m) {
    if (peers_[peer].refused)
        return;
    std::vector<std::string> uuids;
    if (!syncproto::decodeRequest(m.payload, uuids) || uuids.empty())
        return;
    transport_.sendTo(
        peer, {Type::Records, syncproto::encodeRecords(synced_->recordsByUuids(uuids))});
}

void SyncCoordinator::handleRecords(const PeerKey& peer, const Message& m) {
    PeerState& st = peers_[peer];
    if (st.refused)
        return;
    std::vector<Qso> recs;
    if (!syncproto::decodeRecords(m.payload, recs) || recs.empty())
        return;
    const MergeResult r = synced_->applyRemoteDelta(recs, {}, nodeId_, st.peerNodeId);
    if (r.inserted || r.updated)
        status("Sync: received " + std::to_string(r.inserted) + " new, " +
               std::to_string(r.updated) + " updated QSO(s).");
}

void SyncCoordinator::handleTombstones(const PeerKey& peer, const Message& m) {
    PeerState& st = peers_[peer];
    if (st.refused)
        return;
    std::vector<SyncEntry> tombs;
    if (!syncproto::decodeTombstones(m.payload, tombs) || tombs.empty())
        return;
    const MergeResult r = synced_->applyRemoteDelta({}, tombs, nodeId_, st.peerNodeId);
    if (r.deleted)
        status("Sync: removed " + std::to_string(r.deleted) + " QSO(s) deleted on peer.");
}

void SyncCoordinator::handlePush(const PeerKey& peer, const Message& m) {
    PeerState& st = peers_[peer];
    if (st.refused)
        return;
    syncproto::Push p;
    if (!syncproto::decodePush(m.payload, p))
        return;
    if (p.isTombstone)
        synced_->applyRemoteDelta({}, {p.tomb}, nodeId_, st.peerNodeId);
    else
        synced_->applyRemoteDelta({p.record}, {}, nodeId_, st.peerNodeId);
}

void SyncCoordinator::onLocalUpsert(const Qso& q) {
    // Push only to authenticated, ready peers — never to a peer that failed the
    // secret / sync_id gate.
    const Message m{Type::Push, syncproto::encodePushRecord(q)};
    for (const auto& [peer, st] : peers_)
        if (st.ready && !st.refused)
            transport_.sendTo(peer, m);
}

void SyncCoordinator::onLocalDelete(const std::string& uuid,
                                    const std::string& deletedAt) {
    const Message m{Type::Push, syncproto::encodePushTombstone({uuid, deletedAt})};
    for (const auto& [peer, st] : peers_)
        if (st.ready && !st.refused)
            transport_.sendTo(peer, m);
}

void SyncCoordinator::syncNow() {
    int n = 0;
    for (const auto& [peer, st] : peers_) {
        if (st.ready && !st.refused) { beginAntiEntropy(peer); ++n; }
    }
    if (n == 0)
        status("Sync: no connected peers.");
}
