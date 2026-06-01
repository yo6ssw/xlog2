#include "DxClusterPanel.h"

#include "Bands.h"
#include "Dxcc.h"
#include "UiUtil.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <vector>

namespace {
// A spotter is forgotten this long after its report.
constexpr std::chrono::minutes kSpotterTtl{5};

// Spots for the same callsign within this many kHz are treated as one entry.
constexpr double kFreqToleranceKHz = 0.2;  // +/- 200 Hz

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

    // --- band map: store -> band filter -> selection -> view ----------------
    store_  = Gio::ListStore<BandMapItem>::create();
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

    columnView_.append_column(makeColumn("Freq", [](const BandMapItem& r) { return formatKHz(r.freqKHz); }));
    columnView_.append_column(makeColumn("DX",   [](const BandMapItem& r) { return r.dxCall; }));
    columnView_.append_column(makeColumn("Band", [](const BandMapItem& r) { return r.band; }));
    columnView_.append_column(makeCountColumn());
    columnView_.append_column(makeColumn("Entity", [](const BandMapItem& r) { return r.entity; }, true));
    columnView_.append_column(makeColumn("Cont", [](const BandMapItem& r) { return r.continent; }));

    auto* spotScroller = Gtk::make_managed<Gtk::ScrolledWindow>();
    spotScroller->set_policy(Gtk::PolicyType::AUTOMATIC, Gtk::PolicyType::AUTOMATIC);
    spotScroller->set_child(columnView_);
    spotScroller->set_vexpand(true);
    append(*spotScroller);

    // --- raw console --------------------------------------------------------
    console_.set_editable(false);
    console_.set_monospace(true);
    console_.set_cursor_visible(false);
    auto* consoleScroller = Gtk::make_managed<Gtk::ScrolledWindow>();
    consoleScroller->set_policy(Gtk::PolicyType::AUTOMATIC, Gtk::PolicyType::AUTOMATIC);
    consoleScroller->set_child(console_);
    consoleScroller->set_min_content_height(90);
    append(*consoleScroller);

    // Prune expired spotters periodically, so quiet entries disappear even
    // without new traffic.
    expiryTimer_ = Glib::signal_timeout().connect([this]() { rebuild(); return true; }, 5000);
}

DxClusterPanel::~DxClusterPanel() {
    expiryTimer_.disconnect();
}

Glib::RefPtr<Gtk::ColumnViewColumn> DxClusterPanel::makeColumn(
    const Glib::ustring& title, std::function<std::string(const BandMapItem&)> getter,
    bool expand) {
    auto factory = Gtk::SignalListItemFactory::create();
    factory->signal_setup().connect([](const Glib::RefPtr<Gtk::ListItem>& li) {
        auto* label = Gtk::make_managed<Gtk::Label>();
        label->set_xalign(0.0);
        li->set_child(*label);
    });
    factory->signal_bind().connect([getter](const Glib::RefPtr<Gtk::ListItem>& li) {
        auto* label = dynamic_cast<Gtk::Label*>(li->get_child());
        auto* item  = dynamic_cast<BandMapItem*>(li->get_item().get());
        if (label && item)
            label->set_text(getter(*item));  // stable fields, set once per bind
    });
    auto column = Gtk::ColumnViewColumn::create(title, factory);
    column->set_resizable(true);
    column->set_expand(expand);
    return column;
}

Glib::RefPtr<Gtk::ColumnViewColumn> DxClusterPanel::makeCountColumn() {
    auto factory = Gtk::SignalListItemFactory::create();
    factory->signal_setup().connect([](const Glib::RefPtr<Gtk::ListItem>& li) {
        auto* label = Gtk::make_managed<Gtk::Label>();
        label->set_xalign(0.0);
        li->set_child(*label);
    });
    // Bind the count cell to the item's `count`/`spotters` properties so it
    // updates in place — the row widget is never recreated, so a hovered row
    // doesn't flicker when its spotter count changes.
    factory->signal_bind().connect([this](const Glib::RefPtr<Gtk::ListItem>& li) {
        auto* label = dynamic_cast<Gtk::Label*>(li->get_child());
        auto* item  = dynamic_cast<BandMapItem*>(li->get_item().get());
        if (!label || !item)
            return;
        auto update = [label, item]() {
            label->set_text(std::to_string(item->count.get_value()));
            label->set_tooltip_text(item->spotters.get_value());
        };
        update();
        std::vector<sigc::connection> conns;
        conns.push_back(item->count.get_proxy().signal_changed().connect(update));
        conns.push_back(item->spotters.get_proxy().signal_changed().connect(update));
        countConns_[li.get()] = std::move(conns);
    });
    factory->signal_unbind().connect([this](const Glib::RefPtr<Gtk::ListItem>& li) {
        auto it = countConns_.find(li.get());
        if (it != countConns_.end()) {
            for (auto& c : it->second)
                c.disconnect();
            countConns_.erase(it);
        }
    });
    auto column = Gtk::ColumnViewColumn::create("Spotters", factory);
    column->set_resizable(true);
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
        return true;
    auto* item = dynamic_cast<BandMapItem*>(obj.get());
    if (!item)
        return true;
    return activeBands_.count(item->band) > 0;
}

void DxClusterPanel::refreshFilter() {
    filter_->set_expression(Gtk::ClosureExpression<bool>::create(
        sigc::mem_fun(*this, &DxClusterPanel::spotMatchesFilter)));
}

