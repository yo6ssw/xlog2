#include "DxClusterPanel.h"

#include "Bands.h"
#include "UiUtil.h"

#include <cstdio>

namespace {
constexpr guint kMaxSpots = 500;

std::string formatKHz(double khz) {
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%.1f", khz);
    return buf;
}
}  // namespace

DxClusterPanel::DxClusterPanel() : Gtk::Box(Gtk::Orientation::VERTICAL) {
    set_spacing(4);
    ui::setMargin(*this, 4);

    // --- control row: connect, command entry, send -------------------------
    auto* controls = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL);
    controls->set_spacing(4);
    connectButton_.set_label("Connect");
    connectButton_.signal_clicked().connect([this]() { signalConnectToggle_.emit(); });
    controls->append(connectButton_);

    commandEntry_.set_hexpand(true);
    commandEntry_.set_placeholder_text("cluster command (e.g. sh/dx 20, set/filter)…");
    commandEntry_.signal_activate().connect(sigc::mem_fun(*this, &DxClusterPanel::onSendCommand));
    controls->append(commandEntry_);
    auto* send = Gtk::make_managed<Gtk::Button>("Send");
    send->signal_clicked().connect(sigc::mem_fun(*this, &DxClusterPanel::onSendCommand));
    controls->append(*send);
    append(*controls);

    // --- band filter chips --------------------------------------------------
    buildBandChips();
    append(bandChips_);

    // --- spot table: store -> band filter -> selection -> view --------------
    store_  = Gio::ListStore<DxSpotItem>::create();
    filter_ = Gtk::BoolFilter::create(Gtk::ClosureExpression<bool>::create(
        sigc::mem_fun(*this, &DxClusterPanel::spotMatchesFilter)));
    filterModel_ = Gtk::FilterListModel::create(store_, filter_);
    selection_   = Gtk::SingleSelection::create(filterModel_);
    selection_->set_autoselect(false);
    selection_->set_can_unselect(true);
    columnView_.set_model(selection_);
    columnView_.add_css_class("data-table");
    columnView_.set_show_column_separators(true);
    columnView_.signal_activate().connect(sigc::mem_fun(*this, &DxClusterPanel::onActivate));

    columnView_.append_column(makeColumn("Freq", [](const DxSpot& s) { return formatKHz(s.freqKHz); }));
    columnView_.append_column(makeColumn("DX", [](const DxSpot& s) { return s.dxCall; }));
    columnView_.append_column(makeColumn("Band", [](const DxSpot& s) { return s.band; }));
    columnView_.append_column(makeColumn("Spotter", [](const DxSpot& s) { return s.spotter; }));
    columnView_.append_column(makeColumn("Time", [](const DxSpot& s) { return s.timeUtc; }));
    columnView_.append_column(makeColumn("Comment", [](const DxSpot& s) { return s.comment; }, true));

    auto* spotScroller = Gtk::make_managed<Gtk::ScrolledWindow>();
    spotScroller->set_policy(Gtk::PolicyType::AUTOMATIC, Gtk::PolicyType::AUTOMATIC);
    spotScroller->set_child(columnView_);
    spotScroller->set_vexpand(true);
    append(*spotScroller);

    // --- raw console (command responses / announcements) --------------------
    console_.set_editable(false);
    console_.set_monospace(true);
    console_.set_cursor_visible(false);
    auto* consoleScroller = Gtk::make_managed<Gtk::ScrolledWindow>();
    consoleScroller->set_policy(Gtk::PolicyType::AUTOMATIC, Gtk::PolicyType::AUTOMATIC);
    consoleScroller->set_child(console_);
    consoleScroller->set_min_content_height(90);
    append(*consoleScroller);
}

Glib::RefPtr<Gtk::ColumnViewColumn> DxClusterPanel::makeColumn(
    const Glib::ustring& title, std::function<std::string(const DxSpot&)> getter,
    bool expand) {
    auto factory = Gtk::SignalListItemFactory::create();
    factory->signal_setup().connect([](const Glib::RefPtr<Gtk::ListItem>& li) {
        auto* label = Gtk::make_managed<Gtk::Label>();
        label->set_xalign(0.0);
        li->set_child(*label);
    });
    factory->signal_bind().connect([getter](const Glib::RefPtr<Gtk::ListItem>& li) {
        auto* label = dynamic_cast<Gtk::Label*>(li->get_child());
        auto* item  = dynamic_cast<DxSpotItem*>(li->get_item().get());
        if (label && item)
            label->set_text(getter(item->spot));
    });
    auto column = Gtk::ColumnViewColumn::create(title, factory);
    column->set_resizable(true);
    column->set_expand(expand);
    return column;
}

void DxClusterPanel::buildBandChips() {
    bandChips_.set_selection_mode(Gtk::SelectionMode::NONE);
    bandChips_.set_max_children_per_line(64);
    bandChips_.set_column_spacing(4);
    bandChips_.set_row_spacing(4);
    for (const auto& band : bands::names()) {
        auto* chip = Gtk::make_managed<Gtk::ToggleButton>(band);
        chip->signal_toggled().connect([this, band, chip]() {
            if (chip->get_active())
                activeBands_.insert(band);
            else
                activeBands_.erase(band);
            refreshFilter();
        });
        bandChips_.append(*chip);
    }
}

bool DxClusterPanel::spotMatchesFilter(const Glib::RefPtr<Glib::ObjectBase>& obj) {
    if (activeBands_.empty())
        return true;  // no chip selected -> show everything
    auto* item = dynamic_cast<DxSpotItem*>(obj.get());
    if (!item)
        return true;
    return activeBands_.count(item->spot.band) > 0;
}

void DxClusterPanel::refreshFilter() {
    // A fresh ClosureExpression guarantees the FilterListModel re-evaluates.
    filter_->set_expression(Gtk::ClosureExpression<bool>::create(
        sigc::mem_fun(*this, &DxClusterPanel::spotMatchesFilter)));
}

void DxClusterPanel::addSpot(const DxSpot& spot) {
    store_->insert(0, DxSpotItem::create(spot));  // newest on top
    while (store_->get_n_items() > kMaxSpots)
        store_->remove(store_->get_n_items() - 1);
}

void DxClusterPanel::addLine(const std::string& line) {
    auto buffer = console_.get_buffer();
    buffer->insert(buffer->end(), line + "\n");
    // Keep the view pinned to the newest line.
    auto mark = buffer->create_mark(buffer->end());
    console_.scroll_to(mark);
    buffer->delete_mark(mark);
}

void DxClusterPanel::setConnected(bool connected) {
    connected_ = connected;
    connectButton_.set_label(connected ? "Disconnect" : "Connect");
}

void DxClusterPanel::onActivate(guint position) {
    auto* item = dynamic_cast<DxSpotItem*>(selection_->get_object(position).get());
    if (item)
        signalActivate_.emit(item->spot);
}

void DxClusterPanel::onSendCommand() {
    const std::string cmd = commandEntry_.get_text().raw();
    if (cmd.empty())
        return;
    signalCommand_.emit(cmd);
    commandEntry_.set_text("");
}
