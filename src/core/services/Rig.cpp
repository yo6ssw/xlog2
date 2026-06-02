#include "Rig.h"

#include <hamlib/rig.h>

#include <chrono>

namespace {
RIG* asRig(void* p) { return static_cast<RIG*>(p); }
}  // namespace

RigController::RigController(IUiDispatcher& ui) : ui_(ui) {
    // Keep Hamlib quiet unless something goes wrong.
    rig_set_debug(RIG_DEBUG_ERR);
}

RigController::~RigController() {
    stop();
}

void RigController::start(int model, const std::string& device, int pollMs) {
    stop();
    lastError_.clear();
    pendingModel_  = model;
    pendingDevice_ = device;
    pollMs_        = pollMs > 0 ? pollMs : 500;
    running_.store(true);
    // rig_open() can block for the connect timeout (e.g. a network rig with no
    // listener, or a slow serial port); do it on the worker so start() returns
    // immediately and the UI thread keeps running.
    thread_ = std::thread(&RigController::worker, this);
}

void RigController::stop() {
    if (running_.exchange(false)) {
        if (thread_.joinable())
            thread_.join();  // the worker closes the rig as its last action
        return;
    }
    // No worker was running (e.g. rig opened but polling never started): close
    // on this thread as a fallback.
    if (rig_) {
        rig_close(asRig(rig_));
        rig_cleanup(asRig(rig_));
        rig_ = nullptr;
    }
}

void RigController::setFrequency(double mhz) {
    if (!running_.load() || mhz <= 0.0)
        return;
    std::lock_guard<std::mutex> lock(mutex_);
    pendingFreqMhz_ = mhz;
    hasPendingFreq_ = true;
}

void RigController::worker() {
    // Open the rig here (off the UI thread) and report the outcome.
    auto report = [this](bool ok) {
        ui_.post([this, w = std::weak_ptr<bool>(alive_), ok, err = lastError_]() {
            if (!w.expired() && onConnectResult)
                onConnectResult(ok, err);
        });
    };

    RIG* rig = rig_init(static_cast<rig_model_t>(pendingModel_));
    if (!rig) {
        lastError_ = "rig_init failed (unknown model " + std::to_string(pendingModel_) + ")";
        running_.store(false);
        report(false);
        return;
    }
    if (!pendingDevice_.empty())
        rig_set_conf(rig, rig_token_lookup(rig, "rig_pathname"), pendingDevice_.c_str());

    // We run our own polling thread, so disable Hamlib's background readers
    // (internal poll routine + async data handler) — otherwise they'd touch the
    // non-thread-safe RIG handle concurrently with our worker. Set before
    // rig_open(), where those threads would be started.
    rig_set_conf(rig, rig_token_lookup(rig, "poll_interval"), "0");
    rig_set_conf(rig, rig_token_lookup(rig, "async"), "0");

    const int rc = rig_open(rig);
    if (rc != RIG_OK) {
        lastError_ = rigerror(rc);
        rig_cleanup(rig);
        running_.store(false);
        report(false);
        return;
    }
    rig_ = rig;
    report(true);

    while (running_.load()) {
        // Apply a queued tune request before reading back the current state.
        double pending = 0.0;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (hasPendingFreq_) {
                pending = pendingFreqMhz_;
                hasPendingFreq_ = false;
            }
        }
        if (pending > 0.0)
            rig_set_freq(asRig(rig_), RIG_VFO_CURR, static_cast<freq_t>(pending * 1.0e6));

        freq_t   f = 0;
        rmode_t  m = RIG_MODE_NONE;
        pbwidth_t w = 0;
        const int rc = rig_get_freq(asRig(rig_), RIG_VFO_CURR, &f);
        rig_get_mode(asRig(rig_), RIG_VFO_CURR, &m, &w);

        if (rc == RIG_OK) {
            const char* modeStr = rig_strrmode(m);
            std::lock_guard<std::mutex> lock(mutex_);
            mhz_       = f / 1.0e6;
            mode_      = modeStr ? modeStr : "";
            hasUpdate_ = true;
        }
        ui_.post([this, w = std::weak_ptr<bool>(alive_)]() {
            if (!w.expired())
                deliverUpdate();
        });

        // Sleep in small slices so stop() is responsive.
        for (int slept = 0; slept < pollMs_ && running_.load(); slept += 50)
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }

    // Close the rig on the worker thread, strictly after the last poll, so
    // polling and the close never overlap.
    if (rig_) {
        rig_close(asRig(rig_));
        rig_cleanup(asRig(rig_));
        rig_ = nullptr;
    }
}

void RigController::deliverUpdate() {
    double      mhz;
    std::string mode;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!hasUpdate_)
            return;
        mhz        = mhz_;
        mode       = mode_;
        hasUpdate_ = false;
    }
    if (onUpdate)
        onUpdate(mhz, mode);
}
