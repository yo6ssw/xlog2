// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2025 Adrian Scripcă (YO6SSW)

// xlog_jni — the JNI bridge between the Kotlin/Compose UI and the carved-out
// C++ sync core (xlog_mobile). It is the Android equivalent of SyncDaemon.cpp:
// it wires LogPagePresenter + LogbookSync + SyncCoordinator + QrzPeer/QrzClient
// behind a thread-safe core-thread dispatcher, so all logbook + coordinator work
// stays single-threaded and off the mesh IO thread. The UI drives it through the
// Kotlin `XlogCore` class; results come back via four listener callbacks.
//
// QSO rows cross JNI as a single string: fields joined by US (0x1f), rows by RS
// (0x1e), in the fixed order documented in qsoToWire() below. This keeps the JNI
// surface tiny (a handful of String methods) instead of dozens of field getters.

#include "Bands.h"
#include "DupeMessage.h"
#include "Dxcc.h"
#include "DxccDeriver.h"
#include "ILogPageView.h"
#include "IUiDispatcher.h"
#include "LogBook.h"
#include "LogPagePresenter.h"
#include "LogbookSync.h"
#include "Qso.h"
#include "QsoMapper.h"
#include "SyncCoordinator.h"
#include "SyncProtocol.h"
#ifndef XLOG_NO_QRZ
#include "Qrz.h"
#include "QrzPeer.h"
#endif

#include <jni.h>
#include <android/log.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <fstream>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <sys/stat.h>
#include <thread>
#include <vector>

namespace {

constexpr char US = '\x1f';  // unit separator: between fields
constexpr char RS = '\x1e';  // record separator: between rows

JavaVM* g_vm = nullptr;

// ---------------------------------------------------------------------------
// Small string helpers for the US/RS wire format.
// ---------------------------------------------------------------------------
std::vector<std::string> split(const std::string& s, char sep) {
    std::vector<std::string> out;
    std::string cur;
    for (char c : s) {
        if (c == sep) { out.push_back(cur); cur.clear(); }
        else cur.push_back(c);
    }
    out.push_back(cur);
    return out;
}

// Field order shared with the Kotlin side (XlogCore.kt). Append-only.
std::string qsoToWire(const Qso& q) {
    std::string s;
    auto add = [&](const std::string& f) { s += f; s += US; };
    s += std::to_string(q.id); s += US;
    add(q.date); add(q.time_on); add(q.time_off); add(q.call); add(q.band);
    add(q.mode); add(q.freq); add(q.rst_sent); add(q.rst_rcvd); add(q.name);
    add(q.qth); add(q.locator); add(q.power); add(q.qsl_sent); add(q.qsl_rcvd);
    add(q.comment); add(q.country); add(q.cq_zone); add(q.itu_zone);
    s += q.continent;  // last: no trailing separator
    return s;
}

FormData wireToForm(const std::string& wire) {
    const std::vector<std::string> f = split(wire, US);
    auto at = [&](size_t i) { return i < f.size() ? f[i] : std::string(); };
    FormData d;
    // index 0 is id (ignored here; editing id is passed separately)
    d.date = at(1); d.time_on = at(2); d.time_off = at(3); d.call = at(4);
    d.band = at(5); d.mode = at(6); d.freq = at(7); d.rst_sent = at(8);
    d.rst_rcvd = at(9); d.name = at(10); d.qth = at(11); d.locator = at(12);
    d.power = at(13);
    d.qsl_sent = at(14) == "Y" || at(14) == "1";
    d.qsl_rcvd = at(15) == "Y" || at(15) == "1";
    d.comment = at(16);
    return d;
}

std::string readFileTrimmed(const std::string& path) {
    std::ifstream in(path);
    std::string s;
    std::getline(in, s);
    while (!s.empty() && (s.back() == '\n' || s.back() == '\r' || s.back() == ' '))
        s.pop_back();
    return s;
}
void writeFile(const std::string& path, const std::string& c) {
    std::ofstream(path, std::ios::trunc) << c << '\n';
}

// ---------------------------------------------------------------------------
// Core-thread dispatcher: a queue drained by one dedicated thread. The single
// seam the neutral services need. Mirrors SyncDaemon's MainLoopDispatcher.
// ---------------------------------------------------------------------------
class CoreDispatcher : public IUiDispatcher {
public:
    void post(std::function<void()> fn) override {
        { std::lock_guard<std::mutex> lk(m_); q_.push_back(std::move(fn)); }
        cv_.notify_one();
    }
    void drain(std::chrono::milliseconds timeout) {
        std::deque<std::function<void()>> batch;
        {
            std::unique_lock<std::mutex> lk(m_);
            cv_.wait_for(lk, timeout, [&] { return !q_.empty() || stop_; });
            batch.swap(q_);
        }
        for (auto& fn : batch) fn();
    }
    void requestStop() { { std::lock_guard<std::mutex> lk(m_); stop_ = true; } cv_.notify_all(); }

private:
    std::mutex m_;
    std::condition_variable cv_;
    std::deque<std::function<void()>> q_;
    bool stop_ = false;
};

// ---------------------------------------------------------------------------
// The engine. One instance per app; held as an opaque handle by Kotlin.
// ---------------------------------------------------------------------------
class MobileCore : public ILogPageView {
public:
    explicit MobileCore(JNIEnv* env, jobject listener)
        : page_(*this), sync_(disp_), coord_(sync_)
#ifndef XLOG_NO_QRZ
        , qrz_(disp_), qrzPeer_(sync_, qrz_)
#endif
    {
        listener_ = env->NewGlobalRef(listener);
        jclass cls = env->GetObjectClass(listener);
        mLogbook_ = env->GetMethodID(cls, "onLogbookChanged", "()V");
        mPeers_   = env->GetMethodID(cls, "onPeersChanged", "()V");
        mStatus_  = env->GetMethodID(cls, "onStatus", "(Ljava/lang/String;)V");
        mQrz_     = env->GetMethodID(cls, "onQrzResult", "(Ljava/lang/String;)V");
    }

