#pragma once

#include "CwKeyer.h"
#include "DxCluster.h"
#include "IMainView.h"
#include "Lotw.h"
#include "MainPresenter.h"
#include "Qrz.h"
#include "QtDispatcher.h"
#include "Rig.h"
#include "Udp.h"

#include <QMainWindow>

#include <string>

class QTabWidget;
class QLabel;
class QPlainTextEdit;
class QListWidget;
class QtLogPage;
class LogPagePresenter;

// Qt Widgets application shell (the view). Mirrors the gtkmm MainWindow: a menu
// bar, a QTabWidget of QtLogPage tabs, a status bar, and the toolkit-neutral
// service objects. Configuration + service-result routing live in MainPresenter
// (driven via IMainView); dialogs/menus/tabs are this class's concern.
class QtMainWindow : public QMainWindow, public IMainView {
    Q_OBJECT
public:
    QtMainWindow();

    // --- IMainView ---
    void setStatus(const std::string& msg) override;
    LogPagePresenter* currentLog() override;
    bool isLogLive(LogPagePresenter* log) override;
    void showQrzResult(const QrzResult& result) override;

private:
    void buildMenus();
    QtLogPage* addPage(QtLogPage* page, const QString& label);
    QtLogPage* currentPage() const;
    QtLogPage* openDefaultLog();
    void registerPage(QtLogPage* page);
    void updateTabTitle(QtLogPage* page);
    void updateWindowTitle();

    // Menu actions.
    void onNewTab();
    void onOpen();
    void onSaveAs();
    void onCloseTab();
    void onImportAdif();
    void onExportAdif();
    void onStatistics();
    void onFind();
    void onFillDxcc();
    void onAbout();

    void onToggleUdp(bool on);
    void startUdpListening();
    void onUdpSettings();
    void onRigConnect();
    void onRigDisconnect();
    void onLotwUpload();
    void onLotwDownload();
    void onLotwSettings();
    void onQrzSettings();
    void onKeyerSettings();
    void applyKeyerConfig();
    void onClusterConnectToggle();
    void onClusterSettings();

    Settings& cfg() { return presenter_.settings; }

    std::string defaultLogPath() const;
    std::string layoutFilePath() const;
    void loadSettings();
    void saveSettings();
    void closeEvent(QCloseEvent* e) override;

    QtDispatcher    uiDispatcher_;
    MainPresenter   presenter_;
    UdpListener     listener_;
    RigController   rig_;
    LotwClient      lotw_;
    QrzClient       qrz_;
    CwKeyer         keyer_;
    DxCluster       cluster_;

    // The settings loaded at startup, kept so the shared column layout can be
    // applied to newly-created tabs (mirrors the gtkmm shell).
    IniFile         loadedIni_;

    QTabWidget*     tabs_   = nullptr;
    QLabel*         status_ = nullptr;
    QPlainTextEdit* dxConsole_ = nullptr;
    QListWidget*    dxSpots_   = nullptr;
    QAction*        udpAction_ = nullptr;
};
