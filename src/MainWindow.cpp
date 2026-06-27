#include "MainWindow.h"

#include "Adif.h"
#include "LogPage.h"
#include "SettingsDialog.h"
#include "Statistics.h"
#include "UiUtil.h"
#include "Version.h"

#include <gdk/gdkkeysyms.h>

#include <sys/stat.h>

#include <algorithm>
#include <map>
#include <set>
#include <sstream>
#include <stdexcept>

MainWindow::MainWindow()
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
    set_title(Glib::ustring("xlog2 ") + xlog::kVersion);
    set_default_size(1024, 700);
    // Hide (don't destroy) on close so XlogApplication's signal_hide handler
    // can delete us deterministically.
    set_hide_on_close(true);
    signal_close_request().connect(sigc::mem_fun(*this, &MainWindow::onCloseRequest), false);

    buildActions();

    // F1–F9 keyer accelerators, installed once on the window so they fire from
    // anywhere — the log page or the DX-cluster panel — routed to the active
    // tab's presenter (whose form data feeds the CW expansion). CAPTURE phase so
    // they win over the GtkPaned's default F6/F8 bindings; LOCAL scope on the
    // toplevel covers every focused descendant. onSendCwClicked() guards empty
    // slots.
    auto keyerShortcuts = Gtk::ShortcutController::create();
    keyerShortcuts->set_scope(Gtk::ShortcutScope::LOCAL);
    keyerShortcuts->set_propagation_phase(Gtk::PropagationPhase::CAPTURE);
    for (int i = 0; i < 9; ++i) {
        auto action = Gtk::CallbackAction::create(
            [this, i](Gtk::Widget&, const Glib::VariantBase&) {
                if (auto* page = currentPage())
                    page->presenter().onSendCwClicked(i);
                return true;
            });
        keyerShortcuts->add_shortcut(
            Gtk::Shortcut::create(Gtk::KeyvalTrigger::create(GDK_KEY_F1 + i), action));
    }
    add_controller(keyerShortcuts);

    // Paddle simulation for the remote paddle keyer: `[` = dit, `]` = dah, held
    // for the duration of the contact. Only intercepted while the keyer is active,
    // so the brackets type normally otherwise. CAPTURE phase so the press/release
    // is seen before any focused entry consumes it.
    auto paddleKeys = Gtk::EventControllerKey::create();
    paddleKeys->set_propagation_phase(Gtk::PropagationPhase::CAPTURE);
    paddleKeys->signal_key_pressed().connect(
        [this](guint keyval, guint, Gdk::ModifierType) -> bool {
            if (!paddle_.isActive())
                return false;
            if (keyval == GDK_KEY_bracketleft)  { paddle_.setDit(true); return true; }
            if (keyval == GDK_KEY_bracketright) { paddle_.setDah(true); return true; }
            return false;
        },
        false);
    paddleKeys->signal_key_released().connect(
        [this](guint keyval, guint, Gdk::ModifierType) {
            if (keyval == GDK_KEY_bracketleft)  paddle_.setDit(false);
            if (keyval == GDK_KEY_bracketright) paddle_.setDah(false);
        },
        false);
    add_controller(paddleKeys);

    auto* vbox = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL);
    set_child(*vbox);

    auto* menubar = Gtk::make_managed<Gtk::PopoverMenuBar>(buildMenuModel());
    vbox->append(*menubar);

    notebook_.set_scrollable(true);
    notebook_.set_vexpand(true);
    notebook_.signal_switch_page().connect(
        [this](Gtk::Widget* page, guint) {
            if (auto* lp = dynamic_cast<LogPage*>(page)) {
                set_title(Glib::ustring("xlog2 ") + xlog::kVersion + " — " +
                          lp->title() + "  (" +
                          std::to_string(lp->qsoCount()) + " QSOs)");
                mapPanel_.setTo(lp->presenter().currentLocator());  // map "to" = new tab
            }
        });

    // The notebook and the DX-cluster panel share a Gtk::Paned whose
    // orientation/order is set by applyDxDock() from the dock preference.
    dxPanel_.signalActivate().connect(
        sigc::mem_fun(*this, &MainWindow::onSpotActivated));
    dxPanel_.signalCommand().connect(
        [this](const std::string& cmd) { cluster_.sendCommand(cmd); });
    dxPanel_.signalConnectToggle().connect(
        sigc::mem_fun(*this, &MainWindow::onClusterConnect));
    paned_.set_vexpand(true);

    // The rig panel docks around the DX/notebook area via a second paned.
    rigPanel_.signalStep().connect(
        [this](double hz) { if (rig_.isRunning()) rig_.stepFrequency(hz); });
    rigPanel_.signalSetFilter().connect(
        [this](int n) { if (rig_.isRunning()) rig_.setFilter(n); });
    rigPanel_.signalSetPower().connect(
        [this](bool on) { if (rig_.isRunning()) rig_.setPower(on); });
    rigPanel_.signalSetAgc().connect(
        [this](bool on) { if (rig_.isRunning()) rig_.setAgc(on); });
    rigPanel_.signalSetMode().connect(
        [this](std::string mode) { if (rig_.isRunning()) rig_.setMode(mode); });
    rigPaned_.set_vexpand(true);
    // The skimmer panel docks around the rig/DX/notebook area via a third paned.
    skimmerPaned_.set_vexpand(true);
    // The map panel docks around the whole lot via a fourth paned.
    mapPaned_.set_vexpand(true);
    vbox->append(mapPaned_);
    applyDxDock();       // initial layout (defaults); reapplied after loadSettings
    applyRigDock();      // wraps paned_ + rigPanel_
    applySkimmerDock();  // wraps rigPaned_ + skimmerPanel_
    applyMapDock();      // wraps skimmerPaned_ + mapPanel_

    cluster_.onSpot   = [this](const DxSpot& s) { dxPanel_.addSpot(s); };
    cluster_.onLine   = [this](const std::string& l) { dxPanel_.addLine(l); };
    cluster_.onStatus = [this](const std::string& s) {
        dxPanel_.addLine(s);
        dxPanel_.setConnected(cluster_.isConnected());
        setStatus(s);
    };

    // Logbook sync: the transport moves bytes; the coordinator owns the protocol
    // and is the only thing that touches the synced logbook (on the UI thread).
    sync_.onPeerUp = [this](const LogbookSync::PeerKey& p) {
        coordinator_.onPeerUp(p);
        updateSyncIndicator();
    };
    sync_.onPeerDown = [this](const LogbookSync::PeerKey& p) {
        coordinator_.onPeerDown(p);
        updateSyncIndicator();
    };
    sync_.onMessage = [this](const LogbookSync::PeerKey& p, const syncproto::Message& m) {
        // QRZ peer-cache messages ride the same mesh; route them to QrzPeer.
        if (m.type == syncproto::Type::QrzQuery || m.type == syncproto::Type::QrzResponse)
            qrzPeer_.onMessage(p, m);
        else
            coordinator_.onMessage(p, m);
    };
    sync_.onStatus  = [this](const std::string& s) { setStatus(s); };
    coordinator_.onStatus = [this](const std::string& s) { setStatus(s); };

    // Distributed QRZ cache: consult mesh peers between the local cache and
    // qrz.com. The timer bounds how long we wait for a peer to answer.
    qrzPeer_.scheduleOnce = [](int ms, std::function<void()> fn) {
        Glib::signal_timeout().connect_once(std::move(fn), ms);
    };
    qrzPeer_.onStatus = [this](const std::string& s) { setStatus(s); };
    qrz_.setPeerResolver([this](const std::string& call,
                                std::function<void(std::optional<QrzResult>)> reply) {
        qrzPeer_.query(call, std::move(reply));
    });

    auto* statusBox = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL);
    statusLabel_.set_xalign(0.0);
    statusLabel_.set_hexpand(true);
    ui::setMargin(statusLabel_, 4);
    statusBox->append(statusLabel_);
    // A dedicated indicator on the right so the live frame counter doesn't fight
    // with transient status messages.
    audioIndicator_.set_xalign(1.0);
    ui::setMargin(audioIndicator_, 4);
    statusBox->append(audioIndicator_);
    syncIndicator_.set_xalign(1.0);
    ui::setMargin(syncIndicator_, 4);
    statusBox->append(syncIndicator_);
    vbox->append(*statusBox);

    // Service results are routed by the presenter (toolkit-neutral).
    listener_.setCallback(
        [this](const std::vector<Qso>& qsos, const std::string& source) {
            presenter_.routeUdp(qsos, source);
        });
    rig_.onUpdate = [this](double mhz, const std::string& mode) {
        presenter_.routeRigUpdate(mhz, mode);
        lastMhz_  = mhz;
        lastMode_ = mode;
    };
    // onFilter fires immediately after onUpdate each poll tick, so the cached
    // frequency/mode are current — render the whole panel state here.
    rig_.onFilter = [this](int pbwidthHz, int filter) {
        rigPanel_.setState(lastMhz_, lastMode_, pbwidthHz, filter);
        // Feed the live passband to the skimmer so its waterfall can normalize the
        // brightness rise that narrowing the filter otherwise causes.
        skimmer_.setFilterBandwidthHz(pbwidthHz);
    };
    rig_.onPower = [this](bool supported, bool on) {
        rigPanel_.setPowerState(supported, on);
    };
    rig_.onConnectResult = [this](bool ok, const std::string& error) {
        rigPanel_.setConnected(ok);
        if (ok)
            setStatus("Connected to rig (model " + std::to_string(cfg().rigModel) + ").");
        else
            setStatus("Rig connect failed: " + error);
    };
    lotw_.onDownloadDone = [this](const std::string& adif, const std::string& error) {
        presenter_.routeLotwDownload(adif, error);
    };
    lotw_.onUploadDone = [this](bool ok, const std::string& message) {
        presenter_.routeLotwUploadResult(ok, message);
    };
    qrz_.onResult = [this](const QrzResult& result, const std::string& error) {
        presenter_.routeQrzResult(result, error);
    };
    qrz_.onFillProgress = [this](int done, int total) {
        setStatus("QRZ locator fill: " + std::to_string(done) + "/" +
                  std::to_string(total) + "…");
    };
    qrz_.onFillResult = [this](const std::vector<std::pair<std::string, std::string>>& r,
                               int fromCache, int fetched, const std::string& err) {
        presenter_.routeQrzLocatorFill(r, fromCache, fetched, err);
    };
    audio_.onStatus = [this](const std::string& s) { setStatus(s); };
    audio_.onStats  = [this](unsigned long frames) {
        audioIndicator_.set_text("♪ " + std::to_string(frames) + " frames");
    };
    // Tap the decoded rig audio into the skimmer (called on the audio worker
    // thread; pushPcm is cheap + thread-safe and a no-op when the skimmer is off).
    audio_.onPcm = [this](const int16_t* s, int frames, int ch, int rate) {
        skimmer_.pushPcm(s, frames, ch, rate);
    };
    skimmer_.onWaterfall = [this](const std::vector<float>& mags, double lo, double hi) {
        skimmerPanel_.addWaterfall(mags, lo, hi);
    };
    skimmer_.onChannel = [this](int id, double hz, int wpm, const std::string& text,
                                const std::string& call) {
        skimmerPanel_.updateChannel(id, hz, wpm, text, call);
    };
    skimmer_.onChannelRemoved = [this](int id) { skimmerPanel_.removeChannel(id); };
    skimmerPanel_.signalGate().connect([this](int db) {
        cfg().skimmerGate = db;
        skimmer_.setGate(static_cast<float>(db));
    });
    skimmerPanel_.signalMinSnr().connect([this](int db) {
        cfg().skimmerMinSnr = db;
        skimmer_.setMinSnr(static_cast<float>(db));
    });
    skimmerPanel_.signalKnownOnly().connect([this](bool on) {
        cfg().skimmerKnownOnly = on;
        skimmer_.setKnownCallsOnly(on);
    });
    // Master-callsign list (Super Check Partial), used to validate/correct decoded
    // callsigns. Optional: drop a MASTER.SCP at $XDG_DATA_HOME/xlog2/master.scp.
    {
        const std::size_t n = skimmer_.loadCallsignDb(Glib::build_filename(
            Glib::get_user_data_dir(), "xlog2", "master.scp"));
        skimmerPanel_.setCallDbInfo(n > 0, n);
    }
    paddle_.onStatus = [this](const std::string& s) { setStatus(s); };
    // Mute the rig-audio stream while keying (semi-break-in) when configured to.
    paddle_.onTransmit = [this](bool tx) { audio_.setMuted(tx && cfg().paddleMuteAudio); };
    // USB paddle: drive the keyer's lock-free contact atomics straight from the
    // HID worker thread (no UI hop) for lowest latency; status goes via the UI.
    hidPaddle_.onDit    = [this](bool p) { paddle_.setDit(p); };
    hidPaddle_.onDah    = [this](bool p) { paddle_.setDah(p); };
    hidPaddle_.onStatus = [this](const std::string& s) { setStatus(s); };

    loadSettings();
    // Point the QRZ result cache at a file under the data dir (created if needed).
    {
        const std::string dir =
            Glib::build_filename(Glib::get_user_data_dir(), "xlog2");
        try {
            Gio::File::create_for_path(dir)->make_directory_with_parents();
        } catch (const Glib::Error&) {
            // already exists (or cannot be created) — setCache then no-ops
        }
        qrz_.setCache(Glib::build_filename(dir, "qrz-cache.sqlite"), cfg().qrzCacheDays);
    }
    if (notebook_.get_n_pages() == 0)
        openDefaultLog();
    // The first/default tab is the synced logbook.
    if (auto* p = dynamic_cast<LogPage*>(notebook_.get_nth_page(0)))
        attachSyncedLog(p);

    updateTitle();
    setStatus("Ready.");

    // Resume listening for QSOs over UDP if it was enabled last session.
    if (cfg().udpEnabled)
        startUdpListening();

    // Connect to the rig at startup if configured to (non-blocking; the result
    // arrives via rig_.onConnectResult).
    if (cfg().rigAutoConnect) {
        setStatus("Connecting to rig…");
        rig_.start(cfg().rigModel, cfg().rigDevice, cfg().rigPollMs);
    }

    // Resume the rig audio stream if it was playing last session.
    if (cfg().audioEnabled)
        startAudioStream();

    // Resume the remote paddle keyer if it was active last session.
    if (cfg().paddleEnabled)
        startPaddleKeyer();

    // Resume logbook sync if it was enabled last session.
    if (cfg().syncEnabled)
        startSync();
}

