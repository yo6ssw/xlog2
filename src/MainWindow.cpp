#include "MainWindow.h"

#include "Bands.h"

#include <ctime>
#include <map>
#include <set>
#include <sstream>
#include <stdexcept>

namespace {

// Gtk::Widget has no "set all margins" call; this keeps the form tidy.
void setMargin(Gtk::Widget& w, int m) {
    w.set_margin_start(m);
    w.set_margin_end(m);
    w.set_margin_top(m);
    w.set_margin_bottom(m);
}

std::string entryText(const Gtk::Entry& e) {
    return e.get_text().raw();
}

std::string dropdownText(const Gtk::DropDown& d, const Glib::RefPtr<Gtk::StringList>& model) {
    const guint i = d.get_selected();
    if (i == GTK_INVALID_LIST_POSITION)
        return {};
    return model->get_string(i).raw();
}

void setDropdown(Gtk::DropDown& d, const Glib::RefPtr<Gtk::StringList>& model,
                 const std::string& value) {
    for (guint i = 0; i < model->get_n_items(); ++i) {
        if (model->get_string(i).raw() == value) {
            d.set_selected(i);
            return;
        }
    }
    d.set_selected(GTK_INVALID_LIST_POSITION);
}

std::string utcNow(const char* fmt) {
    std::time_t t = std::time(nullptr);
    std::tm tm{};
    gmtime_r(&t, &tm);
    char buf[32];
    std::strftime(buf, sizeof(buf), fmt, &tm);
    return buf;
}

Glib::RefPtr<Gio::ListStore<Gtk::FileFilter>> makeFilters(
    const Glib::ustring& name, const std::vector<Glib::ustring>& patterns) {
    auto filter = Gtk::FileFilter::create();
    filter->set_name(name);
    for (const auto& p : patterns)
        filter->add_pattern(p);
    auto list = Gio::ListStore<Gtk::FileFilter>::create();
    list->append(filter);
    return list;
}

} // namespace

MainWindow::MainWindow() {
    set_title("xlog2");
    set_default_size(1024, 700);
    // Hide (don't destroy) on close so XlogApplication's signal_hide handler
    // can delete us deterministically.
    set_hide_on_close(true);

    buildActions();

    auto* vbox = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL);
    set_child(*vbox);

    auto* menubar = Gtk::make_managed<Gtk::PopoverMenuBar>(buildMenuModel());
    vbox->append(*menubar);

    buildLogView();
    auto* scroller = Gtk::make_managed<Gtk::ScrolledWindow>();
    scroller->set_policy(Gtk::PolicyType::AUTOMATIC, Gtk::PolicyType::AUTOMATIC);
    scroller->set_child(columnView_);
    scroller->set_vexpand(true);
    vbox->append(*scroller);

    vbox->append(buildEntryForm());

    statusLabel_.set_xalign(0.0);
    setMargin(statusLabel_, 4);
    vbox->append(statusLabel_);

    refreshList();
    clearForm();
    updateTitle();
    setStatus("Ready. New in-memory log — use File ▸ Save As to store it.");
}

void MainWindow::buildActions() {
    add_action("new",    sigc::mem_fun(*this, &MainWindow::onNew));
    add_action("open",   sigc::mem_fun(*this, &MainWindow::onOpen));
    add_action("saveas", sigc::mem_fun(*this, &MainWindow::onSaveAs));
    add_action("import", sigc::mem_fun(*this, &MainWindow::onImportAdif));
    add_action("export", sigc::mem_fun(*this, &MainWindow::onExportAdif));
    add_action("stats",  sigc::mem_fun(*this, &MainWindow::onStatistics));
    add_action("about",  sigc::mem_fun(*this, &MainWindow::onAbout));
    add_action("quit",   sigc::mem_fun(*this, &MainWindow::close));
}

