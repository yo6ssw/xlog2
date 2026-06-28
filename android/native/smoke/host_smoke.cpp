// host_smoke — off-device validation of the xlog_mobile carve-out.
//
// It wires the exact pieces the Android JNI core will (and that SyncDaemon.cpp
// already does): a queue dispatcher, a no-op view, LogPagePresenter over a
// file-backed LogBook, LogbookSync + SyncCoordinator + QrzPeer/QrzClient. Then
// it starts the mesh, logs a QSO, and prints what it sees. If this compiles,
// links and runs, the carve-out is toolkit-free and self-consistent — the
// foundation the NDK build and JNI bridge stand on.

#include "ILogPageView.h"
#include "IUiDispatcher.h"
#include "LogBook.h"
#include "LogPagePresenter.h"
#include "LogbookSync.h"
#include "Qrz.h"
#include "QrzPeer.h"
#include "Qso.h"
#include "SyncCoordinator.h"
#include "SyncProtocol.h"

#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <deque>
#include <functional>
#include <iostream>
#include <mutex>
#include <string>
#include <sys/stat.h>

namespace {

// A thread-safe queue drained by the main thread — the host stand-in for the
// Android core-thread dispatcher. Identical in spirit to SyncDaemon's
// MainLoopDispatcher: sync callbacks re-enter the transport and must not run on
// the mesh IO thread.
class QueueDispatcher : public IUiDispatcher {
public:
    void post(std::function<void()> fn) override {
        {
            std::lock_guard<std::mutex> lk(m_);
            q_.push_back(std::move(fn));
        }
        cv_.notify_one();
    }
    void drain(std::chrono::milliseconds timeout) {
        std::deque<std::function<void()>> batch;
        {
            std::unique_lock<std::mutex> lk(m_);
            cv_.wait_for(lk, timeout, [&] { return !q_.empty(); });
            batch.swap(q_);
        }
        for (auto& fn : batch) fn();
    }

private:
    std::mutex                        m_;
    std::condition_variable           cv_;
    std::deque<std::function<void()>> q_;
};

class StubLogPageView : public ILogPageView {
public:
    FormData formData() const override { return {}; }
    void setFormData(const FormData&) override {}
    void setCall(const std::string&) override {}
    void setFreq(const std::string&) override {}
    void setBand(const std::string&) override {}
    void setMode(const std::string&) override {}
    void setRows(const std::vector<Qso>&) override {}
    void clearSelection() override {}
    void setDupeWarning(const std::string&, bool) override {}
    void setDxccText(const std::string&) override {}
    void setEditing(bool) override {}
    void setCwButtons(const std::array<std::string, 9>&) override {}
    void focusCall() override {}
    void showSearch() override {}
};

}  // namespace

// Usage: xlog_mobile_smoke [subdir] [call-to-log] [run-seconds]
//   subdir       data dir under /tmp/xlog_mobile_smoke (default "n0")
//   call-to-log  if non-empty, log one QSO with this callsign
//   run-seconds  how long to pump the mesh before printing the final logbook
// Run two instances with the same build and different subdirs/calls to watch a
// QSO propagate over the real multicast mesh.
int main(int argc, char** argv) {
    const std::string sub  = argc > 1 ? argv[1] : "n0";
    const std::string call = argc > 2 ? argv[2] : "";
    const int runSecs      = argc > 3 ? std::atoi(argv[3]) : 0;

    const std::string base = "/tmp/xlog_mobile_smoke";
    ::mkdir(base.c_str(), 0700);
    const std::string dir = base + "/" + sub;
    ::mkdir(dir.c_str(), 0700);
    const std::string logPath   = dir + "/default.xlog";
    const std::string cachePath = dir + "/qrz-cache.sqlite";

    QueueDispatcher disp;
    auto log = [](const std::string& m) { std::cout << "[smoke] " << m << "\n"; };

    StubLogPageView  view;
    LogPagePresenter page(view);
    if (!page.openFile(logPath)) {
        std::cerr << "[smoke] FAIL: cannot open logbook " << logPath << "\n";
        return 1;
    }

    QrzClient qrz(disp);
    qrz.setCache(cachePath, 365);

    LogbookSync     sync(disp);
    SyncCoordinator coord(sync);
    QrzPeer         qrzPeer(sync, qrz);

    sync.onStatus    = log;
    coord.onStatus   = log;
    qrzPeer.onStatus = log;

    sync.onPeerUp   = [&](const LogbookSync::PeerKey& p) { coord.onPeerUp(p); };
    sync.onPeerDown = [&](const LogbookSync::PeerKey& p) { coord.onPeerDown(p); };
    sync.onMessage  = [&](const LogbookSync::PeerKey& p, const syncproto::Message& m) {
        if (m.type == syncproto::Type::QrzQuery || m.type == syncproto::Type::QrzResponse)
            qrzPeer.onMessage(p, m);
        else
            coord.onMessage(p, m);
    };

    coord.attach(&page);
    page.onLocalUpsert = [&](const Qso& q) { coord.onLocalUpsert(q); };
    page.onLocalDelete = [&](const std::string& uuid, const std::string& at) {
        coord.onLocalDelete(uuid, at);
    };

    const std::string secret = "xlog-mobile-smoke";
    LogbookSync::Config cfg;
    cfg.group = syncproto::meshGroup(secret);
    cfg.port  = 0;  // ephemeral
    cfg.psk   = secret;
    cfg.identityFile    = dir + "/node_identity";
    cfg.requireIdentity = true;
    cfg.nodeName        = "xlog-mobile-smoke";

    sync.start(cfg);
    coord.configure(sync.localId(), secret);
    coord.setTrust(/*enforce=*/false, {});

    log("node id   : " + sync.localId());
    log("identity  : " + sync.identityKey());
    log("group     : " + cfg.group);
    log("sync_id   : " + (page.syncId().empty() ? "(empty, negotiated on pairing)"
                                                 : page.syncId()));

    // Log a QSO through the same presenter the UI will use — and fire the local
    // upsert hook so it pushes to peers, exactly like a UI add.
    if (!call.empty()) {
        Qso q;
        q.date = "2026-06-28"; q.time_on = "12:00"; q.call = call;
        q.band = "20m"; q.mode = "SSB"; q.freq = "14.250";
        q.rst_sent = "599"; q.rst_rcvd = "599";
        const long id = page.addExternalQso(q);
        if (const Qso* stored = page.findQso(id))
            page.onLocalUpsert(*stored);
        log("logged QSO id=" + std::to_string(id) +
            " count=" + std::to_string(page.qsoCount()));
    }

    // Pump the dispatcher for runSecs so the mesh can discover peers and sync.
    const auto deadline = std::chrono::steady_clock::now() +
                          std::chrono::seconds(runSecs);
    do {
        disp.drain(std::chrono::milliseconds(100));
    } while (std::chrono::steady_clock::now() < deadline);

    log("final logbook (" + std::to_string(page.qsoCount()) + " QSOs):");
    for (const Qso& e : page.logbook().qsos())
        log("  row " + std::to_string(e.id) + " " + e.call + " " + e.band +
            " uuid=" + e.uuid);

    sync.stop();
    log("OK");
    return 0;
}