void MainWindow::buildActions() {
    add_action("newtab", sigc::mem_fun(*this, &MainWindow::onNewTab));
    add_action("open",   sigc::mem_fun(*this, &MainWindow::onOpen));
    add_action("saveas", sigc::mem_fun(*this, &MainWindow::onSaveAs));
    add_action("closetab", sigc::mem_fun(*this, &MainWindow::onCloseTab));
    add_action("import", sigc::mem_fun(*this, &MainWindow::onImportAdif));
    add_action("importxlog", sigc::mem_fun(*this, &MainWindow::onImportXlog));
    add_action("export", sigc::mem_fun(*this, &MainWindow::onExportAdif));
    add_action("stats",  sigc::mem_fun(*this, &MainWindow::onStatistics));
    add_action("find",   sigc::mem_fun(*this, &MainWindow::onFind));
    add_action("filldxcc", [this]() {
        if (auto* page = currentPage())
            page->backfillDxcc();
    });
    add_action("filllocators", sigc::mem_fun(*this, &MainWindow::onFillLocators));
    add_action("about",  sigc::mem_fun(*this, &MainWindow::onAbout));
    add_action("quit",   sigc::mem_fun(*this, &MainWindow::close));

    add_action("settings", sigc::mem_fun(*this, &MainWindow::onEditSettings));

    udpAction_ = add_action_bool("udp", sigc::mem_fun(*this, &MainWindow::onToggleUdp), false);

    add_action("rigconnect",    sigc::mem_fun(*this, &MainWindow::onRigConnect));
    add_action("rigdisconnect", sigc::mem_fun(*this, &MainWindow::onRigDisconnect));
    rigShowAction_ = add_action_bool(
        "rigshow", sigc::mem_fun(*this, &MainWindow::onRigToggleShow), true);
    rigDockAction_ = add_action_radio_string(
        "rigdock", sigc::mem_fun(*this, &MainWindow::onRigDock), "right");

    add_action("lotwupload",   sigc::mem_fun(*this, &MainWindow::onLotwUpload));
    add_action("lotwdownload", sigc::mem_fun(*this, &MainWindow::onLotwDownload));

    paddleAction_ =
        add_action_bool("paddle", sigc::mem_fun(*this, &MainWindow::onTogglePaddle), false);

    audioAction_ = add_action_bool("audio", sigc::mem_fun(*this, &MainWindow::onToggleAudio), false);

    dxShowAction_ = add_action_bool(
        "dxshow", sigc::mem_fun(*this, &MainWindow::onClusterToggleShow), false);
    add_action("dxconnect",  sigc::mem_fun(*this, &MainWindow::onClusterConnect));
    dxDockAction_ = add_action_radio_string(
        "dxdock", sigc::mem_fun(*this, &MainWindow::onDxDock), "bottom");

    skimmerShowAction_ = add_action_bool(
        "skimmershow", sigc::mem_fun(*this, &MainWindow::onSkimmerToggleShow), false);
    skimmerDockAction_ = add_action_radio_string(
        "skimmerdock", sigc::mem_fun(*this, &MainWindow::onSkimmerDock), "left");

    mapShowAction_ = add_action_bool(
        "mapshow", sigc::mem_fun(*this, &MainWindow::onMapToggleShow), false);
    mapDockAction_ = add_action_radio_string(
        "mapdock", sigc::mem_fun(*this, &MainWindow::onMapDock), "right");

    add_action("syncnow", sigc::mem_fun(*this, &MainWindow::onSyncNow));
    syncEnableAction_ = add_action_bool("syncenabled", [this]() {
        cfg().syncEnabled = !cfg().syncEnabled;
        startSync();  // also reconciles the toggle state
    }, cfg().syncEnabled);
}

