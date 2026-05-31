#pragma once

#include "LogBook.h"
#include "QsoItem.h"
#include "Udp.h"

#include <gtkmm.h>

#include <functional>
#include <string>

// The application's main window: a menu bar, a sortable log view, an entry
// form for adding/editing QSOs, and a status line.
class MainWindow : public Gtk::ApplicationWindow {
public:
    MainWindow();

private:
    // --- construction helpers ---
    void buildActions();
    Glib::RefPtr<Gio::Menu> buildMenuModel();
    void buildLogView();
    Gtk::Widget& buildEntryForm();
    Glib::RefPtr<Gtk::ColumnViewColumn> makeColumn(
        const Glib::ustring& title, std::function<std::string(const Qso&)> getter,
        bool expand = false);

    // --- data <-> UI ---
    void refreshList();
    void onSelectionChanged();
    Qso  formToQso() const;
    void qsoToForm(const Qso& q);
    void clearForm();

    // --- form actions ---
    void onAddOrUpdate();
    void onDeleteSelected();
    void onFrequencyChanged();
    void onSetNow();

    // --- menu actions ---
    void onNew();
    void onOpen();
    void onSaveAs();
    void onImportAdif();
    void onExportAdif();
    void onStatistics();
    void onAbout();

    // --- UDP network logging ---
    void onToggleUdp();
    void onUdpSettings();
    void onUdpReceived(const std::vector<Qso>& qsos, const std::string& source);

    // --- misc ---
    void setStatus(const Glib::ustring& msg);
    void updateTitle();

    LogBook logbook_;

    // Log view
    Glib::RefPtr<Gio::ListStore<QsoItem>> store_;
    Glib::RefPtr<Gtk::SingleSelection>    selection_;
    Gtk::ColumnView                       columnView_;

    // Entry form fields
    Gtk::Entry    date_, timeOn_, timeOff_, call_, freq_;
    Gtk::Entry    rstSent_, rstRcvd_, name_, qth_, locator_, power_, comment_;
    Gtk::DropDown band_, mode_;
    Glib::RefPtr<Gtk::StringList> bandModel_, modeModel_;
    Gtk::CheckButton qslSent_, qslRcvd_;
    Gtk::Button   addButton_, deleteButton_, clearButton_;

    Gtk::Label statusLabel_;

    // UDP network logging
    UdpListener                   listener_;
    int                           udpPort_ = 2237;  // WSJT-X default
    Glib::RefPtr<Gio::SimpleAction> udpAction_;

    long editingId_ = 0;  // id of the QSO currently loaded in the form, 0 = new
};
