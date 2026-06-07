#include "QtMainWindow.h"

#include "Adif.h"
#include "IniFile.h"
#include "LogPagePresenter.h"
#include "QtDxClusterPanel.h"
#include "QtRigPanel.h"
#include "QtLogPage.h"
#include "Statistics.h"
#include "StrUtil.h"
#include "TimeUtil.h"

#include <QAction>
#include <QActionGroup>
#include <QApplication>
#include <QCheckBox>
#include <QCloseEvent>
#include <QComboBox>
#include <QShowEvent>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDockWidget>
#include <QFileDialog>
#include <QFormLayout>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QMenuBar>
#include <QMessageBox>
#include <QShortcut>
#include <QSpinBox>
#include <QStatusBar>
#include <QTabWidget>
#include <QVBoxLayout>
#include <QWidget>

#include <sys/stat.h>

#include <QResizeEvent>

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>

namespace {

std::string envPath(const char* var, const std::string& fallbackSub) {
    if (const char* v = std::getenv(var); v && *v)
        return std::string(v) + "/xlog2";
    const char* home = std::getenv("HOME");
    return std::string(home ? home : ".") + "/" + fallbackSub + "/xlog2";
}

// The DX-cluster dock side persists as top|bottom|left|right (shared with the
// gtkmm "dock" preference).
Qt::DockWidgetArea dockAreaFromString(const std::string& s) {
    if (s == "top")   return Qt::TopDockWidgetArea;
    if (s == "left")  return Qt::LeftDockWidgetArea;
    if (s == "right") return Qt::RightDockWidgetArea;
    return Qt::BottomDockWidgetArea;
}

std::string stringFromDockArea(Qt::DockWidgetArea a) {
    switch (a) {
        case Qt::TopDockWidgetArea:   return "top";
        case Qt::LeftDockWidgetArea:  return "left";
        case Qt::RightDockWidgetArea: return "right";
        default:                      return "bottom";
    }
}

bool isHorizontalArea(Qt::DockWidgetArea a) {
    return a == Qt::LeftDockWidgetArea || a == Qt::RightDockWidgetArea;
}

}  // namespace

QtMainWindow::QtMainWindow()
    : presenter_(*this),
      listener_(uiDispatcher_),
      rig_(uiDispatcher_),
      lotw_(uiDispatcher_),
      qrz_(uiDispatcher_),
      cluster_(uiDispatcher_),
      audio_(uiDispatcher_),
      paddle_(uiDispatcher_),
      hidPaddle_(uiDispatcher_) {
    setWindowTitle("xlog2");
    resize(1024, 700);

    tabs_ = new QTabWidget;
    tabs_->setTabsClosable(true);
    // Let the central log area shrink below its content's natural width so the
    // DX-cluster dock can take a wide size (and be restored to one). Without an
    // explicit minimum, QMainWindow treats the page's min-size-hint (~950px) as
    // a hard floor and clamps the dock — unlike gtkmm's freely-shrinking paned.
    tabs_->setMinimumWidth(120);
    setCentralWidget(tabs_);
    connect(tabs_, &QTabWidget::tabCloseRequested, this, [this](int i) {
        if (auto* p = qobject_cast<QtLogPage*>(tabs_->widget(i))) {
            tabs_->removeTab(i);
            p->deleteLater();
        }
    });
    connect(tabs_, &QTabWidget::currentChanged, this, [this](int) { updateWindowTitle(); });

    status_ = new QLabel("Ready.");
    statusBar()->addWidget(status_, 1);
    // A permanent widget (right-aligned) so the live frame counter doesn't fight
    // with transient status messages.
    audioIndicator_ = new QLabel;
    statusBar()->addPermanentWidget(audioIndicator_);

    // DX cluster dock: a band-map panel (spots table on top, telnet console
    // below) matching the gtkmm DxClusterPanel.
    dxDock_  = new QDockWidget("DX cluster", this);
    dxDock_->setObjectName("dxClusterDock");
    dxPanel_ = new QtDxClusterPanel;
    dxDock_->setWidget(dxPanel_);
    addDockWidget(Qt::BottomDockWidgetArea, dxDock_);
    dxDock_->hide();
    connect(dxPanel_, &QtDxClusterPanel::activateSpot, this,
            [this](const QString& call, double mhz) {
                if (auto* log = currentLog()) {
                    log->applyDxSpot(call.toStdString(), mhz);
                    rig_.setFrequency(mhz);
                }
            });
    connect(dxPanel_, &QtDxClusterPanel::sendCommand, this,
            [this](const QString& cmd) { cluster_.sendCommand(cmd.toStdString()); });
    connect(dxPanel_, &QtDxClusterPanel::connectToggle, this,
            [this]() { onClusterConnectToggle(); });

    // Rig control dock: big frequency readout + tune/filter controls.
    rigDock_  = new QDockWidget("Rig", this);
    rigDock_->setObjectName("rigDock");
    rigPanel_ = new QtRigPanel;
    rigDock_->setWidget(rigPanel_);
    addDockWidget(Qt::RightDockWidgetArea, rigDock_);
    connect(rigPanel_, &QtRigPanel::stepFrequency, this,
            [this](double hz) { if (rig_.isRunning()) rig_.stepFrequency(hz); });
    connect(rigPanel_, &QtRigPanel::setFilter, this,
            [this](int n) { if (rig_.isRunning()) rig_.setFilter(n); });

    buildMenus();

    // F1..F9 keyer accelerators, registered once on the window (window-wide) so
    // they fire from anywhere — the log page or the DX-cluster dock — and route
    // to the active tab's presenter (whose form data feeds the CW expansion).
    // A single registration avoids the per-tab ambiguity that window-wide
    // shortcuts on each page would cause. onSendCwClicked() guards empty slots.
    for (int i = 0; i < 9; ++i) {
        auto* sc = new QShortcut(QKeySequence(Qt::Key_F1 + i), this);
        connect(sc, &QShortcut::activated, this, [this, i]() {
            if (auto* p = currentPage())
                p->presenter().onSendCwClicked(i);
        });
    }

    // Service-result routing through the presenter (toolkit-neutral).
    listener_.setCallback([this](const std::vector<Qso>& qsos, const std::string& src) {
        presenter_.routeUdp(qsos, src);
    });
    rig_.onUpdate = [this](double mhz, const std::string& mode) {
        presenter_.routeRigUpdate(mhz, mode);
        lastMhz_  = mhz;
        lastMode_ = mode;
    };
    // onFilter fires immediately after onUpdate each poll tick, so the cached
    // frequency/mode are current — render the whole panel state here.
    rig_.onFilter = [this](int pbwidthHz, int filter) {
        rigPanel_->setState(lastMhz_, lastMode_, pbwidthHz, filter);
    };
    rig_.onConnectResult = [this](bool ok, const std::string& err) {
        rigPanel_->setConnected(ok);
        setStatus(ok ? "Connected to rig (model " + std::to_string(cfg().rigModel) + ")."
                     : "Rig connect failed: " + err);
    };
    lotw_.onDownloadDone = [this](const std::string& adif, const std::string& err) {
        presenter_.routeLotwDownload(adif, err);
    };
    lotw_.onUploadDone = [this](bool ok, const std::string& msg) {
        presenter_.routeLotwUploadResult(ok, msg);
    };
    qrz_.onResult = [this](const QrzResult& r, const std::string& err) {
        presenter_.routeQrzResult(r, err);
    };
    cluster_.onLine = [this](const std::string& l) { dxPanel_->addLine(l); };
    cluster_.onStatus = [this](const std::string& s) {
        dxPanel_->addLine(s);
        dxPanel_->setConnected(cluster_.isConnected());
        setStatus(s);
    };
    cluster_.onSpot = [this](const DxSpot& s) { dxPanel_->addSpot(s); };
    audio_.onStatus = [this](const std::string& s) { setStatus(s); };
    audio_.onStats  = [this](unsigned long frames) {
        audioIndicator_->setText(QString("♪ %1 frames").arg(frames));
    };
    paddle_.onStatus = [this](const std::string& s) { setStatus(s); };
    // Mute the rig-audio stream while keying (semi-break-in) when configured to.
    paddle_.onTransmit = [this](bool tx) { audio_.setMuted(tx && cfg().paddleMuteAudio); };
    // USB paddle: drive the keyer's lock-free contact atomics straight from the
    // HID worker thread (no UI hop) for lowest latency; status goes via the UI.
    hidPaddle_.onDit    = [this](bool p) { paddle_.setDit(p); };
    hidPaddle_.onDah    = [this](bool p) { paddle_.setDah(p); };
    hidPaddle_.onStatus = [this](const std::string& s) { setStatus(s); };

    // App-wide key filter for the `[`/`]` paddle simulation (see eventFilter).
    qApp->installEventFilter(this);

    loadSettings();
    if (tabs_->count() == 0)
        openDefaultLog();
    updateWindowTitle();

    if (cfg().udpEnabled)
        startUdpListening();
    if (cfg().rigAutoConnect) {
        setStatus("Connecting to rig…");
        rig_.start(cfg().rigModel, cfg().rigDevice, cfg().rigPollMs);
    }
    if (cfg().audioEnabled)
        startAudioStream();
    if (cfg().paddleEnabled)
        startPaddleKeyer();
}

