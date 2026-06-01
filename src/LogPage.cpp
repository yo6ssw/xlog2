#include "LogPage.h"

#include "Bands.h"
#include "Dxcc.h"
#include "UiUtil.h"

#include <gdk/gdkkeysyms.h>

#include <cctype>
#include <cstdio>
#include <set>
#include <stdexcept>

namespace {

// Install the application's CSS once: the dupe-highlight styling and a raised
// (beveled) look for the log table's column headers. Scoped to the
// "data-table" class so only the log view is affected; theme colours keep it
// working in light and dark themes.
void ensureCss() {
    static bool installed = false;
    if (installed)
        return;
    installed = true;
    auto provider = Gtk::CssProvider::create();
    provider->load_from_string(
        ".dupe-warning { color: #c01c28; font-weight: bold; }\n"
        "entry.dupe { background-image: none; background-color: #f7d4d4; }\n"
        "columnview.data-table > header > button {\n"
        "  border: 1px solid @borders;\n"
        "  border-radius: 0;\n"
        "  background-image: linear-gradient(to bottom,\n"
        "      shade(@theme_bg_color, 1.06), shade(@theme_bg_color, 0.92));\n"
        "  box-shadow: inset 0 1px shade(@theme_bg_color, 1.18),\n"
        "              inset 0 -1px shade(@theme_bg_color, 0.82);\n"
        "  padding: 2px 8px;\n"
        "}\n"
        "columnview.data-table > header > button:hover {\n"
        "  background-image: linear-gradient(to bottom,\n"
        "      shade(@theme_bg_color, 1.10), shade(@theme_bg_color, 0.96));\n"
        "}\n"
        "columnview.data-table > header > button:active {\n"
        "  background-image: linear-gradient(to bottom,\n"
        "      shade(@theme_bg_color, 0.90), shade(@theme_bg_color, 1.02));\n"
        "  box-shadow: inset 0 1px shade(@theme_bg_color, 0.70);\n"
        "}\n");
    if (auto display = Gdk::Display::get_default())
        Gtk::StyleContext::add_provider_for_display(
            display, provider, GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
}

// Format MHz without trailing zeros, e.g. 14.250000 -> "14.25".
std::string formatMhz(double mhz) {
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%.6f", mhz);
    std::string s = buf;
    if (s.find('.') != std::string::npos) {
        while (!s.empty() && s.back() == '0')
            s.pop_back();
        if (!s.empty() && s.back() == '.')
            s.pop_back();
    }
    return s;
}

} // namespace

LogPage::LogPage() : Gtk::Box(Gtk::Orientation::VERTICAL) {
    ensureCss();

    buildLogView();
    buildSearch();
    append(searchBar_);

    auto* scroller = Gtk::make_managed<Gtk::ScrolledWindow>();
    scroller->set_policy(Gtk::PolicyType::AUTOMATIC, Gtk::PolicyType::AUTOMATIC);
    scroller->set_child(columnView_);
    scroller->set_vexpand(true);
    append(*scroller);

    buildEntryForm();

    refreshList();
    clearForm();
}

Glib::RefPtr<Gtk::ColumnViewColumn> LogPage::makeColumn(
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

void LogPage::buildLogView() {
    store_ = Gio::ListStore<QsoItem>::create();
    // store_ -> filter (search) -> selection -> view. The StringFilter
    // substring-matches (case-insensitively, by default) the per-row text
    // produced by rowSearchText().
    filter_ = Gtk::StringFilter::create(Gtk::ClosureExpression<Glib::ustring>::create(
        sigc::mem_fun(*this, &LogPage::rowSearchText)));
    filterModel_ = Gtk::FilterListModel::create(store_, filter_);
    selection_ = Gtk::SingleSelection::create(filterModel_);
    selection_->set_autoselect(false);
    selection_->set_can_unselect(true);
    columnView_.set_model(selection_);
    columnView_.add_css_class("data-table");
    columnView_.set_show_column_separators(true);
    columnView_.set_show_row_separators(true);

    auto add = [&](const std::string& id, const Glib::ustring& title,
                   std::function<std::string(const Qso&)> getter, bool expand = false) {
        auto col = makeColumn(title, std::move(getter), expand);
        col->set_id(id);
        columns_.emplace_back(id, col);
        columnView_.append_column(col);
    };

    add("date", "Date", [](const Qso& q) { return q.date; });
    add("on",   "On",   [](const Qso& q) { return q.time_on; });
    add("off",  "Off",  [](const Qso& q) { return q.time_off; });
    add("call", "Call", [](const Qso& q) { return q.call; });
    add("band", "Band", [](const Qso& q) { return q.band; });
    add("mode", "Mode", [](const Qso& q) { return q.mode; });
    add("freq", "Freq", [](const Qso& q) { return q.freq; });
    add("rst_s", "RST S", [](const Qso& q) { return q.rst_sent; });
    add("rst_r", "RST R", [](const Qso& q) { return q.rst_rcvd; });
    add("name", "Name", [](const Qso& q) { return q.name; });
    add("qth",  "QTH",  [](const Qso& q) { return q.qth; });
    add("loc",  "Loc",  [](const Qso& q) { return q.locator; });
    add("pwr",  "Pwr",  [](const Qso& q) { return q.power; });
    add("qsl",  "QSL",  [](const Qso& q) {
        std::string s = q.qsl_sent.empty() ? "-" : q.qsl_sent;
        std::string r = q.qsl_rcvd.empty() ? "-" : q.qsl_rcvd;
        return s + "/" + r;
    });
    add("lotw", "LoTW", [](const Qso& q) -> std::string {
        if (q.lotw_rcvd == "Y") return "✓";  // ✓ confirmed
        if (q.lotw_sent == "Y") return "↑";  // ↑ uploaded
        return "-";
    });
    add("country", "Country", [](const Qso& q) { return q.country; });
    add("cqz",     "CQ",      [](const Qso& q) { return q.cq_zone; });
    add("comment", "Comment", [](const Qso& q) { return q.comment; });

    // Empty trailing column that soaks up any leftover width. Its factory
    // builds no child, so cells stay blank; it carries no id/header menu and
    // is kept out of columns_, so reorder/hide/persistence ignore it.
    filler_ = Gtk::ColumnViewColumn::create("", Gtk::SignalListItemFactory::create());
    filler_->set_expand(true);
    columnView_.append_column(filler_);

    selection_->property_selected().signal_changed().connect(
        sigc::mem_fun(*this, &LogPage::onSelectionChanged));

    columnView_.set_reorderable(true);  // drag headers to reorder
    buildColumnMenus();                 // right-click header for explicit reorder/hide
}

void LogPage::buildColumnMenus() {
    colActions_ = Gio::SimpleActionGroup::create();
    const auto strType = Glib::Variant<Glib::ustring>::variant_type();

    auto addMove = [&](const char* name, int delta) {
        auto a = Gio::SimpleAction::create(name, strType);
        a->signal_activate().connect([this, delta](const Glib::VariantBase& p) {
            const auto id =
                Glib::VariantBase::cast_dynamic<Glib::Variant<Glib::ustring>>(p).get();
            moveColumn(id, delta);
        });
        colActions_->add_action(a);
    };
    addMove("move-left", -1);
    addMove("move-right", +1);

    auto hide = Gio::SimpleAction::create("hide", strType);
    hide->signal_activate().connect([this](const Glib::VariantBase& p) {
        setColumnVisible(
            Glib::VariantBase::cast_dynamic<Glib::Variant<Glib::ustring>>(p).get(), false);
    });
    colActions_->add_action(hide);

    auto show = Gio::SimpleAction::create("show", strType);
    show->signal_activate().connect([this](const Glib::VariantBase& p) {
        setColumnVisible(
            Glib::VariantBase::cast_dynamic<Glib::Variant<Glib::ustring>>(p).get(), true);
    });
    colActions_->add_action(show);

    auto showAll = Gio::SimpleAction::create("show-all");
    showAll->signal_activate().connect(
        [this](const Glib::VariantBase&) { showAllColumns(); });
    colActions_->add_action(showAll);

    insert_action_group("cols", colActions_);

    // Shared "Show Column" submenu listing every column, so any hidden column
    // can be brought back from any header's context menu.
    auto showMenu = Gio::Menu::create();
    for (const auto& [id, col] : columns_)
        showMenu->append(col->get_title(), "cols.show::" + id);

    // A context menu per column header, targeting that column by id.
    for (const auto& [id, col] : columns_) {
        auto menu = Gio::Menu::create();
        menu->append("Move Left",  "cols.move-left::" + id);
        menu->append("Move Right", "cols.move-right::" + id);
        menu->append("Hide Column", "cols.hide::" + id);
        auto section = Gio::Menu::create();
        section->append_submenu("Show Column", showMenu);
        section->append("Show All Columns", "cols.show-all");
        menu->append_section(section);
        col->set_header_menu(menu);
    }

    // The filler column is never hidden, so its header is always present —
    // give it the show menu too, guaranteeing a way back even if every data
    // column is hidden.
    if (filler_) {
        auto menu = Gio::Menu::create();
        menu->append_submenu("Show Column", showMenu);
        menu->append("Show All Columns", "cols.show-all");
        filler_->set_header_menu(menu);
    }
}

void LogPage::moveColumn(const Glib::ustring& id, int delta) {
    Glib::RefPtr<Gtk::ColumnViewColumn> col;
    for (const auto& [cid, c] : columns_)
        if (cid == id.raw()) { col = c; break; }
    if (!col)
        return;

    auto model = columnView_.get_columns();
    const int n = static_cast<int>(model->get_n_items());
    int pos = -1;
    for (int i = 0; i < n; ++i)
        if (model->get_object(i).get() == col.get()) { pos = i; break; }
    if (pos < 0)
        return;

    const int target = pos + delta;
    if (target < 0 || target >= n)
        return;  // already at an edge

    columnView_.remove_column(col);
    columnView_.insert_column(target, col);
    pinFiller();            // the empty filler always stays last
    signalChanged_.emit();  // (no count change, but keeps the shell in sync)
}

void LogPage::setColumnVisible(const Glib::ustring& id, bool visible) {
    for (const auto& [cid, c] : columns_)
        if (cid == id.raw()) { c->set_visible(visible); return; }
}

void LogPage::showAllColumns() {
    for (const auto& [cid, c] : columns_)
        c->set_visible(true);
}

void LogPage::pinFiller() {
    if (!filler_)
        return;
    columnView_.remove_column(filler_);
    columnView_.append_column(filler_);
}

void LogPage::buildSearch() {
    searchEntry_.set_placeholder_text("Search log — call, name, QTH, locator, comment, band, mode…");
    searchEntry_.set_hexpand(true);
    searchBar_.set_child(searchEntry_);
    searchBar_.connect_entry(searchEntry_);
    searchBar_.set_show_close_button(true);
    // Type while the log table is focused to start searching.
    searchBar_.set_key_capture_widget(columnView_);
    searchEntry_.signal_search_changed().connect(
        sigc::mem_fun(*this, &LogPage::onSearchChanged));
}

Glib::ustring LogPage::rowSearchText(const Glib::RefPtr<Glib::ObjectBase>& obj) {
    auto* item = dynamic_cast<QsoItem*>(obj.get());
    if (!item)
        return {};
    const Qso& q = item->qso;
    // Newline-joined so a query can't span field boundaries.
    return q.call + "\n" + q.name + "\n" + q.qth + "\n" + q.locator + "\n" +
           q.comment + "\n" + q.band + "\n" + q.mode + "\n" + q.date;
}

void LogPage::onSearchChanged() {
    const Glib::ustring query = searchEntry_.get_text();
    filter_->set_search(query);  // StringFilter re-runs the filter itself

    if (query.empty())
        status("Search cleared.");
    else
        status("Search: " + std::to_string(filterModel_->get_n_items()) +
               " of " + std::to_string(store_->get_n_items()) + " QSOs match \"" +
               query + "\".");
}

void LogPage::beginSearch() {
    searchBar_.set_search_mode(true);
    searchEntry_.grab_focus();
}

void LogPage::buildEntryForm() {
    auto* frame = Gtk::make_managed<Gtk::Frame>("QSO entry");
    ui::setMargin(*frame, 6);

    auto* outer = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL);
    ui::setMargin(*outer, 6);
    outer->set_spacing(6);
    frame->set_child(*outer);

    auto* grid = Gtk::make_managed<Gtk::Grid>();
    grid->set_row_spacing(4);
    grid->set_column_spacing(8);
    outer->append(*grid);

    std::vector<Glib::ustring> bandStrings(bands::names().begin(), bands::names().end());
    std::vector<Glib::ustring> modeStrings(bands::modes().begin(), bands::modes().end());
    bandModel_ = Gtk::StringList::create(bandStrings);
    modeModel_ = Gtk::StringList::create(modeStrings);
    band_.set_model(bandModel_);
    mode_.set_model(modeModel_);

    qslSent_.set_label("QSL sent");
    qslRcvd_.set_label("QSL rcvd");

    auto field = [&](const char* text, Gtk::Widget& w, int col, int row) {
        auto* label = Gtk::make_managed<Gtk::Label>(text);
        label->set_xalign(1.0);
        grid->attach(*label, col * 2, row);
        w.set_hexpand(true);
        grid->attach(w, col * 2 + 1, row);
    };

    auto* nowButton = Gtk::make_managed<Gtk::Button>("Now");
    nowButton->signal_clicked().connect(sigc::mem_fun(*this, &LogPage::onSetNow));

    // Dates and times are stored (and exchanged via ADIF/LoTW) as UTC; spell
    // that out so they aren't entered as local time.
    field("Date (UTC)",     date_,    0, 0);
    field("Time on (UTC)",  timeOn_,  1, 0);
    field("Time off (UTC)", timeOff_, 2, 0);
    grid->attach(*nowButton, 6, 0);

    field("Call",     call_,    0, 1);
    // A clickable icon on the right of the callsign entry triggers a QRZ.com
    // lookup (no extra widget/column needed — Gtk::Entry supports this).
    call_.set_icon_from_icon_name("edit-find-symbolic", Gtk::Entry::IconPosition::SECONDARY);
    call_.set_icon_activatable(true, Gtk::Entry::IconPosition::SECONDARY);
    call_.set_icon_tooltip_text("Look up this callsign on QRZ.com",
                                Gtk::Entry::IconPosition::SECONDARY);
    call_.signal_icon_press().connect(
        [this](Gtk::Entry::IconPosition) { onLookupCall(); });

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

    // DXCC entity indicator (derived from the callsign via cty.dat).
    dxccLabel_.set_xalign(0.0);
    dxccLabel_.add_css_class("dim-label");
    outer->append(dxccLabel_);

    // Duplicate-warning indicator (hidden when empty).
    dupeLabel_.set_xalign(0.0);
    dupeLabel_.add_css_class("dupe-warning");
    outer->append(dupeLabel_);

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

    buildKeyerBar(*outer);

    append(*frame);

    addButton_.signal_clicked().connect(sigc::mem_fun(*this, &LogPage::onAddOrUpdate));
    deleteButton_.signal_clicked().connect(sigc::mem_fun(*this, &LogPage::onDeleteSelected));
    clearButton_.signal_clicked().connect(sigc::mem_fun(*this, &LogPage::clearForm));
    freq_.signal_changed().connect(sigc::mem_fun(*this, &LogPage::onFrequencyChanged));
    call_.signal_activate().connect(sigc::mem_fun(*this, &LogPage::onAddOrUpdate));

    // Live dupe detection + DXCC lookup as the key fields change.
    call_.signal_changed().connect(sigc::mem_fun(*this, &LogPage::updateDupeIndicator));
    call_.signal_changed().connect(sigc::mem_fun(*this, &LogPage::updateDxccIndicator));
    date_.signal_changed().connect(sigc::mem_fun(*this, &LogPage::updateDupeIndicator));
    band_.property_selected().signal_changed().connect(
        sigc::mem_fun(*this, &LogPage::updateDupeIndicator));
    mode_.property_selected().signal_changed().connect(
        sigc::mem_fun(*this, &LogPage::updateDupeIndicator));
}

void LogPage::refreshList() {
    store_->remove_all();
    for (const auto& q : logbook_.qsos())
        store_->append(QsoItem::create(q));
    updateDupeIndicator();
}

void LogPage::onSelectionChanged() {
    auto* item = dynamic_cast<QsoItem*>(selection_->get_selected_item().get());
    if (!item)
        return;  // deselection — leave the form alone
    qsoToForm(item->qso);
    editingId_ = item->qso.id;
    addButton_.set_label("Update QSO");
    deleteButton_.set_sensitive(true);
    updateDupeIndicator();
}

Qso LogPage::formToQso() const {
    Qso q;
    q.id       = editingId_;
    q.date     = ui::entryText(date_);
    q.time_on  = ui::entryText(timeOn_);
    q.time_off = ui::entryText(timeOff_);
    q.call     = ui::entryText(call_);
    q.band     = ui::dropdownText(band_, bandModel_);
    q.mode     = ui::dropdownText(mode_, modeModel_);
    q.freq     = ui::entryText(freq_);
    q.rst_sent = ui::entryText(rstSent_);
    q.rst_rcvd = ui::entryText(rstRcvd_);
    q.name     = ui::entryText(name_);
    q.qth      = ui::entryText(qth_);
    q.locator  = ui::entryText(locator_);
    q.power    = ui::entryText(power_);
    q.qsl_sent = qslSent_.get_active() ? "Y" : "N";
    q.qsl_rcvd = qslRcvd_.get_active() ? "Y" : "N";
    q.comment  = ui::entryText(comment_);

    // DXCC entity/zones are derived from the call (kept current by
    // updateDxccIndicator), not edited directly.
    q.country   = dxccCountry_;
    q.cq_zone   = dxccCqZone_;
    q.itu_zone  = dxccItuZone_;
    q.continent = dxccContinent_;

    for (auto& c : q.call) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    for (auto& c : q.locator) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    return q;
}

void LogPage::qsoToForm(const Qso& q) {
    // Remember the record's stored DXCC fields so updateDxccIndicator can fall
    // back to them when no country file is loaded. Set before call_ below,
    // whose change signal triggers updateDxccIndicator.
    loadedCountry_   = q.country;
    loadedCqZone_    = q.cq_zone;
    loadedItuZone_   = q.itu_zone;
    loadedContinent_ = q.continent;

    date_.set_text(q.date);
    timeOn_.set_text(q.time_on);
    timeOff_.set_text(q.time_off);
    call_.set_text(q.call);
    freq_.set_text(q.freq);  // set before band so it does not overwrite it
    ui::setDropdown(band_, bandModel_, q.band);
    ui::setDropdown(mode_, modeModel_, q.mode);
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

void LogPage::clearForm() {
    editingId_ = 0;
    loadedCountry_.clear();
    loadedCqZone_.clear();
    loadedItuZone_.clear();
    loadedContinent_.clear();
    for (Gtk::Entry* e : {&date_, &timeOn_, &timeOff_, &call_, &freq_, &rstSent_,
                          &rstRcvd_, &name_, &qth_, &locator_, &power_, &comment_})
        e->set_text("");
    band_.set_selected(GTK_INVALID_LIST_POSITION);
    mode_.set_selected(GTK_INVALID_LIST_POSITION);
    qslSent_.set_active(false);
    qslRcvd_.set_active(false);

    date_.set_text(ui::utcNow("%Y-%m-%d"));
    timeOn_.set_text(ui::utcNow("%H:%M"));

    addButton_.set_label("Add QSO");
    deleteButton_.set_sensitive(false);
    selection_->unselect_all();
    call_.grab_focus();
}

void LogPage::onAddOrUpdate() {
    Qso q = formToQso();
    if (q.call.empty()) {
        status("Cannot log a QSO without a callsign.");
        return;
    }
    if (editingId_ != 0) {
        logbook_.update(q);
        status("Updated QSO with " + q.call + ".");
    } else {
        logbook_.add(q);
        status("Logged QSO with " + q.call + ".");
    }
    refreshList();
    clearForm();
    signalChanged_.emit();
}

void LogPage::onDeleteSelected() {
    if (editingId_ == 0) {
        status("No QSO selected to delete.");
        return;
    }
    logbook_.remove(editingId_);
    refreshList();
    clearForm();
    signalChanged_.emit();
    status("QSO deleted.");
}

void LogPage::onFrequencyChanged() {
    const std::string text = ui::entryText(freq_);
    if (text.empty())
        return;
    try {
        const double mhz = std::stod(text);
        const std::string b = bands::forFrequencyMHz(mhz);
        if (!b.empty())
            ui::setDropdown(band_, bandModel_, b);
    } catch (const std::exception&) {
        // not a number yet; ignore
    }
}

void LogPage::onSetNow() {
    date_.set_text(ui::utcNow("%Y-%m-%d"));
    timeOn_.set_text(ui::utcNow("%H:%M"));
}

void LogPage::onLookupCall() {
    std::string call = ui::entryText(call_);
    for (auto& c : call)
        c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    if (call.empty()) {
        status("Enter a callsign to look up.");
        return;
    }
    signalLookupCall_.emit(call);  // the shell owns the QRZ client + credentials
}

void LogPage::applyQrzLookup(const QrzResult& r) {
    // Only overwrite a field when QRZ actually has a value, so a lookup that
    // returns nothing for some fields doesn't wipe what the user typed.
    if (!r.name.empty())    name_.set_text(r.name);
    if (!r.qth.empty())     qth_.set_text(r.qth);
    if (!r.locator.empty()) locator_.set_text(r.locator);
}

void LogPage::buildKeyerBar(Gtk::Box& parent) {
    auto* bar = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL);
    bar->set_spacing(4);
    ui::setMargin(*bar, 0);
    bar->append(*Gtk::make_managed<Gtk::Label>("Keyer:"));

    for (int i = 0; i < 9; ++i) {
        auto* b = Gtk::make_managed<Gtk::Button>("F" + std::to_string(i + 1));
        b->set_sensitive(false);  // enabled once a template is assigned
        b->signal_clicked().connect([this, i]() { sendCwMessage(i); });
        cwButtons_[i] = b;
        bar->append(*b);
    }

    auto* stop = Gtk::make_managed<Gtk::Button>("Stop");
    stop->set_tooltip_text("Abort the message being sent");
    stop->signal_clicked().connect([this]() { signalAbortCw_.emit(); });
    bar->append(*stop);
    parent.append(*bar);

    // F1–F9 fire the messages while focus is anywhere within this page.
    auto ctrl = Gtk::ShortcutController::create();
    ctrl->set_scope(Gtk::ShortcutScope::MANAGED);
    for (int i = 0; i < 9; ++i) {
        auto action = Gtk::CallbackAction::create(
            [this, i](Gtk::Widget&, const Glib::VariantBase&) {
                sendCwMessage(i);
                return true;
            });
        ctrl->add_shortcut(
            Gtk::Shortcut::create(Gtk::KeyvalTrigger::create(GDK_KEY_F1 + i), action));
    }
    add_controller(ctrl);
}

void LogPage::setCwMessages(const std::array<std::string, 9>& msgs) {
    cwMessages_ = msgs;
    for (int i = 0; i < 9; ++i) {
        if (!cwButtons_[i])
            continue;
        const bool has = !cwMessages_[i].empty();
        cwButtons_[i]->set_sensitive(has);
        cwButtons_[i]->set_tooltip_text(has ? cwMessages_[i] : "(no message set)");
    }
}

std::string LogPage::expandCwTemplate(const std::string& tmpl) const {
    std::string call = ui::entryText(call_);
    for (auto& c : call)
        c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));

    const std::pair<std::string, std::string> subs[] = {
        {"CALL", call},
        {"NAME", ui::entryText(name_)},
        {"QTH",  ui::entryText(qth_)},
        {"RST",  ui::entryText(rstRcvd_)},  // %RST% = the RST rcvd field
    };

    std::string out;
    const size_t n = tmpl.size();
    size_t i = 0;
    while (i < n) {
        if (tmpl[i] == '%') {
            const size_t end = tmpl.find('%', i + 1);
            if (end != std::string::npos) {
                std::string name = tmpl.substr(i + 1, end - i - 1);
                for (auto& c : name)
                    c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
                bool matched = false;
                for (const auto& [tok, val] : subs)
                    if (name == tok) { out += val; matched = true; break; }
                if (matched) {
                    i = end + 1;
                    continue;
                }
            }
            // Unknown or unterminated token: copy the '%' through literally.
        }
        out += tmpl[i++];
    }
    return out;
}

