#include "MainWindow.h"

#include "Adif.h"
#include "LogPage.h"
#include "UiUtil.h"

#include <sys/stat.h>

#include <algorithm>
#include <map>
#include <set>
#include <sstream>
#include <stdexcept>

MainWindow::MainWindow() {
    set_title("xlog2");
    set_default_size(1024, 700);
    // Hide (don't destroy) on close so XlogApplication's signal_hide handler
    // can delete us deterministically.
    set_hide_on_close(true);
    signal_close_request().connect(sigc::mem_fun(*this, &MainWindow::onCloseRequest), false);

    buildActions();

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

    statusLabel_.set_xalign(0.0);
    ui::setMargin(statusLabel_, 4);
    vbox->append(statusLabel_);

    listener_.setCallback(
        [this](const std::vector<Qso>& qsos, const std::string& source) {
            onUdpReceived(qsos, source);
        });
    rig_.onUpdate = [this](double mhz, const std::string& mode) {
        onRigUpdate(mhz, mode);
    };

    lotw_.onDownloadDone = [this](const std::string& adif, const std::string& error) {
        if (!error.empty()) {
            setStatus("LoTW download failed: " + error);
            return;
        }
        const auto records = adif::parse(adif);
        auto* page = currentPage();
        if (!page)
            return;
        const int n = page->applyLotwConfirmations(records);
        lotwLastDownload_ = ui::utcNow("%Y-%m-%d");
        if (records.empty())
            setStatus("LoTW: no confirmations returned (check username/password).");
        else
            setStatus("LoTW: " + std::to_string(records.size()) +
                      " record(s) downloaded, " + std::to_string(n) +
                      " QSO(s) newly confirmed.");
    };
    lotw_.onUploadDone = [this](bool ok, const std::string& message) {
        if (ok && pendingUploadPage_ && isLivePage(pendingUploadPage_))
            pendingUploadPage_->markLotwSent(pendingUploadIds_, ui::utcNow("%Y-%m-%d"));
        pendingUploadPage_ = nullptr;
        pendingUploadIds_.clear();
        setStatus(std::string("LoTW upload: ") + message);
    };

    qrz_.onResult = [this](const QrzResult& result, const std::string& error) {
        LogPage* page = pendingLookupPage_;
        pendingLookupPage_ = nullptr;
        if (!error.empty()) {
            setStatus("QRZ lookup: " + error);
            return;
        }
        if (page && isLivePage(page))
            page->applyQrzLookup(result);
        std::string msg = "QRZ: " + result.call;
        if (!result.name.empty())    msg += " — " + result.name;
        if (!result.country.empty()) msg += " (" + result.country + ")";
        setStatus(msg);
        showQrzResult(result);
    };

    loadSettings();
    if (notebook_.get_n_pages() == 0)
        openDefaultLog();

    updateTitle();
    setStatus("Ready.");

    // Resume listening for QSOs over UDP if it was enabled last session.
    if (udpEnabled_)
        startUdpListening();
}

void MainWindow::buildActions() {
    add_action("newtab", sigc::mem_fun(*this, &MainWindow::onNewTab));
    add_action("open",   sigc::mem_fun(*this, &MainWindow::onOpen));
    add_action("saveas", sigc::mem_fun(*this, &MainWindow::onSaveAs));
    add_action("closetab", sigc::mem_fun(*this, &MainWindow::onCloseTab));
    add_action("import", sigc::mem_fun(*this, &MainWindow::onImportAdif));
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
    page->setCwMessages(keyerMessages_);
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
    page->signalStatus().connect([this](const Glib::ustring& m) { setStatus(m); });
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
            setStatus(Glib::ustring("Import failed: ") + e.what());
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
            setStatus(Glib::ustring("Export failed: ") + e.what());
        }
    });
}