// --- IMainView ---------------------------------------------------------------

void QtMainWindow::setStatus(const std::string& msg) {
    status_->setText(QString::fromStdString(msg));
}

LogPagePresenter* QtMainWindow::currentLog() {
    auto* p = currentPage();
    return p ? &p->presenter() : nullptr;
}

bool QtMainWindow::isLogLive(LogPagePresenter* log) {
    for (int i = 0; i < tabs_->count(); ++i)
        if (auto* p = qobject_cast<QtLogPage*>(tabs_->widget(i)))
            if (&p->presenter() == log)
                return true;
    return false;
}

void QtMainWindow::showQrzResult(const QrzResult& result) {
    auto* dlg = new QDialog(this);
    dlg->setAttribute(Qt::WA_DeleteOnClose);
    dlg->setWindowTitle(QString("QRZ.com — %1").arg(QString::fromStdString(result.call)));
    auto* form = new QFormLayout(dlg);
    for (const auto& [k, v] : result.fields)
        form->addRow(QString::fromStdString(k),
                     new QLabel(QString::fromStdString(v)));
    if (result.fields.empty())
        form->addRow(new QLabel("(no fields returned)"));
    dlg->show();
}

// --- tabs --------------------------------------------------------------------

QtLogPage* QtMainWindow::currentPage() const {
    return qobject_cast<QtLogPage*>(tabs_->currentWidget());
}

QtLogPage* QtMainWindow::addPage(QtLogPage* page, const QString& label) {
    const int i = tabs_->addTab(page, label);
    tabs_->setCurrentIndex(i);
    registerPage(page);
    return page;
}

void QtMainWindow::registerPage(QtLogPage* page) {
    page->setCwMessages(cfg().keyerMessages);
    page->applyColumnLayout(loadedIni_);  // shared column order/width/visibility
    connect(page, &QtLogPage::changed, this, [this, page]() {
        updateTabTitle(page);
        updateWindowTitle();
    });
    connect(page, &QtLogPage::status, this, [this](const QString& s) { setStatus(s.toStdString()); });
    connect(page, &QtLogPage::lookupCall, this, [this, page](const QString& call) {
        if (cfg().qrzUser.empty() || cfg().qrzPassword.empty()) {
            setStatus("Set your QRZ.com username and password in QRZ ▸ Settings first.");
            return;
        }
        if (qrz_.isBusy()) { setStatus("A QRZ lookup is already in progress."); return; }
        presenter_.beginQrzLookup(&page->presenter());
        setStatus("Looking up " + call.toStdString() + " on QRZ.com…");
        qrz_.lookup(cfg().qrzUser, cfg().qrzPassword, call.toStdString());
    });
    connect(page, &QtLogPage::sendCw, this, [this](const QString& t) {
        if (!keyer_.isConfigured()) keyer_.setEndpoint(cfg().keyerHost, cfg().keyerPort);
        keyer_.send(t.toStdString());
    });
    connect(page, &QtLogPage::abortCw, this, [this]() { keyer_.abort(); });
}

