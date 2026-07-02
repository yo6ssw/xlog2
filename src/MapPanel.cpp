// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2025 Adrian Scripcă (YO6SSW)

#include "MapPanel.h"

#include "UiUtil.h"

#include <cmath>

MapPanel::MapPanel() : Gtk::Box(Gtk::Orientation::VERTICAL) {
    set_spacing(4);
    ui::setMargin(*this, 4);

    coast_ = geo::loadCoastline(geo::defaultCoastlinePath());

    auto* form = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL);
    form->set_spacing(6);
    form->append(*Gtk::make_managed<Gtk::Label>("From:"));
    fromEntry_.set_placeholder_text("my grid");
    fromEntry_.set_hexpand(true);
    form->append(fromEntry_);
    form->append(*Gtk::make_managed<Gtk::Label>("To:"));
    toEntry_.set_placeholder_text("their grid");
    toEntry_.set_hexpand(true);
    form->append(toEntry_);
    append(*form);

    area_.set_hexpand(true);
    area_.set_vexpand(true);
    area_.set_content_width(240);
    area_.set_content_height(140);
    area_.set_draw_func(sigc::mem_fun(*this, &MapPanel::onDraw));
    append(area_);

    info_.set_xalign(0.0);
    append(info_);

    fromEntry_.signal_changed().connect([this]() { if (!loading_) recompute(); });
    toEntry_.signal_changed().connect([this]() { if (!loading_) recompute(); });
    recompute();
}

void MapPanel::setFrom(const std::string& grid) {
    loading_ = true;
    fromEntry_.set_text(grid);
    loading_ = false;
    recompute();
}

void MapPanel::setTo(const std::string& grid) {
    loading_ = true;
    toEntry_.set_text(grid);
    loading_ = false;
    recompute();
}

void MapPanel::recompute() {
    from_ = geo::maidenheadToLatLon(fromEntry_.get_text().raw());
    to_   = geo::maidenheadToLatLon(toEntry_.get_text().raw());
    path_.clear();
    if (from_ && to_) {
        path_ = geo::greatCircle(*from_, *to_, 128);
        const double km = geo::distanceKm(*from_, *to_);
        const double br = geo::bearingDeg(*from_, *to_);
        info_.set_text(std::to_string(static_cast<long>(km + 0.5)) + " km   " +
                       std::to_string(static_cast<long>(br + 0.5)) + "°");
    } else {
        info_.set_text("Enter two valid grid locators.");
    }
    area_.queue_draw();
}

void MapPanel::onDraw(const Cairo::RefPtr<Cairo::Context>& cr, int w, int h) {
    // Centred 2:1 map rectangle.
    const double mapW = std::min(static_cast<double>(w), h * 2.0);
    const double mapH = mapW / 2.0;
    const double ox = (w - mapW) / 2.0;
    const double oy = (h - mapH) / 2.0;

    auto project = [&](geo::LatLon ll) -> std::pair<double, double> {
        const geo::XY xy = geo::equirect(ll);
        return {ox + xy.x * mapW, oy + xy.y * mapH};
    };
    auto stroke = [&](const std::vector<geo::LatLon>& poly) {
        bool have = false;
        double px = 0, py = 0;
        for (const auto& ll : poly) {
            auto [cx, cy] = project(ll);
            if (have && std::abs(cx - px) <= mapW / 2.0) {
                cr->move_to(px, py);
                cr->line_to(cx, cy);
            }
            px = cx; py = cy; have = true;
        }
        cr->stroke();
    };

    cr->set_source_rgb(0.063, 0.094, 0.141);  // background
    cr->paint();
    cr->set_source_rgb(0.086, 0.165, 0.247);  // ocean
    cr->rectangle(ox, oy, mapW, mapH);
    cr->fill();

    // Graticule every 30°.
    cr->set_line_width(1.0);
    cr->set_source_rgb(0.173, 0.267, 0.369);
    for (int lon = -180; lon <= 180; lon += 30) {
        const double x = ox + (lon + 180.0) / 360.0 * mapW;
        cr->move_to(x, oy);
        cr->line_to(x, oy + mapH);
    }
    for (int lat = -90; lat <= 90; lat += 30) {
        const double y = oy + (90.0 - lat) / 180.0 * mapH;
        cr->move_to(ox, y);
        cr->line_to(ox + mapW, y);
    }
    cr->stroke();

    // Coastline.
    cr->set_source_rgb(0.435, 0.616, 0.455);
    for (const auto& poly : coast_)
        stroke(poly);

    // Great-circle path.
    if (!path_.empty()) {
        cr->set_line_width(2.0);
        cr->set_source_rgb(1.0, 0.702, 0.302);
        stroke(path_);
    }

    // Endpoints.
    auto dot = [&](geo::LatLon ll, double r, double g, double b) {
        auto [x, y] = project(ll);
        cr->set_source_rgb(r, g, b);
        cr->arc(x, y, 4.0, 0.0, 2 * M_PI);
        cr->fill();
    };
    if (from_) dot(*from_, 0.302, 0.765, 1.0);
    if (to_)   dot(*to_, 1.0, 0.420, 0.420);
}
