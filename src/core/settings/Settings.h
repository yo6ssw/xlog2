// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2025 Adrian Scripcă (YO6SSW)

#pragma once

#include <array>
#include <string>
#include <vector>

#include "IniFile.h"

// The scalar application configuration, toolkit-neutral. Window geometry, the
// session tab list and the per-column layout stay in the IniFile directly
// (they depend on live widget state), but every backend-agnostic setting lives
// here so the presenter — not the view — owns it.
struct Settings {
  // [udp]
  int udpPort = 2237;  // WSJT-X default
  bool udpEnabled = false;

  // [rig]
  int rigModel = 1;  // 1 == RIG_MODEL_DUMMY
  std::string rigDevice;
  int rigPollMs = 500;
  bool rigAutoConnect = false;
  // Rig control panel (dockable: frequency display + tune/filter controls).
  std::string rigDock = "right";  // top|bottom|left|right
  bool rigVisible = true;
  int rigPanelPos = 0;  // saved dock/divider size (0 = unset)

  // [lotw]
  std::string lotwUser, lotwPassword, lotwStation, lotwLastDownload;
  std::string tqslPath = "tqsl";

  // [qrz]
  std::string qrzUser, qrzPassword;
  int qrzCacheDays = 365;  // cache lifetime in days (<=0 disables it)

  // [station] — the operator's own location (Maidenhead grid), used as the
  // "from" point of the world-map panel.
  std::string myLocator;

  // [map] — world-map panel (dockable: great-circle path between two locators).
  std::string mapDock = "right";  // top|bottom|left|right
  bool mapVisible = false;
  int mapPanelPos = 0;  // saved dock/divider size (0 = unset)

  // [keyer]
  std::string keyerHost = "127.0.0.1";
  int keyerPort = 6789;
  int keyerSpeed = 0;  // 0 = leave cwdaemon's default
  std::array<std::string, 9> keyerMessages{};

  // [paddle] — cwsd remote_key real paddle keying over UDP
  bool paddleEnabled = false;
  std::string paddleHost = "127.0.0.1";
  int paddlePort = 6790;
  int paddleWpm = 20;
  bool paddleIambicB = false;   // false = iambic A
  bool paddleAutospace = true;  // enforce full inter-character spacing
  bool paddleSidetone = true;   // local sidetone on/off
  int paddleToneHz = 600;       // local sidetone frequency
  int paddleLevel = 50;         // local sidetone volume, 0..100
  std::string paddleSidetoneDevice = "default";  // ALSA playback device
  bool paddleMuteAudio = true;  // mute the rig-audio stream while keying
  int paddleMuteTailMs = 500;   // mute hang after the last key-up, ms

  // [audio] — cwsd Opus-over-UDP rig audio stream
  bool audioEnabled = false;
  std::string audioHost = "127.0.0.1";
  int audioPort = 7355;
  int audioSampleRate = 8000;           // must match the server (opus rate)
  int audioChannels = 1;                // must match the server
  std::string audioDevice = "default";  // ALSA playback device

  // [skimmer] — CW Skimmer panel (decodes the rig-audio stream's passband)
  std::string skimmerDock = "left";  // top|bottom|left|right
  bool skimmerVisible = false;
  int skimmerPanelPos = 0;        // saved dock/divider size (0 = unset)
  int skimmerGate = 0;            // detection gating level, dB (see CwSkimmer)
  int skimmerMinSnr = 0;          // minimum per-channel SNR, dB (see CwSkimmer)
  bool skimmerKnownOnly = false;  // Paranoid: only surface DB-confirmed calls
  // Waterfall level compensation for the rig's IF filter: narrowing the filter
  // makes the AGC lift the passband and the percentile floor collapse,
  // brightening the display. Dim it by this many dB per octave the live
  // passband is narrower than the reference width (0 = off). See
  // CwSkimmer::setBandwidthNorm.
  int skimmerBwNormDb = 6;        // dB per octave of narrowing (0 = off)
  int skimmerBwNormRefHz = 2800;  // passband width treated as 0 dB
  int skimmerBwOffsetDb = 0;      // constant waterfall trim applied first,
                                  // dB (positive dims, negative brightens)

  // [sync] — peer-to-peer logbook sync over a multimaster LAN mesh. Symmetric:
  // every instance auto-discovers peers, so there is no listener/connector
  // role.
  bool syncEnabled = false;
  int syncPort = 7388;          // mesh TCP listen port
  std::string syncSecret;       // pre-shared key: auth + which mesh
  std::string syncPeerHost;     // optional WAN peer (host) for the internet
  std::string syncPeerHostAlt;  // optional second WAN peer
  std::string syncNodeId;       // mesh id, minted once; tie-breaker
  // When a secret is set, the mesh is secured (encryption) and each node holds
  // a self-certifying Ed25519 identity; these govern its per-node trust.
  bool syncRequireIdentity = true;  // reject peers without an identity
  std::string syncNodeName;  // signed display name (default: station call)
  // Per-node allowlist: only these mesh ids exchange logbook data. Others still
  // connect and surface in the Trusted-peers dialog for the operator to admit.
  struct SyncTrustedPeer {
    std::string id;
    std::string label;
  };
  std::vector<SyncTrustedPeer> syncTrustedPeers;

  // [dxcluster]
  std::string dxHost;
  int dxPort = 7300;
  std::string dxLogin;
  std::string dxDock = "bottom";  // top|bottom|left|right
  bool dxVisible = false;
  bool dxAutoConnect = false;
  int dxPanelPos = 0;  // saved Gtk::Paned divider (0 = unset)

  // Read the scalar groups out of an IniFile (missing keys keep defaults).
  static Settings load(const IniFile& ini);
  // Write the scalar groups into an IniFile (leaving other groups untouched).
  void store(IniFile& ini) const;
};