void QtMainWindow::updateTabTitle(QtLogPage* page) {
    const int i = tabs_->indexOf(page);
    if (i >= 0)
        tabs_->setTabText(i, QString::fromStdString(page->title()));
}

void QtMainWindow::updateWindowTitle() {
    if (auto* p = currentPage())
        setWindowTitle(QString("xlog2 — %1  (%2 QSOs)")
                           .arg(QString::fromStdString(p->title()))
                           .arg(p->qsoCount()));
    else
        setWindowTitle("xlog2");
}

QtLogPage* QtMainWindow::openDefaultLog() {
    auto* page = new QtLogPage;
    const std::string path = defaultLogPath();
    std::error_code ec;
    std::filesystem::create_directories(std::filesystem::path(path).parent_path(), ec);
    page->openFile(path);
    return addPage(page, QString::fromStdString(page->title()));
}

// --- menus -------------------------------------------------------------------

void QtMainWindow::buildMenus() {
    // Mirrors the gtkmm menu model (labels, order and grouping).
    auto* file = menuBar()->addMenu("&File");
    file->addAction("New Tab", this, &QtMainWindow::onNewTab);
    file->addAction("Open…", this, &QtMainWindow::onOpen);
    file->addAction("Save As…", this, &QtMainWindow::onSaveAs);
    file->addAction("Close Tab", this, &QtMainWindow::onCloseTab);
    file->addSeparator();
    file->addAction("Import ADIF…", this, &QtMainWindow::onImportAdif);
    file->addAction("Import xlog log…", this, &QtMainWindow::onImportXlog);
    file->addAction("Export ADIF…", this, &QtMainWindow::onExportAdif);
    file->addSeparator();
    file->addAction("Quit", this, &QWidget::close);

    auto* log = menuBar()->addMenu("&Log");
    auto* find = log->addAction("Find…", this, &QtMainWindow::onFind);
    find->setShortcut(QKeySequence::Find);
    log->addAction("Fill DXCC entities", this, &QtMainWindow::onFillDxcc);
    log->addAction("Statistics…", this, &QtMainWindow::onStatistics);

    auto* net = menuBar()->addMenu("&Network");
    udpAction_ = net->addAction("Listen for QSOs (UDP)");
    udpAction_->setCheckable(true);
    connect(udpAction_, &QAction::toggled, this, &QtMainWindow::onToggleUdp);
    net->addAction("UDP port…", this, &QtMainWindow::onUdpSettings);

    auto* rig = menuBar()->addMenu("&Rig");
    rig->addAction("Connect…", this, &QtMainWindow::onRigConnect);
    rig->addAction("Disconnect", this, &QtMainWindow::onRigDisconnect);
    rig->addSeparator();
    auto* rigShow = rigDock_->toggleViewAction();
    rigShow->setText("Show panel");
    rig->addAction(rigShow);
    auto* rigDockMenu = rig->addMenu("Dock");
    rigDockGroup_ = new QActionGroup(this);  // exclusive radio
    const std::pair<const char*, const char*> rigSides[] = {
        {"Top", "top"}, {"Bottom", "bottom"}, {"Left", "left"}, {"Right", "right"}};
    for (const auto& [label, side] : rigSides) {
        auto* a = rigDockMenu->addAction(label);
        a->setCheckable(true);
        a->setData(QString::fromLatin1(side));
        rigDockGroup_->addAction(a);
        const std::string s = side;
        connect(a, &QAction::triggered, this, [this, s]() { onRigDock(s); });
    }

    auto* lotw = menuBar()->addMenu("Lo&TW");
    lotw->addAction("Upload to LoTW…", this, &QtMainWindow::onLotwUpload);
    lotw->addAction("Download confirmations", this, &QtMainWindow::onLotwDownload);
    lotw->addAction("Settings…", this, &QtMainWindow::onLotwSettings);

    auto* qrz = menuBar()->addMenu("&QRZ");
    qrz->addAction("Settings…", this, &QtMainWindow::onQrzSettings);

    auto* keyer = menuBar()->addMenu("&Keyer");
    keyer->addAction("Settings…", this, &QtMainWindow::onKeyerSettings);
    keyer->addSeparator();
    paddleAction_ = keyer->addAction("Remote paddle keying ([ / ])");
    paddleAction_->setCheckable(true);
    connect(paddleAction_, &QAction::toggled, this, &QtMainWindow::onTogglePaddle);
    keyer->addAction("Paddle settings…", this, &QtMainWindow::onPaddleSettings);

    auto* audio = menuBar()->addMenu("&Audio");
    audioAction_ = audio->addAction("Play rig audio stream");
    audioAction_->setCheckable(true);
    connect(audioAction_, &QAction::toggled, this, &QtMainWindow::onToggleAudio);
    audio->addAction("Settings…", this, &QtMainWindow::onAudioSettings);

    auto* cluster = menuBar()->addMenu("&Cluster");
    // "Show panel": Qt's built-in dock toggle action auto-syncs its checked
    // state with the dock's actual visibility.
    auto* showPanel = dxDock_->toggleViewAction();
    showPanel->setText("Show panel");
    cluster->addAction(showPanel);
    cluster->addAction("Connect / Disconnect", this, &QtMainWindow::onClusterConnectToggle);
    auto* dock = cluster->addMenu("Dock");
    dxDockGroup_ = new QActionGroup(this);  // exclusive radio
    const std::pair<const char*, const char*> sides[] = {
        {"Top", "top"}, {"Bottom", "bottom"}, {"Left", "left"}, {"Right", "right"}};
    for (const auto& [label, side] : sides) {
        auto* a = dock->addAction(label);
        a->setCheckable(true);
        a->setData(QString::fromLatin1(side));
        dxDockGroup_->addAction(a);
        const std::string s = side;
        connect(a, &QAction::triggered, this, [this, s]() {
            cfg().dxDock = s;
            addDockWidget(dockAreaFromString(s), dxDock_);  // move to the chosen side
            dxDock_->show();                                // picking a dock implies showing it
        });
    }
    cluster->addAction("Settings…", this, &QtMainWindow::onClusterSettings);

    menuBar()->addMenu("&Help")->addAction("About xlog2", this, &QtMainWindow::onAbout);
}

