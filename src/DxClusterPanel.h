#pragma once

#include "BandMapItem.h"
#include "DxSpot.h"

#include <gtkmm.h>

#include <chrono>
#include <functional>
#include <map>
#include <set>
#include <string>

// The DX-cluster band map: incoming spots are aggregated by (frequency, DX
// call), the table shows one row per pair ordered by frequency with a count of
// spotters (hover the count for the spotter list), plus band-filter chips, a
// raw console, a command entry and a connect/disconnect button. Each spotter
// expires 5 minutes after its report; an entry disappears once its last spotter
// has expired. MainWindow owns the DxCluster and feeds this panel via addSpot()
// /addLine()/setConnected().
class DxClusterPanel : public Gtk::Box {
public:
    DxClusterPanel();
    ~DxClusterPanel() override;

    void addSpot(const DxSpot& spot);
    void addLine(const std::string& line);      // raw text -> console
    void setConnected(bool connected);

    sigc::signal<void(const DxSpot&)>&   signalActivate()      { return signalActivate_; }
    sigc::signal<void(const std::string&)>& signalCommand()    { return signalCommand_; }
    sigc::signal<void()>&                signalConnectToggle() { return signalConnectToggle_; }

private:
    using Clock = std::chrono::steady_clock;

    struct SpotterInfo {
        Clock::time_point time;
        std::string       comment;
        std::string       timeUtc;
    };
    struct Entry {
        double      freqKHz = 0.0;
        std::string dxCall;
        std::string band;
        std::map<std::string, SpotterInfo> spotters;  // spotter call -> latest
    };

    Glib::RefPtr<Gtk::ColumnViewColumn> makeColumn(
        const Glib::ustring& title, std::function<std::string(const BandMapItem&)> getter,
        bool expand = false);
    Glib::RefPtr<Gtk::ColumnViewColumn> makeCountColumn();
    void buildBandChips();
    void refreshFilter();
    bool spotMatchesFilter(const Glib::RefPtr<Glib::ObjectBase>& obj);
    void rebuild();             // prune expired spotters + repopulate the store
    void onActivate(guint position);
    void onSendCommand();

    Glib::RefPtr<Gio::ListStore<BandMapItem>> store_;
    Glib::RefPtr<Gtk::FilterListModel>        filterModel_;
    Glib::RefPtr<Gtk::BoolFilter>             filter_;
    Glib::RefPtr<Gtk::SingleSelection>        selection_;
    Gtk::ColumnView columnView_;

    Gtk::FlowBox    bandChips_;
    std::set<std::string> activeBands_;   // empty = show all

    Gtk::TextView   console_;
    Gtk::Entry      commandEntry_;
    Gtk::Button     connectButton_;
    bool            connected_ = false;

    // Band-map state, keyed by (freq in 0.1 kHz units, DX call) so iteration is
    // ordered by frequency then call. items_ holds the live ColumnView row for
    // each key, so rows can be updated in place rather than rebuilt.
    using Key = std::pair<long, std::string>;
    std::map<Key, Entry> entries_;
    std::map<Key, Glib::RefPtr<BandMapItem>> items_;
    sigc::connection expiryTimer_;

    sigc::signal<void(const DxSpot&)>       signalActivate_;
    sigc::signal<void(const std::string&)>  signalCommand_;
    sigc::signal<void()>                    signalConnectToggle_;
};
