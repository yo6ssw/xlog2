// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2025 Adrian Scripcă (YO6SSW)

#include "MainPresenter.h"

#include "Adif.h"
#include "LogPagePresenter.h"
#include "TimeUtil.h"

#include <map>

void MainPresenter::routeUdp(const std::vector<Qso>& qsos, const std::string& source) {
    LogPagePresenter* log = view_.currentLog();
    if (!log || qsos.empty())
        return;
    // Enrich from QRZ only when it's configured; otherwise the lookups would
    // just fail. The QrzClient consults its on-disk cache first, so a repeat
    // callsign never hits the network.
    const bool qrzReady = !settings.qrzUser.empty() && !settings.qrzPassword.empty();
    for (const auto& q : qsos) {
        const long id = log->addExternalQso(q);
        // Queue a lookup when the record is missing something QRZ can supply.
        if (qrzReady && !q.call.empty() &&
            (q.name.empty() || q.qth.empty() || q.locator.empty() || q.country.empty()))
            qrzEnrichQueue_.push_back({log, id, q.call});
    }
    status("Logged " + std::to_string(qsos.size()) + " QSO(s) from " + source +
           ": " + qsos.back().call);
    pumpQrzEnrich();
}

void MainPresenter::pumpQrzEnrich() {
    while (!qrzEnrichActive_ && !qrzEnrichQueue_.empty()) {
        QrzEnrichJob& job = qrzEnrichQueue_.front();
        if (!view_.isLogLive(job.log)) {  // tab closed before we got to it
            qrzEnrichQueue_.pop_front();
            continue;
        }
        // Busy means a manual lookup (or the previous enrich job) is in flight;
        // leave this job queued — routeQrzResult pumps again when it finishes.
        if (!view_.startQrzLookup(job.call))
            return;
        qrzEnrichActive_ = true;
    }
}

void MainPresenter::routeRigUpdate(double mhz, const std::string& mode) {
    if (LogPagePresenter* log = view_.currentLog()) {
        log->setRigFrequency(mhz);
        log->setRigMode(mode);
    }
    // Only repaint the status line when the reading actually changes — the rig
    // polls every ~500 ms, and rewriting it every tick both wastes work and wipes
    // out any other status message between ticks.
    if (mhz == lastRigMhz_ && mode == lastRigMode_)
        return;
    lastRigMhz_  = mhz;
    lastRigMode_ = mode;
    std::string s = "Rig: " + std::to_string(mhz) + " MHz";
    if (!mode.empty())
        s += " " + mode;
    status(s);
}

void MainPresenter::routeLotwDownload(const std::string& adif, const std::string& error) {
    if (!error.empty()) {
        status("LoTW download failed: " + error);
        return;
    }
    const auto records = adif::parse(adif);
    LogPagePresenter* log = view_.currentLog();
    if (!log)
        return;
    const int n = log->applyLotwConfirmations(records);
    settings.lotwLastDownload = timeutil::utcNow("%Y-%m-%d");
    if (records.empty())
        status("LoTW: no confirmations returned (check username/password).");
    else
        status("LoTW: " + std::to_string(records.size()) +
               " record(s) downloaded, " + std::to_string(n) +
               " QSO(s) newly confirmed.");
}

void MainPresenter::routeLotwUploadResult(bool ok, const std::string& message) {
    if (ok && pendingUpload_ && view_.isLogLive(pendingUpload_))
        pendingUpload_->markLotwSent(pendingUploadIds_, timeutil::utcNow("%Y-%m-%d"));
    pendingUpload_ = nullptr;
    pendingUploadIds_.clear();
    status("LoTW upload: " + message);
}

void MainPresenter::routeQrzResult(const QrzResult& result, const std::string& error) {
    // An enrichment lookup (for a UDP-received QSO) is in flight: write the
    // record back to the stored QSO instead of the entry form, with no popup.
    if (qrzEnrichActive_) {
        QrzEnrichJob job = qrzEnrichQueue_.front();
        qrzEnrichQueue_.pop_front();
        qrzEnrichActive_ = false;
        if (error.empty() && view_.isLogLive(job.log) &&
            job.log->enrichFromQrz(job.id, result))
            status("QRZ: filled missing details for " + result.call + ".");
        pumpQrzEnrich();
        return;
    }

    LogPagePresenter* log = pendingLookup_;
    const bool silent = pendingLookupSilent_;
    pendingLookup_ = nullptr;
    pendingLookupSilent_ = false;
    if (!error.empty()) {
        if (!silent)  // a spot-triggered prefill shouldn't nag if QRZ isn't set up
            status("QRZ lookup: " + error);
        pumpQrzEnrich();  // client is free again — run any queued enrich jobs
        return;
    }
    if (log && view_.isLogLive(log))
        log->applyQrzLookup(result);
    std::string msg = "QRZ: " + result.call;
    if (!result.name.empty())    msg += " — " + result.name;
    if (!result.country.empty()) msg += " (" + result.country + ")";
    status(msg);
    if (!silent)  // silent prefill (e.g. from a DX-spot double-click): no popup
        view_.showQrzResult(result);
    pumpQrzEnrich();  // the client is free again — run any queued enrich jobs
}

void MainPresenter::routeQrzLocatorFill(
    const std::vector<std::pair<std::string, std::string>>& callLocators,
    int fromCache, int fetched, const std::string& error) {
    LogPagePresenter* log = pendingFill_;
    pendingFill_ = nullptr;
    if (!error.empty()) {
        status("QRZ locator fill failed: " + error);
        return;
    }
    int filled = 0;
    if (log && view_.isLogLive(log)) {
        std::map<std::string, std::string> m(callLocators.begin(), callLocators.end());
        filled = log->applyLocatorFill(m);
    }
    status("Filled " + std::to_string(filled) + " locator(s) — " +
           std::to_string(fromCache) + " cached, " + std::to_string(fetched) +
           " fetched.");
}

void MainPresenter::beginLotwUpload(LogPagePresenter* target, std::vector<long> ids) {
    pendingUpload_    = target;
    pendingUploadIds_ = std::move(ids);
}

void MainPresenter::beginQrzLookup(LogPagePresenter* target, bool silent) {
    pendingLookup_ = target;
    pendingLookupSilent_ = silent;
}

void MainPresenter::beginQrzLocatorFill(LogPagePresenter* target) {
    pendingFill_ = target;
}
