#pragma once

#include <gtkmm.h>

#include <array>
#include <functional>
#include <string>

// A compact, dockable rig-control panel: the current VFO frequency in a large
// font, four tune buttons (<< / < / > / >> for -500 / -100 / +100 / +500 Hz),
// the current mode, and an IF-filter selector (1 = wide, 2 = normal,
// 3 = narrow). MainWindow owns the RigController and feeds this panel via
// setState()/setConnected(); the panel emits signalStep()/signalSetFilter()
// which the shell forwards to the controller.
class RigPanel : public Gtk::Box {
public:
    RigPanel();

    // Update the readouts (frequency in MHz, mode name, passband width in Hz,
    // and the IF-filter slot 1..3 — 0 if unknown).
    void setState(double mhz, const std::string& mode, int pbwidthHz, int filter);
    // Enable/disable the controls and grey out the readouts when no rig is up.
    void setConnected(bool connected);

    sigc::signal<void(double)>& signalStep()      { return signalStep_; }  // signed Hz
    sigc::signal<void(int)>&    signalSetFilter() { return signalSetFilter_; }

private:
    void buildUi();
    void updateFilterButtons(int filter);

    Gtk::Label freqLabel_;
    Gtk::Label modeLabel_;
    std::array<Gtk::ToggleButton*, 3> filterButtons_{nullptr, nullptr, nullptr};

    bool connected_      = false;
    bool updatingFilter_ = false;  // suppress emission during programmatic toggles
    int  filter_         = 0;

    sigc::signal<void(double)> signalStep_;
    sigc::signal<void(int)>    signalSetFilter_;
};