void MainWindow::onStatistics() {
    auto* page = currentPage();
    if (!page)
        return;
    const auto& qsos = page->logbook().qsos();
    std::map<std::string, int> byBand, byMode;
    std::set<std::string> calls;
    for (const auto& q : qsos) {
        if (!q.band.empty()) ++byBand[q.band];
        if (!q.mode.empty()) ++byMode[q.mode];
        if (!q.call.empty()) calls.insert(q.call);
    }

    std::ostringstream os;
    os << "Logbook:       " << page->title().raw() << "\n";
    os << "Total QSOs:    " << qsos.size() << "\n";
    os << "Unique calls:  " << calls.size() << "\n\n";
    os << "By band\n";
    if (byBand.empty()) os << "  (none)\n";
    for (const auto& [band, n] : byBand)
        os << "  " << band << ":  " << n << "\n";
    os << "\nBy mode\n";
    if (byMode.empty()) os << "  (none)\n";
    for (const auto& [mode, n] : byMode)
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
    // udpEnabled_ tracks the user's intent and is what gets persisted, so it
    // survives onCloseRequest()'s socket teardown.
    udpEnabled_ = listener_.start(udpPort_, error);
    udpAction_->change_state(udpEnabled_);
    if (udpEnabled_)
        setStatus("Listening for QSOs on UDP port " + std::to_string(udpPort_) +
                  " (WSJT-X / ADIF).");
    else
        setStatus("Could not start UDP listener on port " +
                  std::to_string(udpPort_) + ": " + error);
}

void MainWindow::stopUdpListening() {
    listener_.stop();
    udpEnabled_ = false;
    udpAction_->change_state(false);
    setStatus("Stopped UDP listener.");
}