Glib::RefPtr<Gio::Menu> MainWindow::buildMenuModel() {
    auto menu = Gio::Menu::create();

    auto fileMenu = Gio::Menu::create();
    fileMenu->append("_New Tab", "win.newtab");
    fileMenu->append("_Open…", "win.open");
    fileMenu->append("Save _As…", "win.saveas");
    fileMenu->append("_Close Tab", "win.closetab");
    auto adifSection = Gio::Menu::create();
    adifSection->append("_Import ADIF…", "win.import");
    adifSection->append("Import _xlog log…", "win.importxlog");
    adifSection->append("_Export ADIF…", "win.export");
    fileMenu->append_section(adifSection);
    auto quitSection = Gio::Menu::create();
    quitSection->append("_Quit", "win.quit");
    fileMenu->append_section(quitSection);
    menu->append_submenu("_File", fileMenu);

    auto editMenu = Gio::Menu::create();
    editMenu->append("_Settings…", "win.settings");
    menu->append_submenu("_Edit", editMenu);

    auto logMenu = Gio::Menu::create();
    logMenu->append("_Find…", "win.find");
    logMenu->append("Fill _DXCC entities", "win.filldxcc");
    logMenu->append("Fill missing _locators (QRZ)", "win.filllocators");
    logMenu->append("_Statistics…", "win.stats");
    menu->append_submenu("_Log", logMenu);

    auto netMenu = Gio::Menu::create();
    netMenu->append("_Listen for QSOs (UDP)", "win.udp");
    menu->append_submenu("_Network", netMenu);

    auto rigMenu = Gio::Menu::create();
    rigMenu->append("_Connect", "win.rigconnect");
    rigMenu->append("_Disconnect", "win.rigdisconnect");
    auto rigPanelSection = Gio::Menu::create();
    rigPanelSection->append("Show _panel", "win.rigshow");
    auto rigDockMenu = Gio::Menu::create();
    rigDockMenu->append("_Top",    "win.rigdock::top");
    rigDockMenu->append("_Bottom", "win.rigdock::bottom");
    rigDockMenu->append("_Left",   "win.rigdock::left");
    rigDockMenu->append("_Right",  "win.rigdock::right");
    rigPanelSection->append_submenu("Doc_k", rigDockMenu);
    rigMenu->append_section(rigPanelSection);
    menu->append_submenu("_Rig", rigMenu);

    auto lotwMenu = Gio::Menu::create();
    lotwMenu->append("_Upload to LoTW…", "win.lotwupload");
    lotwMenu->append("_Download confirmations", "win.lotwdownload");
    menu->append_submenu("Lo_TW", lotwMenu);

    auto keyerMenu = Gio::Menu::create();
    keyerMenu->append("Remote _paddle keying ([ / ])", "win.paddle");
    menu->append_submenu("_Keyer", keyerMenu);

    auto audioMenu = Gio::Menu::create();
    audioMenu->append("_Play rig audio stream", "win.audio");
    menu->append_submenu("_Audio", audioMenu);

    auto skimmerMenu = Gio::Menu::create();
    skimmerMenu->append("_Show panel", "win.skimmershow");
    auto skimmerDockMenu = Gio::Menu::create();
    skimmerDockMenu->append("_Top",    "win.skimmerdock::top");
    skimmerDockMenu->append("_Bottom", "win.skimmerdock::bottom");
    skimmerDockMenu->append("_Left",   "win.skimmerdock::left");
    skimmerDockMenu->append("_Right",  "win.skimmerdock::right");
    skimmerMenu->append_submenu("Doc_k", skimmerDockMenu);
    menu->append_submenu("S_kimmer", skimmerMenu);

    auto mapMenu = Gio::Menu::create();
    mapMenu->append("_Show panel", "win.mapshow");
    auto mapDockMenu = Gio::Menu::create();
    mapDockMenu->append("_Top",    "win.mapdock::top");
    mapDockMenu->append("_Bottom", "win.mapdock::bottom");
    mapDockMenu->append("_Left",   "win.mapdock::left");
    mapDockMenu->append("_Right",  "win.mapdock::right");
    mapMenu->append_submenu("Doc_k", mapDockMenu);
    menu->append_submenu("_Map", mapMenu);

    auto clusterMenu = Gio::Menu::create();
    clusterMenu->append("_Show panel", "win.dxshow");
    clusterMenu->append("_Connect / Disconnect", "win.dxconnect");
    auto dockMenu = Gio::Menu::create();
    dockMenu->append("_Top",    "win.dxdock::top");
    dockMenu->append("_Bottom", "win.dxdock::bottom");
    dockMenu->append("_Left",   "win.dxdock::left");
    dockMenu->append("_Right",  "win.dxdock::right");
    clusterMenu->append_submenu("_Dock", dockMenu);
    menu->append_submenu("_Cluster", clusterMenu);

    auto syncMenu = Gio::Menu::create();
    syncMenu->append("_Enabled", "win.syncenabled");
    syncMenu->append("Sync _now", "win.syncnow");
    menu->append_submenu("S_ync", syncMenu);

    auto helpMenu = Gio::Menu::create();
    helpMenu->append("_About xlog2", "win.about");
    menu->append_submenu("_Help", helpMenu);

    return menu;
}

// --- tab/page management -----------------------------------------------------

LogPage* MainWindow::currentPage() {
    const int idx = notebook_.get_current_page();
    if (idx < 0)
        return nullptr;
    return dynamic_cast<LogPage*>(notebook_.get_nth_page(idx));
}

LogPage* MainWindow::addPage(LogPage* page) {
    page->applyColumnLayout(settings_);
    page->setCwMessages(cfg().keyerMessages);
    registerTab(page);
    return page;
}

std::string MainWindow::defaultLogPath() const {
    const std::string dir =
        Glib::build_filename(Glib::get_user_data_dir(), "xlog2");
    try {
        Gio::File::create_for_path(dir)->make_directory_with_parents();
    } catch (const Glib::Error&) {
        // already exists (or cannot be created) — open() reports the latter
    }
    return Glib::build_filename(dir, "default.xlog");
}

LogPage* MainWindow::openDefaultLog() {
    auto* page = addPage(Gtk::make_managed<LogPage>());
    const std::string path = defaultLogPath();
    if (!page->openFile(path))
        setStatus("Could not open the default logbook at " + path +
                  " — working in memory.");
    return page;
}

