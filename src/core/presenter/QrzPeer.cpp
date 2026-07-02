// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2025 Adrian Scripcă (YO6SSW)

#include "QrzPeer.h"

#include "Qrz.h"

using syncproto::Message;
using syncproto::Type;

void QrzPeer::query(const std::string& callsign,
                    std::function<void(std::optional<QrzResult>)> reply) {
  // No peers (mesh down or alone) or no timer to bound the wait → straight to
  // the network.
  if (transport_.memberCount() <= 0 || !scheduleOnce) {
    reply(std::nullopt);
    return;
  }

  const std::uint32_t id = nextId_++;
  pending_[id] = Pending{callsign, std::move(reply)};
  transport_.broadcast(
      {Type::QrzQuery, syncproto::encodeQrzQuery(id, callsign)});

  // Resolve to nullopt if no peer has answered by the deadline.
  scheduleOnce(timeoutMs_, [this, id]() {
    auto it = pending_.find(id);
    if (it == pending_.end()) return;  // already answered by a peer
    auto reply = std::move(it->second.reply);
    pending_.erase(it);
    reply(std::nullopt);
  });
}

void QrzPeer::onMessage(const LogbookSync::PeerKey& peer, const Message& m) {
  switch (m.type) {
    case Type::QrzQuery:
      handleQuery(peer, m);
      break;
    case Type::QrzResponse:
      handleResponse(m);
      break;
    default:
      break;
  }
}

void QrzPeer::handleQuery(const LogbookSync::PeerKey& peer, const Message& m) {
  std::uint32_t id = 0;
  std::string callsign;
  if (!syncproto::decodeQrzQuery(m.payload, id, callsign) || callsign.empty())
    return;
  // Answer only if we have a fresh cached record; silence on a miss.
  if (auto cached = qrz_.cachedLookup(callsign);
      cached && !cached->call.empty()) {
    transport_.sendTo(
        peer, {Type::QrzResponse, syncproto::encodeQrzResponse(id, *cached)});
    status("QRZ: answered a peer's lookup of " + cached->call + " from cache.");
  }
}

void QrzPeer::handleResponse(const Message& m) {
  std::uint32_t id = 0;
  QrzResult result;
  if (!syncproto::decodeQrzResponse(m.payload, id, result) ||
      result.call.empty())
    return;
  auto it = pending_.find(id);
  if (it == pending_.end())
    return;  // unknown / already resolved (timeout or an earlier peer)
  auto reply = std::move(it->second.reply);
  pending_.erase(it);
  status("QRZ: " + result.call + " resolved from a peer's cache.");
  reply(std::move(result));
}
