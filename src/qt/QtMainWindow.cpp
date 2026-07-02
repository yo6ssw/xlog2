// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2025 Adrian Scripcă (YO6SSW)

#include "QtMainWindow.h"

#include <sys/stat.h>

#include <QAction>
#include <QActionGroup>
#include <QApplication>
#include <QCheckBox>
#include <QClipboard>
#include <QCloseEvent>
#include <QComboBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDockWidget>
#include <QFileDialog>
#include <QFormLayout>
#include <QGuiApplication>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QMenuBar>
#include <QMessageBox>
#include <QPushButton>
#include <QResizeEvent>
#include <QScrollArea>
#include <QShortcut>
#include <QShowEvent>
#include <QSpinBox>
#include <QStatusBar>
#include <QTabWidget>
#include <QTimer>
#include <QVBoxLayout>
#include <QWidget>
#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>

#include "Adif.h"
#include "IniFile.h"
#include "LogPagePresenter.h"
#include "QtCwSkimmerPanel.h"
#include "QtDxClusterPanel.h"
#include "QtLogPage.h"
#include "QtMapPanel.h"
#include "QtRigPanel.h"
#include "QtSettingsDialog.h"
#include "Statistics.h"
#include "StrUtil.h"
#include "TimeUtil.h"
#include "Version.h"

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
  if (s == "top") return Qt::TopDockWidgetArea;
  if (s == "left") return Qt::LeftDockWidgetArea;
  if (s == "right") return Qt::RightDockWidgetArea;
  return Qt::BottomDockWidgetArea;
}