void MainWindow::registerTab(LogPage* page) {
    page->signalChanged().connect([this, page]() { onPageChanged(page); });
    page->signalStatus().connect([this](const Glib::ustring& m) { setStatus(m.raw()); });
    page->signalLookupCall().connect(
        [this, page](const std::string& call) { onQrzLookup(page, call); });
    page->signalSendCw().connect([this](const std::string& text) {
        if (!keyer_.send(text))
            setStatus("Keyer: " + keyer_.lastError());
    });
    page->signalAbortCw().connect([this]() {
        if (!keyer_.abort())
            setStatus("Keyer: " + keyer_.lastError());
    });
    page->signalLocator().connect([this, page](const std::string& g) {
        if (page == currentPage())  // only the visible tab drives the map
            mapPanel_.setTo(g);
    });

    // Row context menu "Move to": list every other open logbook, and perform
    // the move (add to the target, remove from this page) on request.
    page->queryMoveTargets = [this, page]() {
        std::vector<std::pair<std::string, LogPagePresenter*>> out;
        for (int i = 0; i < notebook_.get_n_pages(); ++i)
            if (auto* p = dynamic_cast<LogPage*>(notebook_.get_nth_page(i)); p && p != page)
                out.emplace_back(p->title(), &p->presenter());
        return out;
    };
    page->requestMove = [this, page](long id, LogPagePresenter* target) {
        moveQso(page, id, target);
    };

    auto* labelBox = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL);
    labelBox->set_spacing(4);
    auto* label = Gtk::make_managed<Gtk::Label>(page->title());
    auto* close = Gtk::make_managed<Gtk::Button>();
    close->set_has_frame(false);
    close->set_icon_name("window-close-symbolic");
    close->set_tooltip_text("Close tab");
    // Defer so the button isn't destroyed inside its own clicked handler.
    close->signal_clicked().connect([this, page]() {
        Glib::signal_idle().connect_once([this, page]() { closePage(page); });
    });
    labelBox->append(*label);
    labelBox->append(*close);
    tabLabels_[page] = label;

    const int idx = notebook_.append_page(*page, *labelBox);
    notebook_.set_current_page(idx);
    updateTabLabel(page);
}

void MainWindow::updateTabLabel(LogPage* page) {
    auto it = tabLabels_.find(page);
    if (it != tabLabels_.end())
        it->second->set_text(page->title() + " (" +
                             std::to_string(page->qsoCount()) + ")");
}

void MainWindow::onPageChanged(LogPage* page) {
    updateTabLabel(page);
    if (page == currentPage())
        updateTitle();
}

void MainWindow::closePage(LogPage* page) {
    const int idx = notebook_.page_num(*page);
    if (idx < 0)
        return;
    tabLabels_.erase(page);
    notebook_.remove_page(idx);
    if (notebook_.get_n_pages() == 0)
        openDefaultLog();
    updateTitle();
}

void MainWindow::moveQso(LogPage* from, long id, LogPagePresenter* target) {
    if (!from || !target)
        return;
    const Qso* q = from->presenter().findQso(id);
    if (!q)
        return;
    Qso copy = *q;
    copy.id = 0;  // the target assigns a fresh row id on insert
    const std::string call = copy.call;
    target->addExternalQso(copy);     // add + refresh + tab-label update
    from->presenter().deleteQso(id);  // remove from the source
    setStatus("Moved QSO with " + call + " to " + target->title() + ".");
}

LogPage* MainWindow::findPageByPath(const std::string& path) {
    for (int i = 0; i < notebook_.get_n_pages(); ++i)
        if (auto* p = dynamic_cast<LogPage*>(notebook_.get_nth_page(i)))
            if (p->isFileBacked() && p->path() == path)
                return p;
    return nullptr;
}

// --- menu actions ------------------------------------------------------------

void MainWindow::onNewTab() {
    addPage(Gtk::make_managed<LogPage>());
    setStatus("New in-memory log.");
}

void MainWindow::onOpen() {
    auto dialog = Gtk::FileDialog::create();
    dialog->set_title("Open logbook");
    dialog->set_filters(ui::makeFilters("xlog2 logbook", {"*.xlog", "*.db", "*.sqlite"}));
    dialog->open(*this, [this, dialog](const Glib::RefPtr<Gio::AsyncResult>& result) {
        try {
            auto file = dialog->open_finish(result);
            if (!file)
                return;
            const std::string path = file->get_path();
            if (auto* existing = findPageByPath(path)) {
                notebook_.set_current_page(notebook_.page_num(*existing));
                setStatus(path + " is already open.");
                return;
            }
            auto* page = addPage(Gtk::make_managed<LogPage>());
            if (page->openFile(path))
                setStatus("Opened " + path);
            else {
                closePage(page);
                setStatus("Could not open that file as a logbook.");
            }
        } catch (const Glib::Error&) {
            // dismissed
        }
    });
}

void MainWindow::onSaveAs() {
    auto* page = currentPage();
    if (!page)
        return;
    auto dialog = Gtk::FileDialog::create();
    dialog->set_title("Save logbook as");
    dialog->set_initial_name("logbook.xlog");
    dialog->save(*this, [this, dialog, page](const Glib::RefPtr<Gio::AsyncResult>& result) {
        try {
            auto file = dialog->save_finish(result);
            if (file && page->saveAs(file->get_path()))
                setStatus("Saved logbook to " + file->get_path());
            else if (file)
                setStatus("Could not save the logbook.");
        } catch (const Glib::Error&) {
        }
    });
}

void MainWindow::onCloseTab() {
    if (auto* page = currentPage())
        closePage(page);
}

void MainWindow::onImportAdif() {
    auto* page = currentPage();
    if (!page)
        return;
    auto dialog = Gtk::FileDialog::create();
    dialog->set_title("Import ADIF");
    dialog->set_filters(ui::makeFilters("ADIF files", {"*.adi", "*.adif", "*.ADI"}));
    dialog->open(*this, [this, dialog, page](const Glib::RefPtr<Gio::AsyncResult>& result) {
        try {
            auto file = dialog->open_finish(result);
            if (!file)
                return;
            const std::string content = Glib::file_get_contents(file->get_path());
            const int n = page->importAdif(content);
            setStatus("Imported " + std::to_string(n) + " QSO(s) from ADIF.");
        } catch (const Glib::Error& e) {
            setStatus(std::string("Import failed: ") + e.what());
        }
    });
}

void MainWindow::onImportXlog() {
    auto* page = currentPage();
    if (!page)
        return;
    auto dialog = Gtk::FileDialog::create();
    dialog->set_title("Import xlog log");
    dialog->set_filters(ui::makeFilters("xlog logs", {"*.xlog"}));
    dialog->open(*this, [this, dialog, page](const Glib::RefPtr<Gio::AsyncResult>& result) {
        try {
            auto file = dialog->open_finish(result);
            if (!file)
                return;
            const std::string content = Glib::file_get_contents(file->get_path());
            const int n = page->importXlog(content);
            setStatus("Imported " + std::to_string(n) + " QSO(s) from xlog.");
        } catch (const Glib::Error& e) {
            setStatus(std::string("Import failed: ") + e.what());
        }
    });
}

void MainWindow::onExportAdif() {
    auto* page = currentPage();
    if (!page)
        return;
    auto dialog = Gtk::FileDialog::create();
    dialog->set_title("Export ADIF");
    dialog->set_initial_name("export.adi");
    dialog->save(*this, [this, dialog, page](const Glib::RefPtr<Gio::AsyncResult>& result) {
        try {
            auto file = dialog->save_finish(result);
            if (!file)
                return;
            Glib::file_set_contents(file->get_path(), page->exportAdif());
            setStatus("Exported " + std::to_string(page->qsoCount()) + " QSO(s) to ADIF.");
        } catch (const Glib::Error& e) {
            setStatus(std::string("Export failed: ") + e.what());
        }
    });
}

void MainWindow::onStatistics() {
    auto* page = currentPage();
    if (!page)
        return;
    const stats::Statistics st = stats::compute(page->logbook().qsos());

    std::ostringstream os;
    os << "Logbook:       " << page->title().raw() << "\n";
    os << "Total QSOs:    " << st.total << "\n";
    os << "Unique calls:  " << st.uniqueCalls << "\n\n";
    os << "By band\n";
    if (st.byBand.empty()) os << "  (none)\n";
    for (const auto& [band, n] : st.byBand)
        os << "  " << band << ":  " << n << "\n";
    os << "\nBy mode\n";
    if (st.byMode.empty()) os << "  (none)\n";
    for (const auto& [mode, n] : st.byMode)
        os << "  " << mode << ":  " << n << "\n";

    auto* win = new Gtk::Window();
    win->set_transient_for(*this);
    win->set_modal(true);
    win->set_title("Statistics");
    win->set_default_size(320, 420);
    win->set_hide_on_close(true);
    auto* sc = Gtk::make_managed<Gtk::ScrolledWindow>();
    auto* label = Gtk::make_managed<Gtk::Label>(os.str());
    label->set_xalign(0.0);
    label->set_yalign(0.0);
    label->set_selectable(true);
    ui::setMargin(*label, 12);
    sc->set_child(*label);
    win->set_child(*sc);
    win->signal_hide().connect([win]() { delete win; });
    win->present();
}