void QtMainWindow::onNewTab() {
    auto* page = new QtLogPage;
    page->newInMemory();
    addPage(page, "Untitled");
}

void QtMainWindow::onOpen() {
    const QString path = QFileDialog::getOpenFileName(this, "Open logbook", {},
                                                      "Logbooks (*.xlog);;All files (*)");
    if (path.isEmpty())
        return;
    auto* page = new QtLogPage;
    if (page->openFile(path.toStdString()))
        addPage(page, QString::fromStdString(page->title()));
    else {
        delete page;
        setStatus("Could not open " + path.toStdString());
    }
}

void QtMainWindow::onSaveAs() {
    auto* page = currentPage();
    if (!page)
        return;
    QString path = QFileDialog::getSaveFileName(this, "Save logbook as", {},
                                                "Logbooks (*.xlog)");
    if (path.isEmpty())
        return;
    if (!path.endsWith(".xlog"))
        path += ".xlog";
    if (page->saveAs(path.toStdString())) {
        updateTabTitle(page);
        updateWindowTitle();
        setStatus("Saved " + path.toStdString());
    }
}

void QtMainWindow::onCloseTab() {
    if (auto* p = currentPage()) {
        tabs_->removeTab(tabs_->indexOf(p));
        p->deleteLater();
    }
}

void QtMainWindow::onImportAdif() {
    auto* page = currentPage();
    if (!page)
        return;
    const QString path = QFileDialog::getOpenFileName(this, "Import ADIF", {},
                                                      "ADIF (*.adi *.adif);;All files (*)");
    if (path.isEmpty())
        return;
    std::ifstream in(path.toStdString());
    std::stringstream ss;
    ss << in.rdbuf();
    const int n = page->importAdif(ss.str());
    setStatus("Imported " + std::to_string(n) + " QSO(s).");
}

void QtMainWindow::onImportXlog() {
    auto* page = currentPage();
    if (!page)
        return;
    const QString path = QFileDialog::getOpenFileName(this, "Import xlog log", {},
                                                      "xlog logs (*.xlog);;All files (*)");
    if (path.isEmpty())
        return;
    std::ifstream in(path.toStdString());
    std::stringstream ss;
    ss << in.rdbuf();
    const int n = page->importXlog(ss.str());
    setStatus("Imported " + std::to_string(n) + " QSO(s) from xlog.");
}

void QtMainWindow::onExportAdif() {
    auto* page = currentPage();
    if (!page)
        return;
    QString path = QFileDialog::getSaveFileName(this, "Export ADIF", {}, "ADIF (*.adi)");
    if (path.isEmpty())
        return;
    if (!path.endsWith(".adi") && !path.endsWith(".adif"))
        path += ".adi";
    std::ofstream out(path.toStdString());
    out << page->exportAdif();
    setStatus("Exported to " + path.toStdString());
}

void QtMainWindow::onStatistics() {
    auto* page = currentPage();
    if (!page)
        return;
    const stats::Statistics st = stats::compute(page->logbook().qsos());
    std::ostringstream os;
    os << "Logbook:       " << page->title() << "\n";
    os << "Total QSOs:    " << st.total << "\n";
    os << "Unique calls:  " << st.uniqueCalls << "\n\nBy band\n";
    if (st.byBand.empty()) os << "  (none)\n";
    for (const auto& [b, n] : st.byBand) os << "  " << b << ":  " << n << "\n";
    os << "\nBy mode\n";
    if (st.byMode.empty()) os << "  (none)\n";
    for (const auto& [m, n] : st.byMode) os << "  " << m << ":  " << n << "\n";
    QMessageBox::information(this, "Statistics", QString::fromStdString(os.str()));
}

void QtMainWindow::onFind() {
    if (auto* p = currentPage())
        p->beginSearch();
}

void QtMainWindow::onFillDxcc() {
    if (auto* p = currentPage())
        p->backfillDxcc();
}

void QtMainWindow::onAbout() {
    QMessageBox::about(this, "About xlog2",
                       "xlog2 — a GTK4/Qt amateur-radio logger.\n"
                       "Qt Widgets backend.");
}

// --- UDP ---------------------------------------------------------------------

void QtMainWindow::onToggleUdp(bool on) {
    if (on)
        startUdpListening();
    else {
        listener_.stop();
        cfg().udpEnabled = false;
        setStatus("Stopped UDP listener.");
    }
}

void QtMainWindow::startUdpListening() {
    std::string error;
    cfg().udpEnabled = listener_.start(cfg().udpPort, error);
    if (udpAction_) {
        QSignalBlocker block(udpAction_);
        udpAction_->setChecked(cfg().udpEnabled);
    }
    setStatus(cfg().udpEnabled
                  ? "Listening for QSOs on UDP port " + std::to_string(cfg().udpPort) + "."
                  : "Could not start UDP listener: " + error);
}

void QtMainWindow::onUdpSettings() {
    QDialog dlg(this);
    dlg.setWindowTitle("UDP settings");
    auto* form = new QFormLayout(&dlg);
    auto* port = new QSpinBox;
    port->setRange(1, 65535);
    port->setValue(cfg().udpPort);
    form->addRow("Listen port:", port);
    auto* bb = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
    form->addRow(bb);
    QObject::connect(bb, &QDialogButtonBox::accepted, &dlg, &QDialog::accept);
    QObject::connect(bb, &QDialogButtonBox::rejected, &dlg, &QDialog::reject);
    if (dlg.exec() == QDialog::Accepted) {
        cfg().udpPort = port->value();
        if (listener_.isListening()) { listener_.stop(); startUdpListening(); }
        else setStatus("UDP port set to " + std::to_string(cfg().udpPort) + ".");
    }
}

// --- rig ---------------------------------------------------------------------

