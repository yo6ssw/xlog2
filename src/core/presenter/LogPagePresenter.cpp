#include "LogPagePresenter.h"

#include "Bands.h"
#include "CwExpander.h"
#include "Dxcc.h"
#include "DupeMessage.h"
#include "QsoMapper.h"
#include "StrUtil.h"
#include "TimeUtil.h"

#include <algorithm>
#include <cstdio>
#include <set>
#include <stdexcept>

namespace {

// Format MHz without trailing zeros, e.g. 14.250000 -> "14.25".
std::string formatMhz(double mhz) {
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%.6f", mhz);
    std::string s = buf;
    if (s.find('.') != std::string::npos) {
        while (!s.empty() && s.back() == '0')
            s.pop_back();
        if (!s.empty() && s.back() == '.')
            s.pop_back();
    }
    return s;
}

std::string basename(const std::string& path) {
    const auto slash = path.find_last_of('/');
    return slash == std::string::npos ? path : path.substr(slash + 1);
}

}  // namespace

void LogPagePresenter::start() {
    refreshList();
    clearForm();
}

std::string LogPagePresenter::title() const {
    return logbook_.isFileBacked() ? basename(logbook_.path()) : "Untitled";
}

// --- derived state -----------------------------------------------------------

dxccderive::Fields LogPagePresenter::deriveDxcc() const {
    return dxccderive::derive(view_.formData().call, loaded_);
}

Qso LogPagePresenter::formQso() const {
    return qsomap::fromForm(view_.formData(), editingId_, deriveDxcc());
}

void LogPagePresenter::updateIndicators() {
    // DXCC label.
    const dxccderive::Fields f = deriveDxcc();
    view_.setDxccText(dxccderive::format(f));

    // Dupe banner (the derived Qso carries upper-cased call etc.).
    const Qso q = qsomap::fromForm(view_.formData(), editingId_, f);
    const auto dup = logbook_.findDuplicate(q, editingId_);
    if (dup)
        view_.setDupeWarning(dupe::format(*dup), true);
    else
        view_.setDupeWarning("", false);
}

// --- form lifecycle ----------------------------------------------------------

void LogPagePresenter::clearForm() {
    editingId_ = 0;
    loaded_ = {};
    FormData f;
    f.date     = timeutil::utcNow("%Y-%m-%d");
    f.time_on  = timeutil::utcNow("%H:%M");
    f.rst_sent = "599";  // sensible default; the operator edits per contact
    f.rst_rcvd = "599";
    view_.setFormData(f);
    view_.setEditing(false);
    view_.clearSelection();
    view_.setDupeWarning("", false);
    view_.setDxccText("");
    view_.focusCall();
}

void LogPagePresenter::refreshList() {
    view_.setRows(logbook_.qsos());
    updateIndicators();
}

void LogPagePresenter::refresh() {
    view_.setRows(logbook_.qsos());
}

// --- view events -------------------------------------------------------------

void LogPagePresenter::onCallChanged() {
    updateIndicators();
}

void LogPagePresenter::onLocatorChanged() {
    if (onLocator)
        onLocator(view_.formData().locator);
}

std::string LogPagePresenter::currentLocator() const {
    return view_.formData().locator;
}

void LogPagePresenter::onDupeKeyChanged() {
    updateIndicators();
}

void LogPagePresenter::onFreqChanged() {
    const std::string text = view_.formData().freq;
    if (text.empty())
        return;
    try {
        const double mhz = std::stod(text);
        const std::string b = bands::forFrequencyMHz(mhz);
        if (!b.empty())
            view_.setBand(b);
    } catch (const std::exception&) {
        // not a number yet; ignore
    }
}

void LogPagePresenter::onAddOrUpdate() {
    const Qso q = formQso();
    if (q.call.empty()) {
        status("Cannot log a QSO without a callsign.");
        return;
    }
    if (editingId_ != 0) {
        logbook_.update(q);
        status("Updated QSO with " + q.call + ".");
    } else {
        logbook_.add(q);
        status("Logged QSO with " + q.call + ".");
    }
    refreshList();
    clearForm();
    changed();
}

