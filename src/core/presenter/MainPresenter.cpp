#include "MainPresenter.h"

#include "Adif.h"
#include "LogPagePresenter.h"
#include "TimeUtil.h"

void MainPresenter::routeUdp(const std::vector<Qso>& qsos, const std::string& source) {
    LogPagePresenter* log = view_.currentLog();
    if (!log || qsos.empty())
        return;
    for (const auto& q : qsos)
        log->addExternalQso(q);
    status("Logged " + std::to_string(qsos.size()) + " QSO(s) from " + source +
           ": " + qsos.back().call);
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
    LogPagePresenter* log = pendingLookup_;
    pendingLookup_ = nullptr;
    if (!error.empty()) {
        status("QRZ lookup: " + error);
        return;
    }
    if (log && view_.isLogLive(log))
        log->applyQrzLookup(result);
    std::string msg = "QRZ: " + result.call;
    if (!result.name.empty())    msg += " — " + result.name;
    if (!result.country.empty()) msg += " (" + result.country + ")";
    status(msg);
    view_.showQrzResult(result);
}

void MainPresenter::beginLotwUpload(LogPagePresenter* target, std::vector<long> ids) {
    pendingUpload_    = target;
    pendingUploadIds_ = std::move(ids);
}

void MainPresenter::beginQrzLookup(LogPagePresenter* target) {
    pendingLookup_ = target;
}