void LogPage::sendCwMessage(int index) {
    if (index < 0 || index >= 9 || cwMessages_[index].empty()) {
        status("Keyer F" + std::to_string(index + 1) + " has no message set.");
        return;
    }
    const std::string text = expandCwTemplate(cwMessages_[index]);
    signalSendCw_.emit(text);
    status("Keyer: " + text);
}

void LogPage::updateDupeIndicator() {
    const Qso q = formToQso();
    const auto dup = logbook_.findDuplicate(q, editingId_);
    if (dup) {
        dupeLabel_.set_text("⚠ Dupe — already worked " + dup->call + " on " +
                            dup->band + " " + dup->mode + " at " + dup->time_on +
                            " (" + dup->date + ")");
        if (!call_.has_css_class("dupe"))
            call_.add_css_class("dupe");
    } else {
        dupeLabel_.set_text("");
        call_.remove_css_class("dupe");
    }
}

void LogPage::updateDxccIndicator() {
    std::string call = ui::entryText(call_);
    for (auto& c : call)
        c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));

    const dxcc::Info* info = call.empty() ? nullptr : dxcc::lookup(call);
    if (info) {
        dxccCountry_   = info->entity;
        dxccCqZone_    = info->cqZone  ? std::to_string(info->cqZone)  : std::string{};
        dxccItuZone_   = info->ituZone ? std::to_string(info->ituZone) : std::string{};
        dxccContinent_ = info->continent;
    } else {
        // No match (or no cty.dat) — keep whatever the loaded record carried so
        // editing a QSO doesn't silently drop imported DXCC data.
        dxccCountry_   = loadedCountry_;
        dxccCqZone_    = loadedCqZone_;
        dxccItuZone_   = loadedItuZone_;
        dxccContinent_ = loadedContinent_;
    }

    if (dxccCountry_.empty()) {
        dxccLabel_.set_text("");
        return;
    }
    std::string s = dxccCountry_;
    if (!dxccCqZone_.empty())    s += "  ·  CQ " + dxccCqZone_;
    if (!dxccItuZone_.empty())   s += "  ·  ITU " + dxccItuZone_;
    if (!dxccContinent_.empty()) s += "  ·  " + dxccContinent_;
    dxccLabel_.set_text(s);
}

