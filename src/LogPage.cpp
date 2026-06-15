#include "LogPage.h"

#include "Bands.h"
#include "UiUtil.h"


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
    const Glib::ustring css =
        ".dupe-warning { color: #c01c28; font-weight: bold; }\n"
        ".dxcc-entity { color: #1c71d8; font-weight: bold; }\n"
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
        "}\n";
    // load_from_string() is gtkmm 4.16+; fall back to load_from_data() on the
    // 4.14 (Ubuntu 24.04 LTS) that the PPA builds against.
#if GTKMM_CHECK_VERSION(4, 16, 0)
    provider->load_from_string(css);
#else
    provider->load_from_data(css);
#endif
    if (auto display = Gdk::Display::get_default())
        Gtk::StyleContext::add_provider_for_display(
            display, provider, GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
}

} // namespace

LogPage::LogPage() : Gtk::Box(Gtk::Orientation::VERTICAL), presenter_(*this) {
    ensureCss();

    // Bridge the presenter's shell-facing hooks to this view's signals.
    presenter_.onChanged    = [this]() { signalChanged_.emit(); };
    presenter_.onStatus     = [this](const std::string& s) { signalStatus_.emit(s); };
    presenter_.onLookupCall = [this](const std::string& c) { signalLookupCall_.emit(c); };
    presenter_.onSendCw     = [this](const std::string& t) { signalSendCw_.emit(t); };
    presenter_.onAbortCw    = [this]() { signalAbortCw_.emit(); };

    buildLogView();
    buildSearch();
    append(searchBar_);

    auto* scroller = Gtk::make_managed<Gtk::ScrolledWindow>();
    scroller->set_policy(Gtk::PolicyType::AUTOMATIC, Gtk::PolicyType::AUTOMATIC);
    scroller->set_child(columnView_);
    scroller->set_vexpand(true);
    append(*scroller);

    buildEntryForm();

    presenter_.start();  // populate rows + reset the form (widgets now exist)
}