std::string stringFromDockArea(Qt::DockWidgetArea a) {
  switch (a) {
    case Qt::TopDockWidgetArea:
      return "top";
    case Qt::LeftDockWidgetArea:
      return "left";
    case Qt::RightDockWidgetArea:
      return "right";
    default:
      return "bottom";
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
      skimmer_(uiDispatcher_),
      audio_(uiDispatcher_),
      paddle_(uiDispatcher_),
      hidPaddle_(uiDispatcher_),
      sync_(uiDispatcher_),
      coordinator_(sync_),
      qrzPeer_(sync_, qrz_) {
  setWindowTitle(QString("xlog2 %1").arg(xlog::kVersion));
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
  connect(tabs_, &QTabWidget::currentChanged, this, [this](int) {
    updateWindowTitle();
    if (auto* p = currentPage())  // move the map "to" to the newly-active tab
      mapPanel_->setTo(p->presenter().currentLocator());
  });

  status_ = new QLabel("Ready.");
  statusBar()->addWidget(status_, 1);
  // A permanent widget (right-aligned) so the live frame counter doesn't fight
  // with transient status messages.
  audioIndicator_ = new QLabel;
  statusBar()->addPermanentWidget(audioIndicator_);
  syncIndicator_ = new QLabel;
  statusBar()->addPermanentWidget(syncIndicator_);

  // DX cluster dock: a band-map panel (spots table on top, telnet console
  // below) matching the gtkmm DxClusterPanel.
  dxDock_ = new QDockWidget("DX cluster", this);
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
              // Prefill name/QTH/locator: cache-first, fetch+cache on miss.
              // Silent — no popup per double-click.
              if (!call.isEmpty() && !qrz_.isBusy()) {
                presenter_.beginQrzLookup(log, /*silent=*/true);
                qrz_.lookup(cfg().qrzUser, cfg().qrzPassword,
                            call.toStdString());
              }
            }
          });
  connect(
      dxPanel_, &QtDxClusterPanel::sendCommand, this,
      [this](const QString& cmd) { cluster_.sendCommand(cmd.toStdString()); });
  connect(dxPanel_, &QtDxClusterPanel::connectToggle, this,
          [this]() { onClusterConnectToggle(); });

  // Rig control dock: big frequency readout + tune/filter controls.
  rigDock_ = new QDockWidget("Rig", this);
  rigDock_->setObjectName("rigDock");
  rigPanel_ = new QtRigPanel;
  rigDock_->setWidget(rigPanel_);
  addDockWidget(Qt::RightDockWidgetArea, rigDock_);
  connect(rigPanel_, &QtRigPanel::stepFrequency, this, [this](double hz) {
    if (rig_.isRunning()) rig_.stepFrequency(hz);
  });
  connect(rigPanel_, &QtRigPanel::setFilter, this, [this](int n) {
    if (rig_.isRunning()) rig_.setFilter(n);
  });
  connect(rigPanel_, &QtRigPanel::setPower, this, [this](bool on) {
    if (rig_.isRunning()) rig_.setPower(on);
  });
  connect(rigPanel_, &QtRigPanel::setAgc, this, [this](bool on) {
    if (rig_.isRunning()) rig_.setAgc(on);
  });
  connect(rigPanel_, &QtRigPanel::setMode, this, [this](const QString& mode) {
    if (rig_.isRunning()) rig_.setMode(mode.toStdString());
  });

  // CW Skimmer dock: waterfall of the rig-audio passband + a decode table.
  skimmerDock_ = new QDockWidget("CW Skimmer", this);
  skimmerDock_->setObjectName("skimmerDock");
  skimmerPanel_ = new QtCwSkimmerPanel;
  skimmerDock_->setWidget(skimmerPanel_);
  addDockWidget(Qt::LeftDockWidgetArea, skimmerDock_);
  skimmerDock_->hide();
  connect(skimmerPanel_, &QtCwSkimmerPanel::gateChanged, this, [this](int db) {
    cfg().skimmerGate = db;
    skimmer_.setGate(static_cast<float>(db));
  });
  connect(skimmerPanel_, &QtCwSkimmerPanel::minSnrChanged, this,
          [this](int db) {
            cfg().skimmerMinSnr = db;
            skimmer_.setMinSnr(static_cast<float>(db));
          });
  connect(skimmerPanel_, &QtCwSkimmerPanel::knownOnlyChanged, this,
          [this](bool on) {
            cfg().skimmerKnownOnly = on;
            skimmer_.setKnownCallsOnly(on);
          });
  mapDock_ = new QDockWidget("World map", this);
  mapDock_->setObjectName("mapDock");
  mapPanel_ = new QtMapPanel;
  mapDock_->setWidget(mapPanel_);
  addDockWidget(Qt::RightDockWidgetArea, mapDock_);
  mapDock_->hide();

  // Master-callsign list (Super Check Partial), used to validate/correct
  // decoded callsigns. Optional: drop a MASTER.SCP at
  // $XDG_DATA_HOME/xlog2/master.scp.
  {
    const std::size_t n = skimmer_.loadCallsignDb(
        envPath("XDG_DATA_HOME", ".local/share") + "/master.scp");
    skimmerPanel_->setCallDbInfo(n > 0, n);
  }

  buildMenus();

  // F1..F9 keyer accelerators, registered once on the window (window-wide) so
  // they fire from anywhere — the log page or the DX-cluster dock — and route
  // to the active tab's presenter (whose form data feeds the CW expansion).
  // A single registration avoids the per-tab ambiguity that window-wide
  // shortcuts on each page would cause. onSendCwClicked() guards empty slots.
  for (int i = 0; i < 9; ++i) {
    auto* sc = new QShortcut(QKeySequence(Qt::Key_F1 + i), this);
    connect(sc, &QShortcut::activated, this, [this, i]() {
      if (auto* p = currentPage()) p->presenter().onSendCwClicked(i);
    });
  }

  // Service-result routing through the presenter (toolkit-neutral).
  listener_.setCallback(
      [this](const std::vector<Qso>& qsos, const std::string& src) {
        presenter_.routeUdp(qsos, src);
      });
  rig_.onUpdate = [this](double mhz, const std::string& mode) {
    presenter_.routeRigUpdate(mhz, mode);
    lastMhz_ = mhz;
    lastMode_ = mode;
  };
  // onFilter fires immediately after onUpdate each poll tick, so the cached
  // frequency/mode are current — render the whole panel state here.
  rig_.onFilter = [this](int pbwidthHz, int filter) {
    rigPanel_->setState(lastMhz_, lastMode_, pbwidthHz, filter);
    // Feed the live passband to the skimmer so its waterfall can normalize the
    // brightness rise that narrowing the filter otherwise causes.
    skimmer_.setFilterBandwidthHz(pbwidthHz);
  };
  rig_.onPower = [this](bool supported, bool on) {
    rigPanel_->setPowerState(supported, on);
  };
  rig_.onConnectResult = [this](bool ok, const std::string& err) {
    rigPanel_->setConnected(ok);
    setStatus(ok ? "Connected to rig (model " + std::to_string(cfg().rigModel) +
                       ")."
                 : "Rig connect failed: " + err);
  };
  lotw_.onDownloadDone = [this](const std::string& adif,
                                const std::string& err) {
    presenter_.routeLotwDownload(adif, err);
  };
  lotw_.onUploadDone = [this](bool ok, const std::string& msg) {
    presenter_.routeLotwUploadResult(ok, msg);
  };
  qrz_.onResult = [this](const QrzResult& r, const std::string& err) {
    presenter_.routeQrzResult(r, err);
  };
  qrz_.onFillProgress = [this](int done, int total) {
    setStatus("QRZ locator fill: " + std::to_string(done) + "/" +
              std::to_string(total) + "…");
  };
  qrz_.onFillResult =
      [this](const std::vector<std::pair<std::string, std::string>>& r,
             int fromCache, int fetched, const std::string& err) {
        presenter_.routeQrzLocatorFill(r, fromCache, fetched, err);
      };
  cluster_.onLine = [this](const std::string& l) { dxPanel_->addLine(l); };
  cluster_.onStatus = [this](const std::string& s) {
    dxPanel_->addLine(s);
    dxPanel_->setConnected(cluster_.isConnected());
    setStatus(s);
  };
  cluster_.onSpot = [this](const DxSpot& s) { dxPanel_->addSpot(s); };
  audio_.onStatus = [this](const std::string& s) { setStatus(s); };
  audio_.onStats = [this](unsigned long frames) {
    audioIndicator_->setText(QString("♪ %1 frames").arg(frames));
  };
  // Tap the decoded rig audio into the skimmer (called on the audio worker
  // thread; pushPcm is cheap + thread-safe and a no-op when the skimmer is
  // off).
  audio_.onPcm = [this](const int16_t* s, int frames, int ch, int rate) {
    skimmer_.pushPcm(s, frames, ch, rate);
  };
  skimmer_.onWaterfall = [this](const std::vector<float>& mags, double lo,
                                double hi) {
    skimmerPanel_->addWaterfall(mags, lo, hi);
  };
  skimmer_.onChannel = [this](int id, double hz, int wpm,
                              const std::string& text,
                              const std::string& call) {
    skimmerPanel_->updateChannel(id, hz, wpm, text, call);
  };
  skimmer_.onChannelRemoved = [this](int id) {
    skimmerPanel_->removeChannel(id);
  };
  paddle_.onStatus = [this](const std::string& s) { setStatus(s); };
  // Mute the rig-audio stream while keying (semi-break-in) when configured to.
  paddle_.onTransmit = [this](bool tx) {
    audio_.setMuted(tx && cfg().paddleMuteAudio);
  };
  // USB paddle: drive the keyer's lock-free contact atomics straight from the
  // HID worker thread (no UI hop) for lowest latency; status goes via the UI.
  hidPaddle_.onDit = [this](bool p) { paddle_.setDit(p); };
  hidPaddle_.onDah = [this](bool p) { paddle_.setDah(p); };
  hidPaddle_.onStatus = [this](const std::string& s) { setStatus(s); };

  // Logbook sync: the mesh transport moves bytes; the coordinator owns the
  // protocol and is the only thing that touches the synced logbook (UI thread).
  sync_.onPeerUp = [this](const LogbookSync::PeerKey& p) {
    coordinator_.onPeerUp(p);
    updateSyncIndicator();
  };
  sync_.onPeerDown = [this](const LogbookSync::PeerKey& p) {
    coordinator_.onPeerDown(p);
    updateSyncIndicator();
  };
  sync_.onMessage = [this](const LogbookSync::PeerKey& p,
                           const syncproto::Message& m) {
    // QRZ peer-cache messages ride the same mesh; route them to QrzPeer — but
    // only for peers we trust, so an untrusted node can't mine our QRZ cache.
    if (m.type == syncproto::Type::QrzQuery ||
        m.type == syncproto::Type::QrzResponse) {
      if (coordinator_.isTrusted(p)) qrzPeer_.onMessage(p, m);
    } else {
      coordinator_.onMessage(p, m);
    }
  };
  sync_.onStatus = [this](const std::string& s) { setStatus(s); };
  coordinator_.onStatus = [this](const std::string& s) { setStatus(s); };
  coordinator_.onPeersChanged = [this] {
    updateSyncIndicator();
    if (trustedPeersDialog_) refreshTrustedPeersDialog();
  };

  // Distributed QRZ cache: consult mesh peers between the local cache and
  // qrz.com. The timer bounds how long we wait for a peer to answer.
  qrzPeer_.scheduleOnce = [](int ms, std::function<void()> fn) {
    QTimer::singleShot(ms, [fn = std::move(fn)]() { fn(); });
  };
  qrzPeer_.onStatus = [this](const std::string& s) { setStatus(s); };
  qrz_.setPeerResolver(
      [this](const std::string& call,
             std::function<void(std::optional<QrzResult>)> reply) {
        qrzPeer_.query(call, std::move(reply));
      });

  // App-wide key filter for the `[`/`]` paddle simulation (see eventFilter).
  qApp->installEventFilter(this);

  loadSettings();
  // Point the QRZ result cache at a file under the data dir (created if
  // needed).
  {
    const std::string dir = envPath("XDG_DATA_HOME", ".local/share");
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    qrz_.setCache(dir + "/qrz-cache.sqlite", cfg().qrzCacheDays);
  }
  if (tabs_->count() == 0) openDefaultLog();
  updateWindowTitle();
  // The first/default tab is the synced logbook.
  if (auto* p = qobject_cast<QtLogPage*>(tabs_->widget(0))) attachSyncedLog(p);

  if (cfg().udpEnabled) startUdpListening();
  if (cfg().rigAutoConnect) {
    setStatus("Connecting to rig…");
    rig_.start(cfg().rigModel, cfg().rigDevice, cfg().rigPollMs);
  }
  if (cfg().audioEnabled) startAudioStream();
  if (cfg().paddleEnabled) startPaddleKeyer();
  if (cfg().syncEnabled) startSync();
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
      if (&p->presenter() == log) return true;
  return false;
}