void LogPage::newInMemory() {
    logbook_.newInMemory();
    refreshList();
    clearForm();
    signalChanged_.emit();
}

bool LogPage::openFile(const std::string& path) {
    if (!logbook_.open(path))
        return false;
    refreshList();
    clearForm();
    signalChanged_.emit();
    return true;
}

bool LogPage::saveAs(const std::string& path) {
    if (!logbook_.saveAs(path))
        return false;
    refreshList();
    signalChanged_.emit();
    return true;
}

int LogPage::importAdif(const std::string& adifText) {
    const int n = logbook_.importAdif(adifText);
    refreshList();
    signalChanged_.emit();
    return n;
}

std::string LogPage::exportAdif() const {
    return logbook_.exportAdif();
}

void LogPage::addExternalQso(const Qso& q) {
    logbook_.add(q);
    refreshList();
    signalChanged_.emit();
}

void LogPage::setRigFrequency(double mhz) {
    if (mhz > 0.0)
        freq_.set_text(formatMhz(mhz));  // triggers band auto-detect
}

void LogPage::applyDxSpot(const std::string& call, double mhz) {
    std::string up = call;
    for (auto& c : up)
        c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    call_.set_text(up);
    setRigFrequency(mhz);  // fills freq and auto-detects the band
    status("DX spot: " + up + " on " + formatMhz(mhz) + " MHz.");
}

