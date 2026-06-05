#include "MainWindow.h"

#include "Adif.h"
#include "LogPage.h"
#include "Statistics.h"
#include "UiUtil.h"

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
      audio_(uiDispatcher_) {
    set_title("xlog2");
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

    auto* vbox = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL);
    set_child(*vbox);

    auto* menubar = Gtk::make_managed<Gtk::PopoverMenuBar>(buildMenuModel());
    vbox->append(*menubar);

    notebook_.set_scrollable(true);
    notebook_.set_vexpand(true);
    notebook_.signal_switch_page().connect(
        [this](Gtk::Widget* page, guint) {
            if (auto* lp = dynamic_cast<LogPage*>(page))
                set_title("xlog2 — " + lp->title() + "  (" +
                          std::to_string(lp->qsoCount()) + " QSOs)");
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
    vbox->append(paned_);
    applyDxDock();  // initial layout (defaults); reapplied after loadSettings

    cluster_.onSpot   = [this](const DxSpot& s) { dxPanel_.addSpot(s); };
    cluster_.onLine   = [this](const std::string& l) { dxPanel_.addLine(l); };
    cluster_.onStatus = [this](const std::string& s) {
        dxPanel_.addLine(s);
        dxPanel_.setConnected(cluster_.isConnected());
        setStatus(s);
    };

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
    vbox->append(*statusBox);

    // Service results are routed by the presenter (toolkit-neutral).
    listener_.setCallback(
        [this](const std::vector<Qso>& qsos, const std::string& source) {
            presenter_.routeUdp(qsos, source);
        });
    rig_.onUpdate = [this](double mhz, const std::string& mode) {
        presenter_.routeRigUpdate(mhz, mode);
    };
    rig_.onConnectResult = [this](bool ok, const std::string& error) {
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
    audio_.onStatus = [this](const std::string& s) { setStatus(s); };
    audio_.onStats  = [this](unsigned long frames) {
        audioIndicator_.set_text("♪ " + std::to_string(frames) + " frames");
    };

    loadSettings();
    if (notebook_.get_n_pages() == 0)
        openDefaultLog();

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
    add_action("about",  sigc::mem_fun(*this, &MainWindow::onAbout));
    add_action("quit",   sigc::mem_fun(*this, &MainWindow::close));

    udpAction_ = add_action_bool("udp", sigc::mem_fun(*this, &MainWindow::onToggleUdp), false);
    add_action("udpport", sigc::mem_fun(*this, &MainWindow::onUdpSettings));

    add_action("rigconnect",    sigc::mem_fun(*this, &MainWindow::onRigConnect));
    add_action("rigdisconnect", sigc::mem_fun(*this, &MainWindow::onRigDisconnect));

    add_action("lotwupload",   sigc::mem_fun(*this, &MainWindow::onLotwUpload));
    add_action("lotwdownload", sigc::mem_fun(*this, &MainWindow::onLotwDownload));
    add_action("lotwsettings", sigc::mem_fun(*this, &MainWindow::onLotwSettings));

    add_action("qrzsettings", sigc::mem_fun(*this, &MainWindow::onQrzSettings));

    add_action("keyersettings", sigc::mem_fun(*this, &MainWindow::onKeyerSettings));

    audioAction_ = add_action_bool("audio", sigc::mem_fun(*this, &MainWindow::onToggleAudio), false);
    add_action("audiosettings", sigc::mem_fun(*this, &MainWindow::onAudioSettings));

    dxShowAction_ = add_action_bool(
        "dxshow", sigc::mem_fun(*this, &MainWindow::onClusterToggleShow), false);
    add_action("dxconnect",  sigc::mem_fun(*this, &MainWindow::onClusterConnect));
    add_action("dxsettings", sigc::mem_fun(*this, &MainWindow::onClusterSettings));
    dxDockAction_ = add_action_radio_string(
        "dxdock", sigc::mem_fun(*this, &MainWindow::onDxDock), "bottom");
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

    auto logMenu = Gio::Menu::create();
    logMenu->append("_Find…", "win.find");
    logMenu->append("Fill _DXCC entities", "win.filldxcc");
    logMenu->append("_Statistics…", "win.stats");
    menu->append_submenu("_Log", logMenu);

    auto netMenu = Gio::Menu::create();
    netMenu->append("_Listen for QSOs (UDP)", "win.udp");
    netMenu->append("UDP _port…", "win.udpport");
    menu->append_submenu("_Network", netMenu);

    auto rigMenu = Gio::Menu::create();
    rigMenu->append("_Connect…", "win.rigconnect");
    rigMenu->append("_Disconnect", "win.rigdisconnect");
    menu->append_submenu("_Rig", rigMenu);

    auto lotwMenu = Gio::Menu::create();
    lotwMenu->append("_Upload to LoTW…", "win.lotwupload");
    lotwMenu->append("_Download confirmations", "win.lotwdownload");
    lotwMenu->append("_Settings…", "win.lotwsettings");
    menu->append_submenu("Lo_TW", lotwMenu);

    auto qrzMenu = Gio::Menu::create();
    qrzMenu->append("_Settings…", "win.qrzsettings");
    menu->append_submenu("_QRZ", qrzMenu);

    auto keyerMenu = Gio::Menu::create();
    keyerMenu->append("_Settings…", "win.keyersettings");
    menu->append_submenu("_Keyer", keyerMenu);

    auto audioMenu = Gio::Menu::create();
    audioMenu->append("_Play rig audio stream", "win.audio");
    audioMenu->append("_Settings…", "win.audiosettings");
    menu->append_submenu("_Audio", audioMenu);

    auto clusterMenu = Gio::Menu::create();
    clusterMenu->append("_Show panel", "win.dxshow");
    clusterMenu->append("_Connect / Disconnect", "win.dxconnect");
    auto dockMenu = Gio::Menu::create();
    dockMenu->append("_Top",    "win.dxdock::top");
    dockMenu->append("_Bottom", "win.dxdock::bottom");
    dockMenu->append("_Left",   "win.dxdock::left");
    dockMenu->append("_Right",  "win.dxdock::right");
    clusterMenu->append_submenu("_Dock", dockMenu);
    clusterMenu->append("S_ettings…", "win.dxsettings");
    menu->append_submenu("_Cluster", clusterMenu);

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
    about->set_version("0.1.0");
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

void MainWindow::onUdpSettings() {
    auto* win = new Gtk::Window();
    win->set_transient_for(*this);
    win->set_modal(true);
    win->set_title("UDP settings");
    win->set_hide_on_close(true);

    auto* box = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL);
    box->set_spacing(8);
    ui::setMargin(*box, 12);

    auto* row = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL);
    row->set_spacing(8);
    row->append(*Gtk::make_managed<Gtk::Label>("Listen port:"));
    auto* entry = Gtk::make_managed<Gtk::Entry>();
    entry->set_text(std::to_string(cfg().udpPort));
    entry->set_hexpand(true);
    row->append(*entry);
    box->append(*row);

    auto* hint = Gtk::make_managed<Gtk::Label>(
        "Receives WSJT-X \"Logged ADIF\" packets and raw ADIF datagrams.\n"
        "WSJT-X default is 2237.");
    hint->set_xalign(0.0);
    box->append(*hint);

    auto* apply = Gtk::make_managed<Gtk::Button>("Apply");
    apply->set_halign(Gtk::Align::END);
    box->append(*apply);

    win->set_child(*box);
    win->signal_hide().connect([win]() { delete win; });

    apply->signal_clicked().connect([this, entry, win]() {
        try {
            const int p = std::stoi(entry->get_text().raw());
            if (p > 0 && p < 65536) {
                cfg().udpPort = p;
                if (listener_.isListening()) {
                    listener_.stop();
                    startUdpListening();  // restart on the new port (updates state)
                } else {
                    setStatus("UDP port set to " + std::to_string(cfg().udpPort) + ".");
                }
            } else {
                setStatus("Port must be between 1 and 65535.");
            }
        } catch (const std::exception&) {
            setStatus("Invalid port number.");
        }
        win->set_visible(false);
    });

    win->present();
}

// --- Hamlib rig control ------------------------------------------------------

void MainWindow::onRigConnect() {
    auto* win = new Gtk::Window();
    win->set_transient_for(*this);
    win->set_modal(true);
    win->set_title("Connect to rig");
    win->set_hide_on_close(true);

    auto* grid = Gtk::make_managed<Gtk::Grid>();
    grid->set_row_spacing(6);
    grid->set_column_spacing(8);
    ui::setMargin(*grid, 12);

    auto* modelEntry = Gtk::make_managed<Gtk::Entry>();
    modelEntry->set_text(std::to_string(cfg().rigModel));
    auto* deviceEntry = Gtk::make_managed<Gtk::Entry>();
    deviceEntry->set_text(cfg().rigDevice);
    deviceEntry->set_placeholder_text("/dev/ttyUSB0");
    auto* pollEntry = Gtk::make_managed<Gtk::Entry>();
    pollEntry->set_text(std::to_string(cfg().rigPollMs));

    auto field = [&](const char* text, Gtk::Widget& w, int row) {
        auto* l = Gtk::make_managed<Gtk::Label>(text);
        l->set_xalign(1.0);
        grid->attach(*l, 0, row);
        w.set_hexpand(true);
        grid->attach(w, 1, row);
    };
    field("Hamlib model:", *modelEntry, 0);
    field("Device:",       *deviceEntry, 1);
    field("Poll (ms):",    *pollEntry, 2);
    auto* autoCheck = Gtk::make_managed<Gtk::CheckButton>("Connect automatically on startup");
    autoCheck->set_active(cfg().rigAutoConnect);
    grid->attach(*autoCheck, 1, 3);

    auto* hint = Gtk::make_managed<Gtk::Label>(
        "Model is a Hamlib rig id (e.g. 1 = dummy, 2 = NET rigctl).\n"
        "Find yours with `rigctl --list`. Device is ignored for the dummy.");
    hint->set_xalign(0.0);
    grid->attach(*hint, 0, 4, 2, 1);

    auto* connect = Gtk::make_managed<Gtk::Button>("Connect");
    connect->set_halign(Gtk::Align::END);
    grid->attach(*connect, 1, 5);

    win->set_child(*grid);
    win->signal_hide().connect([win]() { delete win; });

    connect->signal_clicked().connect(
        [this, modelEntry, deviceEntry, pollEntry, autoCheck, win]() {
            try {
                cfg().rigModel  = std::stoi(modelEntry->get_text().raw());
                cfg().rigDevice = deviceEntry->get_text().raw();
                cfg().rigPollMs = std::max(50, std::stoi(pollEntry->get_text().raw()));
            } catch (const std::exception&) {
                setStatus("Invalid rig settings.");
                return;
            }
            cfg().rigAutoConnect = autoCheck->get_active();
            // Non-blocking; the outcome arrives via rig_.onConnectResult.
            setStatus("Connecting to rig…");
            rig_.start(cfg().rigModel, cfg().rigDevice, cfg().rigPollMs);
            win->set_visible(false);
        });

    win->present();
}

void MainWindow::onRigDisconnect() {
    if (rig_.isRunning()) {
        rig_.stop();
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

void MainWindow::onLotwSettings() {
    auto* win = new Gtk::Window();
    win->set_transient_for(*this);
    win->set_modal(true);
    win->set_title("LoTW settings");
    win->set_hide_on_close(true);

    auto* grid = Gtk::make_managed<Gtk::Grid>();
    grid->set_row_spacing(6);
    grid->set_column_spacing(8);
    ui::setMargin(*grid, 12);

    auto* userEntry = Gtk::make_managed<Gtk::Entry>();
    userEntry->set_text(cfg().lotwUser);
    auto* passEntry = Gtk::make_managed<Gtk::Entry>();
    passEntry->set_text(cfg().lotwPassword);
    passEntry->set_visibility(false);
    auto* stationEntry = Gtk::make_managed<Gtk::Entry>();
    stationEntry->set_text(cfg().lotwStation);
    stationEntry->set_placeholder_text("tqsl station location (optional)");
    auto* tqslEntry = Gtk::make_managed<Gtk::Entry>();
    tqslEntry->set_text(cfg().tqslPath);

    auto field = [&](const char* text, Gtk::Widget& w, int row) {
        auto* l = Gtk::make_managed<Gtk::Label>(text);
        l->set_xalign(1.0);
        grid->attach(*l, 0, row);
        w.set_hexpand(true);
        grid->attach(w, 1, row);
    };
    field("LoTW username:", *userEntry, 0);
    field("LoTW password:", *passEntry, 1);
    field("Station location:", *stationEntry, 2);
    field("tqsl path:", *tqslEntry, 3);

    auto* hint = Gtk::make_managed<Gtk::Label>(
        "Username/password are for downloading confirmations and are stored in\n"
        "plain text in ~/.config/xlog2/layout.ini (mode 0600). Uploading uses\n"
        "the tqsl tool and your certificate — install tqsl and configure it once.");
    hint->set_xalign(0.0);
    grid->attach(*hint, 0, 4, 2, 1);

    auto* save = Gtk::make_managed<Gtk::Button>("Save");
    save->set_halign(Gtk::Align::END);
    grid->attach(*save, 1, 5);

    win->set_child(*grid);
    win->signal_hide().connect([win]() { delete win; });

    save->signal_clicked().connect(
        [this, userEntry, passEntry, stationEntry, tqslEntry, win]() {
            cfg().lotwUser     = userEntry->get_text().raw();
            cfg().lotwPassword = passEntry->get_text().raw();
            cfg().lotwStation  = stationEntry->get_text().raw();
            cfg().tqslPath     = tqslEntry->get_text().raw();
            if (cfg().tqslPath.empty())
                cfg().tqslPath = "tqsl";
            setStatus("LoTW settings saved.");
            win->set_visible(false);
        });

    win->present();
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

void MainWindow::onQrzSettings() {
    auto* win = new Gtk::Window();
    win->set_transient_for(*this);
    win->set_modal(true);
    win->set_title("QRZ.com settings");
    win->set_hide_on_close(true);

    auto* grid = Gtk::make_managed<Gtk::Grid>();
    grid->set_row_spacing(6);
    grid->set_column_spacing(8);
    ui::setMargin(*grid, 12);

    auto* userEntry = Gtk::make_managed<Gtk::Entry>();
    userEntry->set_text(cfg().qrzUser);
    auto* passEntry = Gtk::make_managed<Gtk::Entry>();
    passEntry->set_text(cfg().qrzPassword);
    passEntry->set_visibility(false);

    auto field = [&](const char* text, Gtk::Widget& w, int row) {
        auto* l = Gtk::make_managed<Gtk::Label>(text);
        l->set_xalign(1.0);
        grid->attach(*l, 0, row);
        w.set_hexpand(true);
        grid->attach(w, 1, row);
    };
    field("QRZ username:", *userEntry, 0);
    field("QRZ password:", *passEntry, 1);

    auto* hint = Gtk::make_managed<Gtk::Label>(
        "Used to look up callsign details via QRZ.com's XML service. Credentials\n"
        "are stored in plain text in ~/.config/xlog2/layout.ini (mode 0600).\n"
        "Click the search icon in the Call field to look up a callsign.");
    hint->set_xalign(0.0);
    grid->attach(*hint, 0, 2, 2, 1);

    auto* save = Gtk::make_managed<Gtk::Button>("Save");
    save->set_halign(Gtk::Align::END);
    grid->attach(*save, 1, 3);

    win->set_child(*grid);
    win->signal_hide().connect([win]() { delete win; });

    save->signal_clicked().connect([this, userEntry, passEntry, win]() {
        cfg().qrzUser     = userEntry->get_text().raw();
        cfg().qrzPassword = passEntry->get_text().raw();
        setStatus("QRZ.com settings saved.");
        win->set_visible(false);
    });

    win->present();
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

void MainWindow::onKeyerSettings() {
    auto* win = new Gtk::Window();
    win->set_transient_for(*this);
    win->set_modal(true);
    win->set_title("Network keyer (cwdaemon)");
    win->set_hide_on_close(true);

    auto* grid = Gtk::make_managed<Gtk::Grid>();
    grid->set_row_spacing(6);
    grid->set_column_spacing(8);
    ui::setMargin(*grid, 12);

    auto* hostEntry = Gtk::make_managed<Gtk::Entry>();
    hostEntry->set_text(cfg().keyerHost);
    auto* portEntry = Gtk::make_managed<Gtk::Entry>();
    portEntry->set_text(std::to_string(cfg().keyerPort));
    auto* speedEntry = Gtk::make_managed<Gtk::Entry>();
    speedEntry->set_text(cfg().keyerSpeed > 0 ? std::to_string(cfg().keyerSpeed) : "");
    speedEntry->set_placeholder_text("wpm (blank = leave cwdaemon default)");

    auto field = [&](const char* text, Gtk::Widget& w, int row) {
        auto* l = Gtk::make_managed<Gtk::Label>(text);
        l->set_xalign(1.0);
        grid->attach(*l, 0, row);
        w.set_hexpand(true);
        grid->attach(w, 1, row);
    };
    field("Host:", *hostEntry, 0);
    field("Port:", *portEntry, 1);
    field("Speed:", *speedEntry, 2);

    std::array<Gtk::Entry*, 9> msgEntries{};
    for (int i = 0; i < 9; ++i) {
        msgEntries[i] = Gtk::make_managed<Gtk::Entry>();
        msgEntries[i]->set_text(cfg().keyerMessages[i]);
        field(("F" + std::to_string(i + 1) + ":").c_str(), *msgEntries[i], 3 + i);
    }

    auto* hint = Gtk::make_managed<Gtk::Label>(
        "Sends CW to a cwdaemon over UDP. Messages may contain the tokens\n"
        "%CALL% %NAME% %QTH% %RST% (case-insensitive; %RST% = the RST rcvd\n"
        "field), substituted from the QSO entry form. Trigger with the F1–F9\n"
        "buttons or keys; Stop aborts the message being sent.");
    hint->set_xalign(0.0);
    grid->attach(*hint, 0, 12, 2, 1);

    auto* save = Gtk::make_managed<Gtk::Button>("Save");
    save->set_halign(Gtk::Align::END);
    grid->attach(*save, 1, 13);

    win->set_child(*grid);
    win->signal_hide().connect([win]() { delete win; });

    save->signal_clicked().connect(
        [this, hostEntry, portEntry, speedEntry, msgEntries, win]() {
            cfg().keyerHost = hostEntry->get_text().raw();
            if (cfg().keyerHost.empty())
                cfg().keyerHost = "127.0.0.1";
            try { cfg().keyerPort = std::stoi(portEntry->get_text().raw()); }
            catch (const std::exception&) { cfg().keyerPort = 6789; }
            try {
                const std::string s = speedEntry->get_text().raw();
                cfg().keyerSpeed = s.empty() ? 0 : std::stoi(s);
            } catch (const std::exception&) { cfg().keyerSpeed = 0; }
            for (int i = 0; i < 9; ++i)
                cfg().keyerMessages[i] = msgEntries[i]->get_text().raw();
            applyKeyerConfig();
            setStatus("Keyer settings saved.");
            win->set_visible(false);
        });

    win->present();
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
}

void MainWindow::stopAudioStream() {
    audio_.stop();
    cfg().audioEnabled = false;
    audioAction_->change_state(false);
    audioIndicator_.set_text("");
}

void MainWindow::onAudioSettings() {
    auto* win = new Gtk::Window();
    win->set_transient_for(*this);
    win->set_modal(true);
    win->set_title("Rig audio stream (cwsd)");
    win->set_hide_on_close(true);

    auto* grid = Gtk::make_managed<Gtk::Grid>();
    grid->set_row_spacing(6);
    grid->set_column_spacing(8);
    ui::setMargin(*grid, 12);

    auto* hostEntry = Gtk::make_managed<Gtk::Entry>();
    hostEntry->set_text(cfg().audioHost);
    auto* portEntry = Gtk::make_managed<Gtk::Entry>();
    portEntry->set_text(std::to_string(cfg().audioPort));
    auto* rateEntry = Gtk::make_managed<Gtk::Entry>();
    rateEntry->set_text(std::to_string(cfg().audioSampleRate));
    auto* chanEntry = Gtk::make_managed<Gtk::Entry>();
    chanEntry->set_text(std::to_string(cfg().audioChannels));
    auto* deviceEntry = Gtk::make_managed<Gtk::Entry>();
    deviceEntry->set_text(cfg().audioDevice);
    deviceEntry->set_placeholder_text("ALSA playback device, e.g. default");

    auto field = [&](const char* text, Gtk::Widget& w, int row) {
        auto* l = Gtk::make_managed<Gtk::Label>(text);
        l->set_xalign(1.0);
        grid->attach(*l, 0, row);
        w.set_hexpand(true);
        grid->attach(w, 1, row);
    };
    field("Host:", *hostEntry, 0);
    field("Port:", *portEntry, 1);
    field("Sample rate:", *rateEntry, 2);
    field("Channels:", *chanEntry, 3);
    field("Playback device:", *deviceEntry, 4);

    auto* hint = Gtk::make_managed<Gtk::Label>(
        "Plays a cwsd Opus-over-UDP rig-audio stream. The sample rate (an Opus\n"
        "rate: 8000/12000/16000/24000/48000) and channel count must match the\n"
        "cwsd `audio` section. cwsd's default port is 7355.");
    hint->set_xalign(0.0);
    grid->attach(*hint, 0, 5, 2, 1);

    auto* save = Gtk::make_managed<Gtk::Button>("Save");
    save->set_halign(Gtk::Align::END);
    grid->attach(*save, 1, 6);

    win->set_child(*grid);
    win->signal_hide().connect([win]() { delete win; });

    save->signal_clicked().connect(
        [this, hostEntry, portEntry, rateEntry, chanEntry, deviceEntry, win]() {
            cfg().audioHost = hostEntry->get_text().raw();
            if (cfg().audioHost.empty())
                cfg().audioHost = "127.0.0.1";
            try { cfg().audioPort = std::stoi(portEntry->get_text().raw()); }
            catch (const std::exception&) { cfg().audioPort = 7355; }
            try { cfg().audioSampleRate = std::stoi(rateEntry->get_text().raw()); }
            catch (const std::exception&) { cfg().audioSampleRate = 48000; }
            try { cfg().audioChannels = std::stoi(chanEntry->get_text().raw()); }
            catch (const std::exception&) { cfg().audioChannels = 1; }
            cfg().audioDevice = deviceEntry->get_text().raw();
            if (cfg().audioDevice.empty())
                cfg().audioDevice = "default";
            // Restart on the new settings if currently playing.
            if (audio_.isStreaming())
                startAudioStream();
            else
                setStatus("Rig audio stream settings saved.");
            win->set_visible(false);
        });

    win->present();
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

void MainWindow::onSpotActivated(const DxSpot& spot) {
    const double mhz = spot.freqKHz / 1000.0;
    if (auto* page = currentPage())
        page->applyDxSpot(spot.dxCall, mhz);
    if (rig_.isRunning())
        rig_.setFrequency(mhz);  // tune the connected rig to the spot
}

void MainWindow::onClusterSettings() {
    auto* win = new Gtk::Window();
    win->set_transient_for(*this);
    win->set_modal(true);
    win->set_title("DX cluster settings");
    win->set_hide_on_close(true);

    auto* grid = Gtk::make_managed<Gtk::Grid>();
    grid->set_row_spacing(6);
    grid->set_column_spacing(8);
    ui::setMargin(*grid, 12);

    auto* hostEntry = Gtk::make_managed<Gtk::Entry>();
    hostEntry->set_text(cfg().dxHost);
    hostEntry->set_placeholder_text("cluster.example.net");
    auto* portEntry = Gtk::make_managed<Gtk::Entry>();
    portEntry->set_text(std::to_string(cfg().dxPort));
    auto* loginEntry = Gtk::make_managed<Gtk::Entry>();
    loginEntry->set_text(cfg().dxLogin);
    loginEntry->set_placeholder_text("your callsign (sent at the login prompt)");
    auto* autoCheck = Gtk::make_managed<Gtk::CheckButton>("Connect automatically on startup");
    autoCheck->set_active(cfg().dxAutoConnect);

    auto field = [&](const char* text, Gtk::Widget& w, int row) {
        auto* l = Gtk::make_managed<Gtk::Label>(text);
        l->set_xalign(1.0);
        grid->attach(*l, 0, row);
        w.set_hexpand(true);
        grid->attach(w, 1, row);
    };
    field("Host:", *hostEntry, 0);
    field("Port:", *portEntry, 1);
    field("Login call:", *loginEntry, 2);
    grid->attach(*autoCheck, 1, 3);

    auto* save = Gtk::make_managed<Gtk::Button>("Save");
    save->set_halign(Gtk::Align::END);
    grid->attach(*save, 1, 4);

    win->set_child(*grid);
    win->signal_hide().connect([win]() { delete win; });

    save->signal_clicked().connect(
        [this, hostEntry, portEntry, loginEntry, autoCheck, win]() {
            cfg().dxHost = hostEntry->get_text().raw();
            try { cfg().dxPort = std::stoi(portEntry->get_text().raw()); }
            catch (const std::exception&) { cfg().dxPort = 7300; }
            cfg().dxLogin = loginEntry->get_text().raw();
            cfg().dxAutoConnect = autoCheck->get_active();
            setStatus("DX cluster settings saved.");
            win->set_visible(false);
        });

    win->present();
}

// --- settings persistence ----------------------------------------------------

std::string MainWindow::layoutFilePath() const {
    return Glib::build_filename(Glib::get_user_config_dir(), "xlog2", "layout.ini");
}

bool MainWindow::onCloseRequest() {
    rig_.stop();
    listener_.stop();
    audio_.stop();
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
}

void MainWindow::setStatus(const std::string& msg) {
    statusLabel_.set_text(msg);
}

void MainWindow::updateTitle() {
    auto* page = currentPage();
    const Glib::ustring name = page ? page->title() : "xlog2";
    const std::size_t n = page ? page->qsoCount() : 0;
    set_title("xlog2 — " + name + "  (" + std::to_string(n) + " QSOs)");
}
