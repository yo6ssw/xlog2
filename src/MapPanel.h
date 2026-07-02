// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2025 Adrian Scripcă (YO6SSW)

#pragma once

#include "Geo.h"

#include <gtkmm.h>

#include <optional>
#include <string>
#include <vector>

// gtkmm world-map panel: an equirectangular map (bundled coastline) with a
// great-circle line drawn between two Maidenhead locators, above From/To entry
// fields and a distance/bearing readout. The shell calls setFrom() with the
// operator's locator and setTo() with the selected QSO's locator; the user can
// also override either field by typing. Value member of the shell (like the
// other dockable panels) so it survives reparenting between paned slots.
class MapPanel : public Gtk::Box {
public:
    MapPanel();

    void setFrom(const std::string& grid);
    void setTo(const std::string& grid);

private:
    void recompute();  // re-parse both fields, refresh the drawing + readout
    void onDraw(const Cairo::RefPtr<Cairo::Context>& cr, int w, int h);

    Gtk::DrawingArea area_;
    Gtk::Entry       fromEntry_;
    Gtk::Entry       toEntry_;
    Gtk::Label       info_;
    bool             loading_ = false;

    std::vector<std::vector<geo::LatLon>> coast_;
    std::vector<geo::LatLon>              path_;
    std::optional<geo::LatLon>            from_, to_;
};