    struct Config {
        std::string dataDir, secret, nodeName, ctyPath, qrzUser, qrzPassword;
        int qrzCacheDays = 365;
        std::vector<std::pair<std::string, int>> staticPeers;
        int  syncPort = 7388;          // default port for parsing peer "host"
        bool trustEnforce = false;
        std::vector<std::string> trustedIds;
        bool requireIdentity = true;
    };

    // --- lifecycle (called from a Kotlin thread; not the core thread) -------
    void start(Config cfg) {
        cfg_ = std::move(cfg);
        ::mkdir(cfg_.dataDir.c_str(), 0700);
        if (!cfg_.ctyPath.empty()) {
            const bool ok = dxcc::loadFile(cfg_.ctyPath);
            __android_log_print(ANDROID_LOG_INFO, "xlog2jni",
                "cty.dat load: path='%s' ok=%d available=%d",
                cfg_.ctyPath.c_str(), ok, dxcc::available());
        } else {
            __android_log_print(ANDROID_LOG_WARN, "xlog2jni", "cty.dat path EMPTY");
        }

        running_ = true;
        thread_ = std::thread([this] { coreLoop(); });
    }

    void stop() {
        if (!running_.exchange(false)) return;
        // Wake the drain loop so the core thread exits, then JOIN it before
        // touching sync_ — otherwise sync_.stop() would race the core thread's
        // final drain iteration. After the join we are single-threaded again.
        disp_.requestStop();
        if (thread_.joinable()) thread_.join();
        sync_.stop();
        if (listener_) {
            if (JNIEnv* env = attach()) env->DeleteGlobalRef(listener_);
            listener_ = nullptr;
        }
    }

    // --- synchronous reads (run on the core thread, block for the result) --
    std::string listQsos() {
        std::string out;
        runOnCore([&] {
            bool first = true;
            for (const Qso& q : page_.logbook().qsos()) {
                if (!first) out += RS;
                out += qsoToWire(q);
                first = false;
            }
        });
        return out;
    }

    std::string deriveDxcc(const std::string& call) {
        std::string out;
        runOnCore([&] {
            dxccderive::Fields f = dxccderive::derive(call, {});
            out = f.country + US + f.cq_zone + US + f.itu_zone + US + f.continent;
        });
        return out;
    }

