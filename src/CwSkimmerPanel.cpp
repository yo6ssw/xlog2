#include "CwSkimmerPanel.h"

#include "UiUtil.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <memory>
#include <vector>

namespace {

// Intensity (0..1) -> an "inferno"-style waterfall colormap (packed 0x00RRGGBB
// for a Cairo RGB24 surface): a dark, cool noise floor that recedes, ramping
// through purple/red/orange/yellow to white. The colour stops are packed into
// the upper range (most of the travel happens above 0.55) so differences in
// power among strong signals read as clear colour steps, not just "bright vs
// brighter". A gamma >1 first dims the low end (noise).
uint32_t heat(float v) {
    v = std::pow(std::clamp(v, 0.0f, 1.0f), 1.4f);
    constexpr int N = 8;
    static const float pos[N]    = {0.00f, 0.35f, 0.55f, 0.68f, 0.78f, 0.86f, 0.93f, 1.00f};
    static const float col[N][3] = {
        {  0,   0,   0},   // black
        { 35,  10,  80},   // indigo (noise floor)
        {110,  25, 110},   // purple
        {180,  35,  85},   // red-magenta
        {225,  60,  40},   // red
        {245, 120,  20},   // orange
        {252, 190,  55},   // yellow
        {255, 255, 235},   // white (peak)
    };
    int i = 0;
    while (i < N - 2 && v > pos[i + 1]) ++i;
    const float t = (v - pos[i]) / (pos[i + 1] - pos[i]);
    const uint32_t r = static_cast<uint32_t>(col[i][0] + t * (col[i + 1][0] - col[i][0]));
    const uint32_t g = static_cast<uint32_t>(col[i][1] + t * (col[i + 1][1] - col[i][1]));
    const uint32_t b = static_cast<uint32_t>(col[i][2] + t * (col[i + 1][2] - col[i][2]));
    return (r << 16) | (g << 8) | b;
}

}  // namespace

CwSkimmerPanel::CwSkimmerPanel() : Gtk::Box(Gtk::Orientation::VERTICAL) {
    set_spacing(4);
    ui::setMargin(*this, 4);

    waterfall_.set_content_height(160);
    waterfall_.set_vexpand(true);
    waterfall_.set_draw_func(sigc::mem_fun(*this, &CwSkimmerPanel::onDrawWaterfall));
    append(waterfall_);

    // --- detection-gate slider: higher = stronger signals only -------------
    auto* gateBox = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL);
    gateBox->set_spacing(6);
    gateBox->append(*Gtk::make_managed<Gtk::Label>("Gate"));
    gateScale_ = Gtk::make_managed<Gtk::Scale>(Gtk::Orientation::HORIZONTAL);
    gateScale_->set_range(-12, 24);   // dB offset; 0 = default sensitivity
    gateScale_->set_increments(1, 3);
    gateScale_->set_draw_value(false);
    gateScale_->set_hexpand(true);
    gateScale_->set_value(0);
    gateLabel_ = Gtk::make_managed<Gtk::Label>("0 dB");
    gateScale_->signal_value_changed().connect([this]() {
        const int db = static_cast<int>(gateScale_->get_value());
        gateLabel_->set_text(std::to_string(db) + " dB");
        if (!updatingGate_)
            signalGate_.emit(db);
    });
    gateBox->append(*gateScale_);
    gateBox->append(*gateLabel_);
    append(*gateBox);

    // --- per-channel min-SNR slider: rejects low-SNR channels (spurious E/T) -
    auto* snrBox = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL);
    snrBox->set_spacing(6);
    snrBox->append(*Gtk::make_managed<Gtk::Label>("Min SNR"));
    snrScale_ = Gtk::make_managed<Gtk::Scale>(Gtk::Orientation::HORIZONTAL);
    snrScale_->set_range(0, 30);   // dB; 0 = no per-channel gating
    snrScale_->set_increments(1, 3);
    snrScale_->set_draw_value(false);
    snrScale_->set_hexpand(true);
    snrScale_->set_value(0);
    snrLabel_ = Gtk::make_managed<Gtk::Label>("0 dB");
    snrScale_->signal_value_changed().connect([this]() {
        const int db = static_cast<int>(snrScale_->get_value());
        snrLabel_->set_text(std::to_string(db) + " dB");
        if (!updatingSnr_)
            signalMinSnr_.emit(db);
    });
    snrBox->append(*snrScale_);
    snrBox->append(*snrLabel_);
    append(*snrBox);

    // --- decode list: store -> selection -> view ---------------------------
    store_     = Gio::ListStore<SkimmerItem>::create();
    selection_ = Gtk::SingleSelection::create(store_);
    selection_->set_autoselect(false);
    selection_->set_can_unselect(true);
    columnView_.set_model(selection_);
    columnView_.add_css_class("data-table");
    columnView_.set_show_column_separators(true);

    columnView_.append_column(makeColumn("Freq", [](const SkimmerItem& i) { return i.freq.get_value(); }));
    columnView_.append_column(makeColumn("WPM",  [](const SkimmerItem& i) { return i.wpm.get_value(); }));
    columnView_.append_column(makeColumn("Text", [](const SkimmerItem& i) { return i.text.get_value(); }, true));
    columnView_.append_column(makeColumn("Call", [](const SkimmerItem& i) { return i.call.get_value(); }));

    auto* scroller = Gtk::make_managed<Gtk::ScrolledWindow>();
    scroller->set_policy(Gtk::PolicyType::AUTOMATIC, Gtk::PolicyType::AUTOMATIC);
    scroller->set_child(columnView_);
    scroller->set_min_content_height(120);
    append(*scroller);
}