Glib::RefPtr<Gio::Menu> MainWindow::buildMenuModel() {
    auto menu = Gio::Menu::create();

    auto fileMenu = Gio::Menu::create();
    fileMenu->append("_New Log", "win.new");
    fileMenu->append("_Open…", "win.open");
    fileMenu->append("Save _As…", "win.saveas");
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

    auto helpMenu = Gio::Menu::create();
    helpMenu->append("_About xlog2", "win.about");
    menu->append_submenu("_Help", helpMenu);

    return menu;
}

Glib::RefPtr<Gtk::ColumnViewColumn> MainWindow::makeColumn(
    const Glib::ustring& title, std::function<std::string(const Qso&)> getter,
    bool expand) {
    auto factory = Gtk::SignalListItemFactory::create();
    factory->signal_setup().connect([](const Glib::RefPtr<Gtk::ListItem>& li) {
        auto* label = Gtk::make_managed<Gtk::Label>();
        label->set_xalign(0.0);
        li->set_child(*label);
    });
    factory->signal_bind().connect(
        [getter](const Glib::RefPtr<Gtk::ListItem>& li) {
            auto* label = dynamic_cast<Gtk::Label*>(li->get_child());
            auto* item = dynamic_cast<QsoItem*>(li->get_item().get());
            if (label && item)
                label->set_text(getter(item->qso));
        });

    auto column = Gtk::ColumnViewColumn::create(title, factory);
    column->set_resizable(true);
    column->set_expand(expand);
    return column;
}

void MainWindow::buildLogView() {
    store_ = Gio::ListStore<QsoItem>::create();
    selection_ = Gtk::SingleSelection::create(store_);
    selection_->set_autoselect(false);
    selection_->set_can_unselect(true);
    columnView_.set_model(selection_);
    columnView_.add_css_class("data-table");
    columnView_.set_show_column_separators(true);
    columnView_.set_show_row_separators(true);

    columnView_.append_column(makeColumn("Date",   [](const Qso& q) { return q.date; }));
    columnView_.append_column(makeColumn("On",     [](const Qso& q) { return q.time_on; }));
    columnView_.append_column(makeColumn("Off",    [](const Qso& q) { return q.time_off; }));
    columnView_.append_column(makeColumn("Call",   [](const Qso& q) { return q.call; }));
    columnView_.append_column(makeColumn("Band",   [](const Qso& q) { return q.band; }));
    columnView_.append_column(makeColumn("Mode",   [](const Qso& q) { return q.mode; }));
    columnView_.append_column(makeColumn("Freq",   [](const Qso& q) { return q.freq; }));
    columnView_.append_column(makeColumn("RST S",  [](const Qso& q) { return q.rst_sent; }));
    columnView_.append_column(makeColumn("RST R",  [](const Qso& q) { return q.rst_rcvd; }));
    columnView_.append_column(makeColumn("Name",   [](const Qso& q) { return q.name; }));
    columnView_.append_column(makeColumn("QTH",    [](const Qso& q) { return q.qth; }));
    columnView_.append_column(makeColumn("Loc",    [](const Qso& q) { return q.locator; }));
    columnView_.append_column(makeColumn("Pwr",    [](const Qso& q) { return q.power; }));
    columnView_.append_column(makeColumn("QSL",    [](const Qso& q) {
        std::string s = q.qsl_sent.empty() ? "-" : q.qsl_sent;
        std::string r = q.qsl_rcvd.empty() ? "-" : q.qsl_rcvd;
        return s + "/" + r;
    }));
    columnView_.append_column(makeColumn("Comment",
                                         [](const Qso& q) { return q.comment; }, true));

    selection_->property_selected().signal_changed().connect(
        sigc::mem_fun(*this, &MainWindow::onSelectionChanged));
}

