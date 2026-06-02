#pragma once

#include "DxccDeriver.h"
#include "ILogPageView.h"
#include "LogBook.h"
#include "Qrz.h"
#include "Qso.h"

#include <array>
#include <cstddef>
#include <functional>
#include <string>
#include <vector>

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
    std::function<void()>                    onChanged;     // content/path changed
    std::function<void(const std::string&)>  onStatus;      // status-line text
    std::function<void(const std::string&)>  onLookupCall;  // QRZ lookup requested
    std::function<void(const std::string&)>  onSendCw;      // expanded CW text to key
    std::function<void()>                    onAbortCw;     // abort keying

    // --- logbook operations (each refreshes the view + emits onChanged) ---
    void newInMemory();
    bool openFile(const std::string& path);
    bool saveAs(const std::string& path);
    int  importAdif(const std::string& adifText);
    std::string exportAdif() const;

    const LogBook& logbook() const { return logbook_; }
    std::string path() const { return logbook_.path(); }
    bool isFileBacked() const { return logbook_.isFileBacked(); }
    std::size_t qsoCount() const { return logbook_.qsos().size(); }
    std::string title() const;  // file basename or "Untitled"

    void addExternalQso(const Qso& q);
    void setRigFrequency(double mhz);
    void setRigMode(const std::string& mode);
    void applyQrzLookup(const QrzResult& r);
    void setCwMessages(const std::array<std::string, 9>& msgs);
    void applyDxSpot(const std::string& call, double mhz);
    void backfillDxcc();
    void refresh();

    // LoTW helpers.
    std::vector<Qso> qsosNotLotwSent() const { return logbook_.qsosNotLotwSent(); }
    void markLotwSent(const std::vector<long>& ids, const std::string& date);
    int  applyLotwConfirmations(const std::vector<Qso>& confirmed);

    // --- user events raised by the view ---
    void onCallChanged();           // refresh dupe + DXCC indicators
    void onDupeKeyChanged();        // date/band/mode changed -> refresh dupe
    void onFreqChanged();           // freq typed -> auto-detect band
    void onAddOrUpdate();
    void onDelete();
    void onClear();
    void onRowSelected(long id);    // load a stored row into the form
    void onSetNow();
    void onLookupCallClicked();
    void onSendCwClicked(int index);
    void onAbortCwClicked() { if (onAbortCw) onAbortCw(); }
    void beginSearch() { view_.showSearch(); }

private:
    void refreshList();
    void clearForm();
    void updateIndicators();        // dupe + DXCC from the current form
    dxccderive::Fields deriveDxcc() const;  // for the current call, with fallback
    Qso formQso() const;            // build a Qso from the form + derived DXCC
    void status(const std::string& s) { if (onStatus) onStatus(s); }
    void changed() { if (onChanged) onChanged(); }

    ILogPageView& view_;
    LogBook       logbook_;
    long          editingId_ = 0;   // id loaded in the form, 0 = new
    // DXCC fields the loaded record carried (fallback when no cty.dat match).
    dxccderive::Fields loaded_;
    std::array<std::string, 9> cwMessages_{};
};