Glib::RefPtr<Gtk::ColumnViewColumn> CwSkimmerPanel::makeColumn(
    const Glib::ustring& title, std::function<Glib::ustring(const SkimmerItem&)> getter,
    bool expand) {
    auto factory = Gtk::SignalListItemFactory::create();
    factory->signal_setup().connect([](const Glib::RefPtr<Gtk::ListItem>& li) {
        auto* label = Gtk::make_managed<Gtk::Label>();
        label->set_xalign(0.0);
        li->set_child(*label);
    });
    // Bind the cell to the item's properties so it updates in place as more
    // characters arrive. The per-listitem connections live in a map owned by the
    // factory slots (see DxClusterPanel::makeCountColumn for why) and are
    // disconnected on unbind so they never fire on a recycled/destroyed label.
    auto conns =
        std::make_shared<std::map<Gtk::ListItem*, std::vector<sigc::connection>>>();
    factory->signal_bind().connect([getter, conns](const Glib::RefPtr<Gtk::ListItem>& li) {
        auto* label = dynamic_cast<Gtk::Label*>(li->get_child());
        auto* item  = dynamic_cast<SkimmerItem*>(li->get_item().get());
        if (!label || !item)
            return;
        auto update = [label, item, getter]() { label->set_text(getter(*item)); };
        update();
        std::vector<sigc::connection> v;
        v.push_back(item->freq.get_proxy().signal_changed().connect(update));
        v.push_back(item->wpm.get_proxy().signal_changed().connect(update));
        v.push_back(item->text.get_proxy().signal_changed().connect(update));
        v.push_back(item->call.get_proxy().signal_changed().connect(update));
        (*conns)[li.get()] = std::move(v);
    });
    factory->signal_unbind().connect([conns](const Glib::RefPtr<Gtk::ListItem>& li) {
        auto it = conns->find(li.get());
        if (it != conns->end()) {
            for (auto& c : it->second)
                c.disconnect();
            conns->erase(it);
        }
    });
    auto column = Gtk::ColumnViewColumn::create(title, factory);
    column->set_resizable(true);
    column->set_expand(expand);
    return column;
}

void CwSkimmerPanel::addWaterfall(const std::vector<float>& mags, double minHz,
                                  double maxHz) {
    minHz_ = minHz;
    maxHz_ = maxHz;
    const int cols = static_cast<int>(mags.size());
    if (cols <= 0)
        return;
    if (cols_ != cols) {
        cols_ = cols;
        pixels_.assign(static_cast<std::size_t>(cols) * kHistory, 0);
    }
    // Scroll down one row, write the newest line at the top.
    std::memmove(pixels_.data() + cols_, pixels_.data(),
                 static_cast<std::size_t>(cols_) * (kHistory - 1) * sizeof(uint32_t));
    for (int x = 0; x < cols_; ++x)
        pixels_[x] = heat(mags[x]);
    waterfall_.queue_draw();
}

