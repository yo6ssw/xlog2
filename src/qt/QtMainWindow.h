#pragma once

#include "Audio.h"
#include "CwKeyer.h"
#include "CwSkimmer.h"
#include "DxCluster.h"
#include "IMainView.h"
#include "LogbookSync.h"
#include "Lotw.h"
#include "MainPresenter.h"
#include "Qrz.h"
#include "QrzPeer.h"
#include "QtDispatcher.h"
#include "RemotePaddleKeyer.h"
#include "HidPaddleInput.h"
#include "Rig.h"
#include "SyncCoordinator.h"
#include "Udp.h"

#include <QMainWindow>

#include <string>

class QTabWidget;
class QLabel;
class QDockWidget;
class QActionGroup;
class QDialog;
class QWidget;
class QtDxClusterPanel;
class QtRigPanel;
class QtCwSkimmerPanel;
class QtMapPanel;
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
    bool startQrzLookup(const std::string& callsign) override;

private:
    void buildMenus();
    QtLogPage* addPage(QtLogPage* page, const QString& label);
    QtLogPage* currentPage() const;
    QtLogPage* openDefaultLog();
    void registerPage(QtLogPage* page);
    // Move a QSO from one logbook tab to another (row context menu "Move to").
    void moveQso(QtLogPage* from, long id, LogPagePresenter* target);
    void updateTabTitle(QtLogPage* page);
    void updateWindowTitle();

    // Menu actions.
    void onNewTab();
    void onOpen();
    void onSaveAs();
    void onCloseTab();
    void onImportAdif();
    void onImportXlog();
    void onExportAdif();
    void onStatistics();
    void onFind();
    void onFillDxcc();
    void onFillLocators();  // bulk-fill missing QSO locators via QRZ (+ cache)
    void onAbout();

    // The consolidated Edit ▸ Settings dialog and the routine that applies its
    // result: copies the config-field subset into cfg() and re-applies it to any
    // running service (leaving runtime/view state untouched).
    void onEditSettings();
    void applySettings(const Settings& edited);

    void onToggleUdp(bool on);
    void startUdpListening();
    void onRigConnect();  // connect using the stored rig settings (no dialog)
    void onRigDisconnect();
    void onRigDock(const std::string& side);  // move the rig dock to a side
    void onLotwUpload();
    void onLotwDownload();
    void applyKeyerConfig();
    void onToggleAudio(bool on);
    void startAudioStream();
    void onTogglePaddle(bool on);
    void startPaddleKeyer();
    void onClusterConnectToggle();
    void onSyncNow();          // force an anti-entropy pass with all peers
    void startSync();          // (re)start the mesh transport
    void attachSyncedLog(QtLogPage* page);  // bind the coordinator to the default log
    void updateSyncIndicator();             // status-bar peer count
    void applySyncTrust();                  // push the settings allowlist into the coordinator
    void onManageTrustedPeers();            // open the live trusted-peers dialog
    void refreshTrustedPeersDialog();       // repopulate the open dialog from a snapshot
    void onSkimmerDock(const std::string& side);  // move the skimmer dock to a side
    void onMapDock(const std::string& side);      // move the map dock to a side
    void startSkimmer();
    void stopSkimmer();

    Settings& cfg() { return presenter_.settings; }

    std::string defaultLogPath() const;
    std::string layoutFilePath() const;
    void loadSettings();
    void saveSettings();
    void closeEvent(QCloseEvent* e) override;
    void showEvent(QShowEvent* e) override;
    void resizeEvent(QResizeEvent* e) override;
    // App-wide filter so the `[`/`]` paddle keys work regardless of focus while
    // the remote paddle keyer is active (and pass through normally otherwise).
    bool eventFilter(QObject* obj, QEvent* ev) override;
    void restoreDockSize();  // apply the saved DX-cluster dock size once, post-show

    QtDispatcher    uiDispatcher_;
    MainPresenter   presenter_;
    UdpListener     listener_;
    RigController   rig_;
    LotwClient      lotw_;
    QrzClient       qrz_;
    CwKeyer         keyer_;
    DxCluster       cluster_;
    // skimmer_ is declared before audio_ so it outlives it: the audio worker's
    // onPcm tap calls skimmer_.pushPcm, and members destruct in reverse order.
    CwSkimmer         skimmer_;
    AudioStreamClient audio_;
    RemotePaddleKeyer paddle_;
    HidPaddleInput    hidPaddle_;
    LogbookSync       sync_;
    SyncCoordinator   coordinator_;  // declared after sync_ (holds a reference to it)
    QrzPeer           qrzPeer_;      // after sync_ + qrz_ (holds references to both)

    // The settings loaded at startup, kept so the shared column layout can be
    // applied to newly-created tabs (mirrors the gtkmm shell).
    IniFile         loadedIni_;

    QtLogPage*        syncedPage_ = nullptr;  // the default log the coordinator drives

    QTabWidget*       tabs_    = nullptr;
    QLabel*           status_  = nullptr;
    QLabel*           audioIndicator_ = nullptr;  // live audio-frame counter
    QLabel*           syncIndicator_  = nullptr;  // peer connection state
    // Live trusted-peers dialog (null when closed); peersList_ is repopulated on
    // every onPeersChanged.
    QDialog*          trustedPeersDialog_ = nullptr;
    QWidget*          trustedPeersList_   = nullptr;
    QDockWidget*      dxDock_  = nullptr;
    QtDxClusterPanel* dxPanel_ = nullptr;
    QDockWidget*      rigDock_  = nullptr;
    QtRigPanel*       rigPanel_ = nullptr;
    QDockWidget*      skimmerDock_  = nullptr;
    QtCwSkimmerPanel* skimmerPanel_ = nullptr;
    QDockWidget*      mapDock_  = nullptr;
    QtMapPanel*       mapPanel_ = nullptr;
    QAction*          udpAction_ = nullptr;
    QAction*          audioAction_ = nullptr;
    QAction*          paddleAction_ = nullptr;
    QActionGroup*     dxDockGroup_ = nullptr;   // Cluster ▸ Dock radio (top/bottom/left/right)
    QActionGroup*     rigDockGroup_ = nullptr;  // Rig ▸ Dock radio
    QActionGroup*     skimmerDockGroup_ = nullptr;  // Skimmer ▸ Dock radio
    QActionGroup*     mapDockGroup_ = nullptr;       // Map ▸ Dock radio

    // Latest frequency/mode from rig_.onUpdate, rendered together with the
    // passband/filter that arrives in the paired rig_.onFilter call.
    double            lastMhz_ = 0.0;
    std::string       lastMode_;

    // Persisted dock sizes, applied once on first show (resizeDocks only takes
    // effect after the window has its laid-out geometry).
    int               pendingDockSize_   = 0;   // 0 = nothing to restore
    Qt::Orientation   pendingDockOrient_ = Qt::Vertical;
    int               pendingRigDockSize_   = 0;
    Qt::Orientation   pendingRigDockOrient_ = Qt::Horizontal;
    int               pendingSkimmerDockSize_   = 0;
    Qt::Orientation   pendingSkimmerDockOrient_ = Qt::Horizontal;
    int               pendingMapDockSize_   = 0;
    Qt::Orientation   pendingMapDockOrient_ = Qt::Horizontal;
    bool              dockSizeRestored_  = false;
};