void MainWindow::onFind() {
    if (auto* page = currentPage())
        page->beginSearch();
}

void MainWindow::onAbout() {
    auto* about = new Gtk::AboutDialog();
    about->set_transient_for(*this);
    about->set_modal(true);
    about->set_hide_on_close(true);
    about->set_program_name("xlog2");
    about->set_version(xlog::kVersion);
    about->set_comments(
        "A GTK4 amateur-radio logging program, in the spirit of xlog.\n"
        "Built with gtkmm-4, C++20, SQLite and Hamlib.");
    about->set_license_type(Gtk::License::GPL_3_0);
    about->set_authors({"xlog2 contributors"});
    about->signal_hide().connect([about]() { delete about; });
    about->present();
}

// --- UDP network logging -----------------------------------------------------

void MainWindow::onToggleUdp() {
    if (!listener_.isListening())
        startUdpListening();
    else
        stopUdpListening();
}

void MainWindow::startUdpListening() {
    std::string error;
    // cfg().udpEnabled tracks the user's intent and is what gets persisted, so it
    // survives onCloseRequest()'s socket teardown.
    cfg().udpEnabled = listener_.start(cfg().udpPort, error);
    udpAction_->change_state(cfg().udpEnabled);
    if (cfg().udpEnabled)
        setStatus("Listening for QSOs on UDP port " + std::to_string(cfg().udpPort) +
                  " (WSJT-X / ADIF).");
    else
        setStatus("Could not start UDP listener on port " +
                  std::to_string(cfg().udpPort) + ": " + error);
}

void MainWindow::stopUdpListening() {
    listener_.stop();
    cfg().udpEnabled = false;
    udpAction_->change_state(false);
    setStatus("Stopped UDP listener.");
}

// --- Hamlib rig control ------------------------------------------------------

void MainWindow::onRigConnect() {
    // Rig parameters are configured in Edit ▸ Settings; this just connects.
    // Non-blocking; the outcome arrives via rig_.onConnectResult.
    setStatus("Connecting to rig…");
    rig_.start(cfg().rigModel, cfg().rigDevice, cfg().rigPollMs);
}

void MainWindow::onRigDisconnect() {
    if (rig_.isRunning()) {
        rig_.stop();
        rigPanel_.setConnected(false);
        setStatus("Disconnected from rig.");
    } else {
        setStatus("No rig connected.");
    }
}

// --- IMainView: shell services for the presenter -----------------------------

LogPagePresenter* MainWindow::currentLog() {
    auto* page = currentPage();
    return page ? &page->presenter() : nullptr;
}

bool MainWindow::isLogLive(LogPagePresenter* log) {
    for (int i = 0; i < notebook_.get_n_pages(); ++i)
        if (auto* p = dynamic_cast<LogPage*>(notebook_.get_nth_page(i)))
            if (&p->presenter() == log)
                return true;
    return false;
}

// --- LoTW --------------------------------------------------------------------

void MainWindow::onLotwUpload() {
    auto* page = currentPage();
    if (!page)
        return;
    const auto unsent = page->qsosNotLotwSent();
    if (unsent.empty()) {
        setStatus("No new QSOs to upload to LoTW.");
        return;
    }

    const std::string tmp =
        Glib::build_filename(Glib::get_tmp_dir(), "xlog2-lotw-upload.adi");
    try {
        Glib::file_set_contents(tmp, adif::write(unsent));
    } catch (const Glib::Error&) {
        setStatus("Could not write a temporary ADIF file for upload.");
        return;
    }

    std::vector<long> ids;
    for (const auto& q : unsent)
        ids.push_back(q.id);
    presenter_.beginLotwUpload(&page->presenter(), std::move(ids));

    setStatus("Signing and uploading " + std::to_string(unsent.size()) +
              " QSO(s) via tqsl…");
    lotw_.uploadAdifFile(cfg().tqslPath, cfg().lotwStation, tmp);
}

void MainWindow::onLotwDownload() {
    if (cfg().lotwUser.empty() || cfg().lotwPassword.empty()) {
        setStatus("Set your LoTW username and password in LoTW ▸ Settings first.");
        return;
    }
    if (lotw_.isBusy()) {
        setStatus("A LoTW download is already in progress.");
        return;
    }
    setStatus("Downloading LoTW confirmations…");
    lotw_.downloadConfirmations(cfg().lotwUser, cfg().lotwPassword, cfg().lotwLastDownload);
}

// --- QRZ.com callsign lookup -------------------------------------------------

void MainWindow::onQrzLookup(LogPage* page, const std::string& callsign) {
    if (cfg().qrzUser.empty() || cfg().qrzPassword.empty()) {
        setStatus("Set your QRZ.com username and password in QRZ ▸ Settings first.");
        return;
    }
    if (qrz_.isBusy()) {
        setStatus("A QRZ lookup is already in progress.");
        return;
    }
    presenter_.beginQrzLookup(page ? &page->presenter() : nullptr);
    setStatus("Looking up " + callsign + " on QRZ.com…");
    qrz_.lookup(cfg().qrzUser, cfg().qrzPassword, callsign);
}

bool MainWindow::startQrzLookup(const std::string& callsign) {
    if (cfg().qrzUser.empty() || cfg().qrzPassword.empty() || qrz_.isBusy())
        return false;
    return qrz_.lookup(cfg().qrzUser, cfg().qrzPassword, callsign);
}

void MainWindow::onFillLocators() {
    auto* page = currentPage();
    if (!page)
        return;
    if (cfg().qrzUser.empty() || cfg().qrzPassword.empty()) {
        setStatus("Set your QRZ.com username and password in Edit ▸ Settings ▸ QRZ first.");
        return;
    }
    if (qrz_.isBusy()) {
        setStatus("A QRZ operation is already in progress.");
        return;
    }
    const std::vector<std::string> calls = page->presenter().callsignsMissingLocator();
    if (calls.empty()) {
        setStatus("No QSOs are missing a locator.");
        return;
    }
    presenter_.beginQrzLocatorFill(&page->presenter());
    setStatus("Filling locators for " + std::to_string(calls.size()) +
              " callsign(s) via QRZ…");
    qrz_.fillLocators(cfg().qrzUser, cfg().qrzPassword, calls);
}

// --- consolidated settings (Edit ▸ Settings) ---------------------------------

void MainWindow::onEditSettings() {
    auto* win = new SettingsDialog(
        cfg(), [this](const Settings& s) { applySettings(s); });
    win->set_transient_for(*this);
    win->signal_hide().connect([win]() { delete win; });
    win->present();
}

void MainWindow::applySettings(const Settings& s) {
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
    startSync();  // re-keys the coordinator from the resolved mesh id

    // Re-apply to any running service (rig/DX/LoTW/QRZ take effect on next use).
    applyKeyerConfig();
    if (listener_.isListening()) { listener_.stop(); startUdpListening(); }
    if (audio_.isStreaming()) startAudioStream();  // also re-syncs the skimmer rate
    if (paddle_.isActive()) startPaddleKeyer();

    // Skimmer detector params are live: push to both the service and the panel
    // controls so the panel reflects the dialog (these setters don't re-emit).
    skimmerPanel_.setGate(cfg().skimmerGate);
    skimmer_.setGate(static_cast<float>(cfg().skimmerGate));
    skimmerPanel_.setMinSnr(cfg().skimmerMinSnr);
    skimmer_.setMinSnr(static_cast<float>(cfg().skimmerMinSnr));
    skimmerPanel_.setKnownOnly(cfg().skimmerKnownOnly);
    skimmer_.setKnownCallsOnly(cfg().skimmerKnownOnly);
    skimmer_.setBandwidthNorm(cfg().skimmerBwNormDb, cfg().skimmerBwNormRefHz,
                              cfg().skimmerBwOffsetDb);

    mapPanel_.setFrom(cfg().myLocator);
    qrz_.setCache(Glib::build_filename(Glib::get_user_data_dir(), "xlog2",
                                       "qrz-cache.sqlite"),
                  cfg().qrzCacheDays);

    setStatus("Settings saved.");
}