void QtMainWindow::showQrzResult(const QrzResult& result) {
  auto* dlg = new QDialog(this);
  dlg->setAttribute(Qt::WA_DeleteOnClose);
  dlg->setWindowTitle(
      QString("QRZ.com — %1").arg(QString::fromStdString(result.call)));
  auto* form = new QFormLayout(dlg);
  for (const auto& [k, v] : result.fields)
    form->addRow(QString::fromStdString(k),
                 new QLabel(QString::fromStdString(v)));
  if (result.fields.empty()) form->addRow(new QLabel("(no fields returned)"));
  dlg->show();
}

bool QtMainWindow::startQrzLookup(const std::string& callsign) {
  if (cfg().qrzUser.empty() || cfg().qrzPassword.empty() || qrz_.isBusy())
    return false;
  return qrz_.lookup(cfg().qrzUser, cfg().qrzPassword, callsign);
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
  connect(page, &QtLogPage::status, this,
          [this](const QString& s) { setStatus(s.toStdString()); });
  connect(page, &QtLogPage::lookupCall, this,
          [this, page](const QString& call) {
            if (cfg().qrzUser.empty() || cfg().qrzPassword.empty()) {
              setStatus(
                  "Set your QRZ.com username and password in QRZ ▸ Settings "
                  "first.");
              return;
            }
            if (qrz_.isBusy()) {
              setStatus("A QRZ lookup is already in progress.");
              return;
            }
            presenter_.beginQrzLookup(&page->presenter());
            setStatus("Looking up " + call.toStdString() + " on QRZ.com…");
            qrz_.lookup(cfg().qrzUser, cfg().qrzPassword, call.toStdString());
          });
  connect(page, &QtLogPage::sendCw, this, [this](const QString& t) {
    if (!keyer_.isConfigured())
      keyer_.setEndpoint(cfg().keyerHost, cfg().keyerPort);
    keyer_.send(t.toStdString());
  });
  connect(page, &QtLogPage::abortCw, this, [this]() { keyer_.abort(); });
  connect(page, &QtLogPage::locatorChanged, this,
          [this, page](const QString& g) {
            if (page == currentPage())  // only the visible tab drives the map
              mapPanel_->setTo(g.toStdString());
          });

  // Row context menu "Move to": list every other open logbook, and perform
  // the move (add to the target, remove from this page) on request.
  page->queryMoveTargets = [this, page]() {
    std::vector<std::pair<std::string, LogPagePresenter*>> out;
    for (int i = 0; i < tabs_->count(); ++i)
      if (auto* p = qobject_cast<QtLogPage*>(tabs_->widget(i)); p && p != page)
        out.emplace_back(p->title(), &p->presenter());
    return out;
  };
  page->requestMove = [this, page](long id, LogPagePresenter* target) {
    moveQso(page, id, target);
  };
}

void QtMainWindow::moveQso(QtLogPage* from, long id, LogPagePresenter* target) {
  if (!from || !target) return;
  const Qso* q = from->presenter().findQso(id);
  if (!q) return;
  Qso copy = *q;
  copy.id = 0;  // the target assigns a fresh row id on insert
  const std::string call = copy.call;
  target->addExternalQso(copy);     // add + refresh + tab-title update
  from->presenter().deleteQso(id);  // remove from the source
  setStatus("Moved QSO with " + call + " to " + target->title() + ".");
}

void QtMainWindow::updateTabTitle(QtLogPage* page) {
  const int i = tabs_->indexOf(page);
  if (i >= 0) tabs_->setTabText(i, QString::fromStdString(page->title()));
}

void QtMainWindow::updateWindowTitle() {
  if (auto* p = currentPage())
    setWindowTitle(QString("xlog2 %1 — %2  (%3 QSOs)")
                       .arg(xlog::kVersion)
                       .arg(QString::fromStdString(p->title()))
                       .arg(p->qsoCount()));
  else
    setWindowTitle(QString("xlog2 %1").arg(xlog::kVersion));
}