    std::string bandForFreq(double mhz) {
        std::string out;
        runOnCore([&] { out = bands::forFrequencyMHz(mhz); });
        return out;
    }

    std::string findDupe(const std::string& wire, long editId) {
        std::string out;
        runOnCore([&] {
            FormData d = wireToForm(wire);
            dxccderive::Fields f = dxccderive::derive(d.call, {});
            Qso q = qsomap::fromForm(d, editId, f);
            if (auto dup = page_.logbook().findDuplicate(q, editId))
                out = dupe::format(*dup);
        });
        return out;
    }

    // Add/update flow exactly the desktop's: drive the presenter through the
    // form so uuid minting, DXCC derivation, and the onLocalUpsert push all
    // happen identically. The view (this object) stashes the form in form_.
    long addQso(const std::string& wire) {
        long id = 0;
        runOnCore([&] {
            page_.onClear();              // editingId_ = 0, resets form_
            form_ = wireToForm(wire);
            page_.onAddOrUpdate();        // adds + fires onLocalUpsert
            id = lastUpsertId_;
        });
        return id;
    }

    void updateQso(long editId, const std::string& wire) {
        runOnCore([&] {
            page_.onRowSelected(editId);  // editingId_ = editId, loads form_
            form_ = wireToForm(wire);     // overwrite with the edited values
            page_.onAddOrUpdate();        // updates + fires onLocalUpsert
        });
    }

    void deleteQso(long id) {
        runOnCore([&] { page_.deleteQso(id); });  // fires onLocalDelete
    }

    int importAdif(const std::string& text) {
        int n = 0;
        runOnCore([&] { n = page_.importAdif(text); });
        return n;
    }

    std::string exportAdif() {
        std::string out;
        runOnCore([&] { out = page_.exportAdif(); });
        return out;
    }

    void lookup(const std::string& call) {
#ifndef XLOG_NO_QRZ
        runOnCore([&] {
            if (!cfg_.qrzUser.empty())
                qrz_.lookup(cfg_.qrzUser, cfg_.qrzPassword, call);
        });
#else
        (void)call;
#endif
    }

    std::string localId()     { std::string s; runOnCore([&]{ s = sync_.localId(); });    return s; }
    std::string identityKey() { std::string s; runOnCore([&]{ s = sync_.identityKey(); }); return s; }
    int memberCount()         { int n = 0;      runOnCore([&]{ n = sync_.memberCount(); }); return n; }

    std::string peerSnapshot() {
        std::string out;
        runOnCore([&] {
            bool first = true;
            for (const auto& p : coord_.peerSnapshot()) {
                if (!first) out += RS;
                out += p.id + US + p.name + US +
                       (p.trusted ? "1" : "0") + US +
                       (p.online ? "1" : "0") + US +
                       (p.ready ? "1" : "0");
                first = false;
            }
        });
        return out;
    }

    void trustPeer(const std::string& id)  { runOnCore([&]{ coord_.trustPeer(id); }); }
    void revokePeer(const std::string& id) { runOnCore([&]{ coord_.revokePeer(id); }); }
    void syncNow()                         { runOnCore([&]{ coord_.syncNow(); }); }