void QtMainWindow::onRigConnect() {
    QDialog dlg(this);
    dlg.setWindowTitle("Connect to rig");
    auto* form = new QFormLayout(&dlg);
    auto* model = new QSpinBox; model->setRange(1, 99999); model->setValue(cfg().rigModel);
    auto* device = new QLineEdit(QString::fromStdString(cfg().rigDevice));
    auto* poll = new QSpinBox; poll->setRange(50, 60000); poll->setValue(cfg().rigPollMs);
    auto* autoc = new QCheckBox("Connect at startup"); autoc->setChecked(cfg().rigAutoConnect);
    form->addRow("Hamlib model id:", model);
    form->addRow("Device / host:", device);
    form->addRow("Poll interval (ms):", poll);
    form->addRow(autoc);
    auto* bb = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
    form->addRow(bb);
    QObject::connect(bb, &QDialogButtonBox::accepted, &dlg, &QDialog::accept);
    QObject::connect(bb, &QDialogButtonBox::rejected, &dlg, &QDialog::reject);
    if (dlg.exec() == QDialog::Accepted) {
        cfg().rigModel = model->value();
        cfg().rigDevice = device->text().toStdString();
        cfg().rigPollMs = poll->value();
        cfg().rigAutoConnect = autoc->isChecked();
        setStatus("Connecting to rig…");
        rig_.start(cfg().rigModel, cfg().rigDevice, cfg().rigPollMs);
    }
}

void QtMainWindow::onRigDisconnect() {
    if (rig_.isRunning()) {
        rig_.stop();
        rigPanel_->setConnected(false);
        setStatus("Disconnected from rig.");
    } else {
        setStatus("No rig connected.");
    }
}

void QtMainWindow::onRigDock(const std::string& side) {
    cfg().rigDock = side;
    addDockWidget(dockAreaFromString(side), rigDock_);  // move to the chosen side
    rigDock_->show();                                   // picking a dock implies showing it
}

// --- LoTW --------------------------------------------------------------------

void QtMainWindow::onLotwUpload() {
    auto* page = currentPage();
    if (!page)
        return;
    const auto unsent = page->logbook().qsosNotLotwSent();
    if (unsent.empty()) { setStatus("No new QSOs to upload to LoTW."); return; }
    const std::string tmp = std::filesystem::temp_directory_path() / "xlog2-lotw-upload.adi";
    { std::ofstream(tmp) << adif::write(unsent); }
    std::vector<long> ids;
    for (const auto& q : unsent) ids.push_back(q.id);
    presenter_.beginLotwUpload(&page->presenter(), std::move(ids));
    setStatus("Signing and uploading " + std::to_string(unsent.size()) + " QSO(s) via tqsl…");
    lotw_.uploadAdifFile(cfg().tqslPath, cfg().lotwStation, tmp);
}

void QtMainWindow::onLotwDownload() {
    if (cfg().lotwUser.empty() || cfg().lotwPassword.empty()) {
        setStatus("Set your LoTW username and password in LoTW ▸ Settings first.");
        return;
    }
    if (lotw_.isBusy()) { setStatus("A LoTW download is already in progress."); return; }
    setStatus("Downloading LoTW confirmations…");
    lotw_.downloadConfirmations(cfg().lotwUser, cfg().lotwPassword, cfg().lotwLastDownload);
}

void QtMainWindow::onLotwSettings() {
    QDialog dlg(this);
    dlg.setWindowTitle("LoTW settings");
    auto* form = new QFormLayout(&dlg);
    auto* user = new QLineEdit(QString::fromStdString(cfg().lotwUser));
    auto* pass = new QLineEdit(QString::fromStdString(cfg().lotwPassword));
    pass->setEchoMode(QLineEdit::Password);
    auto* station = new QLineEdit(QString::fromStdString(cfg().lotwStation));
    auto* tqsl = new QLineEdit(QString::fromStdString(cfg().tqslPath));
    form->addRow("LoTW username:", user);
    form->addRow("LoTW password:", pass);
    form->addRow("Station location:", station);
    form->addRow("tqsl path:", tqsl);
    auto* bb = new QDialogButtonBox(QDialogButtonBox::Save | QDialogButtonBox::Cancel);
    form->addRow(bb);
    QObject::connect(bb, &QDialogButtonBox::accepted, &dlg, &QDialog::accept);
    QObject::connect(bb, &QDialogButtonBox::rejected, &dlg, &QDialog::reject);
    if (dlg.exec() == QDialog::Accepted) {
        cfg().lotwUser = user->text().toStdString();
        cfg().lotwPassword = pass->text().toStdString();
        cfg().lotwStation = station->text().toStdString();
        cfg().tqslPath = tqsl->text().isEmpty() ? "tqsl" : tqsl->text().toStdString();
        setStatus("LoTW settings saved.");
    }
}

// --- QRZ ---------------------------------------------------------------------

void QtMainWindow::onQrzSettings() {
    QDialog dlg(this);
    dlg.setWindowTitle("QRZ.com settings");
    auto* form = new QFormLayout(&dlg);
    auto* user = new QLineEdit(QString::fromStdString(cfg().qrzUser));
    auto* pass = new QLineEdit(QString::fromStdString(cfg().qrzPassword));
    pass->setEchoMode(QLineEdit::Password);
    form->addRow("QRZ.com username:", user);
    form->addRow("QRZ.com password:", pass);
    auto* bb = new QDialogButtonBox(QDialogButtonBox::Save | QDialogButtonBox::Cancel);
    form->addRow(bb);
    QObject::connect(bb, &QDialogButtonBox::accepted, &dlg, &QDialog::accept);
    QObject::connect(bb, &QDialogButtonBox::rejected, &dlg, &QDialog::reject);
    if (dlg.exec() == QDialog::Accepted) {
        cfg().qrzUser = user->text().toStdString();
        cfg().qrzPassword = pass->text().toStdString();
        setStatus("QRZ settings saved.");
    }
}

// --- keyer -------------------------------------------------------------------

void QtMainWindow::applyKeyerConfig() {
    keyer_.setEndpoint(cfg().keyerHost, cfg().keyerPort);
    if (cfg().keyerSpeed > 0)
        keyer_.setSpeed(cfg().keyerSpeed);
    for (int i = 0; i < tabs_->count(); ++i)
        if (auto* p = qobject_cast<QtLogPage*>(tabs_->widget(i)))
            p->setCwMessages(cfg().keyerMessages);
}