Gtk::Widget& MainWindow::buildEntryForm() {
    auto* frame = Gtk::make_managed<Gtk::Frame>("QSO entry");
    setMargin(*frame, 6);

    auto* outer = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL);
    setMargin(*outer, 6);
    outer->set_spacing(6);
    frame->set_child(*outer);

    auto* grid = Gtk::make_managed<Gtk::Grid>();
    grid->set_row_spacing(4);
    grid->set_column_spacing(8);
    outer->append(*grid);

    // Populate the band/mode drop-downs from the reference tables.
    std::vector<Glib::ustring> bandStrings(bands::names().begin(), bands::names().end());
    std::vector<Glib::ustring> modeStrings(bands::modes().begin(), bands::modes().end());
    bandModel_ = Gtk::StringList::create(bandStrings);
    modeModel_ = Gtk::StringList::create(modeStrings);
    band_.set_model(bandModel_);
    mode_.set_model(modeModel_);

    qslSent_.set_label("QSL sent");
    qslRcvd_.set_label("QSL rcvd");

    // label + widget pairs; col is the logical column (0..2), each takes two
    // grid columns (label, field).
    auto field = [&](const char* text, Gtk::Widget& w, int col, int row) {
        auto* label = Gtk::make_managed<Gtk::Label>(text);
        label->set_xalign(1.0);
        grid->attach(*label, col * 2, row);
        w.set_hexpand(true);
        grid->attach(w, col * 2 + 1, row);
    };

    auto* nowButton = Gtk::make_managed<Gtk::Button>("Now");
    nowButton->signal_clicked().connect(sigc::mem_fun(*this, &MainWindow::onSetNow));

    field("Date",     date_,    0, 0);
    field("Time on",  timeOn_,  1, 0);
    field("Time off", timeOff_, 2, 0);
    grid->attach(*nowButton, 6, 0);

    field("Call",     call_,    0, 1);
    field("Band",     band_,    1, 1);
    field("Mode",     mode_,    2, 1);

    field("Freq MHz", freq_,    0, 2);
    field("RST sent", rstSent_, 1, 2);
    field("RST rcvd", rstRcvd_, 2, 2);

    field("Name",     name_,    0, 3);
    field("QTH",      qth_,     1, 3);
    field("Locator",  locator_, 2, 3);

    field("Power W",  power_,   0, 4);
    grid->attach(qslSent_, 3, 4);
    grid->attach(qslRcvd_, 5, 4);

    auto* commentLabel = Gtk::make_managed<Gtk::Label>("Comment");
    commentLabel->set_xalign(1.0);
    grid->attach(*commentLabel, 0, 5);
    comment_.set_hexpand(true);
    grid->attach(comment_, 1, 5, 5, 1);

    // Buttons
    auto* buttons = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL);
    buttons->set_spacing(6);
    buttons->set_halign(Gtk::Align::END);
    addButton_.set_label("Add QSO");
    deleteButton_.set_label("Delete");
    clearButton_.set_label("Clear");
    deleteButton_.set_sensitive(false);
    buttons->append(clearButton_);
    buttons->append(deleteButton_);
    buttons->append(addButton_);
    outer->append(*buttons);

    addButton_.signal_clicked().connect(sigc::mem_fun(*this, &MainWindow::onAddOrUpdate));
    deleteButton_.signal_clicked().connect(sigc::mem_fun(*this, &MainWindow::onDeleteSelected));
    clearButton_.signal_clicked().connect(sigc::mem_fun(*this, &MainWindow::clearForm));
    freq_.signal_changed().connect(sigc::mem_fun(*this, &MainWindow::onFrequencyChanged));
    call_.signal_activate().connect(sigc::mem_fun(*this, &MainWindow::onAddOrUpdate));

    return *frame;
}

void MainWindow::refreshList() {
    store_->remove_all();
    for (const auto& q : logbook_.qsos())
        store_->append(QsoItem::create(q));
    updateTitle();
}

void MainWindow::onSelectionChanged() {
    auto* item = dynamic_cast<QsoItem*>(selection_->get_selected_item().get());
    if (!item)
        return;  // deselection (e.g. after clearForm) — leave the form alone
    qsoToForm(item->qso);
    editingId_ = item->qso.id;
    addButton_.set_label("Update QSO");
    deleteButton_.set_sensitive(true);
}

