// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2025 Adrian Scripcă (YO6SSW)

#pragma once

#include <gtkmm.h>

#include <array>
#include <map>
#include <string>
#include <vector>

#include "Audio.h"
#include "CwKeyer.h"
#include "CwSkimmer.h"
#include "CwSkimmerPanel.h"
#include "DxCluster.h"
#include "DxClusterPanel.h"
#include "GlibDispatcher.h"
#include "HidPaddleInput.h"
#include "IMainView.h"
#include "LogbookSync.h"
#include "Lotw.h"
#include "MainPresenter.h"
#include "MapPanel.h"
#include "Qrz.h"
#include "QrzPeer.h"
#include "Qso.h"
#include "RemotePaddleKeyer.h"
#include "Rig.h"
#include "RigPanel.h"
#include "SyncCoordinator.h"
#include "Udp.h"

class LogPage;
class LogPagePresenter;

// The application shell (the view): a menu bar, a Gtk::Notebook of logbook tabs
// (LogPage), a status line, and the toolkit-neutral service objects (UDP, rig,
// LoTW, QRZ, keyer, DX cluster). It owns the dialogs and notebook; the
// configuration model and service-result routing live in MainPresenter, which
// it drives via IMainView.
class MainWindow : public Gtk::ApplicationWindow, public IMainView {
 public:
  MainWindow();

  // --- IMainView ---
  void setStatus(const std::string& msg) override;
  LogPagePresenter* currentLog() override;
  bool isLogLive(LogPagePresenter* log) override;
  void showQrzResult(const QrzResult& result) override;
  bool startQrzLookup(const std::string& callsign) override;

 private:
  void buildActions();
  Glib::RefPtr<Gio::Menu> buildMenuModel();

  // --- tab/page management ---
  LogPage* currentPage();
  LogPage* addPage(LogPage* page);     // append, wire up, focus; returns page
  LogPage* openDefaultLog();           // open the persistent default logbook
  std::string defaultLogPath() const;  // $XDG_DATA_HOME/xlog2/default.xlog
  void registerTab(LogPage* page);
  void updateTabLabel(LogPage* page);
  void onPageChanged(LogPage* page);
  void closePage(LogPage* page);
  LogPage* findPageByPath(const std::string& path);
  // Move a QSO from one logbook tab to another (row context menu "Move to").
  void moveQso(LogPage* from, long id, LogPagePresenter* target);

  // --- menu actions ---
  void onNewTab();
  void onOpen();
  void onSaveAs();
  void onCloseTab();
  void onImportAdif();
  void onImportXlog();
  void onExportAdif();
  void onStatistics();
  void onFind();
  void onAbout();

  // --- consolidated settings (Edit ▸ Settings) ---
  // Open the preferences window; applySettings copies the config-field subset
  // of `edited` into cfg() and re-applies it to any running service.
  void onEditSettings();
  void applySettings(const Settings& edited);

  // --- UDP network logging ---
  void onToggleUdp();
  void startUdpListening();
  void stopUdpListening();

  // --- Hamlib rig control ---
  void onRigConnect();  // connect using the stored rig settings (no dialog)
  void onRigDisconnect();

  // --- rig control panel ---
  void onRigToggleShow();                     // show/hide the panel
  void onRigDock(const Glib::ustring& side);  // dock-side radio action
  void applyRigDock();    // (re)build the paned from dock/visibility
  void applyRigConfig();  // after load: dock + action states

  // --- LoTW ---
  void onLotwUpload();
  void onLotwDownload();

  // --- QRZ.com callsign lookup ---
  void onQrzLookup(LogPage* page, const std::string& callsign);
  void onFillLocators();  // bulk-fill missing QSO locators via QRZ (+ cache)

  // --- network keyer (cwdaemon) ---
  void applyKeyerConfig();  // push endpoint/speed to keyer_ + messages to pages

  // --- remote paddle keyer (cwsd remote_key) ---
  void onTogglePaddle();  // start/stop toggle
  void startPaddleKeyer();
  void stopPaddleKeyer();

  // --- rig audio stream (cwsd) ---
  void onToggleAudio();  // start/stop toggle
  void startAudioStream();
  void stopAudioStream();

  // --- CW skimmer ---
  void onSkimmerToggleShow();                     // show/hide the panel
  void onSkimmerDock(const Glib::ustring& side);  // dock-side radio action
  void applySkimmerDock();                        // (re)build the paned
  void applySkimmerConfig();  // after load: dock + start/stop
  void startSkimmer();
  void stopSkimmer();

  // --- world map ---
  void onMapToggleShow();                     // show/hide the panel
  void onMapDock(const Glib::ustring& side);  // dock-side radio action
  void applyMapDock();                        // (re)build the paned
  void applyMapConfig();                      // after load: dock + seed locator

  // --- logbook sync ---
  void onSyncNow();  // force an anti-entropy pass with all peers
  void startSync();  // (re)start the mesh transport
  void attachSyncedLog(
      LogPage* page);          // bind the coordinator to the default log
  void updateSyncIndicator();  // status-bar peer count
  void applySyncTrust();  // push the settings allowlist into the coordinator
  void onManageTrustedPeers();  // open the live trusted-peers dialog
  void
  refreshTrustedPeersDialog();  // repopulate the open dialog from a snapshot

