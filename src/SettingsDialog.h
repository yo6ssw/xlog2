// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2025 Adrian Scripcă (YO6SSW)

#pragma once

#include <gtkmm.h>

#include <array>
#include <functional>

#include "Settings.h"

// The consolidated preferences window (Edit ▸ Settings): a Gtk::StackSidebar of
// component categories switching a Gtk::Stack page each. It edits only the
// scalar *config* fields of Settings; runtime/view state (panel
// dock/visibility, the enable toggles, lotwLastDownload) is left to the menus
// and preserved.
//
// Seeded from the current Settings; collect() returns a copy with the config
// fields overwritten from the widgets. On Apply/OK it hands that copy to the
// apply callback (the shell restarts any running service); the dialog itself
// touches no services. Follows the shell's heap-allocate + delete-on-hide
// idiom.
class SettingsDialog : public Gtk::Window {
 public:
  SettingsDialog(const Settings& s,
                 std::function<void(const Settings&)> onApply);

 private:
  Gtk::Grid& addPage(Gtk::Stack& stack, const char* id, const char* title);

  Settings collect() const;

  Settings seed_;  // preserves the fields the dialog does not edit
  std::function<void(const Settings&)> onApply_;

  // --- station ---
  Gtk::Entry* myLocator_ = nullptr;
  // --- network ---
  Gtk::Entry* udpPort_ = nullptr;
  // --- rig ---
  Gtk::Entry* rigModel_ = nullptr;
  Gtk::Entry* rigDevice_ = nullptr;
  Gtk::Entry* rigPoll_ = nullptr;
  Gtk::CheckButton* rigAuto_ = nullptr;
  // --- dx cluster ---
  Gtk::Entry* dxHost_ = nullptr;
  Gtk::Entry* dxPort_ = nullptr;
  Gtk::Entry* dxLogin_ = nullptr;
  Gtk::CheckButton* dxAuto_ = nullptr;
  // --- lotw ---
  Gtk::Entry* lotwUser_ = nullptr;
  Gtk::Entry* lotwPass_ = nullptr;
  Gtk::Entry* lotwStation_ = nullptr;
  Gtk::Entry* tqslPath_ = nullptr;
  // --- qrz ---
  Gtk::Entry* qrzUser_ = nullptr;
  Gtk::Entry* qrzPass_ = nullptr;
  Gtk::Entry* qrzCacheDays_ = nullptr;
  // --- keyer ---
  Gtk::Entry* keyerHost_ = nullptr;
  Gtk::Entry* keyerPort_ = nullptr;
  Gtk::Entry* keyerSpeed_ = nullptr;
  std::array<Gtk::Entry*, 9> keyerMsgs_{};
  // --- paddle ---
  Gtk::Entry* paddleHost_ = nullptr;
  Gtk::Entry* paddlePort_ = nullptr;
  Gtk::Entry* paddleWpm_ = nullptr;
  Gtk::CheckButton* paddleIambicB_ = nullptr;
  Gtk::CheckButton* paddleAutospace_ = nullptr;
  Gtk::CheckButton* paddleSidetone_ = nullptr;
  Gtk::Entry* paddleTone_ = nullptr;
  Gtk::Entry* paddleLevel_ = nullptr;
  Gtk::Entry* paddleDevice_ = nullptr;
  Gtk::CheckButton* paddleMute_ = nullptr;
  Gtk::Entry* paddleMuteTail_ = nullptr;
  // --- audio ---
  Gtk::Entry* audioHost_ = nullptr;
  Gtk::Entry* audioPort_ = nullptr;
  Gtk::Entry* audioRate_ = nullptr;
  Gtk::Entry* audioChan_ = nullptr;
  Gtk::Entry* audioDevice_ = nullptr;
  // --- sync ---
  Gtk::CheckButton* syncEnabled_ = nullptr;
  Gtk::Entry* syncSecret_ = nullptr;
  Gtk::Entry* syncPort_ = nullptr;
  Gtk::Entry* syncPeerHost_ = nullptr;
  Gtk::Entry* syncPeerHostAlt_ = nullptr;
  Gtk::Entry* syncNodeName_ = nullptr;
  Gtk::CheckButton* syncRequireIdentity_ = nullptr;
  // --- skimmer ---
  Gtk::Entry* skGate_ = nullptr;
  Gtk::Entry* skMinSnr_ = nullptr;
  Gtk::CheckButton* skKnownOnly_ = nullptr;
  Gtk::Entry* skBwNormDb_ = nullptr;
  Gtk::Entry* skBwNormRef_ = nullptr;
  Gtk::Entry* skBwOffset_ = nullptr;
};