Glib::RefPtr<Gtk::ColumnViewColumn> LogPage::makeColumn(
    const Glib::ustring& title, std::function<std::string(const Qso&)> getter,
    bool expand) {
    auto factory = Gtk::SignalListItemFactory::create();
    factory->signal_setup().connect([this](const Glib::RefPtr<Gtk::ListItem>& li) {
        auto* label = Gtk::make_managed<Gtk::Label>();
        label->set_xalign(0.0);
        li->set_child(*label);

        // Secondary-button click anywhere in the row opens the row context menu.
        // The ListItem outlives its child label, so a raw pointer is safe; its
        // get_item() yields whichever QSO is currently bound to this row slot.
        auto click = Gtk::GestureClick::create();
        click->set_button(GDK_BUTTON_SECONDARY);
        Gtk::ListItem* liPtr = li.get();
        click->signal_pressed().connect(
            [this, liPtr, label](int, double x, double y) {
                showRowContextMenu(liPtr, *label, x, y);
            });
        label->add_controller(click);
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
    add("country", "Country", [](const Qso& q) { return q.country; });
    add("cont", "Cont", [](const Qso& q) { return q.continent; });
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

    // Row context menu actions (Delete / Move to). The popover is parented once
    // to the column view; showRowContextMenu rebuilds its model per click.
    rowActions_ = Gio::SimpleActionGroup::create();
    auto del = Gio::SimpleAction::create("delete");
    del->signal_activate().connect(
        [this](const Glib::VariantBase&) { confirmDeleteRow(contextId_); });
    rowActions_->add_action(del);
    auto move = Gio::SimpleAction::create(
        "move", Glib::Variant<Glib::ustring>::variant_type());
    move->signal_activate().connect([this](const Glib::VariantBase& p) {
        const int idx = std::stoi(
            Glib::VariantBase::cast_dynamic<Glib::Variant<Glib::ustring>>(p).get().raw());
        if (idx >= 0 && idx < static_cast<int>(moveTargets_.size()) && requestMove)
            requestMove(contextId_, moveTargets_[idx]);
    });
    rowActions_->add_action(move);
    insert_action_group("row", rowActions_);

    rowMenu_ = Gtk::make_managed<Gtk::PopoverMenu>();
    rowMenu_->set_parent(columnView_);
    rowMenu_->set_has_arrow(false);
}

void LogPage::showRowContextMenu(Gtk::ListItem* li, Gtk::Widget& anchor,
                                 double x, double y) {
    auto* item = dynamic_cast<QsoItem*>(li->get_item().get());
    if (!item)
        return;
    contextId_ = item->qso.id;
    selection_->set_selected(li->get_position());  // also loads it into the form

    auto menu = Gio::Menu::create();
    menu->append("Delete QSO", "row.delete");

    // "Move to" lists every other open logbook (supplied by the shell).
    moveTargets_.clear();
    if (queryMoveTargets) {
        const auto targets = queryMoveTargets();
        if (!targets.empty()) {
            auto sub = Gio::Menu::create();
            for (std::size_t i = 0; i < targets.size(); ++i) {
                moveTargets_.push_back(targets[i].second);
                sub->append(targets[i].first, "row.move::" + std::to_string(i));
            }
            menu->append_submenu("Move to", sub);
        }
    }
    rowMenu_->set_menu_model(menu);

    // Pop up under the pointer: translate the click point (in the cell's
    // coordinates) into the column view's space, the popover's parent.
    if (auto p = anchor.compute_point(columnView_, Gdk::Graphene::Point(x, y)))
        rowMenu_->set_pointing_to(
            Gdk::Rectangle(static_cast<int>(p->get_x()),
                           static_cast<int>(p->get_y()), 1, 1));
    rowMenu_->popup();
}

void LogPage::confirmDeleteRow(long id) {
    const Qso* q = presenter_.findQso(id);
    if (!q)
        return;
    const Glib::ustring call = q->call.empty() ? Glib::ustring("this QSO")
                                               : Glib::ustring(q->call);
    auto dialog = Gtk::AlertDialog::create("Delete " + call + "?");
    dialog->set_detail("This permanently removes the QSO from the logbook.");
    dialog->set_buttons({"Cancel", "Delete"});
    dialog->set_cancel_button(0);
    dialog->set_default_button(0);
    auto* win = dynamic_cast<Gtk::Window*>(get_root());
    if (!win)
        return;
    dialog->choose(*win, [this, dialog, id](const Glib::RefPtr<Gio::AsyncResult>& res) {
        try {
            if (dialog->choose_finish(res) == 1)
                presenter_.deleteQso(id);
        } catch (const Glib::Error&) {
            // dialog dismissed (Escape) — nothing to do
        }
    });
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
    showSearch();
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
    nowButton->signal_clicked().connect([this]() { presenter_.onSetNow(); });

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
        [this](Gtk::Entry::IconPosition) { presenter_.onLookupCallClicked(); });

    field("Band",     band_,    1, 1);
    field("Mode",     mode_,    2, 1);

    // DXCC entity shown right on the call row, updated live as the call is typed.
    dxccLabel_.set_xalign(0.0);
    dxccLabel_.set_margin_start(8);
    dxccLabel_.add_css_class("dxcc-entity");
    grid->attach(dxccLabel_, 6, 1);

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

    addButton_.signal_clicked().connect([this]() { presenter_.onAddOrUpdate(); });
    deleteButton_.signal_clicked().connect([this]() { presenter_.onDelete(); });
    clearButton_.signal_clicked().connect([this]() { presenter_.onClear(); });
    freq_.signal_changed().connect([this]() { if (!loadingForm_) presenter_.onFreqChanged(); });
    call_.signal_activate().connect([this]() { presenter_.onAddOrUpdate(); });

    // Live dupe detection + DXCC lookup as the key fields change.
    call_.signal_changed().connect([this]() { if (!loadingForm_) presenter_.onCallChanged(); });
    date_.signal_changed().connect([this]() { if (!loadingForm_) presenter_.onDupeKeyChanged(); });
    band_.property_selected().signal_changed().connect(
        [this]() { if (!loadingForm_) presenter_.onDupeKeyChanged(); });
    mode_.property_selected().signal_changed().connect(
        [this]() { if (!loadingForm_) presenter_.onDupeKeyChanged(); });
}

void LogPage::onSelectionChanged() {
    auto* item = dynamic_cast<QsoItem*>(selection_->get_selected_item().get());
    if (!item)
        return;  // deselection — leave the form alone
    presenter_.onRowSelected(item->qso.id);
}

// --- ILogPageView: log list --------------------------------------------------

void LogPage::setRows(const std::vector<Qso>& qsos) {
    store_->remove_all();
    for (const auto& q : qsos)
        store_->append(QsoItem::create(q));
}

void LogPage::clearSelection() {
    selection_->unselect_all();
}

// --- ILogPageView: entry form ------------------------------------------------

FormData LogPage::formData() const {
    FormData f;
    f.date     = ui::entryText(date_);
    f.time_on  = ui::entryText(timeOn_);
    f.time_off = ui::entryText(timeOff_);
    f.call     = ui::entryText(call_);
    f.band     = ui::dropdownText(band_, bandModel_);
    f.mode     = ui::dropdownText(mode_, modeModel_);
    f.freq     = ui::entryText(freq_);
    f.rst_sent = ui::entryText(rstSent_);
    f.rst_rcvd = ui::entryText(rstRcvd_);
    f.name     = ui::entryText(name_);
    f.qth      = ui::entryText(qth_);
    f.locator  = ui::entryText(locator_);
    f.power    = ui::entryText(power_);
    f.comment  = ui::entryText(comment_);
    f.qsl_sent = qslSent_.get_active();
    f.qsl_rcvd = qslRcvd_.get_active();
    return f;
}

void LogPage::setFormData(const FormData& f) {
    // Suppress event forwarding while we write widgets: the presenter refreshes
    // indicators itself after a bulk load.
    loadingForm_ = true;
    date_.set_text(f.date);
    timeOn_.set_text(f.time_on);
    timeOff_.set_text(f.time_off);
    call_.set_text(f.call);
    freq_.set_text(f.freq);  // set before band so it does not overwrite it
    ui::setDropdown(band_, bandModel_, f.band);
    ui::setDropdown(mode_, modeModel_, f.mode);
    rstSent_.set_text(f.rst_sent);
    rstRcvd_.set_text(f.rst_rcvd);
    name_.set_text(f.name);
    qth_.set_text(f.qth);
    locator_.set_text(f.locator);
    power_.set_text(f.power);
    qslSent_.set_active(f.qsl_sent);
    qslRcvd_.set_active(f.qsl_rcvd);
    comment_.set_text(f.comment);
    loadingForm_ = false;
}

void LogPage::setCall(const std::string& s) { call_.set_text(s); }
void LogPage::setFreq(const std::string& s) { freq_.set_text(s); }
void LogPage::setBand(const std::string& s) { ui::setDropdown(band_, bandModel_, s); }
void LogPage::setMode(const std::string& s) { ui::setDropdown(mode_, modeModel_, s); }

// --- ILogPageView: indicators / button state ---------------------------------

void LogPage::setDupeWarning(const std::string& msg, bool highlight) {
    dupeLabel_.set_text(msg);
    if (highlight) {
        if (!call_.has_css_class("dupe"))
            call_.add_css_class("dupe");
    } else {
        call_.remove_css_class("dupe");
    }
}

void LogPage::setDxccText(const std::string& s) {
    dxccLabel_.set_text(s);
}

void LogPage::setEditing(bool editing) {
    addButton_.set_label(editing ? "Update QSO" : "Add QSO");
    deleteButton_.set_sensitive(editing);
}

void LogPage::focusCall() {
    call_.grab_focus();
}

void LogPage::buildKeyerBar(Gtk::Box& parent) {
    auto* bar = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL);
    bar->set_spacing(4);
    ui::setMargin(*bar, 0);
    bar->append(*Gtk::make_managed<Gtk::Label>("Keyer:"));

    for (int i = 0; i < 9; ++i) {
        auto* b = Gtk::make_managed<Gtk::Button>("F" + std::to_string(i + 1));
        b->set_sensitive(false);  // enabled once a template is assigned
        b->signal_clicked().connect([this, i]() { presenter_.onSendCwClicked(i); });
        cwButtons_[i] = b;
        bar->append(*b);
    }

    auto* stop = Gtk::make_managed<Gtk::Button>("Stop");
    stop->set_tooltip_text("Abort the message being sent");
    stop->signal_clicked().connect([this]() { presenter_.onAbortCwClicked(); });
    bar->append(*stop);
    parent.append(*bar);

    // The F1–F9 keyer accelerators are installed once on the MainWindow (so they
    // fire from anywhere in the window, including the DX-cluster panel) and
    // routed to the active page. See MainWindow's constructor.
}

