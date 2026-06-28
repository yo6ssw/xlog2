// xlog2-syncd — a headless, GUI-free backup peer.
//
// It joins the same peer-to-peer sync mesh the GUIs use and keeps a full replica
// of the default logbook (every add/edit/delete on any node gossips to it), and
// it answers the mesh's distributed QRZ-cache queries. Because that QRZ protocol
// is pull-only (a peer's rich cache is never pushed), the daemon also *actively
// enriches*: it looks up newly-synced callsigns on qrz.com using the configured
// credentials, so its qrz-cache.sqlite grows into a durable cache other nodes can
// resolve against.
//
// It reuses the exact toolkit-neutral pieces the GUIs wire (LogbookSync +
// SyncCoordinator + LogPagePresenter, QrzClient + QrzPeer, IniFile + Settings) —
// only the widgets are gone, replaced by a no-op view and a tiny main-loop
// dispatcher. Run it, Ctrl-C to quit; foreground, so systemd can supervise it.
//
// It is meant to coexist with a GUI on the *same* machine, so it keeps its own
// data dir (default.xlog, qrz-cache.sqlite, node_id) and uses an ephemeral mesh
// port — never touching the GUI's files, port, or persisted node id.

#include "IUiDispatcher.h"
#include "ILogPageView.h"
#include "IniFile.h"
#include "LogBook.h"
#include "LogbookSync.h"
#include "LogPagePresenter.h"
#include "Qrz.h"
#include "QrzPeer.h"
#include "Settings.h"
#include "SyncCoordinator.h"
#include "SyncProtocol.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <csignal>
#include <cstdlib>
#include <deque>
#include <fstream>
#include <functional>
#include <iostream>
#include <mutex>
#include <set>
#include <string>
#include <sys/stat.h>
#include <utility>
#include <vector>

namespace {

std::atomic<bool> g_running{true};

void onSignal(int) { g_running.store(false); }

// ---------------------------------------------------------------------------
// Main-loop dispatcher: services marshal worker-thread callbacks through this;
// post() enqueues a closure, the main thread drains them. Unlike a run-inline
// dispatcher, this guarantees sync callbacks (which re-enter the transport via
// SyncCoordinator::sendTo/broadcast) never execute inside the mesh's own IO
// thread, and keeps all logbook + coordinator + QRZ work single-threaded.
// ---------------------------------------------------------------------------
class MainLoopDispatcher : public IUiDispatcher {
public:
    void post(std::function<void()> fn) override {
        {
            std::lock_guard<std::mutex> lk(m_);
            q_.push_back(std::move(fn));
        }
        cv_.notify_one();
    }

    // Wait up to `timeout` for work, then run everything currently queued.
    template <class Rep, class Period>
    void drain(std::chrono::duration<Rep, Period> timeout) {
        std::deque<std::function<void()>> batch;
        {
            std::unique_lock<std::mutex> lk(m_);
            cv_.wait_for(lk, timeout, [&] { return !q_.empty() || !g_running.load(); });
            batch.swap(q_);
        }
        for (auto& fn : batch)
            fn();
    }