    // --- ILogPageView: the presenter reads/writes the form through form_;
    // setRows signals the UI to re-query the logbook. -----------------------
    FormData formData() const override { return form_; }
    void setFormData(const FormData& f) override { form_ = f; }
    void setCall(const std::string& s) override { form_.call = s; }
    void setFreq(const std::string& s) override { form_.freq = s; }
    void setBand(const std::string& s) override { form_.band = s; }
    void setMode(const std::string& s) override { form_.mode = s; }
    void setRows(const std::vector<Qso>&) override { callV(mLogbook_); }
    void clearSelection() override {}
    void setDupeWarning(const std::string&, bool) override {}
    void setDxccText(const std::string&) override {}
    void setEditing(bool) override {}
    void setCwButtons(const std::array<std::string, 9>&) override {}
    void focusCall() override {}
    void showSearch() override {}

private:
    // The core thread: open the logbook, wire everything (exactly like
    // SyncDaemon.cpp), start the mesh, then drain the dispatcher until stop.
    void coreLoop() {
        JNIEnv* env = attach();  // attach this thread to the JVM for callbacks
        (void)env;

        const std::string logPath    = cfg_.dataDir + "/default.xlog";
        const std::string cachePath  = cfg_.dataDir + "/qrz-cache.sqlite";
        const std::string nodeIdPath = cfg_.dataDir + "/node_id";

        page_.openFile(logPath);

        auto status = [this](const std::string& m) { callS(mStatus_, m); };
        sync_.onStatus    = status;
        coord_.onStatus   = status;
        coord_.onPeersChanged = [this] { callV(mPeers_); };

        sync_.onPeerUp   = [this](const LogbookSync::PeerKey& p) { coord_.onPeerUp(p); };
        sync_.onPeerDown = [this](const LogbookSync::PeerKey& p) { coord_.onPeerDown(p); };
#ifndef XLOG_NO_QRZ
        qrz_.setCache(cachePath, cfg_.qrzCacheDays);
        qrzPeer_.onStatus = status;
        sync_.onMessage  = [this](const LogbookSync::PeerKey& p, const syncproto::Message& m) {
            if (m.type == syncproto::Type::QrzQuery || m.type == syncproto::Type::QrzResponse)
                qrzPeer_.onMessage(p, m);
            else
                coord_.onMessage(p, m);
        };
#else
        (void)cachePath;
        sync_.onMessage  = [this](const LogbookSync::PeerKey& p, const syncproto::Message& m) {
            coord_.onMessage(p, m);
        };
#endif

        coord_.attach(&page_);
        page_.onLocalUpsert = [this](const Qso& q) {
            lastUpsertId_ = q.id;       // captured by addQso/updateQso
            coord_.onLocalUpsert(q);
        };
        page_.onLocalDelete = [this](const std::string& uuid, const std::string& at) {
            coord_.onLocalDelete(uuid, at);
        };

#ifndef XLOG_NO_QRZ
        // QRZ: peers between cache and qrz.com; a timer for the peer timeout.
        qrz_.setPeerResolver([this](const std::string& call,
                                    std::function<void(std::optional<QrzResult>)> reply) {
            qrzPeer_.query(call, std::move(reply));
        });
        qrzPeer_.scheduleOnce = [this](int ms, std::function<void()> fn) {
            std::weak_ptr<bool> alive = alive_;
            std::thread([this, alive, ms, fn = std::move(fn)]() mutable {
                std::this_thread::sleep_for(std::chrono::milliseconds(ms));
                if (auto a = alive.lock()) disp_.post(std::move(fn));
            }).detach();
        };
        qrz_.onResult = [this](const QrzResult& r, const std::string& err) {
            std::string w = r.call + US + r.name + US + r.qth + US +
                            r.locator + US + r.country + US + err;
            callS(mQrz_, w);
        };
#endif  // XLOG_NO_QRZ

        // Start the mesh — same config shape as SyncDaemon.
        LogbookSync::Config cfg;
        cfg.nodeId = readFileTrimmed(nodeIdPath);
        cfg.group  = syncproto::meshGroup(cfg_.secret);
        cfg.port   = 0;
        cfg.staticPeers = cfg_.staticPeers;
        cfg.psk    = cfg_.secret;
        if (!cfg_.secret.empty()) {
            cfg.identityFile    = cfg_.dataDir + "/node_identity";
            cfg.requireIdentity = cfg_.requireIdentity;
            cfg.nodeName        = cfg_.nodeName.empty() ? "xlog2-android" : cfg_.nodeName;
        }
        sync_.start(cfg);
        if (cfg.nodeId.empty() && !sync_.localId().empty())
            writeFile(nodeIdPath, sync_.localId());
        coord_.configure(sync_.localId(), cfg_.secret);
        coord_.setTrust(cfg_.trustEnforce, cfg_.trustedIds);

        callV(mLogbook_);  // initial population
        callV(mPeers_);

        while (running_.load())
            disp_.drain(std::chrono::milliseconds(200));

        detach();
    }