QtLogPage* QtMainWindow::openDefaultLog() {
  auto* page = new QtLogPage;
  const std::string path = defaultLogPath();
  std::error_code ec;
  std::filesystem::create_directories(std::filesystem::path(path).parent_path(),
                                      ec);
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

  auto* edit = menuBar()->addMenu("&Edit");
  auto* prefs =
      edit->addAction("Settings…", this, &QtMainWindow::onEditSettings);
  prefs->setShortcut(QKeySequence::Preferences);  // Ctrl+,

  auto* log = menuBar()->addMenu("&Log");
  auto* find = log->addAction("Find…", this, &QtMainWindow::onFind);
  find->setShortcut(QKeySequence::Find);
  log->addAction("Fill DXCC entities", this, &QtMainWindow::onFillDxcc);
  log->addAction("Fill missing locators (QRZ)", this,
                 &QtMainWindow::onFillLocators);
  log->addAction("Statistics…", this, &QtMainWindow::onStatistics);

  auto* net = menuBar()->addMenu("&Network");
  udpAction_ = net->addAction("Listen for QSOs (UDP)");
  udpAction_->setCheckable(true);
  connect(udpAction_, &QAction::toggled, this, &QtMainWindow::onToggleUdp);

  auto* rig = menuBar()->addMenu("&Rig");
  rig->addAction("Connect", this, &QtMainWindow::onRigConnect);
  rig->addAction("Disconnect", this, &QtMainWindow::onRigDisconnect);
  rig->addSeparator();
  auto* rigShow = rigDock_->toggleViewAction();
  rigShow->setText("Show panel");
  rig->addAction(rigShow);
  auto* rigDockMenu = rig->addMenu("Dock");
  rigDockGroup_ = new QActionGroup(this);  // exclusive radio
  const std::pair<const char*, const char*> rigSides[] = {{"Top", "top"},
                                                          {"Bottom", "bottom"},
                                                          {"Left", "left"},
                                                          {"Right", "right"}};
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
  lotw->addAction("Download confirmations", this,
                  &QtMainWindow::onLotwDownload);

  auto* keyer = menuBar()->addMenu("&Keyer");
  paddleAction_ = keyer->addAction("Remote paddle keying ([ / ])");
  paddleAction_->setCheckable(true);
  connect(paddleAction_, &QAction::toggled, this,
          &QtMainWindow::onTogglePaddle);

  auto* audio = menuBar()->addMenu("&Audio");
  audioAction_ = audio->addAction("Play rig audio stream");
  audioAction_->setCheckable(true);
  connect(audioAction_, &QAction::toggled, this, &QtMainWindow::onToggleAudio);

  auto* skimmer = menuBar()->addMenu("S&kimmer");
  auto* skShow = skimmerDock_->toggleViewAction();
  skShow->setText("Show panel");
  skimmer->addAction(skShow);
  auto* skDockMenu = skimmer->addMenu("Dock");
  skimmerDockGroup_ = new QActionGroup(this);
  const std::pair<const char*, const char*> skSides[] = {{"Top", "top"},
                                                         {"Bottom", "bottom"},
                                                         {"Left", "left"},
                                                         {"Right", "right"}};
  for (const auto& [label, side] : skSides) {
    auto* a = skDockMenu->addAction(label);
    a->setCheckable(true);
    a->setData(QString::fromLatin1(side));
    skimmerDockGroup_->addAction(a);
    const std::string s = side;
    connect(a, &QAction::triggered, this, [this, s]() { onSkimmerDock(s); });
  }
  // Start/stop the DSP with the panel's visibility (PCM only flows while the
  // rig-audio stream is also playing).
  connect(skimmerDock_, &QDockWidget::visibilityChanged, this,
          [this](bool vis) {
            cfg().skimmerVisible = vis;
            if (vis)
              startSkimmer();
            else
              stopSkimmer();
          });

  auto* cluster = menuBar()->addMenu("&Cluster");
  // "Show panel": Qt's built-in dock toggle action auto-syncs its checked
  // state with the dock's actual visibility.
  auto* showPanel = dxDock_->toggleViewAction();
  showPanel->setText("Show panel");
  cluster->addAction(showPanel);
  cluster->addAction("Connect / Disconnect", this,
                     &QtMainWindow::onClusterConnectToggle);
  auto* dock = cluster->addMenu("Dock");
  dxDockGroup_ = new QActionGroup(this);  // exclusive radio
  const std::pair<const char*, const char*> sides[] = {{"Top", "top"},
                                                       {"Bottom", "bottom"},
                                                       {"Left", "left"},
                                                       {"Right", "right"}};
  for (const auto& [label, side] : sides) {
    auto* a = dock->addAction(label);
    a->setCheckable(true);
    a->setData(QString::fromLatin1(side));
    dxDockGroup_->addAction(a);
    const std::string s = side;
    connect(a, &QAction::triggered, this, [this, s]() {
      cfg().dxDock = s;
      addDockWidget(dockAreaFromString(s), dxDock_);  // move to the chosen side
      dxDock_->show();  // picking a dock implies showing it
    });
  }

  auto* map = menuBar()->addMenu("&Map");
  auto* mapShow = mapDock_->toggleViewAction();
  mapShow->setText("Show panel");
  map->addAction(mapShow);
  auto* mapDockMenu = map->addMenu("Dock");
  mapDockGroup_ = new QActionGroup(this);
  const std::pair<const char*, const char*> mapSides[] = {{"Top", "top"},
                                                          {"Bottom", "bottom"},
                                                          {"Left", "left"},
                                                          {"Right", "right"}};
  for (const auto& [label, side] : mapSides) {
    auto* a = mapDockMenu->addAction(label);
    a->setCheckable(true);
    a->setData(QString::fromLatin1(side));
    mapDockGroup_->addAction(a);
    const std::string s = side;
    connect(a, &QAction::triggered, this, [this, s]() { onMapDock(s); });
  }

  auto* sync = menuBar()->addMenu("S&ync");
  sync->addAction("Sync now", this, &QtMainWindow::onSyncNow);
  auto* syncEnable = sync->addAction("Enabled");
  syncEnable->setCheckable(true);
  syncEnable->setChecked(cfg().syncEnabled);
  connect(syncEnable, &QAction::toggled, this, [this](bool on) {
    cfg().syncEnabled = on;
    startSync();
  });
  sync->addAction("Trusted peers…", this, &QtMainWindow::onManageTrustedPeers);

  menuBar()->addMenu("&Help")->addAction("About xlog2", this,
                                         &QtMainWindow::onAbout);
}

void QtMainWindow::onMapDock(const std::string& side) {
  cfg().mapDock = side;
  addDockWidget(dockAreaFromString(side), mapDock_);  // move to the chosen side
  mapDock_->show();  // picking a dock implies showing it
}

void QtMainWindow::onNewTab() {
  auto* page = new QtLogPage;
  page->newInMemory();
  addPage(page, "Untitled");
}

void QtMainWindow::onOpen() {
  const QString path = QFileDialog::getOpenFileName(
      this, "Open logbook", {}, "Logbooks (*.xlog);;All files (*)");
  if (path.isEmpty()) return;
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
  if (!page) return;
  QString path = QFileDialog::getSaveFileName(this, "Save logbook as", {},
                                              "Logbooks (*.xlog)");
  if (path.isEmpty()) return;
  if (!path.endsWith(".xlog")) path += ".xlog";
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
  if (!page) return;
  const QString path = QFileDialog::getOpenFileName(
      this, "Import ADIF", {}, "ADIF (*.adi *.adif);;All files (*)");
  if (path.isEmpty()) return;
  std::ifstream in(path.toStdString());
  std::stringstream ss;
  ss << in.rdbuf();
  const int n = page->importAdif(ss.str());
  setStatus("Imported " + std::to_string(n) + " QSO(s).");
}

