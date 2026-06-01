#pragma once

#include "DxSpot.h"
#include "DxSpotItem.h"

#include <gtkmm.h>

#include <functional>
#include <set>
#include <string>

// The DX-cluster panel: a spot table (filterable by band), a raw-text console,
// a command entry and a connect/disconnect button. It is purely presentational
// — MainWindow owns the DxCluster and wires it to this panel's signals and to
// addSpot()/addLine()/setConnected().
class DxClusterPanel : public Gtk::Box {
public:
    DxClusterPanel();

    void addSpot(const DxSpot& spot);
    void addLine(const std::string& line);      // raw text -> console
    void setConnected(bool connected);          // updates the toggle + state

    sigc::signal<void(const DxSpot&)>&   signalActivate()      { return signalActivate_; }
    sigc::signal<void(const std::string&)>& signalCommand()    { return signalCommand_; }
    sigc::signal<void()>&                signalConnectToggle() { return signalConnectToggle_; }

private:
    Glib::RefPtr<Gtk::ColumnViewColumn> makeColumn(
        const Glib::ustring& title, std::function<std::string(const DxSpot&)> getter,
        bool expand = false);
    void buildBandChips();
    void refreshFilter();
    bool spotMatchesFilter(const Glib::RefPtr<Glib::ObjectBase>& obj);
    void onActivate(guint position);
    void onSendCommand();

    Glib::RefPtr<Gio::ListStore<DxSpotItem>> store_;
    Glib::RefPtr<Gtk::FilterListModel>       filterModel_;
    Glib::RefPtr<Gtk::BoolFilter>            filter_;
    Glib::RefPtr<Gtk::SingleSelection>       selection_;
    Gtk::ColumnView columnView_;

    Gtk::FlowBox    bandChips_;
    std::set<std::string> activeBands_;   // empty = show all

    Gtk::TextView   console_;
    Gtk::Entry      commandEntry_;
    Gtk::Button     connectButton_;
    bool            connected_ = false;

    sigc::signal<void(const DxSpot&)>       signalActivate_;
    sigc::signal<void(const std::string&)>  signalCommand_;
    sigc::signal<void()>                    signalConnectToggle_;
};