Qso MainWindow::formToQso() const {
    Qso q;
    q.id       = editingId_;
    q.date     = entryText(date_);
    q.time_on  = entryText(timeOn_);
    q.time_off = entryText(timeOff_);
    q.call     = entryText(call_);
    q.band     = dropdownText(band_, bandModel_);
    q.mode     = dropdownText(mode_, modeModel_);
    q.freq     = entryText(freq_);
    q.rst_sent = entryText(rstSent_);
    q.rst_rcvd = entryText(rstRcvd_);
    q.name     = entryText(name_);
    q.qth      = entryText(qth_);
    q.locator  = entryText(locator_);
    q.power    = entryText(power_);
    q.qsl_sent = qslSent_.get_active() ? "Y" : "N";
    q.qsl_rcvd = qslRcvd_.get_active() ? "Y" : "N";
    q.comment  = entryText(comment_);

    // Uppercase the callsign and locator for consistency.
    for (auto& c : q.call) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    for (auto& c : q.locator) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    return q;
}

void MainWindow::qsoToForm(const Qso& q) {
    date_.set_text(q.date);
    timeOn_.set_text(q.time_on);
    timeOff_.set_text(q.time_off);
    call_.set_text(q.call);
    freq_.set_text(q.freq);  // set before band so it does not overwrite it
    setDropdown(band_, bandModel_, q.band);
    setDropdown(mode_, modeModel_, q.mode);
    rstSent_.set_text(q.rst_sent);
    rstRcvd_.set_text(q.rst_rcvd);
    name_.set_text(q.name);
    qth_.set_text(q.qth);
    locator_.set_text(q.locator);
    power_.set_text(q.power);
    qslSent_.set_active(q.qsl_sent == "Y");
    qslRcvd_.set_active(q.qsl_rcvd == "Y");
    comment_.set_text(q.comment);
}

void MainWindow::clearForm() {
    editingId_ = 0;
    for (Gtk::Entry* e : {&date_, &timeOn_, &timeOff_, &call_, &freq_, &rstSent_,
                          &rstRcvd_, &name_, &qth_, &locator_, &power_, &comment_})
        e->set_text("");
    band_.set_selected(GTK_INVALID_LIST_POSITION);
    mode_.set_selected(GTK_INVALID_LIST_POSITION);
    qslSent_.set_active(false);
    qslRcvd_.set_active(false);

    // Pre-fill date and time-on with the current UTC, like xlog does.
    date_.set_text(utcNow("%Y-%m-%d"));
    timeOn_.set_text(utcNow("%H:%M"));

    addButton_.set_label("Add QSO");
    deleteButton_.set_sensitive(false);
    selection_->unselect_all();
    call_.grab_focus();
}

void MainWindow::onAddOrUpdate() {
    Qso q = formToQso();
    if (q.call.empty()) {
        setStatus("Cannot log a QSO without a callsign.");
        return;
    }
    if (editingId_ != 0) {
        logbook_.update(q);
        setStatus("Updated QSO with " + q.call + ".");
    } else {
        logbook_.add(q);
        setStatus("Logged QSO with " + q.call + ".");
    }
    refreshList();
    clearForm();
}

void MainWindow::onDeleteSelected() {
    if (editingId_ == 0) {
        setStatus("No QSO selected to delete.");
        return;
    }
    logbook_.remove(editingId_);
    refreshList();
    clearForm();
    setStatus("QSO deleted.");
}

void MainWindow::onFrequencyChanged() {
    const std::string text = entryText(freq_);
    if (text.empty())
        return;
    try {
        const double mhz = std::stod(text);
        const std::string b = bands::forFrequencyMHz(mhz);
        if (!b.empty())
            setDropdown(band_, bandModel_, b);
    } catch (const std::exception&) {
        // not a number yet; ignore
    }
}

void MainWindow::onSetNow() {
    date_.set_text(utcNow("%Y-%m-%d"));
    timeOn_.set_text(utcNow("%H:%M"));
}

void MainWindow::onNew() {
    logbook_.newInMemory();
    refreshList();
    clearForm();
    updateTitle();
    setStatus("Started a new in-memory log.");
}