void QtMainWindow::onKeyerSettings() {
    QDialog dlg(this);
    dlg.setWindowTitle("Keyer settings");
    auto* form = new QFormLayout(&dlg);
    auto* host = new QLineEdit(QString::fromStdString(cfg().keyerHost));
    auto* port = new QSpinBox; port->setRange(1, 65535); port->setValue(cfg().keyerPort);
    auto* speed = new QSpinBox; speed->setRange(0, 60); speed->setValue(cfg().keyerSpeed);
    form->addRow("cwdaemon host:", host);
    form->addRow("Port:", port);
    form->addRow("Speed (wpm, 0=default):", speed);
    std::array<QLineEdit*, 9> msgs{};
    for (int i = 0; i < 9; ++i) {
        msgs[i] = new QLineEdit(QString::fromStdString(cfg().keyerMessages[i]));
        form->addRow(QString("F%1 message:").arg(i + 1), msgs[i]);
    }
    auto* bb = new QDialogButtonBox(QDialogButtonBox::Save | QDialogButtonBox::Cancel);
    form->addRow(bb);
    QObject::connect(bb, &QDialogButtonBox::accepted, &dlg, &QDialog::accept);
    QObject::connect(bb, &QDialogButtonBox::rejected, &dlg, &QDialog::reject);
    if (dlg.exec() == QDialog::Accepted) {
        cfg().keyerHost = host->text().toStdString();
        cfg().keyerPort = port->value();
        cfg().keyerSpeed = speed->value();
        for (int i = 0; i < 9; ++i)
            cfg().keyerMessages[i] = msgs[i]->text().toStdString();
        applyKeyerConfig();
        setStatus("Keyer settings saved.");
    }
}

// --- rig audio stream (cwsd) -------------------------------------------------

void QtMainWindow::onToggleAudio(bool on) {
    if (on)
        startAudioStream();
    else {
        audio_.stop();
        cfg().audioEnabled = false;
        audioIndicator_->clear();
    }
}

void QtMainWindow::startAudioStream() {
    AudioStreamConfig ac;
    ac.host       = cfg().audioHost;
    ac.port       = cfg().audioPort;
    ac.sampleRate = cfg().audioSampleRate;
    ac.channels   = cfg().audioChannels;
    ac.device     = cfg().audioDevice;
    audio_.start(ac);
    cfg().audioEnabled = true;  // user intent, persisted; survives teardown
    if (audioAction_) {
        QSignalBlocker block(audioAction_);
        audioAction_->setChecked(true);
    }
}

void QtMainWindow::onAudioSettings() {
    QDialog dlg(this);
    dlg.setWindowTitle("Rig audio stream (cwsd)");
    auto* form = new QFormLayout(&dlg);
    auto* host = new QLineEdit(QString::fromStdString(cfg().audioHost));
    auto* port = new QSpinBox; port->setRange(1, 65535); port->setValue(cfg().audioPort);
    auto* rate = new QComboBox;
    for (int r : {8000, 12000, 16000, 24000, 48000})
        rate->addItem(QString::number(r), r);
    rate->setCurrentIndex(std::max(0, rate->findData(cfg().audioSampleRate)));
    auto* chan = new QSpinBox; chan->setRange(1, 2); chan->setValue(cfg().audioChannels);
    auto* device = new QLineEdit(QString::fromStdString(cfg().audioDevice));
    device->setPlaceholderText("ALSA playback device, e.g. default");
    form->addRow("Host:", host);
    form->addRow("Port:", port);
    form->addRow("Sample rate:", rate);
    form->addRow("Channels:", chan);
    form->addRow("Playback device:", device);
    auto* hint = new QLabel(
        "Sample rate and channels must match the cwsd `audio` section.\n"
        "cwsd's default port is 7355.");
    form->addRow(hint);
    auto* bb = new QDialogButtonBox(QDialogButtonBox::Save | QDialogButtonBox::Cancel);
    form->addRow(bb);
    QObject::connect(bb, &QDialogButtonBox::accepted, &dlg, &QDialog::accept);
    QObject::connect(bb, &QDialogButtonBox::rejected, &dlg, &QDialog::reject);
    if (dlg.exec() == QDialog::Accepted) {
        cfg().audioHost = host->text().toStdString();
        if (cfg().audioHost.empty()) cfg().audioHost = "127.0.0.1";
        cfg().audioPort = port->value();
        cfg().audioSampleRate = rate->currentData().toInt();
        cfg().audioChannels = chan->value();
        cfg().audioDevice = device->text().toStdString();
        if (cfg().audioDevice.empty()) cfg().audioDevice = "default";
        if (audio_.isStreaming())
            startAudioStream();  // restart on the new settings
        else
            setStatus("Rig audio stream settings saved.");
    }
}

// --- remote paddle keyer (cwsd remote_key) -----------------------------------

void QtMainWindow::onTogglePaddle(bool on) {
    if (on)
        startPaddleKeyer();
    else {
        hidPaddle_.stop();
        paddle_.stop();
        cfg().paddleEnabled = false;
    }
}

void QtMainWindow::startPaddleKeyer() {
    RemotePaddleConfig pc;
    pc.host     = cfg().paddleHost;
    pc.port     = cfg().paddlePort;
    pc.wpm      = cfg().paddleWpm;
    pc.iambicB  = cfg().paddleIambicB;
    pc.autospace = cfg().paddleAutospace;
    pc.sidetone = cfg().paddleSidetone;
    pc.toneHz   = cfg().paddleToneHz;
    pc.level    = cfg().paddleLevel;
    pc.device   = cfg().paddleSidetoneDevice;
    paddle_.start(pc);
    hidPaddle_.start();          // also accept a USB HID paddle, if present
    cfg().paddleEnabled = true;  // user intent, persisted; survives teardown
    if (paddleAction_) {
        QSignalBlocker block(paddleAction_);
        paddleAction_->setChecked(true);
    }
}

