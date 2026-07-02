// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2025 Adrian Scripcă (YO6SSW)

#pragma once

#include "LogbookSync.h"
#include "QrzResult.h"
#include "SyncProtocol.h"

#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <unordered_map>

class QrzClient;

// Distributed QRZ cache over the sync mesh. Two roles, both on the UI thread:
//
//  - Responder: when a peer asks for a callsign (QrzQuery), answer from our own
//    local QRZ cache (QrzResponse) if we have it; stay silent otherwise.
//  - Asker: query() broadcasts a QrzQuery and resolves the caller's reply with
//    the first record a peer returns, or nullopt after a short timeout / when no
//    peers are connected. Wired as QrzClient's PeerResolver so a lookup consults
//    peers between the local cache and qrz.com.
//
// Because the mesh group is derived from the shared secret, every reachable peer
// already proved knowledge of the secret — so sharing cached callsign data
// across the mesh needs no further gating.
class QrzPeer {
public:
    QrzPeer(LogbookSync& transport, QrzClient& qrz) : transport_(transport), qrz_(qrz) {}

    // Schedule fn() once after `ms` on the UI thread. Wired by the shell to the
    // toolkit timer (QTimer::singleShot / Glib signal_timeout). Required for the
    // query timeout; if unset, query() falls back to the network immediately.
    std::function<void(int ms, std::function<void()> fn)> scheduleOnce;

    std::function<void(const std::string&)> onStatus;

    // QrzClient::PeerResolver: ask the mesh, then call reply once.
    void query(const std::string& callsign,
               std::function<void(std::optional<QrzResult>)> reply);

    // A QRZ-typed mesh message arrived (routed here by the shell).
    void onMessage(const LogbookSync::PeerKey& peer, const syncproto::Message& m);

    // How long to wait for a peer to answer before falling back to qrz.com.
    void setTimeoutMs(int ms) { timeoutMs_ = ms; }

private:
    void handleQuery(const LogbookSync::PeerKey& peer, const syncproto::Message& m);
    void handleResponse(const syncproto::Message& m);
    void status(const std::string& s) { if (onStatus) onStatus(s); }

    LogbookSync& transport_;
    QrzClient&   qrz_;
    int          timeoutMs_ = 1500;
    std::uint32_t nextId_ = 1;

    struct Pending {
        std::string callsign;
        std::function<void(std::optional<QrzResult>)> reply;
    };
    std::unordered_map<std::uint32_t, Pending> pending_;
};
