// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2025 Adrian Scripcă (YO6SSW)

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
    // Reflect the rig's power state; the power button is shown only when the
    // backend reports a power status (`supported`).
    void setPowerState(bool supported, bool on);

    sigc::signal<void(double)>& signalStep()      { return signalStep_; }  // signed Hz
    sigc::signal<void(int)>&    signalSetFilter() { return signalSetFilter_; }
    sigc::signal<void(bool)>&   signalSetPower()  { return signalSetPower_; }
    sigc::signal<void(bool)>&   signalSetAgc()    { return signalSetAgc_; }  // true = AGC on
    sigc::signal<void(std::string)>& signalSetMode() { return signalSetMode_; }

private:
    void buildUi();
    void updateFilterButtons(int filter);
    void updateModeDropdown(const std::string& mode);

    Gtk::Label freqLabel_;
    Gtk::Label modeLabel_;
    Gtk::DropDown* modeDropdown_ = nullptr;
    Gtk::ToggleButton* powerButton_ = nullptr;
    Gtk::ToggleButton* agcButton_   = nullptr;
    std::array<Gtk::ToggleButton*, 3> filterButtons_{nullptr, nullptr, nullptr};

    bool connected_      = false;
    bool updatingFilter_ = false;  // suppress emission during programmatic toggles
    bool updatingPower_  = false;  // suppress emission during programmatic toggles
    bool updatingMode_   = false;  // suppress emission during programmatic selects
    int  filter_         = 0;

    sigc::signal<void(double)> signalStep_;
    sigc::signal<void(int)>    signalSetFilter_;
    sigc::signal<void(bool)>   signalSetPower_;
    sigc::signal<void(bool)>   signalSetAgc_;
    sigc::signal<void(std::string)> signalSetMode_;
};