void QtMainWindow::onPaddleSettings() {
    QDialog dlg(this);
    dlg.setWindowTitle("Remote paddle keyer (cwsd)");
    auto* form = new QFormLayout(&dlg);
    auto* host = new QLineEdit(QString::fromStdString(cfg().paddleHost));
    auto* port = new QSpinBox; port->setRange(1, 65535); port->setValue(cfg().paddlePort);
    auto* wpm  = new QSpinBox; wpm->setRange(1, 99); wpm->setValue(cfg().paddleWpm);
    auto* iambicB = new QCheckBox("Iambic B (default: iambic A)");
    iambicB->setChecked(cfg().paddleIambicB);
    auto* autospace = new QCheckBox("Autospace (enforce inter-character spacing)");
    autospace->setChecked(cfg().paddleAutospace);
    auto* sidetone = new QCheckBox("Local sidetone");
    sidetone->setChecked(cfg().paddleSidetone);
    auto* tone = new QSpinBox; tone->setRange(100, 2000); tone->setSuffix(" Hz");
    tone->setValue(cfg().paddleToneHz);
    auto* level = new QSpinBox; level->setRange(0, 100); level->setValue(cfg().paddleLevel);
    auto* muteAudio = new QCheckBox("Mute rig audio while keying");
    muteAudio->setChecked(cfg().paddleMuteAudio);
    form->addRow("Host:", host);
    form->addRow("Port:", port);
    form->addRow("Speed (wpm):", wpm);
    form->addRow(iambicB);
    form->addRow(autospace);
    form->addRow(sidetone);
    form->addRow("Tone:", tone);
    form->addRow("Volume (0–100):", level);
    form->addRow(muteAudio);
    auto* hint = new QLabel(
        "Streams timestamped key edges to cwsd's `remote_key` service, with an\n"
        "instant local sidetone for feel. Test with the [ (dit) and ] (dah)\n"
        "keys while active. cwsd's default remote_key port is 6790.");
    form->addRow(hint);
    auto* bb = new QDialogButtonBox(QDialogButtonBox::Save | QDialogButtonBox::Cancel);
    form->addRow(bb);
    QObject::connect(bb, &QDialogButtonBox::accepted, &dlg, &QDialog::accept);
    QObject::connect(bb, &QDialogButtonBox::rejected, &dlg, &QDialog::reject);
    if (dlg.exec() == QDialog::Accepted) {
        cfg().paddleHost = host->text().toStdString();
        if (cfg().paddleHost.empty()) cfg().paddleHost = "127.0.0.1";
        cfg().paddlePort = port->value();
        cfg().paddleWpm = wpm->value();
        cfg().paddleIambicB = iambicB->isChecked();
        cfg().paddleAutospace = autospace->isChecked();
        cfg().paddleSidetone = sidetone->isChecked();
        cfg().paddleToneHz = tone->value();
        cfg().paddleLevel = level->value();
        cfg().paddleMuteAudio = muteAudio->isChecked();
        if (paddle_.isActive())
            startPaddleKeyer();  // restart on the new settings
        else
            setStatus("Remote paddle keyer settings saved.");
    }
}

bool QtMainWindow::eventFilter(QObject* obj, QEvent* ev) {
    if (paddle_.isActive() &&
        (ev->type() == QEvent::KeyPress || ev->type() == QEvent::KeyRelease)) {
        auto* ke = static_cast<QKeyEvent*>(ev);
        if (!ke->isAutoRepeat() &&
            (ke->key() == Qt::Key_BracketLeft || ke->key() == Qt::Key_BracketRight)) {
            const bool down = ev->type() == QEvent::KeyPress;
            if (ke->key() == Qt::Key_BracketLeft)
                paddle_.setDit(down);
            else
                paddle_.setDah(down);
            return true;  // consume so the bracket isn't typed while keying
        }
    }
    return QMainWindow::eventFilter(obj, ev);
}

// --- DX cluster --------------------------------------------------------------

void QtMainWindow::onClusterConnectToggle() {
    if (cluster_.isConnected()) {
        cluster_.disconnect();
        return;
    }
    if (cfg().dxHost.empty()) { setStatus("Set a DX cluster host in its settings first."); return; }
    dxDock_->show();
    cluster_.connectTo(cfg().dxHost, cfg().dxPort, cfg().dxLogin);
}

void QtMainWindow::onClusterSettings() {
    QDialog dlg(this);
    dlg.setWindowTitle("DX cluster settings");
    auto* form = new QFormLayout(&dlg);
    auto* host = new QLineEdit(QString::fromStdString(cfg().dxHost));
    auto* port = new QSpinBox; port->setRange(1, 65535); port->setValue(cfg().dxPort);
    auto* login = new QLineEdit(QString::fromStdString(cfg().dxLogin));
    form->addRow("Host:", host);
    form->addRow("Port:", port);
    form->addRow("Login callsign:", login);
    auto* bb = new QDialogButtonBox(QDialogButtonBox::Save | QDialogButtonBox::Cancel);
    form->addRow(bb);
    QObject::connect(bb, &QDialogButtonBox::accepted, &dlg, &QDialog::accept);
    QObject::connect(bb, &QDialogButtonBox::rejected, &dlg, &QDialog::reject);
    if (dlg.exec() == QDialog::Accepted) {
        cfg().dxHost = host->text().toStdString();
        cfg().dxPort = port->value();
        cfg().dxLogin = login->text().toStdString();
        setStatus("DX cluster settings saved.");
    }
}

// --- settings ----------------------------------------------------------------

std::string QtMainWindow::defaultLogPath() const {
    return envPath("XDG_DATA_HOME", ".local/share") + "/default.xlog";
}

std::string QtMainWindow::layoutFilePath() const {
    return envPath("XDG_CONFIG_HOME", ".config") + "/layout.ini";
}

