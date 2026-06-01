#include "Rig.h"

#include <hamlib/rig.h>

#include <chrono>

namespace {
RIG* asRig(void* p) { return static_cast<RIG*>(p); }
}  // namespace

RigController::RigController() {
    dispatcher_.connect(sigc::mem_fun(*this, &RigController::onDispatch));
    // Keep Hamlib quiet unless something goes wrong.
    rig_set_debug(RIG_DEBUG_ERR);
}

RigController::~RigController() {
    stop();
}

bool RigController::start(int model, const std::string& device, int pollMs) {
    stop();
    lastError_.clear();

    RIG* rig = rig_init(static_cast<rig_model_t>(model));
    if (!rig) {
        lastError_ = "rig_init failed (unknown model " + std::to_string(model) + ")";
        return false;
    }
    if (!device.empty())
        rig_set_conf(rig, rig_token_lookup(rig, "rig_pathname"), device.c_str());

    const int rc = rig_open(rig);
    if (rc != RIG_OK) {
        lastError_ = rigerror(rc);
        rig_cleanup(rig);
        return false;
    }

    rig_     = rig;
    pollMs_  = pollMs > 0 ? pollMs : 500;
    running_.store(true);
    thread_  = std::thread(&RigController::worker, this);
    return true;
}

void RigController::stop() {
    if (running_.exchange(false)) {
        if (thread_.joinable())
            thread_.join();
    }
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
        dispatcher_.emit();

        // Sleep in small slices so stop() is responsive.
        for (int slept = 0; slept < pollMs_ && running_.load(); slept += 50)
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
}

void RigController::onDispatch() {
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