void MainWindow::onOpen() {
    auto dialog = Gtk::FileDialog::create();
    dialog->set_title("Open logbook");
    dialog->set_filters(makeFilters("xlog2 logbook", {"*.xlog", "*.db", "*.sqlite"}));
    dialog->open(*this, [this, dialog](const Glib::RefPtr<Gio::AsyncResult>& result) {
        try {
            auto file = dialog->open_finish(result);
            if (file && logbook_.open(file->get_path())) {
                refreshList();
                clearForm();
                updateTitle();
                setStatus("Opened " + file->get_path());
            } else if (file) {
                setStatus("Could not open that file as a logbook.");
            }
        } catch (const Glib::Error&) {
            // dismissed
        }
    });
}

void MainWindow::onSaveAs() {
    auto dialog = Gtk::FileDialog::create();
    dialog->set_title("Save logbook as");
    dialog->set_initial_name("logbook.xlog");
    dialog->save(*this, [this, dialog](const Glib::RefPtr<Gio::AsyncResult>& result) {
        try {
            auto file = dialog->save_finish(result);
            if (file && logbook_.saveAs(file->get_path())) {
                refreshList();
                updateTitle();
                setStatus("Saved logbook to " + file->get_path());
            } else if (file) {
                setStatus("Could not save the logbook.");
            }
        } catch (const Glib::Error&) {
        }
    });
}

void MainWindow::onImportAdif() {
    auto dialog = Gtk::FileDialog::create();
    dialog->set_title("Import ADIF");
    dialog->set_filters(makeFilters("ADIF files", {"*.adi", "*.adif", "*.ADI"}));
    dialog->open(*this, [this, dialog](const Glib::RefPtr<Gio::AsyncResult>& result) {
        try {
            auto file = dialog->open_finish(result);
            if (!file)
                return;
            const std::string content = Glib::file_get_contents(file->get_path());
            const int n = logbook_.importAdif(content);
            refreshList();
            setStatus("Imported " + std::to_string(n) + " QSO(s) from ADIF.");
        } catch (const Glib::Error& e) {
            setStatus(Glib::ustring("Import failed: ") + e.what());
        }
    });
}

void MainWindow::onExportAdif() {
    auto dialog = Gtk::FileDialog::create();
    dialog->set_title("Export ADIF");
    dialog->set_initial_name("export.adi");
    dialog->save(*this, [this, dialog](const Glib::RefPtr<Gio::AsyncResult>& result) {
        try {
            auto file = dialog->save_finish(result);
            if (!file)
                return;
            Glib::file_set_contents(file->get_path(), logbook_.exportAdif());
            setStatus("Exported " + std::to_string(logbook_.qsos().size()) +
                      " QSO(s) to ADIF.");
        } catch (const Glib::Error& e) {
            setStatus(Glib::ustring("Export failed: ") + e.what());
        }
    });
}

void MainWindow::onStatistics() {
    const auto& qsos = logbook_.qsos();
    std::map<std::string, int> byBand, byMode;
    std::set<std::string> calls;
    for (const auto& q : qsos) {
        if (!q.band.empty()) ++byBand[q.band];
        if (!q.mode.empty()) ++byMode[q.mode];
        if (!q.call.empty()) calls.insert(q.call);
    }

    std::ostringstream os;
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
    setMargin(*label, 12);
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
        "Built with gtkmm-4, C++20 and SQLite.");
    about->set_license_type(Gtk::License::GPL_3_0);
    about->set_authors({"xlog2 contributors"});
    about->signal_hide().connect([about]() { delete about; });
    about->present();
}

void MainWindow::setStatus(const Glib::ustring& msg) {
    statusLabel_.set_text(msg);
}

void MainWindow::updateTitle() {
    const std::string name =
        logbook_.isFileBacked() ? Glib::path_get_basename(logbook_.path()) : "Untitled";
    set_title("xlog2 — " + name + "  (" + std::to_string(logbook_.qsos().size()) +
              " QSOs)");
}