    // Unblock a drain() in progress (e.g. on signal) without queueing work.
    void wake() { cv_.notify_one(); }

private:
    std::mutex                        m_;
    std::condition_variable           cv_;
    std::deque<std::function<void()>> q_;
};

// ---------------------------------------------------------------------------
// A do-nothing view: the presenter drives it (e.g. setRows during a remote
// merge), but a headless node has nothing to display, so everything is a no-op.
// ---------------------------------------------------------------------------
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

// ~/.config/xlog2/layout.ini, honouring XDG_CONFIG_HOME — matching the GUI.
std::string defaultConfigPath() {
    if (const char* xdg = std::getenv("XDG_CONFIG_HOME"); xdg && *xdg)
        return std::string(xdg) + "/xlog2/layout.ini";
    const char* home = std::getenv("HOME");
    return std::string(home ? home : ".") + "/.config/xlog2/layout.ini";
}

// Default data dir: $XDG_DATA_HOME/xlog2-syncd (or ~/.local/share/xlog2-syncd).
// Deliberately distinct from the GUI's xlog2/ so the two never share a file.
std::string defaultDataDir() {
    if (const char* xdg = std::getenv("XDG_DATA_HOME"); xdg && *xdg)
        return std::string(xdg) + "/xlog2-syncd";
    const char* home = std::getenv("HOME");
    return std::string(home ? home : ".") + "/.local/share/xlog2-syncd";
}

bool makeDir(const std::string& path) {
    if (::mkdir(path.c_str(), 0700) == 0) return true;
    return errno == EEXIST;  // already there is fine
}

std::string readFileTrimmed(const std::string& path) {
    std::ifstream in(path);
    std::string s;
    std::getline(in, s);
    while (!s.empty() && (s.back() == '\n' || s.back() == '\r' || s.back() == ' '))
        s.pop_back();
    return s;
}

void writeFile(const std::string& path, const std::string& contents) {
    std::ofstream out(path, std::ios::trunc);
    out << contents << '\n';
}

void usage(const char* argv0) {
    std::cout
        << "Usage: " << argv0 << " [--config PATH] [--data-dir PATH]\n\n"
        << "Headless xlog2 backup peer: replicates the synced logbook and\n"
        << "enriches the shared qrz.com cache.\n\n"
        << "  --config PATH    layout.ini to read (default: " << defaultConfigPath() << ")\n"
        << "  --data-dir PATH  where to keep this peer's default.xlog,\n"
        << "                   qrz-cache.sqlite and node_id (default: " << defaultDataDir() << ")\n"
        << "  --help           show this help\n";
}

}  // namespace