void LogPage::setCwButtons(const std::array<std::string, 9>& messages) {
    for (int i = 0; i < 9; ++i) {
        if (!cwButtons_[i])
            continue;
        const bool has = !messages[i].empty();
        cwButtons_[i]->set_sensitive(has);
        cwButtons_[i]->set_tooltip_text(has ? messages[i] : "(no message set)");
    }
}

void LogPage::showSearch() {
    searchBar_.set_search_mode(true);
    searchEntry_.grab_focus();
}

void LogPage::applyColumnLayout(const IniFile& ini) {
    for (const auto& [id, col] : columns_) {
        if (ini.hasKey("width", id)) {
            const int width = ini.getInt("width", id, 0);
            if (width > 0)
                col->set_fixed_width(width);
        }
        if (ini.hasKey("visible", id))
            col->set_visible(ini.getBool("visible", id, true));
    }
    if (ini.hasKey("columns", "order"))
        applyColumnOrder(ui::splitSemicolons(ini.getString("columns", "order")));
}

void LogPage::storeColumnLayout(IniFile& ini) {
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
    ini.setString("columns", "order", order);
    for (const auto& [id, col] : columns_) {
        const int width = col->get_fixed_width();
        if (width > 0)
            ini.setInt("width", id, width);
        ini.setBool("visible", id, col->get_visible());
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
