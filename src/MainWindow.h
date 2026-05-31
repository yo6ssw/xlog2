#pragma once

#include "Lotw.h"
#include "Qrz.h"
#include "Qso.h"
#include "Rig.h"
#include "Udp.h"

#include <gtkmm.h>

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

    // Loaded settings, used to apply the shared column layout to new pages.
    Glib::RefPtr<Glib::KeyFile> settings_;
};