int main(int argc, char* argv[]) {
    std::string configPath = defaultConfigPath();
    std::string dataDir     = defaultDataDir();

    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        auto next = [&](const char* name) -> std::string {
            if (i + 1 >= argc) {
                std::cerr << "xlog2-syncd: " << name << " needs an argument\n";
                std::exit(2);
            }
            return argv[++i];
        };
        if (a == "--config")        configPath = next("--config");
        else if (a == "--data-dir") dataDir = next("--data-dir");
        else if (a == "--help" || a == "-h") { usage(argv[0]); return 0; }
        else { std::cerr << "xlog2-syncd: unknown option '" << a << "'\n"; usage(argv[0]); return 2; }
    }

    IniFile ini;
    if (!ini.loadFromFile(configPath))
        std::cerr << "xlog2-syncd: could not read " << configPath << " (using defaults)\n";
    const Settings s = Settings::load(ini);

    if (!makeDir(dataDir)) {
        std::cerr << "xlog2-syncd: cannot create data dir " << dataDir << "\n";
        return 1;
    }
    const std::string logPath    = dataDir + "/default.xlog";
    const std::string cachePath  = dataDir + "/qrz-cache.sqlite";
    const std::string nodeIdPath = dataDir + "/node_id";

    std::signal(SIGINT, onSignal);
    std::signal(SIGTERM, onSignal);

    MainLoopDispatcher disp;
    auto log = [](const std::string& msg) { std::cout << msg << std::endl; };

    // --- logbook replica (file-backed, via the same presenter the GUI uses) ---
    StubLogPageView  view;
    LogPagePresenter page(view);
    if (!page.openFile(logPath)) {
        std::cerr << "xlog2-syncd: cannot open logbook " << logPath << "\n";
        return 1;
    }

    // --- QRZ: local cache (answers peers) + qrz.com lookups (enrichment) ---
    QrzClient qrz(disp);
    qrz.setCache(cachePath, s.qrzCacheDays);

    // --- mesh transport + protocol drivers ---
    LogbookSync     sync(disp);
    SyncCoordinator coord(sync);
    QrzPeer         qrzPeer(sync, qrz);

    sync.onStatus    = log;
    coord.onStatus   = log;
    qrzPeer.onStatus = log;

    sync.onPeerUp   = [&](const LogbookSync::PeerKey& p) { coord.onPeerUp(p); };
    sync.onPeerDown = [&](const LogbookSync::PeerKey& p) { coord.onPeerDown(p); };
    sync.onMessage  = [&](const LogbookSync::PeerKey& p, const syncproto::Message& m) {
        // QRZ peer-cache messages ride the same mesh; route them to QrzPeer.
        if (m.type == syncproto::Type::QrzQuery || m.type == syncproto::Type::QrzResponse)
            qrzPeer.onMessage(p, m);
        else
            coord.onMessage(p, m);
    };

    coord.attach(&page);
    // The daemon never edits QSOs locally, so these stay dormant — wired for
    // completeness so any future local mutation would still propagate.
    page.onLocalUpsert = [&](const Qso& q) { coord.onLocalUpsert(q); };
    page.onLocalDelete = [&](const std::string& uuid, const std::string& at) {
        coord.onLocalDelete(uuid, at);
    };

    // --- QRZ enrichment state (all touched on the main thread only) ---
    const bool enrichEnabled = !s.qrzUser.empty() && !s.qrzPassword.empty();
    if (!enrichEnabled)
        std::cerr << "xlog2-syncd: no [qrz] credentials; cache enrichment disabled "
                     "(still answering peer queries from cache)\n";
    std::deque<std::string> enrichQueue;
    std::set<std::string>   attempted;  // per-session: don't re-fetch no-record calls

    auto pumpEnrich = [&]() {
        if (!enrichEnabled || qrz.isBusy() || enrichQueue.empty())
            return;
        std::string call = enrichQueue.front();
        enrichQueue.pop_front();
        qrz.lookup(s.qrzUser, s.qrzPassword, call);  // result lands in qrz-cache.sqlite
    };
    // The only consumer of onResult: chain to the next queued callsign.
    qrz.onResult = [&](const QrzResult&, const std::string&) { pumpEnrich(); };

    auto sweepEnrich = [&]() {
        if (!enrichEnabled)
            return;
        for (const Qso& q : page.logbook().qsos()) {
            if (q.call.empty() || attempted.count(q.call))
                continue;
            attempted.insert(q.call);
            if (!qrz.cachedLookup(q.call))  // not already cached -> fetch
                enrichQueue.push_back(q.call);
        }
        pumpEnrich();
    };

    // --- start the mesh (own node id, ephemeral port; multicast discovery) ---
    LogbookSync::Config cfg;
    cfg.nodeId = readFileTrimmed(nodeIdPath);   // empty => the mesh mints one
    cfg.group  = syncproto::meshGroup(s.syncSecret);
    cfg.port   = 0;                             // ephemeral — avoid the GUI's port
    for (const std::string& h : {s.syncPeerHost, s.syncPeerHostAlt})
        if (auto pr = LogbookSync::parsePeer(h, s.syncPort); !pr.first.empty())
            cfg.staticPeers.push_back(pr);

    sync.start(cfg);
    if (cfg.nodeId.empty() && !sync.localId().empty())
        writeFile(nodeIdPath, sync.localId());  // persist the minted id
    coord.configure(sync.localId(), s.syncSecret);

    std::cout << "xlog2-syncd: node " << sync.localId()
              << "  group " << cfg.group
              << "  logbook " << logPath
              << "  (Ctrl-C to quit)" << std::endl;

    // --- main loop: drain marshalled callbacks; sweep for enrichment ~60s ---
    using clock = std::chrono::steady_clock;
    auto lastSweep = clock::now() - std::chrono::seconds(60);  // sweep immediately
    while (g_running.load()) {
        disp.drain(std::chrono::milliseconds(200));
        if (clock::now() - lastSweep >= std::chrono::seconds(60)) {
            sweepEnrich();
            lastSweep = clock::now();
        }
    }

    sync.stop();
    return 0;
}