void DxClusterPanel::addSpot(const DxSpot& spot) {
    if (spot.dxCall.empty() || spot.freqKHz <= 0.0)
        return;

    // Group spots for the same callsign within +/- 200 Hz of an existing
    // entry's frequency (operators/skimmers report slightly different freqs for
    // the same station). Reuse that entry; otherwise start a new one keyed by
    // this frequency. The entry keeps its original representative frequency.
    Key key{};
    bool found = false;
    for (const auto& [k, e] : entries_) {
        if (e.dxCall == spot.dxCall &&
            std::abs(e.freqKHz - spot.freqKHz) <= kFreqToleranceKHz) {
            key = k;
            found = true;
            break;
        }
    }
    if (!found)
        key = std::make_pair(std::lround(spot.freqKHz * 10.0), spot.dxCall);

    Entry& e = entries_[key];
    if (!found) {
        e.freqKHz = spot.freqKHz;
        e.dxCall  = spot.dxCall;
        e.band    = spot.band;
    }
    // A repeat from the same spotter refreshes its timer; a new spotter is
    // added (extending the entry's life). Empty spotter name is unlikely but
    // guarded.
    const std::string who = spot.spotter.empty() ? "?" : spot.spotter;
    e.spotters[who] = SpotterInfo{Clock::now(), spot.comment, spot.timeUtc};
    rebuild();
}

void DxClusterPanel::rebuild() {
    const auto now = Clock::now();

    // Discard spotters older than the TTL; collect entries left with none.
    std::vector<Key> expired;
    for (auto& [key, e] : entries_) {
        auto& spotters = e.spotters;
        for (auto s = spotters.begin(); s != spotters.end();) {
            if (now - s->second.time > kSpotterTtl)
                s = spotters.erase(s);
            else
                ++s;
        }
        if (spotters.empty())
            expired.push_back(key);
    }
    // Remove emptied entries and their rows (only the affected rows, so other
    // rows — including a hovered one — are untouched).
    for (const auto& key : expired) {
        if (auto it = items_.find(key); it != items_.end()) {
            if (auto [found, pos] = store_->find(it->second); found)
                store_->remove(pos);
            items_.erase(it);
        }
        entries_.erase(key);
    }

    // Keep rows ordered by frequency then call when inserting new ones.
    const auto cmp = [](const Glib::RefPtr<const BandMapItem>& a,
                        const Glib::RefPtr<const BandMapItem>& b) -> int {
        if (a->freqKHz < b->freqKHz) return -1;
        if (a->freqKHz > b->freqKHz) return 1;
        return a->dxCall.compare(b->dxCall);
    };

    for (const auto& [key, e] : entries_) {
        // Spotters newest-first for the displayed comment/time and tooltip.
        std::vector<const std::pair<const std::string, SpotterInfo>*> sorted;
        sorted.reserve(e.spotters.size());
        for (const auto& kv : e.spotters)
            sorted.push_back(&kv);
        std::sort(sorted.begin(), sorted.end(),
                  [](auto* a, auto* b) { return a->second.time > b->second.time; });

        std::string tip;
        for (const auto* p : sorted) {
            const auto mins = std::chrono::duration_cast<std::chrono::minutes>(
                                  now - p->second.time).count();
            if (!tip.empty())
                tip += '\n';
            tip += p->first + "  (" + std::to_string(mins) + "m ago)";
        }
        const int count = static_cast<int>(sorted.size());
        const std::string comment = sorted.empty() ? std::string{} : sorted.front()->second.comment;
        const std::string timeUtc = sorted.empty() ? std::string{} : sorted.front()->second.timeUtc;

        auto it = items_.find(key);
        if (it == items_.end()) {
            // New entry: build the row once and insert it in frequency order.
            auto item = BandMapItem::create();
            item->freqKHz = e.freqKHz;
            item->dxCall  = e.dxCall;
            item->band    = e.band;
            if (const dxcc::Info* info = dxcc::lookup(e.dxCall)) {
                item->entity    = info->entity;
                item->continent = info->continent;
            }
            item->comment = comment;
            item->timeUtc = timeUtc;
            item->count.set_value(count);
            item->spotters.set_value(tip);
            store_->insert_sorted(item, cmp);
            items_.emplace(key, std::move(item));
        } else {
            // Existing row: update only the mutable cell data in place.
            it->second->comment = comment;
            it->second->timeUtc = timeUtc;
            if (it->second->count.get_value() != count)
                it->second->count.set_value(count);
            if (it->second->spotters.get_value().raw() != tip)
                it->second->spotters.set_value(tip);
        }
    }
}

void DxClusterPanel::addLine(const std::string& line) {
    auto buffer = console_.get_buffer();
    buffer->insert(buffer->end(), line + "\n");
    auto mark = buffer->create_mark(buffer->end());
    console_.scroll_to(mark);
    buffer->delete_mark(mark);
}

void DxClusterPanel::setConnected(bool connected) {
    connected_ = connected;
    connectButton_.set_label(connected ? "Disconnect" : "Connect");
}

void DxClusterPanel::onActivate(guint position) {
    auto* item = dynamic_cast<BandMapItem*>(selection_->get_object(position).get());
    if (!item)
        return;
    // Synthesise a spot from the band-map row for the form/rig.
    DxSpot s;
    s.freqKHz = item->freqKHz;
    s.dxCall  = item->dxCall;
    s.band    = item->band;
    s.comment = item->comment;
    s.timeUtc = item->timeUtc;
    signalActivate_.emit(s);
}

void DxClusterPanel::onSendCommand() {
    const std::string cmd = commandEntry_.get_text().raw();
    if (cmd.empty())
        return;
    signalCommand_.emit(cmd);
    commandEntry_.set_text("");
}
