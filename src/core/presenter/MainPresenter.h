// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2025 Adrian Scripcă (YO6SSW)

#pragma once

#include "IMainView.h"
#include "Qrz.h"
#include "Qso.h"
#include "Settings.h"

#include <deque>
#include <string>
#include <utility>
#include <vector>

// Shell-level orchestration with no toolkit dependency: owns the configuration
// model (Settings) and routes service results (UDP, rig, LoTW, QRZ) to the
// current logbook tab via IMainView. The backend's shell still owns the service
// objects, the menus, dialogs and the notebook; it constructs the services
// (they are toolkit-neutral too) and forwards their callbacks here.
class MainPresenter {
public:
    explicit MainPresenter(IMainView& view) : view_(view) {}

    // The configuration model. Dialogs read/write it directly; settings
    // load/save round-trips it through an IniFile.
    Settings settings;

    // --- service-result routing (wired to the service callbacks) ---

    // UDP "logged ADIF" packets / raw ADIF datagrams: add to the current tab.
    void routeUdp(const std::vector<Qso>& qsos, const std::string& source);

    // A rig frequency/mode reading: push to the current tab's entry form.
    void routeRigUpdate(double mhz, const std::string& mode);

    // A finished LoTW confirmation download (ADIF body, or error).
    void routeLotwDownload(const std::string& adif, const std::string& error);

    // A finished tqsl upload: mark the uploaded QSOs as sent on success.
    void routeLotwUploadResult(bool ok, const std::string& message);

    // A finished QRZ lookup: prefill the requesting tab + show the popup.
    void routeQrzResult(const QrzResult& result, const std::string& error);

    // A finished bulk locator fill: write the locators back to the requesting tab.
    void routeQrzLocatorFill(
        const std::vector<std::pair<std::string, std::string>>& callLocators,
        int fromCache, int fetched, const std::string& error);

    // --- pending-target bookkeeping (set before kicking off async work) ---
    void beginLotwUpload(LogPagePresenter* target, std::vector<long> ids);
    // silent = fill the form but show no popup (used by DX-spot double-click).
    void beginQrzLookup(LogPagePresenter* target, bool silent = false);
    void beginQrzLocatorFill(LogPagePresenter* target);

private:
    void status(const std::string& s) { view_.setStatus(s); }

    // Drives the QRZ-enrichment queue for UDP-received QSOs: starts the next
    // lookup if the QRZ client is free, otherwise leaves it queued for later.
    void pumpQrzEnrich();

    IMainView&        view_;
    LogPagePresenter* pendingUpload_ = nullptr;
    std::vector<long> pendingUploadIds_;
    LogPagePresenter* pendingLookup_ = nullptr;
    bool              pendingLookupSilent_ = false;
    LogPagePresenter* pendingFill_ = nullptr;

    // Pending QRZ enrichment of QSOs received over UDP: each job names the tab,
    // the stored QSO id and the callsign to look up. Jobs run one at a time
    // (the QRZ client is single-flight); `qrzEnrichActive_` is true while the
    // head job's lookup is in flight, so routeQrzResult can tell an enrichment
    // result from a form-prefill / popup lookup.
    struct QrzEnrichJob {
        LogPagePresenter* log;
        long              id;
        std::string       call;
    };
    std::deque<QrzEnrichJob> qrzEnrichQueue_;
    bool                     qrzEnrichActive_ = false;

    // Last rig reading shown in the status bar; the rig polls every ~500 ms but we
    // only refresh the status line when the frequency or mode actually changes, so
    // a steady rig doesn't repaint it every tick (and stomp other status messages).
    double            lastRigMhz_ = -1.0;
    std::string       lastRigMode_;
};