void LogPagePresenter::onDelete() {
    if (editingId_ == 0) {
        status("No QSO selected to delete.");
        return;
    }
    logbook_.remove(editingId_);
    refreshList();
    clearForm();
    changed();
    status("QSO deleted.");
}

void LogPagePresenter::onClear() {
    clearForm();
}

void LogPagePresenter::onRowSelected(long id) {
    for (const auto& q : logbook_.qsos()) {
        if (q.id != id)
            continue;
        loaded_ = qsomap::dxccOf(q);
        view_.setFormData(qsomap::toForm(q));
        editingId_ = q.id;
        view_.setEditing(true);
        updateIndicators();
        if (onLocator)
            onLocator(q.locator);  // move the map "to" point to the selected QSO
        return;
    }
}

const Qso* LogPagePresenter::findQso(long id) const {
    for (const auto& q : logbook_.qsos())
        if (q.id == id)
            return &q;
    return nullptr;
}

void LogPagePresenter::deleteQso(long id) {
    if (id == 0)
        return;
    logbook_.remove(id);
    refreshList();
    if (editingId_ == id)
        clearForm();  // the deleted row was loaded in the form
    changed();
    status("QSO deleted.");
}

void LogPagePresenter::onSetNow() {
    FormData f = view_.formData();
    f.date    = timeutil::utcNow("%Y-%m-%d");
    f.time_on = timeutil::utcNow("%H:%M");
    view_.setFormData(f);
}

void LogPagePresenter::onLookupCallClicked() {
    const std::string call = strutil::toUpper(view_.formData().call);
    if (call.empty()) {
        status("Enter a callsign to look up.");
        return;
    }
    if (onLookupCall)
        onLookupCall(call);  // the shell owns the QRZ client + credentials
}

void LogPagePresenter::onSendCwClicked(int index) {
    if (index < 0 || index >= 9 || cwMessages_[index].empty()) {
        status("Keyer F" + std::to_string(index + 1) + " has no message set.");
        return;
    }
    const FormData f = view_.formData();
    cw::Substitutions subs;
    subs.call = strutil::toUpper(f.call);
    subs.name = f.name;
    subs.qth  = f.qth;
    subs.rst  = f.rst_rcvd;  // %RST% = the RST rcvd field
    const std::string text = cw::expand(cwMessages_[index], subs);
    if (onSendCw)
        onSendCw(text);
    status("Keyer: " + text);
}

// --- logbook operations ------------------------------------------------------

void LogPagePresenter::newInMemory() {
    logbook_.newInMemory();
    refreshList();
    clearForm();
    changed();
}

bool LogPagePresenter::openFile(const std::string& path) {
    if (!logbook_.open(path))
        return false;
    backfillDxcc();  // fill DXCC for any pre-existing QSOs that lack it
    refreshList();
    clearForm();
    changed();
    return true;
}

bool LogPagePresenter::saveAs(const std::string& path) {
    if (!logbook_.saveAs(path))
        return false;
    refreshList();
    changed();
    return true;
}

int LogPagePresenter::importAdif(const std::string& adifText) {
    const int n = logbook_.importAdif(adifText);
    refreshList();
    changed();
    return n;
}

int LogPagePresenter::importXlog(const std::string& xlogText) {
    const int n = logbook_.importXlog(xlogText);
    refreshList();
    changed();
    return n;
}

std::string LogPagePresenter::exportAdif() const {
    return logbook_.exportAdif();
}

void LogPagePresenter::addExternalQso(const Qso& q) {
    logbook_.add(q);
    refreshList();
    changed();
}

void LogPagePresenter::setRigFrequency(double mhz) {
    if (mhz <= 0.0)
        return;
    // Only write when the value actually changes: rig polling calls this every
    // tick, and an unconditional setFreq() would reset the freq field's cursor
    // (and fight the operator) even when the frequency hasn't moved.
    const std::string f = formatMhz(mhz);
    if (view_.formData().freq != f)
        view_.setFreq(f);
    const std::string b = bands::forFrequencyMHz(mhz);
    if (!b.empty())
        view_.setBand(b);
}