void MainWindow::showQrzResult(const QrzResult& result) {
    auto* win = new Gtk::Window();
    win->set_transient_for(*this);
    win->set_title("QRZ.com — " + result.call);
    win->set_hide_on_close(true);
    win->set_default_size(380, 500);

    auto* box = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL);

    auto* scroller = Gtk::make_managed<Gtk::ScrolledWindow>();
    scroller->set_policy(Gtk::PolicyType::NEVER, Gtk::PolicyType::AUTOMATIC);
    scroller->set_vexpand(true);

    auto* grid = Gtk::make_managed<Gtk::Grid>();
    grid->set_row_spacing(4);
    grid->set_column_spacing(12);
    ui::setMargin(*grid, 12);

    int row = 0;
    for (const auto& [key, value] : result.fields) {
        auto* k = Gtk::make_managed<Gtk::Label>(key);
        k->set_xalign(0.0);
        k->set_yalign(0.0);
        k->add_css_class("dim-label");
        auto* v = Gtk::make_managed<Gtk::Label>(value);
        v->set_xalign(0.0);
        v->set_selectable(true);  // let the user copy values out
        v->set_wrap(true);
        v->set_hexpand(true);
        grid->attach(*k, 0, row);
        grid->attach(*v, 1, row);
        ++row;
    }
    if (row == 0)
        grid->attach(*Gtk::make_managed<Gtk::Label>("No fields returned."), 0, 0);
    scroller->set_child(*grid);
    box->append(*scroller);

    auto* close = Gtk::make_managed<Gtk::Button>("Close");
    close->set_halign(Gtk::Align::END);
    ui::setMargin(*close, 8);
    close->signal_clicked().connect([win]() { win->set_visible(false); });
    box->append(*close);

    win->set_child(*box);
    win->signal_hide().connect([win]() { delete win; });
    win->present();
}

// --- network keyer (cwdaemon) ------------------------------------------------

void MainWindow::applyKeyerConfig() {
    keyer_.setEndpoint(cfg().keyerHost, cfg().keyerPort);
    keyer_.setSpeed(cfg().keyerSpeed);
    for (int i = 0; i < notebook_.get_n_pages(); ++i)
        if (auto* p = dynamic_cast<LogPage*>(notebook_.get_nth_page(i)))
            p->setCwMessages(cfg().keyerMessages);
}

// --- rig audio stream (cwsd) -------------------------------------------------

void MainWindow::onToggleAudio() {
    if (!audio_.isStreaming())
        startAudioStream();
    else
        stopAudioStream();
}

void MainWindow::startAudioStream() {
    AudioStreamConfig ac;
    ac.host       = cfg().audioHost;
    ac.port       = cfg().audioPort;
    ac.sampleRate = cfg().audioSampleRate;
    ac.channels   = cfg().audioChannels;
    ac.device     = cfg().audioDevice;
    audio_.start(ac);
    // cfg().audioEnabled tracks the user's intent (what gets persisted), so it
    // survives onCloseRequest()'s teardown — mirrors the UDP listener.
    cfg().audioEnabled = true;
    audioAction_->change_state(true);
    if (skimmer_.isRunning())  // re-sync the skimmer to the (possibly new) rate
        startSkimmer();
}

void MainWindow::stopAudioStream() {
    audio_.stop();
    cfg().audioEnabled = false;
    audioAction_->change_state(false);
    audioIndicator_.set_text("");
}

// --- remote paddle keyer (cwsd remote_key) -----------------------------------

void MainWindow::onTogglePaddle() {
    if (!paddle_.isActive())
        startPaddleKeyer();
    else
        stopPaddleKeyer();
}

void MainWindow::startPaddleKeyer() {
    RemotePaddleConfig pc;
    pc.host     = cfg().paddleHost;
    pc.port     = cfg().paddlePort;
    pc.wpm      = cfg().paddleWpm;
    pc.iambicB  = cfg().paddleIambicB;
    pc.autospace = cfg().paddleAutospace;
    pc.muteTailMs = cfg().paddleMuteTailMs;
    pc.sidetone = cfg().paddleSidetone;
    pc.toneHz   = cfg().paddleToneHz;
    pc.level    = cfg().paddleLevel;
    pc.device   = cfg().paddleSidetoneDevice;
    paddle_.start(pc);
    hidPaddle_.start();          // also accept a USB HID paddle, if present
    cfg().paddleEnabled = true;  // user intent, persisted; survives teardown
    paddleAction_->change_state(true);
}

void MainWindow::stopPaddleKeyer() {
    hidPaddle_.stop();
    paddle_.stop();
    cfg().paddleEnabled = false;
    paddleAction_->change_state(false);
}

// --- DX cluster --------------------------------------------------------------

void MainWindow::applyDxDock() {
    // Clear both slots by setting the child properties to null — portable across
    // gtkmm versions (Paned::unset_start_child()/unset_end_child() are 4.16+,
    // but the PPA builds against gtkmm 4.14 on Ubuntu 24.04 LTS).
    paned_.property_start_child().set_value(nullptr);
    paned_.property_end_child().set_value(nullptr);
    const bool horizontal = (cfg().dxDock == "left" || cfg().dxDock == "right");
    paned_.set_orientation(horizontal ? Gtk::Orientation::HORIZONTAL
                                      : Gtk::Orientation::VERTICAL);
    const bool panelFirst = (cfg().dxDock == "top" || cfg().dxDock == "left");
    if (panelFirst) {
        paned_.set_start_child(dxPanel_);
        paned_.set_end_child(notebook_);
    } else {
        paned_.set_start_child(notebook_);
        paned_.set_end_child(dxPanel_);
    }
    // The log keeps the bulk of the space; the panel holds its own size.
    paned_.set_resize_start_child(!panelFirst);
    paned_.set_resize_end_child(panelFirst);
    paned_.set_shrink_start_child(false);
    paned_.set_shrink_end_child(false);
    dxPanel_.set_visible(cfg().dxVisible);
}

void MainWindow::applyDxConfig() {
    if (cfg().dxDock != "top" && cfg().dxDock != "bottom" &&
        cfg().dxDock != "left" && cfg().dxDock != "right")
        cfg().dxDock = "bottom";
    if (dxDockAction_)
        dxDockAction_->set_state(Glib::Variant<Glib::ustring>::create(cfg().dxDock));
    if (dxShowAction_)
        dxShowAction_->set_state(Glib::Variant<bool>::create(cfg().dxVisible));
    applyDxDock();
    // Restore the panel size (divider position). Honoured on first allocation;
    // the persisted window geometry + dock side make it reproduce the size.
    if (cfg().dxPanelPos > 0)
        paned_.set_position(cfg().dxPanelPos);
    if (cfg().dxAutoConnect && !cfg().dxHost.empty())
        cluster_.connectTo(cfg().dxHost, cfg().dxPort, cfg().dxLogin);
}

void MainWindow::onClusterToggleShow() {
    // The bool action has already toggled its own state; mirror it.
    bool state = false;
    dxShowAction_->get_state(state);
    cfg().dxVisible = state;
    dxPanel_.set_visible(cfg().dxVisible);
}

void MainWindow::onDxDock(const Glib::ustring& side) {
    cfg().dxDock = side.raw();
    dxDockAction_->set_state(Glib::Variant<Glib::ustring>::create(side));
    if (!cfg().dxVisible) {  // picking a dock implies wanting the panel shown
        cfg().dxVisible = true;
        dxShowAction_->set_state(Glib::Variant<bool>::create(true));
    }
    applyDxDock();
}