void QtMainWindow::onImportXlog() {
  auto* page = currentPage();
  if (!page) return;
  const QString path = QFileDialog::getOpenFileName(
      this, "Import xlog log", {}, "xlog logs (*.xlog);;All files (*)");
  if (path.isEmpty()) return;
  std::ifstream in(path.toStdString());
  std::stringstream ss;
  ss << in.rdbuf();
  const int n = page->importXlog(ss.str());
  setStatus("Imported " + std::to_string(n) + " QSO(s) from xlog.");
}

void QtMainWindow::onExportAdif() {
  auto* page = currentPage();
  if (!page) return;
  QString path =
      QFileDialog::getSaveFileName(this, "Export ADIF", {}, "ADIF (*.adi)");
  if (path.isEmpty()) return;
  if (!path.endsWith(".adi") && !path.endsWith(".adif")) path += ".adi";
  std::ofstream out(path.toStdString());
  out << page->exportAdif();
  setStatus("Exported to " + path.toStdString());
}

void QtMainWindow::onStatistics() {
  auto* page = currentPage();
  if (!page) return;
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
  QMessageBox::information(this, "Statistics",
                           QString::fromStdString(os.str()));
}

void QtMainWindow::onFind() {
  if (auto* p = currentPage()) p->beginSearch();
}

void QtMainWindow::onFillDxcc() {
  if (auto* p = currentPage()) p->backfillDxcc();
}

void QtMainWindow::onFillLocators() {
  auto* page = currentPage();
  if (!page) return;
  if (cfg().qrzUser.empty() || cfg().qrzPassword.empty()) {
    setStatus(
        "Set your QRZ.com username and password in Edit ▸ Settings ▸ QRZ "
        "first.");
    return;
  }
  if (qrz_.isBusy()) {
    setStatus("A QRZ operation is already in progress.");
    return;
  }
  const std::vector<std::string> calls =
      page->presenter().callsignsMissingLocator();
  if (calls.empty()) {
    setStatus("No QSOs are missing a locator.");
    return;
  }
  presenter_.beginQrzLocatorFill(&page->presenter());
  setStatus("Filling locators for " + std::to_string(calls.size()) +
            " callsign(s) via QRZ…");
  qrz_.fillLocators(cfg().qrzUser, cfg().qrzPassword, calls);
}

void QtMainWindow::onAbout() {
  QMessageBox::about(this, "About xlog2",
                     QString("xlog2 %1\n\n"
                             "A GTK4/Qt amateur-radio logger.\n"
                             "Qt Widgets backend.")
                         .arg(xlog::kVersion));
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
  setStatus(cfg().udpEnabled ? "Listening for QSOs on UDP port " +
                                   std::to_string(cfg().udpPort) + "."
                             : "Could not start UDP listener: " + error);
}

// --- rig ---------------------------------------------------------------------

void QtMainWindow::onRigConnect() {
  // Rig parameters are configured in Edit ▸ Settings; this just connects.
  setStatus("Connecting to rig…");
  rig_.start(cfg().rigModel, cfg().rigDevice, cfg().rigPollMs);
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
  rigDock_->show();  // picking a dock implies showing it
}

// --- CW skimmer --------------------------------------------------------------

void QtMainWindow::onSkimmerDock(const std::string& side) {
  cfg().skimmerDock = side;
  addDockWidget(dockAreaFromString(side), skimmerDock_);
  skimmerDock_->show();
}

void QtMainWindow::startSkimmer() {
  SkimmerConfig sc;
  sc.sampleRate = cfg().audioSampleRate;
  sc.channels = cfg().audioChannels;
  skimmer_.start(sc);
}

void QtMainWindow::stopSkimmer() {
  skimmer_.stop();
  skimmerPanel_->clear();
}

// --- LoTW --------------------------------------------------------------------

void QtMainWindow::onLotwUpload() {
  auto* page = currentPage();
  if (!page) return;
  const auto unsent = page->logbook().qsosNotLotwSent();
  if (unsent.empty()) {
    setStatus("No new QSOs to upload to LoTW.");
    return;
  }
  const std::string tmp =
      std::filesystem::temp_directory_path() / "xlog2-lotw-upload.adi";
  {
    std::ofstream(tmp) << adif::write(unsent);
  }
  std::vector<long> ids;
  for (const auto& q : unsent) ids.push_back(q.id);
  presenter_.beginLotwUpload(&page->presenter(), std::move(ids));
  setStatus("Signing and uploading " + std::to_string(unsent.size()) +
            " QSO(s) via tqsl…");
  lotw_.uploadAdifFile(cfg().tqslPath, cfg().lotwStation, tmp);
}

void QtMainWindow::onLotwDownload() {
  if (cfg().lotwUser.empty() || cfg().lotwPassword.empty()) {
    setStatus("Set your LoTW username and password in LoTW ▸ Settings first.");
    return;
  }
  if (lotw_.isBusy()) {
    setStatus("A LoTW download is already in progress.");
    return;
  }
  setStatus("Downloading LoTW confirmations…");
  lotw_.downloadConfirmations(cfg().lotwUser, cfg().lotwPassword,
                              cfg().lotwLastDownload);
}

// --- keyer -------------------------------------------------------------------

void QtMainWindow::applyKeyerConfig() {
  keyer_.setEndpoint(cfg().keyerHost, cfg().keyerPort);
  if (cfg().keyerSpeed > 0) keyer_.setSpeed(cfg().keyerSpeed);
  for (int i = 0; i < tabs_->count(); ++i)
    if (auto* p = qobject_cast<QtLogPage*>(tabs_->widget(i)))
      p->setCwMessages(cfg().keyerMessages);
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
  ac.host = cfg().audioHost;
  ac.port = cfg().audioPort;
  ac.sampleRate = cfg().audioSampleRate;
  ac.channels = cfg().audioChannels;
  ac.device = cfg().audioDevice;
  audio_.start(ac);
  cfg().audioEnabled = true;  // user intent, persisted; survives teardown
  if (audioAction_) {
    QSignalBlocker block(audioAction_);
    audioAction_->setChecked(true);
  }
  if (skimmer_.isRunning())  // re-sync the skimmer to the (possibly new) rate
    startSkimmer();
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
  pc.host = cfg().paddleHost;
  pc.port = cfg().paddlePort;
  pc.wpm = cfg().paddleWpm;
  pc.iambicB = cfg().paddleIambicB;
  pc.autospace = cfg().paddleAutospace;
  pc.muteTailMs = cfg().paddleMuteTailMs;
  pc.sidetone = cfg().paddleSidetone;
  pc.toneHz = cfg().paddleToneHz;
  pc.level = cfg().paddleLevel;
  pc.device = cfg().paddleSidetoneDevice;
  paddle_.start(pc);
  hidPaddle_.start();          // also accept a USB HID paddle, if present
  cfg().paddleEnabled = true;  // user intent, persisted; survives teardown
  if (paddleAction_) {
    QSignalBlocker block(paddleAction_);
    paddleAction_->setChecked(true);
  }
}