void CwSkimmerPanel::onDrawWaterfall(const Cairo::RefPtr<Cairo::Context>& cr, int w, int h) {
    cr->set_source_rgb(0, 0, 0);
    cr->paint();
    if (cols_ > 0 && !pixels_.empty()) {
        const int stride =
            Cairo::ImageSurface::format_stride_for_width(Cairo::Surface::Format::RGB24, cols_);
        auto surf = Cairo::ImageSurface::create(
            reinterpret_cast<unsigned char*>(pixels_.data()),
            Cairo::Surface::Format::RGB24, cols_, kHistory, stride);
        cr->save();
        cr->scale(static_cast<double>(w) / cols_, static_cast<double>(h) / kHistory);
        cr->set_source(surf, 0, 0);
        cr->paint();
        cr->restore();
    }
    if (maxHz_ <= minHz_)
        return;
    const double span = maxHz_ - minHz_;

    // Frequency grid + axis labels every 500 Hz.
    cr->set_font_size(10);
    for (int hz = static_cast<int>(std::ceil(minHz_ / 500) * 500); hz < maxHz_; hz += 500) {
        const double x = (hz - minHz_) / span * w;
        cr->set_source_rgba(1, 1, 1, 0.15);
        cr->move_to(x, 0);
        cr->line_to(x, h);
        cr->stroke();
        cr->set_source_rgba(0.7, 0.7, 0.7, 1.0);
        cr->move_to(x + 2, h - 3);
        cr->show_text(std::to_string(hz));
    }
    // Callsign tags over the traces they were decoded from.
    for (const auto& [hz, call] : labels_) {
        if (hz < minHz_ || hz > maxHz_)
            continue;
        const double x = (hz - minHz_) / span * w;
        cr->set_source_rgba(0, 0, 0, 0.6);
        cr->rectangle(x + 2, 1, call.size() * 7.0 + 4, 14);
        cr->fill();
        cr->set_source_rgb(0.47, 1.0, 0.47);
        cr->move_to(x + 4, 12);
        cr->show_text(call);
    }
}

void CwSkimmerPanel::updateChannel(int id, double hz, int wpm, const std::string& text,
                                   const std::string& call) {
    char freq[32];
    std::snprintf(freq, sizeof(freq), "%d Hz", static_cast<int>(std::lround(hz)));
    auto it = items_.find(id);
    if (it == items_.end()) {
        auto item = SkimmerItem::create();
        item->id = id;
        item->hz = hz;
        item->freq.set_value(freq);
        item->wpm.set_value(std::to_string(wpm));
        item->text.set_value(text);
        item->call.set_value(call);
        // Insert so the list stays ordered by frequency (a channel's frequency
        // never changes, so rows only move on insert/remove — never on update).
        guint pos = store_->get_n_items();
        for (guint i = 0; i < store_->get_n_items(); ++i)
            if (store_->get_item(i)->hz > hz) { pos = i; break; }
        store_->insert(pos, item);
        items_[id] = item;
    } else {
        // Update the bound properties in place (freq is fixed; row keeps its slot).
        it->second->wpm.set_value(std::to_string(wpm));
        it->second->text.set_value(text);
        it->second->call.set_value(call);
    }

    // Refresh the waterfall callsign tags from the live channel set.
    labels_.clear();
    for (const auto& [cid, item] : items_) {
        const std::string c = item->call.get_value().raw();
        if (!c.empty()) {
            const double f = std::atof(item->freq.get_value().raw().c_str());
            labels_[f] = c;
        }
    }
    waterfall_.queue_draw();
}

void CwSkimmerPanel::removeChannel(int id) {
    auto it = items_.find(id);
    if (it == items_.end())
        return;
    for (guint i = 0; i < store_->get_n_items(); ++i)
        if (store_->get_item(i).get() == it->second.get()) {
            store_->remove(i);
            break;
        }
    items_.erase(it);
}

void CwSkimmerPanel::clear() {
    store_->remove_all();
    items_.clear();
    labels_.clear();
    std::fill(pixels_.begin(), pixels_.end(), 0);
    waterfall_.queue_draw();
}

void CwSkimmerPanel::setGate(int db) {
    updatingGate_ = true;   // restore from settings without re-emitting
    gateScale_->set_value(db);
    gateLabel_->set_text(std::to_string(db) + " dB");
    updatingGate_ = false;
}

void CwSkimmerPanel::setMinSnr(int db) {
    updatingSnr_ = true;
    snrScale_->set_value(db);
    snrLabel_->set_text(std::to_string(db) + " dB");
    updatingSnr_ = false;
}