void LogPage::setRigMode(const std::string& mode) {
    if (mode.empty())
        return;
    for (guint i = 0; i < modeModel_->get_n_items(); ++i) {
        if (modeModel_->get_string(i).raw() == mode) {
            mode_.set_selected(i);
            return;
        }
    }
    // Unknown mode name: leave the current selection untouched.
}

Glib::ustring LogPage::title() const {
    return isFileBacked() ? Glib::path_get_basename(path()) : "Untitled";
}

std::vector<Qso> LogPage::qsosNotLotwSent() const {
    return logbook_.qsosNotLotwSent();
}

void LogPage::markLotwSent(const std::vector<long>& ids, const std::string& date) {
    logbook_.markLotwSent(ids, date);
    refreshList();
    signalChanged_.emit();
}

int LogPage::applyLotwConfirmations(const std::vector<Qso>& confirmed) {
    const int n = logbook_.applyLotwConfirmations(confirmed);
    refreshList();
    signalChanged_.emit();
    return n;
}

void LogPage::refresh() {
    refreshList();
}

void LogPage::applyColumnLayout(const Glib::RefPtr<Glib::KeyFile>& keyfile) {
    if (!keyfile)
        return;
    for (const auto& [id, col] : columns_) {
        try {
            if (keyfile->has_group("width") && keyfile->has_key("width", id)) {
                const int width = keyfile->get_integer("width", id);
                if (width > 0)
                    col->set_fixed_width(width);
            }
            if (keyfile->has_group("visible") && keyfile->has_key("visible", id))
                col->set_visible(keyfile->get_boolean("visible", id));
        } catch (const Glib::Error&) {
        }
    }
    try {
        if (keyfile->has_group("columns") && keyfile->has_key("columns", "order"))
            applyColumnOrder(ui::splitSemicolons(keyfile->get_string("columns", "order")));
    } catch (const Glib::Error&) {
    }
}

