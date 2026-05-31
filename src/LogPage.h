#pragma once

#include "LogBook.h"
#include "Qrz.h"
#include "QsoItem.h"

#include <gtkmm.h>

#include <functional>
#include <string>
#include <utility>
#include <vector>

// One logbook tab: a sortable log view plus an entry form for adding/editing
// QSOs, with live duplicate detection. Owns its own LogBook. Emits signals so
// the hosting window can update tab labels, the title and the status line.
class LogPage : public Gtk::Box {
public:
    LogPage();

    // Logbook operations (each refreshes the view and emits signal_changed).
    void newInMemory();
    bool openFile(const std::string& path);
    bool saveAs(const std::string& path);
    int  importAdif(const std::string& adifText);
    std::string exportAdif() const;

    const LogBook& logbook() const { return logbook_; }
    std::string path() const { return logbook_.path(); }
    bool isFileBacked() const { return logbook_.isFileBacked(); }
    Glib::ustring title() const;            // file basename or "Untitled"
    std::size_t qsoCount() const { return logbook_.qsos().size(); }

    // Add a QSO received from an external source (e.g. UDP), flagging dupes.
    void addExternalQso(const Qso& q);

    // Rig control: fill the frequency (auto-detecting band) and/or mode.
    void setRigFrequency(double mhz);
    void setRigMode(const std::string& mode);

    // Reveal the search bar and focus it (the "Find" action).
    void beginSearch();

    // Prefill the entry form's name/QTH/locator from a QRZ.com lookup.
    void applyQrzLookup(const QrzResult& r);

    // LoTW helpers (delegate to the LogBook, then refresh + notify).
    std::vector<Qso> qsosNotLotwSent() const;
    void markLotwSent(const std::vector<long>& ids, const std::string& date);
    int  applyLotwConfirmations(const std::vector<Qso>& confirmed);
    void refresh();  // repaint the list after external changes

    // Shared column layout (order/width/visibility) persistence.
    void applyColumnLayout(const Glib::RefPtr<Glib::KeyFile>& keyfile);
    void storeColumnLayout(const Glib::RefPtr<Glib::KeyFile>& keyfile);

    // Emitted when the logbook content/path changes (for tab label + title).
    sigc::signal<void()>& signalChanged() { return signalChanged_; }
    // Emitted to surface a status message to the hosting window.
    sigc::signal<void(const Glib::ustring&)>& signalStatus() { return signalStatus_; }
    // Emitted (with the entered callsign) when the user asks for a QRZ lookup.
    sigc::signal<void(const std::string&)>& signalLookupCall() { return signalLookupCall_; }

private:
    void buildLogView();
    void buildSearch();
    // Combined searchable text for a row; the StringFilter substring-matches it.
    Glib::ustring rowSearchText(const Glib::RefPtr<Glib::ObjectBase>& obj);
    void onSearchChanged();
    void buildEntryForm();
    Glib::RefPtr<Gtk::ColumnViewColumn> makeColumn(
        const Glib::ustring& title, std::function<std::string(const Qso&)> getter,
        bool expand = false);
    void applyColumnOrder(const std::vector<std::string>& ids);

    // Column reordering / visibility via the header context menu.
    void buildColumnMenus();
    void moveColumn(const Glib::ustring& id, int delta);
    void setColumnVisible(const Glib::ustring& id, bool visible);
    void showAllColumns();
    void pinFiller();  // keep the empty filler column last

    void refreshList();
    void onSelectionChanged();
    Qso  formToQso() const;
    void qsoToForm(const Qso& q);
    void clearForm();
    void onAddOrUpdate();
    void onDeleteSelected();
    void onFrequencyChanged();
    void onLookupCall();  // QRZ icon in the callsign entry
    void onSetNow();
    void updateDupeIndicator();
    void status(const Glib::ustring& msg) { signalStatus_.emit(msg); }

    LogBook logbook_;

    Glib::RefPtr<Gio::ListStore<QsoItem>> store_;
    Glib::RefPtr<Gtk::StringFilter>       filter_;
    Glib::RefPtr<Gtk::FilterListModel>    filterModel_;
    Glib::RefPtr<Gtk::SingleSelection>    selection_;
    Gtk::ColumnView                       columnView_;
    Gtk::SearchBar                        searchBar_;
    Gtk::SearchEntry                      searchEntry_;
    std::vector<std::pair<std::string, Glib::RefPtr<Gtk::ColumnViewColumn>>> columns_;
    // Empty trailing column that expands to absorb leftover horizontal space,
    // so the last data column isn't stretched. Not part of columns_, so it is
    // excluded from reordering, hiding and layout persistence.
    Glib::RefPtr<Gtk::ColumnViewColumn>   filler_;
    Glib::RefPtr<Gio::SimpleActionGroup>  colActions_;

    Gtk::Entry    date_, timeOn_, timeOff_, call_, freq_;
    Gtk::Entry    rstSent_, rstRcvd_, name_, qth_, locator_, power_, comment_;
    Gtk::DropDown band_, mode_;
    Glib::RefPtr<Gtk::StringList> bandModel_, modeModel_;
    Gtk::CheckButton qslSent_, qslRcvd_;
    Gtk::Button   addButton_, deleteButton_, clearButton_;
    Gtk::Label    dupeLabel_;

    sigc::signal<void()>                     signalChanged_;
    sigc::signal<void(const Glib::ustring&)> signalStatus_;
    sigc::signal<void(const std::string&)>   signalLookupCall_;

    long editingId_ = 0;  // id of the QSO loaded in the form, 0 = new
};