void MainWindow::onUdpReceived(const std::vector<Qso>& qsos,
                               const std::string& source) {
    auto* page = currentPage();
    if (!page || qsos.empty())
        return;
    for (const auto& q : qsos)
        page->addExternalQso(q);
    setStatus("Logged " + std::to_string(qsos.size()) + " QSO(s) from " + source +
              ": " + qsos.back().call);
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
    entry->set_text(std::to_string(udpPort_));
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
                udpPort_ = p;
                if (listener_.isListening()) {
                    listener_.stop();
                    startUdpListening();  // restart on the new port (updates state)
                } else {
                    setStatus("UDP port set to " + std::to_string(udpPort_) + ".");
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
    modelEntry->set_text(std::to_string(rigModel_));
    auto* deviceEntry = Gtk::make_managed<Gtk::Entry>();
    deviceEntry->set_text(rigDevice_);
    deviceEntry->set_placeholder_text("/dev/ttyUSB0");
    auto* pollEntry = Gtk::make_managed<Gtk::Entry>();
    pollEntry->set_text(std::to_string(rigPollMs_));

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

    auto* hint = Gtk::make_managed<Gtk::Label>(
        "Model is a Hamlib rig id (e.g. 1 = dummy, 2 = NET rigctl).\n"
        "Find yours with `rigctl --list`. Device is ignored for the dummy.");
    hint->set_xalign(0.0);
    grid->attach(*hint, 0, 3, 2, 1);

    auto* connect = Gtk::make_managed<Gtk::Button>("Connect");
    connect->set_halign(Gtk::Align::END);
    grid->attach(*connect, 1, 4);

    win->set_child(*grid);
    win->signal_hide().connect([win]() { delete win; });

    connect->signal_clicked().connect(
        [this, modelEntry, deviceEntry, pollEntry, win]() {
            try {
                rigModel_  = std::stoi(modelEntry->get_text().raw());
                rigDevice_ = deviceEntry->get_text().raw();
                rigPollMs_ = std::max(50, std::stoi(pollEntry->get_text().raw()));
            } catch (const std::exception&) {
                setStatus("Invalid rig settings.");
                return;
            }
            if (rig_.start(rigModel_, rigDevice_, rigPollMs_))
                setStatus("Connected to rig (model " + std::to_string(rigModel_) +
                          "). Polling frequency.");
            else
                setStatus("Rig connect failed: " + rig_.lastError());
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

void MainWindow::onRigUpdate(double mhz, const std::string& mode) {
    if (auto* page = currentPage()) {
        page->setRigFrequency(mhz);
        page->setRigMode(mode);
    }
    std::ostringstream os;
    os << "Rig: " << mhz << " MHz";
    if (!mode.empty())
        os << " " << mode;
    setStatus(os.str());
}

// --- LoTW --------------------------------------------------------------------

bool MainWindow::isLivePage(LogPage* page) {
    for (int i = 0; i < notebook_.get_n_pages(); ++i)
        if (notebook_.get_nth_page(i) == page)
            return true;
    return false;
}

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

    pendingUploadIds_.clear();
    for (const auto& q : unsent)
        pendingUploadIds_.push_back(q.id);
    pendingUploadPage_ = page;

    setStatus("Signing and uploading " + std::to_string(unsent.size()) +
              " QSO(s) via tqsl…");
    lotw_.uploadAdifFile(tqslPath_, lotwStation_, tmp);
}

void MainWindow::onLotwDownload() {
    if (lotwUser_.empty() || lotwPassword_.empty()) {
        setStatus("Set your LoTW username and password in LoTW ▸ Settings first.");
        return;
    }
    if (lotw_.isBusy()) {
        setStatus("A LoTW download is already in progress.");
        return;
    }
    setStatus("Downloading LoTW confirmations…");
    lotw_.downloadConfirmations(lotwUser_, lotwPassword_, lotwLastDownload_);
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
    userEntry->set_text(lotwUser_);
    auto* passEntry = Gtk::make_managed<Gtk::Entry>();
    passEntry->set_text(lotwPassword_);
    passEntry->set_visibility(false);
    auto* stationEntry = Gtk::make_managed<Gtk::Entry>();
    stationEntry->set_text(lotwStation_);
    stationEntry->set_placeholder_text("tqsl station location (optional)");
    auto* tqslEntry = Gtk::make_managed<Gtk::Entry>();
    tqslEntry->set_text(tqslPath_);

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
            lotwUser_     = userEntry->get_text().raw();
            lotwPassword_ = passEntry->get_text().raw();
            lotwStation_  = stationEntry->get_text().raw();
            tqslPath_     = tqslEntry->get_text().raw();
            if (tqslPath_.empty())
                tqslPath_ = "tqsl";
            setStatus("LoTW settings saved.");
            win->set_visible(false);
        });

    win->present();
}

// --- QRZ.com callsign lookup -------------------------------------------------

void MainWindow::onQrzLookup(LogPage* page, const std::string& callsign) {
    if (qrzUser_.empty() || qrzPassword_.empty()) {
        setStatus("Set your QRZ.com username and password in QRZ ▸ Settings first.");
        return;
    }
    if (qrz_.isBusy()) {
        setStatus("A QRZ lookup is already in progress.");
        return;
    }
    pendingLookupPage_ = page;
    setStatus("Looking up " + callsign + " on QRZ.com…");
    qrz_.lookup(qrzUser_, qrzPassword_, callsign);
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
    userEntry->set_text(qrzUser_);
    auto* passEntry = Gtk::make_managed<Gtk::Entry>();
    passEntry->set_text(qrzPassword_);
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
        qrzUser_     = userEntry->get_text().raw();
        qrzPassword_ = passEntry->get_text().raw();
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
    keyer_.setEndpoint(keyerHost_, keyerPort_);
    keyer_.setSpeed(keyerSpeed_);
    for (int i = 0; i < notebook_.get_n_pages(); ++i)
        if (auto* p = dynamic_cast<LogPage*>(notebook_.get_nth_page(i)))
            p->setCwMessages(keyerMessages_);
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
    hostEntry->set_text(keyerHost_);
    auto* portEntry = Gtk::make_managed<Gtk::Entry>();
    portEntry->set_text(std::to_string(keyerPort_));
    auto* speedEntry = Gtk::make_managed<Gtk::Entry>();
    speedEntry->set_text(keyerSpeed_ > 0 ? std::to_string(keyerSpeed_) : "");
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
        msgEntries[i]->set_text(keyerMessages_[i]);
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
            keyerHost_ = hostEntry->get_text().raw();
            if (keyerHost_.empty())
                keyerHost_ = "127.0.0.1";
            try { keyerPort_ = std::stoi(portEntry->get_text().raw()); }
            catch (const std::exception&) { keyerPort_ = 6789; }
            try {
                const std::string s = speedEntry->get_text().raw();
                keyerSpeed_ = s.empty() ? 0 : std::stoi(s);
            } catch (const std::exception&) { keyerSpeed_ = 0; }
            for (int i = 0; i < 9; ++i)
                keyerMessages_[i] = msgEntries[i]->get_text().raw();
            applyKeyerConfig();
            setStatus("Keyer settings saved.");
            win->set_visible(false);
        });

    win->present();
}