void LogPage::storeColumnLayout(const Glib::RefPtr<Glib::KeyFile>& keyfile) {
    if (!keyfile)
        return;
    std::string order;
    auto displayed = columnView_.get_columns();
    for (guint i = 0; i < displayed->get_n_items(); ++i) {
        auto* colPtr = dynamic_cast<Gtk::ColumnViewColumn*>(displayed->get_object(i).get());
        for (const auto& [id, col] : columns_) {
            if (col.get() == colPtr) {
                if (!order.empty())
                    order += ';';
                order += id;
                break;
            }
        }
    }
    keyfile->set_string("columns", "order", order);
    for (const auto& [id, col] : columns_) {
        const int width = col->get_fixed_width();
        if (width > 0)
            keyfile->set_integer("width", id, width);
        keyfile->set_boolean("visible", id, col->get_visible());
    }
}

void LogPage::applyColumnOrder(const std::vector<std::string>& ids) {
    std::vector<Glib::RefPtr<Gtk::ColumnViewColumn>> desired;
    std::set<std::string> used;

    for (const auto& id : ids) {
        for (const auto& [cid, col] : columns_) {
            if (cid == id && !used.count(cid)) {
                desired.push_back(col);
                used.insert(cid);
                break;
            }
        }
    }
    for (const auto& [cid, col] : columns_)
        if (!used.count(cid))
            desired.push_back(col);

    if (desired.size() != columns_.size())
        return;

    for (const auto& [cid, col] : columns_)
        columnView_.remove_column(col);
    for (const auto& col : desired)
        columnView_.append_column(col);
    pinFiller();  // re-append the empty filler after the reordered columns
}