bool QtMainWindow::eventFilter(QObject* obj, QEvent* ev) {
  if (paddle_.isActive() &&
      (ev->type() == QEvent::KeyPress || ev->type() == QEvent::KeyRelease)) {
    auto* ke = static_cast<QKeyEvent*>(ev);
    if (!ke->isAutoRepeat() && (ke->key() == Qt::Key_BracketLeft ||
                                ke->key() == Qt::Key_BracketRight)) {
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
  if (cfg().dxHost.empty()) {
    setStatus("Set a DX cluster host in its settings first.");
    return;
  }
  dxDock_->show();
  cluster_.connectTo(cfg().dxHost, cfg().dxPort, cfg().dxLogin);
}

// --- logbook sync ------------------------------------------------------------

void QtMainWindow::attachSyncedLog(QtLogPage* page) {
  syncedPage_ = page;
  if (!page) {
    coordinator_.detach();
    return;
  }
  coordinator_.attach(&page->presenter());
  // Propagate local edits/deletes to authenticated peers in real time.
  page->presenter().onLocalUpsert = [this](const Qso& q) {
    coordinator_.onLocalUpsert(q);
  };
  page->presenter().onLocalDelete = [this](const std::string& u,
                                           const std::string& d) {
    coordinator_.onLocalDelete(u, d);
  };
}

void QtMainWindow::updateSyncIndicator() {
  if (!cfg().syncEnabled || !sync_.isRunning()) {
    syncIndicator_->clear();
    return;
  }
  const int n = sync_.memberCount();
  syncIndicator_->setText(n > 0 ? QString("⇄ %1").arg(n) : "⇄ …");
}

void QtMainWindow::startSync() {
  if (!cfg().syncEnabled) {
    sync_.stop();
    updateSyncIndicator();
    return;
  }
  LogbookSync::Config c;
  c.nodeId = cfg().syncNodeId;  // empty => the mesh mints one (persisted below)
  c.group = syncproto::meshGroup(cfg().syncSecret);
  c.port = cfg().syncPort;
  // Optional WAN peers (the internet); "host" or "host:port".
  for (const std::string& h : {cfg().syncPeerHost, cfg().syncPeerHostAlt})
    if (auto p = LogbookSync::parsePeer(h, cfg().syncPort); !p.first.empty())
      c.staticPeers.push_back(p);
  // A shared secret secures the mesh (PSK) and enables a persistent identity
  // (node id derives from the key), which in turn powers per-node trust.
  c.psk = cfg().syncSecret;
  if (!cfg().syncSecret.empty()) {
    c.identityFile =
        envPath("XDG_DATA_HOME", ".local/share") + "/node_identity";
    c.requireIdentity = cfg().syncRequireIdentity;
    c.nodeName = cfg().syncNodeName;  // advisory; may be empty
  }
  sync_.start(c);
  // Persist the resolved mesh id so the LWW tiebreak is stable across restarts,
  // and key the coordinator off the same id. (Empty if the mesh failed to
  // start.)
  if (!sync_.localId().empty()) cfg().syncNodeId = sync_.localId();
  coordinator_.configure(cfg().syncNodeId, cfg().syncSecret);
  applySyncTrust();
  updateSyncIndicator();
}

void QtMainWindow::onSyncNow() { coordinator_.syncNow(); }

void QtMainWindow::applySyncTrust() {
  // Trust is enforced only on a secured (identity-bearing) mesh.
  std::vector<std::string> ids;
  ids.reserve(cfg().syncTrustedPeers.size());
  for (const auto& tp : cfg().syncTrustedPeers) ids.push_back(tp.id);
  coordinator_.setTrust(/*enforce=*/!cfg().syncSecret.empty(), ids);
}

void QtMainWindow::refreshTrustedPeersDialog() {
  if (!trustedPeersList_) return;
  // Replace the list widget's layout wholesale.
  if (auto* old = trustedPeersList_->layout()) delete old;
  auto* col = new QVBoxLayout(trustedPeersList_);
  col->setContentsMargins(0, 0, 0, 0);
  col->setSpacing(6);

  auto rows = coordinator_.peerSnapshot();
  if (rows.empty()) {
    auto* empty = new QLabel(
        "No peers discovered yet. Peers appear here once they join the mesh.");
    empty->setWordWrap(true);
    col->addWidget(empty);
    col->addStretch();
    return;
  }
  std::sort(rows.begin(), rows.end(), [](const auto& a, const auto& b) {
    if (a.trusted != b.trusted) return a.trusted > b.trusted;
    if (a.online != b.online) return a.online > b.online;
    return a.id < b.id;
  });

  for (const auto& r : rows) {
    auto* row = new QWidget;
    auto* h = new QHBoxLayout(row);
    h->setContentsMargins(0, 0, 0, 0);
    h->setSpacing(8);
    h->addWidget(new QLabel(QString::fromUtf8(r.online ? "●" : "○")));

    const std::string shortId =
        r.id.size() > 12 ? r.id.substr(0, 12) + "…" : r.id;
    std::string text =
        r.name.empty() ? shortId : (r.name + "  (" + shortId + ")");
    if (r.trusted && r.ready)
      text += "  — syncing";
    else if (r.trusted)
      text += "  — trusted";
    auto* lbl = new QLabel(QString::fromStdString(text));
    h->addWidget(lbl, /*stretch=*/1);

    const std::string id = r.id, name = r.name;
    if (r.trusted) {
      auto* btn = new QPushButton("Revoke");
      connect(btn, &QPushButton::clicked, this, [this, id]() {
        auto& v = cfg().syncTrustedPeers;
        v.erase(std::remove_if(v.begin(), v.end(),
                               [&](const auto& tp) { return tp.id == id; }),
                v.end());
        coordinator_.revokePeer(id);
        saveSettings();
        refreshTrustedPeersDialog();
      });
      h->addWidget(btn);
    } else {
      auto* btn = new QPushButton("Trust");
      connect(btn, &QPushButton::clicked, this, [this, id, name]() {
        cfg().syncTrustedPeers.push_back({id, name});
        coordinator_.trustPeer(id);
        saveSettings();
        refreshTrustedPeersDialog();
      });
      h->addWidget(btn);
    }
    col->addWidget(row);
  }
  col->addStretch();
}

void QtMainWindow::onManageTrustedPeers() {
  if (trustedPeersDialog_) {  // already open — raise it
    trustedPeersDialog_->raise();
    trustedPeersDialog_->activateWindow();
    return;
  }
  auto* dlg = new QDialog(this);
  dlg->setWindowTitle("Trusted peers");
  dlg->resize(460, 420);
  dlg->setAttribute(Qt::WA_DeleteOnClose);
  auto* v = new QVBoxLayout(dlg);

  // This node's identity key (to share into a peer's allowlist).
  const std::string key = sync_.identityKey();
  auto* keyRow = new QHBoxLayout;
  keyRow->addWidget(new QLabel("This node's key:"));
  auto* keyEdit = new QLineEdit(QString::fromStdString(
      key.empty() ? "(identity disabled — set a shared secret)" : key));
  keyEdit->setReadOnly(true);
  keyRow->addWidget(keyEdit, 1);
  auto* copy = new QPushButton("Copy");
  copy->setEnabled(!key.empty());
  connect(copy, &QPushButton::clicked, this, [key]() {
    QGuiApplication::clipboard()->setText(QString::fromStdString(key));
  });
  keyRow->addWidget(copy);
  v->addLayout(keyRow);

  auto* hint = new QLabel(
      "Trusted peers exchange logbook data. Newly-discovered peers connect but "
      "stay on hold until you trust them.");
  hint->setWordWrap(true);
  v->addWidget(hint);

  auto* scroll = new QScrollArea;
  scroll->setWidgetResizable(true);
  trustedPeersList_ = new QWidget;
  scroll->setWidget(trustedPeersList_);
  v->addWidget(scroll, 1);

  auto* buttons = new QHBoxLayout;
  buttons->addStretch();
  auto* refresh = new QPushButton("Refresh");
  connect(refresh, &QPushButton::clicked, this,
          [this]() { refreshTrustedPeersDialog(); });
  auto* close = new QPushButton("Close");
  connect(close, &QPushButton::clicked, dlg, &QDialog::close);
  buttons->addWidget(refresh);
  buttons->addWidget(close);
  v->addLayout(buttons);

  trustedPeersDialog_ = dlg;
  connect(dlg, &QObject::destroyed, this, [this]() {
    trustedPeersDialog_ = nullptr;
    trustedPeersList_ = nullptr;
  });
  refreshTrustedPeersDialog();
  dlg->show();
}

// --- settings ----------------------------------------------------------------

void QtMainWindow::onEditSettings() {
  QtSettingsDialog dlg(cfg(), this);
  connect(&dlg, &QtSettingsDialog::applied, this, &QtMainWindow::applySettings);
  dlg.exec();
}

void QtMainWindow::applySettings(const Settings& s) {
  // Copy only the config-field subset; runtime/view state (enable toggles,
  // dock/visibility, lotwLastDownload) is owned by the menus and preserved.
  cfg().udpPort = s.udpPort;

  cfg().rigModel = s.rigModel;
  cfg().rigDevice = s.rigDevice;
  cfg().rigPollMs = s.rigPollMs;
  cfg().rigAutoConnect = s.rigAutoConnect;

  cfg().dxHost = s.dxHost;
  cfg().dxPort = s.dxPort;
  cfg().dxLogin = s.dxLogin;
  cfg().dxAutoConnect = s.dxAutoConnect;

  cfg().lotwUser = s.lotwUser;
  cfg().lotwPassword = s.lotwPassword;
  cfg().lotwStation = s.lotwStation;
  cfg().tqslPath = s.tqslPath;

  cfg().qrzUser = s.qrzUser;
  cfg().qrzPassword = s.qrzPassword;
  cfg().qrzCacheDays = s.qrzCacheDays;

  cfg().myLocator = s.myLocator;

  cfg().keyerHost = s.keyerHost;
  cfg().keyerPort = s.keyerPort;
  cfg().keyerSpeed = s.keyerSpeed;
  cfg().keyerMessages = s.keyerMessages;

  cfg().paddleHost = s.paddleHost;
  cfg().paddlePort = s.paddlePort;
  cfg().paddleWpm = s.paddleWpm;
  cfg().paddleIambicB = s.paddleIambicB;
  cfg().paddleAutospace = s.paddleAutospace;
  cfg().paddleSidetone = s.paddleSidetone;
  cfg().paddleToneHz = s.paddleToneHz;
  cfg().paddleLevel = s.paddleLevel;
  cfg().paddleSidetoneDevice = s.paddleSidetoneDevice;
  cfg().paddleMuteAudio = s.paddleMuteAudio;
  cfg().paddleMuteTailMs = s.paddleMuteTailMs;

  cfg().audioHost = s.audioHost;
  cfg().audioPort = s.audioPort;
  cfg().audioSampleRate = s.audioSampleRate;
  cfg().audioChannels = s.audioChannels;
  cfg().audioDevice = s.audioDevice;

  cfg().skimmerGate = s.skimmerGate;
  cfg().skimmerMinSnr = s.skimmerMinSnr;
  cfg().skimmerKnownOnly = s.skimmerKnownOnly;
  cfg().skimmerBwNormDb = s.skimmerBwNormDb;
  cfg().skimmerBwNormRefHz = s.skimmerBwNormRefHz;
  cfg().skimmerBwOffsetDb = s.skimmerBwOffsetDb;

  cfg().syncEnabled = s.syncEnabled;
  cfg().syncPeerHost = s.syncPeerHost;
  cfg().syncPeerHostAlt = s.syncPeerHostAlt;
  cfg().syncPort = s.syncPort;
  cfg().syncSecret = s.syncSecret;
  // (Re)start the mesh transport; startSync re-keys the coordinator from the
  // resolved mesh id.
  startSync();

  // Re-apply to any running service (rig/DX/LoTW/QRZ take effect on next use).
  applyKeyerConfig();
  if (listener_.isListening()) {
    listener_.stop();
    startUdpListening();
  }
  if (audio_.isStreaming())
    startAudioStream();  // also re-syncs the skimmer rate
  if (paddle_.isActive()) startPaddleKeyer();

  // Skimmer detector params are live: push to both the service and the panel
  // controls so the panel reflects the dialog (these setters don't re-emit).
  skimmerPanel_->setGate(cfg().skimmerGate);
  skimmer_.setGate(static_cast<float>(cfg().skimmerGate));
  skimmerPanel_->setMinSnr(cfg().skimmerMinSnr);
  skimmer_.setMinSnr(static_cast<float>(cfg().skimmerMinSnr));
  skimmerPanel_->setKnownOnly(cfg().skimmerKnownOnly);
  skimmer_.setKnownCallsOnly(cfg().skimmerKnownOnly);
  skimmer_.setBandwidthNorm(cfg().skimmerBwNormDb, cfg().skimmerBwNormRefHz,
                            cfg().skimmerBwOffsetDb);

  mapPanel_->setFrom(cfg().myLocator);
  qrz_.setCache(envPath("XDG_DATA_HOME", ".local/share") + "/qrz-cache.sqlite",
                cfg().qrzCacheDays);

  setStatus("Settings saved.");
}

// --- paths -------------------------------------------------------------------

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
    if (w > 0 && h > 0) resize(w, h);
    if (ini.getBool("window", "maximized", false)) showMaximized();
  }

  if (loaded && ini.hasKey("session", "open")) {
    for (const auto& path :
         strutil::splitSemicolons(ini.getString("session", "open"))) {
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
    pendingDockSize_ = cfg().dxPanelPos;
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
    pendingRigDockSize_ = cfg().rigPanelPos;
    pendingRigDockOrient_ =
        isHorizontalArea(rigArea) ? Qt::Horizontal : Qt::Vertical;
  }
  if (rigDockGroup_)
    for (QAction* a : rigDockGroup_->actions())
      if (a->data().toString().toStdString() == cfg().rigDock) {
        a->setChecked(true);
        break;
      }

  // CW-skimmer gate level, applied before the dock is shown (so the skimmer
  // starts with it). setGate on the panel doesn't re-emit.
  skimmerPanel_->setGate(cfg().skimmerGate);
  skimmer_.setGate(static_cast<float>(cfg().skimmerGate));
  skimmerPanel_->setMinSnr(cfg().skimmerMinSnr);
  skimmer_.setMinSnr(static_cast<float>(cfg().skimmerMinSnr));
  skimmerPanel_->setKnownOnly(cfg().skimmerKnownOnly);
  skimmer_.setKnownCallsOnly(cfg().skimmerKnownOnly);
  skimmer_.setBandwidthNorm(cfg().skimmerBwNormDb, cfg().skimmerBwNormRefHz,
                            cfg().skimmerBwOffsetDb);

  // CW-skimmer dock placement + visibility.
  const Qt::DockWidgetArea skArea = dockAreaFromString(cfg().skimmerDock);
  addDockWidget(skArea, skimmerDock_);
  skimmerDock_->setVisible(
      cfg().skimmerVisible);  // fires visibilityChanged -> startSkimmer
  if (cfg().skimmerVisible && cfg().skimmerPanelPos > 0) {
    pendingSkimmerDockSize_ = cfg().skimmerPanelPos;
    pendingSkimmerDockOrient_ =
        isHorizontalArea(skArea) ? Qt::Horizontal : Qt::Vertical;
  }
  if (skimmerDockGroup_)
    for (QAction* a : skimmerDockGroup_->actions())
      if (a->data().toString().toStdString() == cfg().skimmerDock) {
        a->setChecked(true);
        break;
      }

  // World-map panel: seed the "from" point and place/show the dock.
  mapPanel_->setFrom(cfg().myLocator);
  const Qt::DockWidgetArea mapArea = dockAreaFromString(cfg().mapDock);
  addDockWidget(mapArea, mapDock_);
  mapDock_->setVisible(cfg().mapVisible);
  if (cfg().mapVisible && cfg().mapPanelPos > 0) {
    pendingMapDockSize_ = cfg().mapPanelPos;
    pendingMapDockOrient_ =
        isHorizontalArea(mapArea) ? Qt::Horizontal : Qt::Vertical;
  }
  if (mapDockGroup_)
    for (QAction* a : mapDockGroup_->actions())
      if (a->data().toString().toStdString() == cfg().mapDock) {
        a->setChecked(true);
        break;
      }
}

void QtMainWindow::saveSettings() {
  IniFile ini;
  ini.loadFromFile(layoutFilePath());

  // Capture the live DX-cluster dock state so Settings::store persists it.
  const Qt::DockWidgetArea area = dockWidgetArea(dxDock_);
  if (area != Qt::NoDockWidgetArea) cfg().dxDock = stringFromDockArea(area);
  cfg().dxVisible = dxDock_->isVisible();
  if (cfg().dxVisible) {
    const int sz =
        isHorizontalArea(area) ? dxDock_->width() : dxDock_->height();
    if (sz > 0) cfg().dxPanelPos = sz;
  }

  // Same for the rig dock.
  const Qt::DockWidgetArea rigArea = dockWidgetArea(rigDock_);
  if (rigArea != Qt::NoDockWidgetArea)
    cfg().rigDock = stringFromDockArea(rigArea);
  cfg().rigVisible = rigDock_->isVisible();
  if (cfg().rigVisible) {
    const int sz =
        isHorizontalArea(rigArea) ? rigDock_->width() : rigDock_->height();
    if (sz > 0) cfg().rigPanelPos = sz;
  }

  // Same for the skimmer dock.
  const Qt::DockWidgetArea skArea = dockWidgetArea(skimmerDock_);
  if (skArea != Qt::NoDockWidgetArea)
    cfg().skimmerDock = stringFromDockArea(skArea);
  cfg().skimmerVisible = skimmerDock_->isVisible();
  if (cfg().skimmerVisible) {
    const int sz = isHorizontalArea(skArea) ? skimmerDock_->width()
                                            : skimmerDock_->height();
    if (sz > 0) cfg().skimmerPanelPos = sz;
  }

  // Same for the world-map dock.
  const Qt::DockWidgetArea mapArea = dockWidgetArea(mapDock_);
  if (mapArea != Qt::NoDockWidgetArea)
    cfg().mapDock = stringFromDockArea(mapArea);
  cfg().mapVisible = mapDock_->isVisible();
  if (cfg().mapVisible) {
    const int sz =
        isHorizontalArea(mapArea) ? mapDock_->width() : mapDock_->height();
    if (sz > 0) cfg().mapPanelPos = sz;
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
    if (auto* p = qobject_cast<QtLogPage*>(tabs_->widget(i));
        p && p->isFileBacked()) {
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
  ::chmod(layoutFilePath().c_str(),
          S_IRUSR | S_IWUSR);  // plaintext LoTW password
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
      (pendingDockSize_ <= 0 && pendingRigDockSize_ <= 0 &&
       pendingSkimmerDockSize_ <= 0 && pendingMapDockSize_ <= 0))
    return;
  dockSizeRestored_ = true;
  if (pendingDockSize_ > 0)
    resizeDocks({dxDock_}, {pendingDockSize_}, pendingDockOrient_);
  if (pendingRigDockSize_ > 0)
    resizeDocks({rigDock_}, {pendingRigDockSize_}, pendingRigDockOrient_);
  if (pendingSkimmerDockSize_ > 0)
    resizeDocks({skimmerDock_}, {pendingSkimmerDockSize_},
                pendingSkimmerDockOrient_);
  if (pendingMapDockSize_ > 0)
    resizeDocks({mapDock_}, {pendingMapDockSize_}, pendingMapDockOrient_);
}
