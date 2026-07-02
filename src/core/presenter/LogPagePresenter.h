// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2025 Adrian Scripcă (YO6SSW)

#pragma once

#include <array>
#include <cstddef>
#include <functional>
#include <map>
#include <string>
#include <vector>

#include "DxccDeriver.h"
#include "ILogPageView.h"
#include "LogBook.h"
#include "Qrz.h"
#include "Qso.h"

// All the per-logbook business logic, with no toolkit dependency. Owns the
// LogBook; drives an ILogPageView for display and is driven by the view's user
// events (the on*() handlers). Reports up to the hosting shell through the
// std::function hooks, which the shell wires.
class LogPagePresenter {
 public:
  explicit LogPagePresenter(ILogPageView& view) : view_(view) {}

  // Populate the (now-built) view and reset the form. Call once after the view
  // has constructed its widgets — the presenter must not touch them earlier.
  void start();

  // --- outputs to the shell (wired by the host) ---
  std::function<void()> onChanged;                       // content/path changed
  std::function<void(const std::string&)> onStatus;      // status-line text
  std::function<void(const std::string&)> onLookupCall;  // QRZ lookup requested
  std::function<void(const std::string&)> onSendCw;  // expanded CW text to key
  std::function<void()> onAbortCw;                   // abort keying
  std::function<void(const std::string&)>
      onLocator;  // current locator (map "to")

  // --- sync hooks (wired only for the synced logbook) ---
  // A QSO was added or edited locally: propagate it to peers. Fired with the
  // stored record (uuid + updated_at populated). Remote-applied merges do NOT
  // fire these, so there is no echo loop.
  std::function<void(const Qso&)> onLocalUpsert;
  // A QSO was deleted locally: propagate the tombstone (uuid + deleted_at).
  std::function<void(const std::string&, const std::string&)> onLocalDelete;

  // --- logbook operations (each refreshes the view + emits onChanged) ---
  void newInMemory();
  bool openFile(const std::string& path);
  bool saveAs(const std::string& path);
  int importAdif(const std::string& adifText);
  int importXlog(const std::string& xlogText);
  std::string exportAdif() const;

  const LogBook& logbook() const { return logbook_; }
  std::string path() const { return logbook_.path(); }
  bool isFileBacked() const { return logbook_.isFileBacked(); }
  std::size_t qsoCount() const { return logbook_.qsos().size(); }
  std::string title() const;  // file basename or "Untitled"

  long addExternalQso(const Qso& q);  // returns the new QSO's stored id
  // Fill a stored QSO's empty name/QTH/locator/country from a QRZ record
  // (used to enrich QSOs received over UDP). Returns true if anything changed.
  bool enrichFromQrz(long id, const QrzResult& r);
  void setRigFrequency(double mhz);
  void setRigMode(const std::string& mode);
  void applyQrzLookup(const QrzResult& r);
  void setCwMessages(const std::array<std::string, 9>& msgs);
  void applyDxSpot(const std::string& call, double mhz);
  void backfillDxcc();
  void refresh();

  // Bulk locator fill (driven by the shell's QRZ client). The first returns the
  // distinct callsigns of QSOs that have a call but no locator; the second
  // writes locators back from a call->grid map and returns the number filled.
  std::vector<std::string> callsignsMissingLocator() const;
  int applyLocatorFill(const std::map<std::string, std::string>& callToLocator);

  // LoTW helpers.
  std::vector<Qso> qsosNotLotwSent() const {
    return logbook_.qsosNotLotwSent();
  }
  void markLotwSent(const std::vector<long>& ids, const std::string& date);
  int applyLotwConfirmations(const std::vector<Qso>& confirmed);

  // --- sync passthroughs (used by SyncCoordinator on the UI thread) ---
  SyncManifest syncManifest() const { return logbook_.syncManifest(); }
  std::vector<Qso> recordsByUuids(const std::vector<std::string>& uuids) const {
    return logbook_.recordsByUuids(uuids);
  }
  // Merge a remote delta and refresh the view. Does NOT fire onLocal* hooks.
  MergeResult applyRemoteDelta(const std::vector<Qso>& records,
                               const std::vector<SyncEntry>& tombstones,
                               const std::string& localNodeId,
                               const std::string& peerNodeId);
  std::string syncId() const { return logbook_.syncId(); }
  std::string ensureSyncId() { return logbook_.ensureSyncId(); }
  void setSyncId(const std::string& id) { logbook_.setSyncId(id); }

  // --- user events raised by the view ---
  void onCallChanged();     // refresh dupe + DXCC indicators
  void onLocatorChanged();  // locator typed -> push to the map panel
  std::string currentLocator()
      const;                // the form's current locator (for the map)
  void onDupeKeyChanged();  // date/band/mode changed -> refresh dupe
  void onFreqChanged();     // freq typed -> auto-detect band
  void onAddOrUpdate();
  void onDelete();
  void onClear();
  void onRowSelected(long id);  // load a stored row into the form

  // Row context-menu operations (identify the QSO by its stored id, not the
  // form's editingId_, so they work on any right-clicked row).
  const Qso* findQso(long id) const;  // stored QSO by id, nullptr if none
  void deleteQso(long id);            // delete a specific stored QSO

  void onSetNow();
  void onLookupCallClicked();
  void onSendCwClicked(int index);
  void onAbortCwClicked() {
    if (onAbortCw) onAbortCw();
  }
  void beginSearch() { view_.showSearch(); }

 private:
  void emitUpsert(long id);  // fire onLocalUpsert with the stored row
  void refreshList();
  void clearForm();
  void updateIndicators();                // dupe + DXCC from the current form
  dxccderive::Fields deriveDxcc() const;  // for the current call, with fallback
  Qso formQso() const;  // build a Qso from the form + derived DXCC
  void status(const std::string& s) {
    if (onStatus) onStatus(s);
  }
  void changed() {
    if (onChanged) onChanged();
  }

  ILogPageView& view_;
  LogBook logbook_;
  long editingId_ = 0;  // id loaded in the form, 0 = new
  // DXCC fields the loaded record carried (fallback when no cty.dat match).
  dxccderive::Fields loaded_;
  std::array<std::string, 9> cwMessages_{};
};
