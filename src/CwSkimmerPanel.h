#pragma once

#include "SkimmerItem.h"

#include <gtkmm.h>

#include <cstdint>
#include <map>
#include <string>
#include <vector>

// gtkmm CW-Skimmer panel: a Cairo waterfall of the rig-audio passband with the
// decoded callsigns labelled on the active traces, above a ColumnView listing
// every CW signal currently being copied (audio pitch / wpm / rolling text /
// callsign). MainWindow owns the CwSkimmer service and feeds this panel via
// addWaterfall()/updateChannel()/removeChannel(). It is a value member of the
// shell (like dxPanel_) so it survives being reparented between paned slots.
class CwSkimmerPanel : public Gtk::Box {
public:
    CwSkimmerPanel();

    void addWaterfall(const std::vector<float>& mags, double minHz, double maxHz);
    void updateChannel(int id, double hz, int wpm, const std::string& text,
                       const std::string& call);
    void removeChannel(int id);
    void clear();

private:
    Glib::RefPtr<Gtk::ColumnViewColumn> makeColumn(
        const Glib::ustring& title,
        std::function<Glib::ustring(const SkimmerItem&)> getter, bool expand = false);
    void onDrawWaterfall(const Cairo::RefPtr<Cairo::Context>& cr, int w, int h);

    Gtk::DrawingArea waterfall_;
    Gtk::ColumnView  columnView_;
    Glib::RefPtr<Gio::ListStore<SkimmerItem>> store_;
    Glib::RefPtr<Gtk::SingleSelection>        selection_;
    std::map<int, Glib::RefPtr<SkimmerItem>>  items_;  // id -> live row

    // Waterfall pixel history (cols_ wide, kHistory tall; RGB24, newest at top).
    static constexpr int kHistory = 400;
    int                   cols_ = 0;
    std::vector<uint32_t> pixels_;
    double                minHz_ = 0.0, maxHz_ = 0.0;
    std::map<double, std::string> labels_;  // pitch Hz -> callsign tag
};