    // Run fn on the core thread and block until it has executed. Safe because
    // JNI calls never originate on the core thread (no self-deadlock).
    void runOnCore(const std::function<void()>& fn) {
        if (!running_.load()) { fn(); return; }
        std::mutex m;
        std::condition_variable cv;
        bool done = false;
        disp_.post([&] {
            fn();
            { std::lock_guard<std::mutex> lk(m); done = true; }
            cv.notify_one();
        });
        std::unique_lock<std::mutex> lk(m);
        cv.wait(lk, [&] { return done; });
    }

    // JNIEnv for the calling thread (attaching if needed).
    JNIEnv* attach() {
        JNIEnv* env = nullptr;
        if (g_vm->GetEnv(reinterpret_cast<void**>(&env), JNI_VERSION_1_6) == JNI_OK)
            return env;
        // AttachCurrentThread takes JNIEnv** on the Android NDK but void** on the
        // desktop JDK we host-compile against; bridge the two.
#if defined(__ANDROID__)
        g_vm->AttachCurrentThread(&env, nullptr);
#else
        g_vm->AttachCurrentThread(reinterpret_cast<void**>(&env), nullptr);
#endif
        return env;
    }
    void detach() { if (g_vm) g_vm->DetachCurrentThread(); }

    void callV(jmethodID m) {
        if (!listener_ || !m) return;
        if (JNIEnv* env = attach()) env->CallVoidMethod(listener_, m);
    }
    void callS(jmethodID m, const std::string& s) {
        if (!listener_ || !m) return;
        if (JNIEnv* env = attach()) {
            jstring js = env->NewStringUTF(s.c_str());
            env->CallVoidMethod(listener_, m, js);
            env->DeleteLocalRef(js);
        }
    }

    CoreDispatcher   disp_;
    LogPagePresenter page_;
    LogbookSync      sync_;
    SyncCoordinator  coord_;
#ifndef XLOG_NO_QRZ
    QrzClient        qrz_;
    QrzPeer          qrzPeer_;
#endif

    Config            cfg_;
    mutable FormData  form_;          // the entry form the presenter drives
    long              lastUpsertId_ = 0;
    std::thread       thread_;
    std::atomic<bool> running_{false};
    std::shared_ptr<bool> alive_ = std::make_shared<bool>(true);

    jobject   listener_ = nullptr;
    jmethodID mLogbook_ = nullptr, mPeers_ = nullptr, mStatus_ = nullptr, mQrz_ = nullptr;
};

// Convert a jstring to std::string (UTF-8), tolerating null.
std::string jstr(JNIEnv* env, jstring s) {
    if (!s) return {};
    const char* c = env->GetStringUTFChars(s, nullptr);
    std::string out = c ? c : "";
    env->ReleaseStringUTFChars(s, c);
    return out;
}
jstring toJ(JNIEnv* env, const std::string& s) { return env->NewStringUTF(s.c_str()); }

MobileCore* core(jlong handle) { return reinterpret_cast<MobileCore*>(handle); }

}  // namespace