// --- DX cluster --------------------------------------------------------------

void MainWindow::applyDxDock() {
    paned_.unset_start_child();
    paned_.unset_end_child();
    const bool horizontal = (dxDock_ == "left" || dxDock_ == "right");
    paned_.set_orientation(horizontal ? Gtk::Orientation::HORIZONTAL
                                      : Gtk::Orientation::VERTICAL);
    const bool panelFirst = (dxDock_ == "top" || dxDock_ == "left");
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
    dxPanel_.set_visible(dxVisible_);
}

void MainWindow::applyDxConfig() {
    if (dxDock_ != "top" && dxDock_ != "bottom" &&
        dxDock_ != "left" && dxDock_ != "right")
        dxDock_ = "bottom";
    if (dxDockAction_)
        dxDockAction_->set_state(Glib::Variant<Glib::ustring>::create(dxDock_));
    if (dxShowAction_)
        dxShowAction_->set_state(Glib::Variant<bool>::create(dxVisible_));
    applyDxDock();
    // Restore the panel size (divider position). Honoured on first allocation;
    // the persisted window geometry + dock side make it reproduce the size.
    if (dxPanelPos_ > 0)
        paned_.set_position(dxPanelPos_);
    if (dxAutoConnect_ && !dxHost_.empty())
        cluster_.connectTo(dxHost_, dxPort_, dxLogin_);
}

void MainWindow::onClusterToggleShow() {
    // The bool action has already toggled its own state; mirror it.
    bool state = false;
    dxShowAction_->get_state(state);
    dxVisible_ = state;
    dxPanel_.set_visible(dxVisible_);
}

void MainWindow::onDxDock(const Glib::ustring& side) {
    dxDock_ = side.raw();
    dxDockAction_->set_state(Glib::Variant<Glib::ustring>::create(side));
    if (!dxVisible_) {  // picking a dock implies wanting the panel shown
        dxVisible_ = true;
        dxShowAction_->set_state(Glib::Variant<bool>::create(true));
    }
    applyDxDock();
}

void MainWindow::onClusterConnect() {
    if (cluster_.isConnected()) {
        cluster_.disconnect();
        return;
    }
    if (dxHost_.empty()) {
        setStatus("Set a DX cluster host in Cluster ▸ Settings first.");
        return;
    }
    if (!dxVisible_) {
        dxVisible_ = true;
        dxShowAction_->set_state(Glib::Variant<bool>::create(true));
        applyDxDock();
    }
    cluster_.connectTo(dxHost_, dxPort_, dxLogin_);
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
    hostEntry->set_text(dxHost_);
    hostEntry->set_placeholder_text("cluster.example.net");
    auto* portEntry = Gtk::make_managed<Gtk::Entry>();
    portEntry->set_text(std::to_string(dxPort_));
    auto* loginEntry = Gtk::make_managed<Gtk::Entry>();
    loginEntry->set_text(dxLogin_);
    loginEntry->set_placeholder_text("your callsign (sent at the login prompt)");
    auto* autoCheck = Gtk::make_managed<Gtk::CheckButton>("Connect automatically on startup");
    autoCheck->set_active(dxAutoConnect_);

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
            dxHost_ = hostEntry->get_text().raw();
            try { dxPort_ = std::stoi(portEntry->get_text().raw()); }
            catch (const std::exception&) { dxPort_ = 7300; }
            dxLogin_ = loginEntry->get_text().raw();
            dxAutoConnect_ = autoCheck->get_active();
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
    cluster_.disconnect();  // close the socket while the panel is still alive
    saveSettings();
    return false;  // proceed with the default close (hide) handling
}