  // --- DX cluster ---
  void onClusterConnect();                   // connect/disconnect toggle
  void onClusterToggleShow();                // show/hide the panel
  void onDxDock(const Glib::ustring& side);  // dock-side radio action
  void onSpotActivated(const DxSpot& spot);  // fill form + tune rig
  void applyDxDock();    // (re)build the paned from dock/visibility
  void applyDxConfig();  // after load: dock + optional auto-connect

  // --- settings persistence ---
  std::string layoutFilePath() const;
  void saveSettings();
  void loadSettings();
  bool onCloseRequest();

  void updateTitle();

  // Convenience accessor for the configuration model owned by the presenter.
  Settings& cfg() { return presenter_.settings; }

  Gtk::Notebook notebook_;
  Gtk::Label statusLabel_;
  Gtk::Label audioIndicator_;      // live audio-frame counter
  Gtk::Label syncIndicator_;       // peer connection state
  LogPage* syncedPage_ = nullptr;  // the synced default log
  std::map<LogPage*, Gtk::Label*> tabLabels_;

  // Marshals worker-thread results from the services onto the UI thread.
  // Declared before the services so it outlives them (and is constructed
  // first, as they take a reference to it).
  GlibDispatcher uiDispatcher_;

  // Configuration model + service-result routing (toolkit-neutral).
  MainPresenter presenter_;

  // Toolkit-neutral service objects (owned by the shell; their callbacks are
  // forwarded to the presenter for routing).
  UdpListener listener_;
  RigController rig_;
  LotwClient lotw_;
  QrzClient qrz_;
  CwKeyer keyer_;
  DxCluster cluster_;
  // skimmer_ is declared before audio_ so it outlives it: the audio worker's
  // onPcm tap calls skimmer_.pushPcm, and members destruct in reverse order.
  CwSkimmer skimmer_;
  AudioStreamClient audio_;
  RemotePaddleKeyer paddle_;
  HidPaddleInput hidPaddle_;
  LogbookSync sync_;
  SyncCoordinator coordinator_;  // after sync_ (holds a reference)
  QrzPeer qrzPeer_;              // after sync_ + qrz_ (refs both)

  Glib::RefPtr<Gio::SimpleAction> udpAction_;
  Glib::RefPtr<Gio::SimpleAction> audioAction_;
  Glib::RefPtr<Gio::SimpleAction> paddleAction_;
  Glib::RefPtr<Gio::SimpleAction> syncEnableAction_;

  // Live trusted-peers dialog (heap window; null when closed). trustedPeersBox_
  // is the row container repopulated on every onPeersChanged.
  Gtk::Window* trustedPeersDialog_ = nullptr;
  Gtk::Box* trustedPeersBox_ = nullptr;

  // DX cluster panel + layout. dxPanel_ is a value member (like notebook_) so
  // it survives being reparented between paned slots when the dock side
  // changes — a make_managed widget would be destroyed the moment
  // unset_*_child drops the paned's only reference.
  DxClusterPanel dxPanel_;
  Gtk::Paned paned_;  // wraps notebook_ + dxPanel_
  Glib::RefPtr<Gio::SimpleAction> dxShowAction_;
  Glib::RefPtr<Gio::SimpleAction> dxDockAction_;

  // Rig control panel + layout. rigPaned_ wraps the whole DX/notebook area
  // (paned_) and rigPanel_, so the rig panel docks independently of the DX one.
  RigPanel rigPanel_;
  Gtk::Paned rigPaned_;  // wraps paned_ + rigPanel_
  Glib::RefPtr<Gio::SimpleAction> rigShowAction_;
  Glib::RefPtr<Gio::SimpleAction> rigDockAction_;

  // CW skimmer panel + layout. skimmerPaned_ wraps the rig/DX/notebook area
  // (rigPaned_) and skimmerPanel_, so the skimmer docks independently.
  CwSkimmerPanel skimmerPanel_;
  Gtk::Paned skimmerPaned_;  // wraps rigPaned_ + skimmerPanel_
  Glib::RefPtr<Gio::SimpleAction> skimmerShowAction_;
  Glib::RefPtr<Gio::SimpleAction> skimmerDockAction_;

  // World-map panel + layout. mapPaned_ wraps the skimmer/rig/DX/notebook area
  // (skimmerPaned_) and mapPanel_, so the map docks independently.
  MapPanel mapPanel_;
  Gtk::Paned mapPaned_;  // wraps skimmerPaned_ + mapPanel_
  Glib::RefPtr<Gio::SimpleAction> mapShowAction_;
  Glib::RefPtr<Gio::SimpleAction> mapDockAction_;
  // Latest frequency/mode from rig_.onUpdate, rendered together with the
  // passband/filter that arrives in the paired rig_.onFilter call.
  double lastMhz_ = 0.0;
  std::string lastMode_;

  // Loaded settings, used to apply the shared column layout to new pages.
  IniFile settings_;
};