void QtMainWindow::loadSettings() {
    IniFile ini;
    const bool loaded = ini.loadFromFile(layoutFilePath());
    presenter_.settings = Settings::load(ini);
    loadedIni_ = ini;  // kept so new tabs get the shared column layout

    if (loaded && ini.hasGroup("window")) {
        const int w = ini.getInt("window", "width", 1024);
        const int h = ini.getInt("window", "height", 700);
        if (w > 0 && h > 0)
            resize(w, h);
        if (ini.getBool("window", "maximized", false))
            showMaximized();
    }

    if (loaded && ini.hasKey("session", "open")) {
        for (const auto& path : strutil::splitSemicolons(ini.getString("session", "open"))) {
            auto* page = new QtLogPage;
            if (page->openFile(path))
                addPage(page, QString::fromStdString(page->title()));
            else
                delete page;
        }
        // Restore the previously-active tab.
        const std::string active = ini.getString("session", "active");
        if (!active.empty())
            for (int i = 0; i < tabs_->count(); ++i)
                if (auto* p = qobject_cast<QtLogPage*>(tabs_->widget(i));
                    p && p->path() == active) {
                    tabs_->setCurrentIndex(i);
                    break;
                }
    }

    applyKeyerConfig();

    // Honour the persisted DX-cluster dock placement + visibility (shared keys,
    // also used by the gtkmm backend). The exact size is applied in showEvent():
    // resizeDocks() only sticks once the window has its laid-out size, so doing
    // it here (pre-show) is ignored.
    const Qt::DockWidgetArea area = dockAreaFromString(cfg().dxDock);
    addDockWidget(area, dxDock_);
    dxDock_->setVisible(cfg().dxVisible);
    if (cfg().dxVisible && cfg().dxPanelPos > 0) {
        pendingDockSize_   = cfg().dxPanelPos;
        pendingDockOrient_ = isHorizontalArea(area) ? Qt::Horizontal : Qt::Vertical;
    }
    // Reflect the loaded dock side in the Cluster ▸ Dock radio (settings are
    // loaded after buildMenus, which defaulted them to "bottom").
    if (dxDockGroup_)
        for (QAction* a : dxDockGroup_->actions())
            if (a->data().toString().toStdString() == cfg().dxDock) {
                a->setChecked(true);
                break;
            }

    if (cfg().dxAutoConnect && !cfg().dxHost.empty()) {
        dxDock_->show();
        cluster_.connectTo(cfg().dxHost, cfg().dxPort, cfg().dxLogin);
    }

    // Rig-control dock placement + visibility (shared keys with the gtkmm shell).
    const Qt::DockWidgetArea rigArea = dockAreaFromString(cfg().rigDock);
    addDockWidget(rigArea, rigDock_);
    rigDock_->setVisible(cfg().rigVisible);
    if (cfg().rigVisible && cfg().rigPanelPos > 0) {
        pendingRigDockSize_   = cfg().rigPanelPos;
        pendingRigDockOrient_ = isHorizontalArea(rigArea) ? Qt::Horizontal : Qt::Vertical;
    }
    if (rigDockGroup_)
        for (QAction* a : rigDockGroup_->actions())
            if (a->data().toString().toStdString() == cfg().rigDock) {
                a->setChecked(true);
                break;
            }
}

void QtMainWindow::saveSettings() {
    IniFile ini;
    ini.loadFromFile(layoutFilePath());

    // Capture the live DX-cluster dock state so Settings::store persists it.
    const Qt::DockWidgetArea area = dockWidgetArea(dxDock_);
    if (area != Qt::NoDockWidgetArea)
        cfg().dxDock = stringFromDockArea(area);
    cfg().dxVisible = dxDock_->isVisible();
    if (cfg().dxVisible) {
        const int sz = isHorizontalArea(area) ? dxDock_->width() : dxDock_->height();
        if (sz > 0)
            cfg().dxPanelPos = sz;
    }

    // Same for the rig dock.
    const Qt::DockWidgetArea rigArea = dockWidgetArea(rigDock_);
    if (rigArea != Qt::NoDockWidgetArea)
        cfg().rigDock = stringFromDockArea(rigArea);
    cfg().rigVisible = rigDock_->isVisible();
    if (cfg().rigVisible) {
        const int sz = isHorizontalArea(rigArea) ? rigDock_->width() : rigDock_->height();
        if (sz > 0)
            cfg().rigPanelPos = sz;
    }
    cfg().store(ini);
    if (auto* p = currentPage())
        p->storeColumnLayout(ini);  // shared column order/width/visibility

    if (!isMaximized()) {
        ini.setInt("window", "width", width());
        ini.setInt("window", "height", height());
    }
    ini.setBool("window", "maximized", isMaximized());

    std::string open, active;
    for (int i = 0; i < tabs_->count(); ++i) {
        if (auto* p = qobject_cast<QtLogPage*>(tabs_->widget(i)); p && p->isFileBacked()) {
            if (!open.empty()) open += ';';
            open += p->path();
        }
    }
    if (auto* cur = currentPage(); cur && cur->isFileBacked())
        active = cur->path();
    ini.setString("session", "open", open);
    ini.setString("session", "active", active);

    std::error_code ec;
    std::filesystem::create_directories(
        std::filesystem::path(layoutFilePath()).parent_path(), ec);
    std::ofstream(layoutFilePath()) << ini.toString();
    ::chmod(layoutFilePath().c_str(), S_IRUSR | S_IWUSR);  // plaintext LoTW password
}

void QtMainWindow::closeEvent(QCloseEvent* e) {
    saveSettings();
    QMainWindow::closeEvent(e);
}

void QtMainWindow::showEvent(QShowEvent* e) {
    QMainWindow::showEvent(e);
    // Apply the persisted DX-cluster dock size once, now that the window has its
    // real laid-out geometry (resizeDocks is a no-op before the window is shown).
    restoreDockSize();
}

void QtMainWindow::resizeEvent(QResizeEvent* e) {
    QMainWindow::resizeEvent(e);
    restoreDockSize();
}

void QtMainWindow::restoreDockSize() {
    // Apply the persisted DX-cluster dock size exactly once, as soon as the
    // window is visible with the pending size set. We hook both showEvent and
    // resizeEvent because with a maximized window the final (maximized) geometry
    // arrives via a late resizeEvent — after loadSettings has set the pending
    // size — whereas showEvent fires earlier (during showMaximized(), before the
    // dock params are known). resizeDocks only sticks once the window is laid
    // out, so neither can run pre-show.
    if (dockSizeRestored_ || !isVisible() ||
        (pendingDockSize_ <= 0 && pendingRigDockSize_ <= 0))
        return;
    dockSizeRestored_ = true;
    if (pendingDockSize_ > 0)
        resizeDocks({dxDock_}, {pendingDockSize_}, pendingDockOrient_);
    if (pendingRigDockSize_ > 0)
        resizeDocks({rigDock_}, {pendingRigDockSize_}, pendingRigDockOrient_);
}