void MainWindow::applyRigDock() {
    // Mirror applyDxDock() on rigPaned_, whose "content" child is the entire
    // DX/notebook paned_. null-then-reparent is portable across gtkmm versions.
    rigPaned_.property_start_child().set_value(nullptr);
    rigPaned_.property_end_child().set_value(nullptr);
    const bool horizontal = (cfg().rigDock == "left" || cfg().rigDock == "right");
    rigPaned_.set_orientation(horizontal ? Gtk::Orientation::HORIZONTAL
                                         : Gtk::Orientation::VERTICAL);
    const bool panelFirst = (cfg().rigDock == "top" || cfg().rigDock == "left");
    if (panelFirst) {
        rigPaned_.set_start_child(rigPanel_);
        rigPaned_.set_end_child(paned_);
    } else {
        rigPaned_.set_start_child(paned_);
        rigPaned_.set_end_child(rigPanel_);
    }
    // The content keeps the bulk of the space; the panel holds its own size.
    rigPaned_.set_resize_start_child(!panelFirst);
    rigPaned_.set_resize_end_child(panelFirst);
    rigPaned_.set_shrink_start_child(false);
    rigPaned_.set_shrink_end_child(false);
    rigPanel_.set_visible(cfg().rigVisible);
}

void MainWindow::applyRigConfig() {
    if (cfg().rigDock != "top" && cfg().rigDock != "bottom" &&
        cfg().rigDock != "left" && cfg().rigDock != "right")
        cfg().rigDock = "right";
    if (rigDockAction_)
        rigDockAction_->set_state(Glib::Variant<Glib::ustring>::create(cfg().rigDock));
    if (rigShowAction_)
        rigShowAction_->set_state(Glib::Variant<bool>::create(cfg().rigVisible));
    applyRigDock();
    if (cfg().rigPanelPos > 0)
        rigPaned_.set_position(cfg().rigPanelPos);
}

void MainWindow::onRigToggleShow() {
    bool state = false;
    rigShowAction_->get_state(state);
    cfg().rigVisible = state;
    rigPanel_.set_visible(cfg().rigVisible);
}

void MainWindow::onRigDock(const Glib::ustring& side) {
    cfg().rigDock = side.raw();
    rigDockAction_->set_state(Glib::Variant<Glib::ustring>::create(side));
    if (!cfg().rigVisible) {  // picking a dock implies wanting the panel shown
        cfg().rigVisible = true;
        rigShowAction_->set_state(Glib::Variant<bool>::create(true));
    }
    applyRigDock();
}

// --- CW skimmer --------------------------------------------------------------

void MainWindow::applySkimmerDock() {
    // Mirror applyRigDock() on skimmerPaned_, whose "content" child is the entire
    // rig/DX/notebook area (rigPaned_).
    skimmerPaned_.property_start_child().set_value(nullptr);
    skimmerPaned_.property_end_child().set_value(nullptr);
    const bool horizontal = (cfg().skimmerDock == "left" || cfg().skimmerDock == "right");
    skimmerPaned_.set_orientation(horizontal ? Gtk::Orientation::HORIZONTAL
                                             : Gtk::Orientation::VERTICAL);
    const bool panelFirst = (cfg().skimmerDock == "top" || cfg().skimmerDock == "left");
    if (panelFirst) {
        skimmerPaned_.set_start_child(skimmerPanel_);
        skimmerPaned_.set_end_child(rigPaned_);
    } else {
        skimmerPaned_.set_start_child(rigPaned_);
        skimmerPaned_.set_end_child(skimmerPanel_);
    }
    skimmerPaned_.set_resize_start_child(!panelFirst);
    skimmerPaned_.set_resize_end_child(panelFirst);
    skimmerPaned_.set_shrink_start_child(false);
    skimmerPaned_.set_shrink_end_child(false);
    skimmerPanel_.set_visible(cfg().skimmerVisible);
}

void MainWindow::applySkimmerConfig() {
    if (cfg().skimmerDock != "top" && cfg().skimmerDock != "bottom" &&
        cfg().skimmerDock != "left" && cfg().skimmerDock != "right")
        cfg().skimmerDock = "left";
    if (skimmerDockAction_)
        skimmerDockAction_->set_state(Glib::Variant<Glib::ustring>::create(cfg().skimmerDock));
    if (skimmerShowAction_)
        skimmerShowAction_->set_state(Glib::Variant<bool>::create(cfg().skimmerVisible));
    applySkimmerDock();
    if (cfg().skimmerPanelPos > 0)
        skimmerPaned_.set_position(cfg().skimmerPanelPos);
    // Restore the gate + min-SNR levels (slider + service) before the skimmer starts.
    skimmerPanel_.setGate(cfg().skimmerGate);
    skimmer_.setGate(static_cast<float>(cfg().skimmerGate));
    skimmerPanel_.setMinSnr(cfg().skimmerMinSnr);
    skimmer_.setMinSnr(static_cast<float>(cfg().skimmerMinSnr));
    skimmerPanel_.setKnownOnly(cfg().skimmerKnownOnly);
    skimmer_.setKnownCallsOnly(cfg().skimmerKnownOnly);
    skimmer_.setBandwidthNorm(cfg().skimmerBwNormDb, cfg().skimmerBwNormRefHz,
                              cfg().skimmerBwOffsetDb);
    if (cfg().skimmerVisible)
        startSkimmer();
}

void MainWindow::onSkimmerToggleShow() {
    bool state = false;
    skimmerShowAction_->get_state(state);
    cfg().skimmerVisible = state;
    skimmerPanel_.set_visible(cfg().skimmerVisible);
    if (state)
        startSkimmer();
    else
        stopSkimmer();
}

void MainWindow::onSkimmerDock(const Glib::ustring& side) {
    cfg().skimmerDock = side.raw();
    skimmerDockAction_->set_state(Glib::Variant<Glib::ustring>::create(side));
    if (!cfg().skimmerVisible) {  // picking a dock implies wanting the panel shown
        cfg().skimmerVisible = true;
        skimmerShowAction_->set_state(Glib::Variant<bool>::create(true));
        startSkimmer();
    }
    applySkimmerDock();
}

void MainWindow::startSkimmer() {
    SkimmerConfig sc;
    sc.sampleRate = cfg().audioSampleRate;
    sc.channels   = cfg().audioChannels;
    skimmer_.start(sc);
}

void MainWindow::stopSkimmer() {
    skimmer_.stop();
    skimmerPanel_.clear();
}

// --- world map ---------------------------------------------------------------

void MainWindow::applyMapDock() {
    // Mirror applySkimmerDock() on mapPaned_, whose "content" child is the entire
    // skimmer/rig/DX/notebook area (skimmerPaned_).
    mapPaned_.property_start_child().set_value(nullptr);
    mapPaned_.property_end_child().set_value(nullptr);
    const bool horizontal = (cfg().mapDock == "left" || cfg().mapDock == "right");
    mapPaned_.set_orientation(horizontal ? Gtk::Orientation::HORIZONTAL
                                         : Gtk::Orientation::VERTICAL);
    const bool panelFirst = (cfg().mapDock == "top" || cfg().mapDock == "left");
    if (panelFirst) {
        mapPaned_.set_start_child(mapPanel_);
        mapPaned_.set_end_child(skimmerPaned_);
    } else {
        mapPaned_.set_start_child(skimmerPaned_);
        mapPaned_.set_end_child(mapPanel_);
    }
    mapPaned_.set_resize_start_child(!panelFirst);
    mapPaned_.set_resize_end_child(panelFirst);
    mapPaned_.set_shrink_start_child(false);
    mapPaned_.set_shrink_end_child(false);
    mapPanel_.set_visible(cfg().mapVisible);
}

void MainWindow::applyMapConfig() {
    if (cfg().mapDock != "top" && cfg().mapDock != "bottom" &&
        cfg().mapDock != "left" && cfg().mapDock != "right")
        cfg().mapDock = "right";
    if (mapDockAction_)
        mapDockAction_->set_state(Glib::Variant<Glib::ustring>::create(cfg().mapDock));
    if (mapShowAction_)
        mapShowAction_->set_state(Glib::Variant<bool>::create(cfg().mapVisible));
    applyMapDock();
    if (cfg().mapPanelPos > 0)
        mapPaned_.set_position(cfg().mapPanelPos);
    mapPanel_.setFrom(cfg().myLocator);  // seed the home point
}

void MainWindow::onMapToggleShow() {
    bool state = false;
    mapShowAction_->get_state(state);
    cfg().mapVisible = state;
    mapPanel_.set_visible(cfg().mapVisible);
}