extern "C" {

JNIEXPORT jint JNICALL JNI_OnLoad(JavaVM* vm, void*) {
    g_vm = vm;
    return JNI_VERSION_1_6;
}

#define XC(name) Java_ro_scripca_xlog2_XlogCore_##name

// nativeStart(listener, dataDir, secret, nodeName, ctyPath, qrzUser, qrzPass,
//             qrzCacheDays, staticPeers (RS-separated "host:port"), syncPort,
//             trustEnforce, trustedIds (RS-separated), requireIdentity) -> handle
JNIEXPORT jlong JNICALL XC(nativeStart)(
        JNIEnv* env, jobject /*thiz*/, jobject listener,
        jstring dataDir, jstring secret, jstring nodeName, jstring ctyPath,
        jstring qrzUser, jstring qrzPass, jint qrzCacheDays,
        jstring staticPeers, jint syncPort, jboolean trustEnforce,
        jstring trustedIds, jboolean requireIdentity) {
    auto* mc = new MobileCore(env, listener);
    MobileCore::Config cfg;
    cfg.dataDir      = jstr(env, dataDir);
    cfg.secret       = jstr(env, secret);
    cfg.nodeName     = jstr(env, nodeName);
    cfg.ctyPath      = jstr(env, ctyPath);
    cfg.qrzUser      = jstr(env, qrzUser);
    cfg.qrzPassword  = jstr(env, qrzPass);
    cfg.qrzCacheDays = qrzCacheDays;
    cfg.syncPort     = syncPort;
    cfg.trustEnforce = trustEnforce;
    cfg.requireIdentity = requireIdentity;
    for (const std::string& h : split(jstr(env, staticPeers), RS)) {
        if (h.empty()) continue;
        auto pr = LogbookSync::parsePeer(h, syncPort);
        if (!pr.first.empty()) cfg.staticPeers.push_back(pr);
    }
    for (const std::string& id : split(jstr(env, trustedIds), RS))
        if (!id.empty()) cfg.trustedIds.push_back(id);
    mc->start(std::move(cfg));
    return reinterpret_cast<jlong>(mc);
}

JNIEXPORT void JNICALL XC(nativeStop)(JNIEnv*, jobject, jlong h) {
    if (auto* mc = core(h)) { mc->stop(); delete mc; }
}

JNIEXPORT jstring JNICALL XC(nativeListQsos)(JNIEnv* env, jobject, jlong h) {
    return toJ(env, core(h)->listQsos());
}
JNIEXPORT jlong JNICALL XC(nativeAddQso)(JNIEnv* env, jobject, jlong h, jstring wire) {
    return core(h)->addQso(jstr(env, wire));
}
JNIEXPORT void JNICALL XC(nativeUpdateQso)(JNIEnv* env, jobject, jlong h, jlong id, jstring wire) {
    core(h)->updateQso(id, jstr(env, wire));
}
JNIEXPORT void JNICALL XC(nativeDeleteQso)(JNIEnv*, jobject, jlong h, jlong id) {
    core(h)->deleteQso(id);
}
JNIEXPORT jstring JNICALL XC(nativeDeriveDxcc)(JNIEnv* env, jobject, jlong h, jstring call) {
    return toJ(env, core(h)->deriveDxcc(jstr(env, call)));
}
JNIEXPORT jstring JNICALL XC(nativeBandForFreq)(JNIEnv* env, jobject, jlong h, jdouble mhz) {
    return toJ(env, core(h)->bandForFreq(mhz));
}
JNIEXPORT jstring JNICALL XC(nativeFindDupe)(JNIEnv* env, jobject, jlong h, jstring wire, jlong editId) {
    return toJ(env, core(h)->findDupe(jstr(env, wire), editId));
}
JNIEXPORT jint JNICALL XC(nativeImportAdif)(JNIEnv* env, jobject, jlong h, jstring text) {
    return core(h)->importAdif(jstr(env, text));
}
JNIEXPORT jstring JNICALL XC(nativeExportAdif)(JNIEnv* env, jobject, jlong h) {
    return toJ(env, core(h)->exportAdif());
}
JNIEXPORT void JNICALL XC(nativeLookup)(JNIEnv* env, jobject, jlong h, jstring call) {
    core(h)->lookup(jstr(env, call));
}
JNIEXPORT jstring JNICALL XC(nativeLocalId)(JNIEnv* env, jobject, jlong h) {
    return toJ(env, core(h)->localId());
}
JNIEXPORT jstring JNICALL XC(nativeIdentityKey)(JNIEnv* env, jobject, jlong h) {
    return toJ(env, core(h)->identityKey());
}
JNIEXPORT jint JNICALL XC(nativeMemberCount)(JNIEnv*, jobject, jlong h) {
    return core(h)->memberCount();
}
JNIEXPORT jstring JNICALL XC(nativePeerSnapshot)(JNIEnv* env, jobject, jlong h) {
    return toJ(env, core(h)->peerSnapshot());
}
JNIEXPORT void JNICALL XC(nativeTrustPeer)(JNIEnv* env, jobject, jlong h, jstring id) {
    core(h)->trustPeer(jstr(env, id));
}
JNIEXPORT void JNICALL XC(nativeRevokePeer)(JNIEnv* env, jobject, jlong h, jstring id) {
    core(h)->revokePeer(jstr(env, id));
}
JNIEXPORT void JNICALL XC(nativeSyncNow)(JNIEnv*, jobject, jlong h) {
    core(h)->syncNow();
}

}  // extern "C"
