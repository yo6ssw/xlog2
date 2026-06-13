#include "RigPanel.h"

#include "UiUtil.h"

#include <cmath>
#include <cstdio>

namespace {
// Classic transceiver readout: MHz.kHz.tens-of-Hz, e.g. 14074000 Hz -> "14.074.00".
std::string formatFreq(double mhz) {
    if (mhz <= 0.0)
        return "—.———.——";
    const long hz      = std::lround(mhz * 1.0e6);
    const long mhzPart = hz / 1000000;
    const long khzPart = (hz / 1000) % 1000;
    const long tens    = (hz % 1000) / 10;
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%ld.%03ld.%02ld", mhzPart, khzPart, tens);
    return buf;
}
}  // namespace

RigPanel::RigPanel() : Gtk::Box(Gtk::Orientation::VERTICAL) {
    buildUi();
    setConnected(false);
}

void RigPanel::buildUi() {
    set_spacing(6);
    ui::setMargin(*this, 8);

    // Power on/off, top-right. Shown only once the rig reports a power status.
    auto* powerBox = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL);
    powerBox->set_halign(Gtk::Align::END);
    powerButton_ = Gtk::make_managed<Gtk::ToggleButton>("⏻");
    powerButton_->set_tooltip_text("Power the rig on/off");
    powerButton_->signal_toggled().connect([this]() {
        if (updatingPower_)
            return;
        signalSetPower_.emit(powerButton_->get_active());
    });
    powerButton_->set_visible(false);
    powerBox->append(*powerButton_);
    append(*powerBox);

    // Frequency, large and centred.
    freqLabel_.set_use_markup(true);
    freqLabel_.set_markup("<span size='28000' weight='bold'>" + formatFreq(0.0) + "</span>");
    freqLabel_.set_halign(Gtk::Align::CENTER);
    freqLabel_.set_hexpand(true);
    append(freqLabel_);

    modeLabel_.set_halign(Gtk::Align::CENTER);
    modeLabel_.add_css_class("dim-label");
    append(modeLabel_);

    // Tune buttons: << < > >>  (-500 / -100 / +100 / +500 Hz).
    auto* tuneBox = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL);
    tuneBox->set_halign(Gtk::Align::CENTER);
    tuneBox->set_spacing(4);
    tuneBox->add_css_class("linked");
    const struct { const char* label; double hz; const char* tip; } steps[] = {
        {"«", -500.0, "Down 500 Hz"},
        {"‹", -100.0, "Down 100 Hz"},
        {"›",  100.0, "Up 100 Hz"},
        {"»",  500.0, "Up 500 Hz"},
    };
    for (const auto& s : steps) {
        auto* b = Gtk::make_managed<Gtk::Button>(s.label);
        b->set_tooltip_text(s.tip);
        const double hz = s.hz;
        b->signal_clicked().connect([this, hz]() { signalStep_.emit(hz); });
        tuneBox->append(*b);
    }
    append(*tuneBox);

    // IF-filter selector: 1 = wide, 2 = normal, 3 = narrow.
    auto* filterBox = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL);
    filterBox->set_halign(Gtk::Align::CENTER);
    filterBox->set_spacing(4);
    auto* filterLabel = Gtk::make_managed<Gtk::Label>("Filter:");
    filterBox->append(*filterLabel);
    auto* toggles = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL);
    toggles->add_css_class("linked");
    const char* tips[3] = {"Filter 1 (wide)", "Filter 2 (normal)", "Filter 3 (narrow)"};
    for (int i = 0; i < 3; ++i) {
        auto* t = Gtk::make_managed<Gtk::ToggleButton>(std::to_string(i + 1));
        t->set_tooltip_text(tips[i]);
        const int n = i + 1;
        t->signal_toggled().connect([this, n, t]() {
            if (updatingFilter_)
                return;
            if (t->get_active())
                signalSetFilter_.emit(n);
            else
                // Don't allow un-selecting the active slot by clicking it again.
                updateFilterButtons(filter_);
        });
        filterButtons_[i] = t;
        toggles->append(*t);
    }
    filterBox->append(*toggles);
    append(*filterBox);

    // AGC enable/disable. Starts enabled (the rig's normal state); disabling keeps
    // signal levels proportional, which the CW skimmer wants. There is no readback,
    // so the toggle reflects the operator's intent.
    auto* agcBox = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL);
    agcBox->set_halign(Gtk::Align::CENTER);
    agcButton_ = Gtk::make_managed<Gtk::ToggleButton>("AGC");
    agcButton_->set_active(true);
    agcButton_->set_tooltip_text("Enable/disable the rig's AGC");
    agcButton_->signal_toggled().connect(
        [this]() { signalSetAgc_.emit(agcButton_->get_active()); });
    agcBox->append(*agcButton_);
    append(*agcBox);
}

void RigPanel::updateFilterButtons(int filter) {
    updatingFilter_ = true;
    for (int i = 0; i < 3; ++i)
        if (filterButtons_[i])
            filterButtons_[i]->set_active(filter == i + 1);
    updatingFilter_ = false;
}

void RigPanel::setState(double mhz, const std::string& mode, int pbwidthHz, int filter) {
    freqLabel_.set_markup("<span size='28000' weight='bold'>" + formatFreq(mhz) + "</span>");

    std::string info = mode;
    if (pbwidthHz > 0) {
        if (!info.empty())
            info += "  •  ";
        info += std::to_string(pbwidthHz) + " Hz";
    }
    modeLabel_.set_text(info);

    filter_ = filter;
    updateFilterButtons(filter);
}

void RigPanel::setPowerState(bool supported, bool on) {
    if (!powerButton_)
        return;
    powerButton_->set_visible(supported);
    updatingPower_ = true;
    powerButton_->set_active(on);
    updatingPower_ = false;
}

void RigPanel::setConnected(bool connected) {
    connected_ = connected;
    set_sensitive(connected);
    if (!connected) {
        modeLabel_.set_text("not connected");
        freqLabel_.set_markup("<span size='28000' weight='bold'>" + formatFreq(0.0) + "</span>");
        updateFilterButtons(0);
        if (powerButton_)
            powerButton_->set_visible(false);
    }
}
