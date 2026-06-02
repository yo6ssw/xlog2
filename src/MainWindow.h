#pragma once

#include "CwKeyer.h"
#include "DxCluster.h"
#include "DxClusterPanel.h"
#include "GlibDispatcher.h"
#include "IMainView.h"
#include "Lotw.h"
#include "MainPresenter.h"
#include "Qrz.h"
#include "Qso.h"
#include "Rig.h"
#include "Udp.h"

#include <gtkmm.h>

#include <array>
#include <map>
#include <string>
#include <vector>

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

private:
    void buildActions();
    Glib::RefPtr<Gio::Menu> buildMenuModel();

    // --- tab/page management ---
    LogPage* currentPage();
    LogPage* addPage(LogPage* page);          // append, wire up, focus; returns page
    LogPage* openDefaultLog();                // open the persistent default logbook
    std::string defaultLogPath() const;       // $XDG_DATA_HOME/xlog2/default.xlog
    void     registerTab(LogPage* page);
    void     updateTabLabel(LogPage* page);
    void     onPageChanged(LogPage* page);
    void     closePage(LogPage* page);
    LogPage* findPageByPath(const std::string& path);

    // --- menu actions ---
    void onNewTab();
    void onOpen();
    void onSaveAs();
    void onCloseTab();
    void onImportAdif();
    void onExportAdif();
    void onStatistics();
    void onFind();
    void onAbout();

    // --- UDP network logging ---
    void onToggleUdp();
    void startUdpListening();
    void stopUdpListening();
    void onUdpSettings();

    // --- Hamlib rig control ---
    void onRigConnect();
    void onRigDisconnect();

    // --- LoTW ---
    void onLotwUpload();
    void onLotwDownload();
    void onLotwSettings();

    // --- QRZ.com callsign lookup ---
    void onQrzLookup(LogPage* page, const std::string& callsign);
    void onQrzSettings();

    // --- network keyer (cwdaemon) ---
    void onKeyerSettings();
    void applyKeyerConfig();   // push endpoint/speed to keyer_ + messages to pages

    // --- DX cluster ---
    void onClusterConnect();          // connect/disconnect toggle
    void onClusterSettings();
    void onClusterToggleShow();       // show/hide the panel
    void onDxDock(const Glib::ustring& side);  // dock-side radio action
    void onSpotActivated(const DxSpot& spot);  // fill form + tune rig
    void applyDxDock();               // (re)build the paned from dock/visibility
    void applyDxConfig();             // after load: dock + optional auto-connect

    // --- settings persistence ---
    std::string layoutFilePath() const;
    void saveSettings();
    void loadSettings();
    bool onCloseRequest();

    void updateTitle();

    // Convenience accessor for the configuration model owned by the presenter.
    Settings& cfg() { return presenter_.settings; }

    Gtk::Notebook                   notebook_;
    Gtk::Label                      statusLabel_;
    std::map<LogPage*, Gtk::Label*> tabLabels_;

    // Marshals worker-thread results from the services onto the UI thread.
    // Declared before the services so it outlives them (and is constructed
    // first, as they take a reference to it).
    GlibDispatcher                  uiDispatcher_;

    // Configuration model + service-result routing (toolkit-neutral).
    MainPresenter                   presenter_;

    // Toolkit-neutral service objects (owned by the shell; their callbacks are
    // forwarded to the presenter for routing).
    UdpListener                     listener_;
    RigController                   rig_;
    LotwClient                      lotw_;
    QrzClient                       qrz_;
    CwKeyer                         keyer_;
    DxCluster                       cluster_;

    Glib::RefPtr<Gio::SimpleAction> udpAction_;

    // DX cluster panel + layout. dxPanel_ is a value member (like notebook_) so
    // it survives being reparented between paned slots when the dock side
    // changes — a make_managed widget would be destroyed the moment
    // unset_*_child drops the paned's only reference.
    DxClusterPanel   dxPanel_;
    Gtk::Paned       paned_;                      // wraps notebook_ + dxPanel_
    Glib::RefPtr<Gio::SimpleAction> dxShowAction_;
    Glib::RefPtr<Gio::SimpleAction> dxDockAction_;

    // Loaded settings, used to apply the shared column layout to new pages.
    Glib::RefPtr<Glib::KeyFile> settings_;
};