void LogPagePresenter::setRigMode(const std::string& mode) {
    if (mode.empty())
        return;
    // Unknown mode name: leave the current selection untouched (matches the
    // drop-down, whose entries are bands::modes()).
    const auto& modes = bands::modes();
    if (std::find(modes.begin(), modes.end(), mode) == modes.end())
        return;
    // Set only the mode drop-down. Rebuilding the whole form via setFormData()
    // here would re-write every text field on each rig poll, yanking the cursor
    // out of whatever the operator is typing (e.g. the callsign).
    view_.setMode(mode);
}

void LogPagePresenter::applyQrzLookup(const QrzResult& r) {
    // Only overwrite a field when QRZ has a value, so a partial lookup doesn't
    // wipe what the user typed.
    FormData f = view_.formData();
    if (!r.name.empty()) f.name = r.name;
    if (!r.qth.empty())  f.qth  = r.qth;
    // Fill the locator only when empty, so a grid already entered isn't lost.
    if (!r.locator.empty() && f.locator.empty())
        f.locator = r.locator;
    view_.setFormData(f);
    // setFormData suppresses the locator field's change handler (loadingForm_),
    // so push the (possibly QRZ-filled) grid to the map destination directly.
    if (onLocator)
        onLocator(f.locator);
}

void LogPagePresenter::setCwMessages(const std::array<std::string, 9>& msgs) {
    cwMessages_ = msgs;
    view_.setCwButtons(cwMessages_);
}

void LogPagePresenter::applyDxSpot(const std::string& call, double mhz) {
    view_.setCall(strutil::toUpper(call));
    setRigFrequency(mhz);  // fills freq + auto-detects band
    status("DX spot: " + strutil::toUpper(call) + " on " + formatMhz(mhz) + " MHz.");
}

void LogPagePresenter::backfillDxcc() {
    if (!dxcc::available())
        return;
    std::vector<Qso> updates;
    for (const auto& q : logbook_.qsos()) {
        if (q.call.empty() || !q.country.empty())
            continue;  // nothing to do, or already filled (idempotent)
        const dxcc::Info* info = dxcc::lookup(q.call);
        if (!info)
            continue;
        Qso u = q;
        u.country   = info->entity;
        u.cq_zone   = info->cqZone  ? std::to_string(info->cqZone)  : std::string{};
        u.itu_zone  = info->ituZone ? std::to_string(info->ituZone) : std::string{};
        u.continent = info->continent;
        updates.push_back(std::move(u));
    }
    if (updates.empty())
        return;
    logbook_.updateBatch(updates);
    refreshList();
    changed();
    status("Filled DXCC for " + std::to_string(updates.size()) + " QSO(s).");
}

std::vector<std::string> LogPagePresenter::callsignsMissingLocator() const {
    std::vector<std::string> calls;
    std::set<std::string> seen;
    for (const auto& q : logbook_.qsos()) {
        if (q.call.empty() || !q.locator.empty())
            continue;
        const std::string up = strutil::toUpper(q.call);
        if (seen.insert(up).second)
            calls.push_back(up);
    }
    return calls;
}

int LogPagePresenter::applyLocatorFill(
    const std::map<std::string, std::string>& callToLocator) {
    std::vector<Qso> updates;
    for (const auto& q : logbook_.qsos()) {
        if (q.call.empty() || !q.locator.empty())
            continue;
        const auto it = callToLocator.find(strutil::toUpper(q.call));
        if (it == callToLocator.end() || it->second.empty())
            continue;
        Qso u = q;
        u.locator = it->second;
        updates.push_back(std::move(u));
    }
    if (updates.empty())
        return 0;
    logbook_.updateBatch(updates);
    refreshList();
    changed();
    return static_cast<int>(updates.size());
}

void LogPagePresenter::markLotwSent(const std::vector<long>& ids, const std::string& date) {
    logbook_.markLotwSent(ids, date);
    refreshList();
    changed();
}

int LogPagePresenter::applyLotwConfirmations(const std::vector<Qso>& confirmed) {
    const int n = logbook_.applyLotwConfirmations(confirmed);
    if (n > 0) {
        refreshList();
        changed();
    }
    return n;
}
