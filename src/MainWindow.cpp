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
    vbox->append(notebook_);

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
    add_action("about",  sigc::mem_fun(*this, &MainWindow::onAbout));
    add_action("quit",   sigc::mem_fun(*this, &MainWindow::close));

    udpAction_ = add_action_bool("udp", sigc::mem_fun(*this, &MainWindow::onToggleUdp), false);
    add_action("udpport", sigc::mem_fun(*this, &MainWindow::onUdpSettings));

    add_action("rigconnect",    sigc::mem_fun(*this, &MainWindow::onRigConnect));
    add_action("rigdisconnect", sigc::mem_fun(*this, &MainWindow::onRigDisconnect));

    add_action("lotwupload",   sigc::mem_fun(*this, &MainWindow::onLotwUpload));
    add_action("lotwdownload", sigc::mem_fun(*this, &MainWindow::onLotwDownload));
    add_action("lotwsettings", sigc::mem_fun(*this, &MainWindow::onLotwSettings));
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

// --- settings persistence ----------------------------------------------------

std::string MainWindow::layoutFilePath() const {
    return Glib::build_filename(Glib::get_user_config_dir(), "xlog2", "layout.ini");
}

bool MainWindow::onCloseRequest() {
    rig_.stop();
    listener_.stop();
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
