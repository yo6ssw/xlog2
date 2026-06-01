#pragma once

#include "CwKeyer.h"
#include "DxCluster.h"
#include "DxClusterPanel.h"
#include "Lotw.h"
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

// The application shell: a menu bar, a Gtk::Notebook of logbook tabs
// (LogPage), a status line, the UDP listener and the Hamlib rig controller.
// Per-logbook state lives in each LogPage; MainWindow routes menu actions,
// network QSOs and rig readings to the current page and persists settings.
class MainWindow : public Gtk::ApplicationWindow {
public:
    MainWindow();

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
    void onUdpReceived(const std::vector<Qso>& qsos, const std::string& source);

    // --- Hamlib rig control ---
    void onRigConnect();
    void onRigDisconnect();
    void onRigUpdate(double mhz, const std::string& mode);

    // --- LoTW ---
    void onLotwUpload();
    void onLotwDownload();
    void onLotwSettings();
    bool isLivePage(LogPage* page);

    // --- QRZ.com callsign lookup ---
    void onQrzLookup(LogPage* page, const std::string& callsign);
    void onQrzSettings();
    void showQrzResult(const QrzResult& result);  // popup with all returned fields

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

    void setStatus(const Glib::ustring& msg);
    void updateTitle();

    Gtk::Notebook                   notebook_;
    Gtk::Label                      statusLabel_;
    std::map<LogPage*, Gtk::Label*> tabLabels_;

    // UDP network logging
    UdpListener                     listener_;
    int                             udpPort_ = 2237;  // WSJT-X default
    bool                            udpEnabled_ = false;  // restored at startup
    Glib::RefPtr<Gio::SimpleAction> udpAction_;

    // Hamlib rig control
    RigController rig_;
    int           rigModel_  = 1;     // 1 == RIG_MODEL_DUMMY
    std::string   rigDevice_;
    int           rigPollMs_ = 500;

    // LoTW
    LotwClient    lotw_;
    std::string   lotwUser_, lotwPassword_, lotwStation_, lotwLastDownload_;
    std::string   tqslPath_ = "tqsl";
    LogPage*      pendingUploadPage_ = nullptr;   // page awaiting an upload result
    std::vector<long> pendingUploadIds_;

    // QRZ.com callsign lookup
    QrzClient     qrz_;
    std::string   qrzUser_, qrzPassword_;
    LogPage*      pendingLookupPage_ = nullptr;   // page awaiting a QRZ result

    // Network keyer (cwdaemon)
    CwKeyer       keyer_;
    std::string   keyerHost_ = "127.0.0.1";
    int           keyerPort_ = 6789;
    int           keyerSpeed_ = 0;                // 0 = leave cwdaemon's default
    std::array<std::string, 9> keyerMessages_{};

    // DX cluster
    DxCluster        cluster_;
    // A value member (like notebook_) so it survives being reparented between
    // paned slots when the dock side changes — a make_managed widget would be
    // destroyed the moment unset_*_child drops the paned's only reference.
    DxClusterPanel   dxPanel_;
    Gtk::Paned       paned_;                      // wraps notebook_ + dxPanel_
    std::string      dxHost_;
    int              dxPort_ = 7300;
    std::string      dxLogin_;
    std::string      dxDock_ = "bottom";          // top|bottom|left|right
    bool             dxVisible_ = false;
    bool             dxAutoConnect_ = false;
    Glib::RefPtr<Gio::SimpleAction> dxShowAction_;
    Glib::RefPtr<Gio::SimpleAction> dxDockAction_;

    // Loaded settings, used to apply the shared column layout to new pages.
    Glib::RefPtr<Glib::KeyFile> settings_;
};