void MainWindow::saveSettings() {
    auto keyfile = Glib::KeyFile::create();
    try {
        keyfile->load_from_file(layoutFilePath());
    } catch (const Glib::Error&) {
    }

    // Window geometry (no position under GTK4; avoid recording maximized size).
    if (!is_maximized()) {
        keyfile->set_integer("window", "width", get_width());
        keyfile->set_integer("window", "height", get_height());
    }
    keyfile->set_boolean("window", "maximized", is_maximized());

    // Shared column layout from the current page.
    if (auto* page = currentPage())
        page->storeColumnLayout(keyfile);

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
    keyfile->set_string("session", "open", open);
    keyfile->set_string("session", "active", active);

    keyfile->set_integer("udp", "port", udpPort_);
    keyfile->set_boolean("udp", "enabled", udpEnabled_);
    keyfile->set_integer("rig", "model", rigModel_);
    keyfile->set_string("rig", "device", rigDevice_);
    keyfile->set_integer("rig", "poll_ms", rigPollMs_);

    keyfile->set_string("lotw", "username", lotwUser_);
    keyfile->set_string("lotw", "password", lotwPassword_);
    keyfile->set_string("lotw", "station_location", lotwStation_);
    keyfile->set_string("lotw", "tqsl_path", tqslPath_);
    keyfile->set_string("lotw", "last_download", lotwLastDownload_);

    keyfile->set_string("qrz", "username", qrzUser_);
    keyfile->set_string("qrz", "password", qrzPassword_);

    keyfile->set_string("keyer", "host", keyerHost_);
    keyfile->set_integer("keyer", "port", keyerPort_);
    keyfile->set_integer("keyer", "speed", keyerSpeed_);
    for (int i = 0; i < 9; ++i)
        keyfile->set_string("keyer", "message" + std::to_string(i + 1),
                            keyerMessages_[i]);

    keyfile->set_string("dxcluster", "host", dxHost_);
    keyfile->set_integer("dxcluster", "port", dxPort_);
    keyfile->set_string("dxcluster", "login", dxLogin_);
    keyfile->set_string("dxcluster", "dock", dxDock_);
    keyfile->set_boolean("dxcluster", "visible", dxVisible_);
    keyfile->set_boolean("dxcluster", "autoconnect", dxAutoConnect_);
    // Only record the divider while the panel is shown; a hidden panel's
    // position is meaningless and would clobber the last good value.
    if (dxVisible_) {
        const int pos = paned_.get_position();
        if (pos > 0)
            keyfile->set_integer("dxcluster", "position", pos);
    }

    try {
        Gio::File::create_for_path(Glib::path_get_dirname(layoutFilePath()))
            ->make_directory_with_parents();
    } catch (const Glib::Error&) {
    }
    try {
        Glib::file_set_contents(layoutFilePath(), keyfile->to_data());
        // The file holds a plaintext LoTW password — restrict to the owner.
        ::chmod(layoutFilePath().c_str(), S_IRUSR | S_IWUSR);
    } catch (const Glib::Error&) {
    }
}