void MainWindow::onMapDock(const Glib::ustring& side) {
    cfg().mapDock = side.raw();
    mapDockAction_->set_state(Glib::Variant<Glib::ustring>::create(side));
    if (!cfg().mapVisible) {  // picking a dock implies wanting the panel shown
        cfg().mapVisible = true;
        mapShowAction_->set_state(Glib::Variant<bool>::create(true));
    }
    applyMapDock();
}

void MainWindow::onClusterConnect() {
    if (cluster_.isConnected()) {
        cluster_.disconnect();
        return;
    }
    if (cfg().dxHost.empty()) {
        setStatus("Set a DX cluster host in Cluster ▸ Settings first.");
        return;
    }
    if (!cfg().dxVisible) {
        cfg().dxVisible = true;
        dxShowAction_->set_state(Glib::Variant<bool>::create(true));
        applyDxDock();
    }
    cluster_.connectTo(cfg().dxHost, cfg().dxPort, cfg().dxLogin);
}

// --- logbook sync ------------------------------------------------------------

void MainWindow::attachSyncedLog(LogPage* page) {
    syncedPage_ = page;
    if (!page) {
        coordinator_.detach();
        return;
    }
    coordinator_.attach(&page->presenter());
    page->presenter().onLocalUpsert = [this](const Qso& q) { coordinator_.onLocalUpsert(q); };
    page->presenter().onLocalDelete = [this](const std::string& u, const std::string& d) {
        coordinator_.onLocalDelete(u, d);
    };
}

void MainWindow::updateSyncIndicator() {
    if (!cfg().syncEnabled || !sync_.isRunning()) {
        syncIndicator_.set_text("");
        return;
    }
    const int n = sync_.memberCount();
    syncIndicator_.set_text(n > 0 ? "⇄ " + std::to_string(n) : "⇄ …");
}

void MainWindow::startSync() {
    if (syncEnableAction_)
        syncEnableAction_->set_state(Glib::Variant<bool>::create(cfg().syncEnabled));
    if (!cfg().syncEnabled) {
        sync_.stop();
        updateSyncIndicator();
        return;
    }
    LogbookSync::Config c;
    c.nodeId = cfg().syncNodeId;  // empty => the mesh mints one (persisted below)
    c.group  = syncproto::meshGroup(cfg().syncSecret);
    c.port   = cfg().syncPort;
    for (const std::string& h : {cfg().syncPeerHost, cfg().syncPeerHostAlt})
        if (auto p = LogbookSync::parsePeer(h, cfg().syncPort); !p.first.empty())
            c.staticPeers.push_back(p);
    sync_.start(c);
    if (!sync_.localId().empty())
        cfg().syncNodeId = sync_.localId();
    coordinator_.configure(cfg().syncNodeId, cfg().syncSecret);
    updateSyncIndicator();
}

void MainWindow::onSyncNow() {
    coordinator_.syncNow();
}

void MainWindow::onSpotActivated(const DxSpot& spot) {
    const double mhz = spot.freqKHz / 1000.0;
    if (auto* page = currentPage()) {
        page->applyDxSpot(spot.dxCall, mhz);
        // Prefill name/QTH/locator for the spotted call: cache-first, fetching
        // from QRZ (and caching) on a miss. Silent — no popup per double-click.
        if (!spot.dxCall.empty() && !qrz_.isBusy()) {
            presenter_.beginQrzLookup(&page->presenter(), /*silent=*/true);
            qrz_.lookup(cfg().qrzUser, cfg().qrzPassword, spot.dxCall);
        }
    }
    if (rig_.isRunning())
        rig_.setFrequency(mhz);  // tune the connected rig to the spot
}

// --- settings persistence ----------------------------------------------------

std::string MainWindow::layoutFilePath() const {
    return Glib::build_filename(Glib::get_user_config_dir(), "xlog2", "layout.ini");
}

bool MainWindow::onCloseRequest() {
    rig_.stop();
    listener_.stop();
    audio_.stop();      // joins the audio worker before the skimmer is torn down
    skimmer_.stop();
    paddle_.stop();
    cluster_.disconnect();  // close the socket while the panel is still alive
    saveSettings();
    return false;  // proceed with the default close (hide) handling
}

void MainWindow::saveSettings() {
    IniFile ini;
    ini.loadFromFile(layoutFilePath());  // preserve any groups we don't manage

    // Capture the live paned divider so Settings::store can persist it (it only
    // records the divider while the panel is shown).
    if (cfg().dxVisible) {
        const int pos = paned_.get_position();
        if (pos > 0)
            cfg().dxPanelPos = pos;
    }
    if (cfg().rigVisible) {
        const int pos = rigPaned_.get_position();
        if (pos > 0)
            cfg().rigPanelPos = pos;
    }
    if (cfg().skimmerVisible) {
        const int pos = skimmerPaned_.get_position();
        if (pos > 0)
            cfg().skimmerPanelPos = pos;
    }
    if (cfg().mapVisible) {
        const int pos = mapPaned_.get_position();
        if (pos > 0)
            cfg().mapPanelPos = pos;
    }
    cfg().store(ini);  // udp/rig/lotw/qrz/keyer/dxcluster scalars

    // Window geometry (no position under GTK4; avoid recording maximized size).
    if (!is_maximized()) {
        ini.setInt("window", "width", get_width());
        ini.setInt("window", "height", get_height());
    }
    ini.setBool("window", "maximized", is_maximized());

    // Shared column layout from the current page.
    if (auto* page = currentPage())
        page->storeColumnLayout(ini);

    // Session: file-backed tabs, in display order, plus the active one's path.
    std::string open;
    std::string active;
    for (int i = 0; i < notebook_.get_n_pages(); ++i) {
        auto* p = dynamic_cast<LogPage*>(notebook_.get_nth_page(i));
        if (p && p->isFileBacked()) {
            if (!open.empty())
                open += ';';
            open += p->path();
        }
    }
    if (auto* cur = currentPage(); cur && cur->isFileBacked())
        active = cur->path();
    ini.setString("session", "open", open);
    ini.setString("session", "active", active);

    try {
        Gio::File::create_for_path(Glib::path_get_dirname(layoutFilePath()))
            ->make_directory_with_parents();
    } catch (const Glib::Error&) {
    }
    try {
        Glib::file_set_contents(layoutFilePath(), ini.toString());
        // The file holds a plaintext LoTW password — restrict to the owner.
        ::chmod(layoutFilePath().c_str(), S_IRUSR | S_IWUSR);
    } catch (const Glib::Error&) {
    }
}

void MainWindow::loadSettings() {
    const bool loaded = settings_.loadFromFile(layoutFilePath());
    // Scalar config (udp/rig/lotw/qrz/keyer/dxcluster) is parsed by the core.
    presenter_.settings = Settings::load(settings_);

    if (loaded && settings_.hasGroup("window")) {
        if (settings_.hasKey("window", "width") && settings_.hasKey("window", "height")) {
            const int w = settings_.getInt("window", "width", 0);
            const int h = settings_.getInt("window", "height", 0);
            if (w > 0 && h > 0)
                set_default_size(w, h);
        }
        if (settings_.getBool("window", "maximized", false))
            maximize();
    }

    // Restore session tabs.
    std::vector<std::string> open;
    std::string active;
    if (loaded) {
        open   = ui::splitSemicolons(settings_.getString("session", "open"));
        active = settings_.getString("session", "active");
    }
    for (const auto& path : open) {
        auto* page = addPage(Gtk::make_managed<LogPage>());
        if (!page->openFile(path))
            closePage(page);
    }
    if (!active.empty())
        if (auto* p = findPageByPath(active))
            notebook_.set_current_page(notebook_.page_num(*p));

    // Open the keyer socket and push messages to the restored pages.
    applyKeyerConfig();

    // Apply the DX-cluster dock/visibility and optionally auto-connect.
    applyDxConfig();
    // Apply the rig-panel dock/visibility.
    applyRigConfig();
    // Apply the skimmer dock/visibility (starts the skimmer if it was shown).
    applySkimmerConfig();
    applyMapConfig();
}

void MainWindow::setStatus(const std::string& msg) {
    statusLabel_.set_text(msg);
}

void MainWindow::updateTitle() {
    auto* page = currentPage();
    const Glib::ustring base = Glib::ustring("xlog2 ") + xlog::kVersion;
    if (!page) {
        set_title(base);
        return;
    }
    set_title(base + " — " + page->title() + "  (" +
              std::to_string(page->qsoCount()) + " QSOs)");
}
