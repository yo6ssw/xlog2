// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2025 Adrian Scripcă (YO6SSW)

#pragma once

#include <gtkmm.h>

#include <array>
#include <functional>
#include <string>
#include <utility>
#include <vector>

#include "ILogPageView.h"
#include "IniFile.h"
#include "LogPagePresenter.h"
#include "Qrz.h"
#include "QsoItem.h"

// One logbook tab: a gtkmm view (sortable log + entry form + keyer bar) over a
// LogPagePresenter, which owns the LogBook and all business logic. This class
// is the view in the MVP split: it builds widgets, implements ILogPageView for
// the presenter to drive, and forwards user events to the presenter. It keeps
// the same public API + signals the MainWindow shell already uses, delegating
// to the presenter.
class LogPage : public Gtk::Box, public ILogPageView {
 public:
  LogPage();

  // Logbook operations (delegate to the presenter).
  void newInMemory() { presenter_.newInMemory(); }
  bool openFile(const std::string& path) { return presenter_.openFile(path); }
  bool saveAs(const std::string& path) { return presenter_.saveAs(path); }
  int importAdif(const std::string& text) {
    return presenter_.importAdif(text);
  }
  int importXlog(const std::string& text) {
    return presenter_.importXlog(text);
  }
  std::string exportAdif() const { return presenter_.exportAdif(); }

  const LogBook& logbook() const { return presenter_.logbook(); }
  std::string path() const { return presenter_.path(); }
  bool isFileBacked() const { return presenter_.isFileBacked(); }
  Glib::ustring title() const { return presenter_.title(); }
  std::size_t qsoCount() const { return presenter_.qsoCount(); }

  void addExternalQso(const Qso& q) { presenter_.addExternalQso(q); }
  void setRigFrequency(double mhz) { presenter_.setRigFrequency(mhz); }
  void setRigMode(const std::string& mode) { presenter_.setRigMode(mode); }
  void beginSearch();
  void applyQrzLookup(const QrzResult& r) { presenter_.applyQrzLookup(r); }
  void setCwMessages(const std::array<std::string, 9>& m) {
    presenter_.setCwMessages(m);
  }
  void applyDxSpot(const std::string& c, double mhz) {
    presenter_.applyDxSpot(c, mhz);
  }
  void backfillDxcc() { presenter_.backfillDxcc(); }

  std::vector<Qso> qsosNotLotwSent() const {
    return presenter_.qsosNotLotwSent();
  }
  void markLotwSent(const std::vector<long>& ids, const std::string& date) {
    presenter_.markLotwSent(ids, date);
  }
  int applyLotwConfirmations(const std::vector<Qso>& c) {
    return presenter_.applyLotwConfirmations(c);
  }
  void refresh() { presenter_.refresh(); }

  // The shell's MainPresenter routes service results to the current tab's
  // presenter (UDP QSOs, rig readings, QRZ/LoTW results).
  LogPagePresenter& presenter() { return presenter_; }

  // Shell-wired hooks for the row context menu's "Move to" submenu.
  // queryMoveTargets returns (title, presenter) for every OTHER open logbook;
  // requestMove asks the shell to move the QSO with the given id there.
  std::function<std::vector<std::pair<std::string, LogPagePresenter*>>()>
      queryMoveTargets;
  std::function<void(long, LogPagePresenter*)> requestMove;

  // Shared column layout (order/width/visibility) persistence.
  void applyColumnLayout(const IniFile& ini);
  void storeColumnLayout(IniFile& ini);

  // Shell-facing signals (bridged from the presenter's hooks).
  sigc::signal<void()>& signalChanged() { return signalChanged_; }
  sigc::signal<void(const Glib::ustring&)>& signalStatus() {
    return signalStatus_;
  }
  sigc::signal<void(const std::string&)>& signalLookupCall() {
    return signalLookupCall_;
  }
  sigc::signal<void(const std::string&)>& signalSendCw() {
    return signalSendCw_;
  }
  sigc::signal<void()>& signalAbortCw() { return signalAbortCw_; }
  sigc::signal<void(const std::string&)>& signalLocator() {
    return signalLocator_;
  }

  // --- ILogPageView ---
  FormData formData() const override;
  void setFormData(const FormData&) override;
  void setCall(const std::string&) override;
  void setFreq(const std::string&) override;
  void setBand(const std::string&) override;
  void setMode(const std::string&) override;
  void setRows(const std::vector<Qso>&) override;
  void clearSelection() override;
  void setDupeWarning(const std::string& msg, bool highlight) override;
  void setDxccText(const std::string&) override;
  void setEditing(bool editing) override;
  void setCwButtons(const std::array<std::string, 9>& messages) override;
  void focusCall() override;
  void showSearch() override;

 private:
  void buildLogView();
  void buildSearch();
  Glib::ustring rowSearchText(const Glib::RefPtr<Glib::ObjectBase>& obj);
  void onSearchChanged();
  void buildEntryForm();
  void buildKeyerBar(Gtk::Box& parent);  // F1–F9 keyer buttons + Stop
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

  void onSelectionChanged();
  // Row context menu (right-click a QSO): Delete + Move to.
  void showRowContextMenu(Gtk::ListItem* li, Gtk::Widget& anchor, double x,
                          double y);
  void confirmDeleteRow(long id);
  void status(const Glib::ustring& msg) { signalStatus_.emit(msg); }

  LogPagePresenter presenter_;

  Glib::RefPtr<Gio::ListStore<QsoItem>> store_;
  Glib::RefPtr<Gtk::StringFilter> filter_;
  Glib::RefPtr<Gtk::FilterListModel> filterModel_;
  Glib::RefPtr<Gtk::SingleSelection> selection_;
  Gtk::ColumnView columnView_;
  Gtk::SearchBar searchBar_;
  Gtk::SearchEntry searchEntry_;
  std::vector<std::pair<std::string, Glib::RefPtr<Gtk::ColumnViewColumn>>>
      columns_;
  Glib::RefPtr<Gtk::ColumnViewColumn> filler_;
  Glib::RefPtr<Gio::SimpleActionGroup> colActions_;

  // Row context menu: one popover parented to the column view (stable parent),
  // its model rebuilt per click. moveTargets_ parallels the "Move to" items.
  Glib::RefPtr<Gio::SimpleActionGroup> rowActions_;
  Gtk::PopoverMenu* rowMenu_ = nullptr;
  long contextId_ = 0;
  std::vector<LogPagePresenter*> moveTargets_;

  Gtk::Entry date_, timeOn_, timeOff_, call_, freq_;
  Gtk::Entry rstSent_, rstRcvd_, name_, qth_, locator_, power_, comment_;
  Gtk::DropDown band_, mode_;
  Glib::RefPtr<Gtk::StringList> bandModel_, modeModel_;
  Gtk::CheckButton qslSent_, qslRcvd_;
  Gtk::Button addButton_, deleteButton_, clearButton_;
  Gtk::Label dupeLabel_;
  Gtk::Label dxccLabel_;

  std::array<Gtk::Button*, 9> cwButtons_{};

  // Guards against re-entrant indicator updates while setFormData writes
  // widgets (which fire change signals that would call back into the
  // presenter).
  bool loadingForm_ = false;

  sigc::signal<void()> signalChanged_;
  sigc::signal<void(const Glib::ustring&)> signalStatus_;
  sigc::signal<void(const std::string&)> signalLookupCall_;
  sigc::signal<void(const std::string&)> signalSendCw_;
  sigc::signal<void()> signalAbortCw_;
  sigc::signal<void(const std::string&)> signalLocator_;
};