void MainWindow::loadSettings() {
    settings_ = Glib::KeyFile::create();
    bool loaded = false;
    try {
        loaded = settings_->load_from_file(layoutFilePath());
    } catch (const Glib::Error&) {
        loaded = false;
    }

    if (loaded) {
        try {
            if (settings_->has_group("window")) {
                if (settings_->has_key("window", "width") &&
                    settings_->has_key("window", "height")) {
                    const int w = settings_->get_integer("window", "width");
                    const int h = settings_->get_integer("window", "height");
                    if (w > 0 && h > 0)
                        set_default_size(w, h);
                }
                if (settings_->has_key("window", "maximized") &&
                    settings_->get_boolean("window", "maximized"))
                    maximize();
            }
            if (settings_->has_group("udp")) {
                if (settings_->has_key("udp", "port"))
                    udpPort_ = settings_->get_integer("udp", "port");
                if (settings_->has_key("udp", "enabled"))
                    udpEnabled_ = settings_->get_boolean("udp", "enabled");
            }
            if (settings_->has_group("rig")) {
                if (settings_->has_key("rig", "model"))
                    rigModel_ = settings_->get_integer("rig", "model");
                if (settings_->has_key("rig", "device"))
                    rigDevice_ = settings_->get_string("rig", "device").raw();
                if (settings_->has_key("rig", "poll_ms"))
                    rigPollMs_ = settings_->get_integer("rig", "poll_ms");
            }
            if (settings_->has_group("lotw")) {
                if (settings_->has_key("lotw", "username"))
                    lotwUser_ = settings_->get_string("lotw", "username").raw();
                if (settings_->has_key("lotw", "password"))
                    lotwPassword_ = settings_->get_string("lotw", "password").raw();
                if (settings_->has_key("lotw", "station_location"))
                    lotwStation_ = settings_->get_string("lotw", "station_location").raw();
                if (settings_->has_key("lotw", "tqsl_path"))
                    tqslPath_ = settings_->get_string("lotw", "tqsl_path").raw();
                if (settings_->has_key("lotw", "last_download"))
                    lotwLastDownload_ = settings_->get_string("lotw", "last_download").raw();
            }
            if (settings_->has_group("qrz")) {
                if (settings_->has_key("qrz", "username"))
                    qrzUser_ = settings_->get_string("qrz", "username").raw();
                if (settings_->has_key("qrz", "password"))
                    qrzPassword_ = settings_->get_string("qrz", "password").raw();
            }
            if (settings_->has_group("keyer")) {
                if (settings_->has_key("keyer", "host"))
                    keyerHost_ = settings_->get_string("keyer", "host").raw();
                if (settings_->has_key("keyer", "port"))
                    keyerPort_ = settings_->get_integer("keyer", "port");
                if (settings_->has_key("keyer", "speed"))
                    keyerSpeed_ = settings_->get_integer("keyer", "speed");
                for (int i = 0; i < 9; ++i) {
                    const std::string key = "message" + std::to_string(i + 1);
                    if (settings_->has_key("keyer", key))
                        keyerMessages_[i] = settings_->get_string("keyer", key).raw();
                }
            }
            if (settings_->has_group("dxcluster")) {
                if (settings_->has_key("dxcluster", "host"))
                    dxHost_ = settings_->get_string("dxcluster", "host").raw();
                if (settings_->has_key("dxcluster", "port"))
                    dxPort_ = settings_->get_integer("dxcluster", "port");
                if (settings_->has_key("dxcluster", "login"))
                    dxLogin_ = settings_->get_string("dxcluster", "login").raw();
                if (settings_->has_key("dxcluster", "dock"))
                    dxDock_ = settings_->get_string("dxcluster", "dock").raw();
                if (settings_->has_key("dxcluster", "visible"))
                    dxVisible_ = settings_->get_boolean("dxcluster", "visible");
                if (settings_->has_key("dxcluster", "autoconnect"))
                    dxAutoConnect_ = settings_->get_boolean("dxcluster", "autoconnect");
                if (settings_->has_key("dxcluster", "position"))
                    dxPanelPos_ = settings_->get_integer("dxcluster", "position");
            }
        } catch (const Glib::Error&) {
        }
    }

    // Restore session tabs.
    std::vector<std::string> open;
    std::string active;
    if (loaded) {
        try {
            if (settings_->has_group("session")) {
                if (settings_->has_key("session", "open"))
                    open = ui::splitSemicolons(settings_->get_string("session", "open"));
                if (settings_->has_key("session", "active"))
                    active = settings_->get_string("session", "active").raw();
            }
        } catch (const Glib::Error&) {
        }
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

void MainWindow::setStatus(const Glib::ustring& msg) {
    statusLabel_.set_text(msg);
}

void MainWindow::updateTitle() {
    auto* page = currentPage();
    const Glib::ustring name = page ? page->title() : "xlog2";
    const std::size_t n = page ? page->qsoCount() : 0;
    set_title("xlog2 — " + name + "  (" + std::to_string(n) + " QSOs)");
}
