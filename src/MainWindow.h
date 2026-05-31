#pragma once

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
    void onAbout();

    // --- UDP network logging ---
    void onToggleUdp();
    void onUdpSettings();
    void onUdpReceived(const std::vector<Qso>& qsos, const std::string& source);

    // --- Hamlib rig control ---
    void onRigConnect();
    void onRigDisconnect();
    void onRigUpdate(double mhz, const std::string& mode);

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
    Glib::RefPtr<Gio::SimpleAction> udpAction_;

    // Hamlib rig control
    RigController rig_;
    int           rigModel_  = 1;     // 1 == RIG_MODEL_DUMMY
    std::string   rigDevice_;
    int           rigPollMs_ = 500;

    // Loaded settings, used to apply the shared column layout to new pages.
    Glib::RefPtr<Glib::KeyFile> settings_;
};
