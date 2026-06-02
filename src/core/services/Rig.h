#pragma once

#include "IUiDispatcher.h"

#include <atomic>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

// Polls a radio for its frequency and mode using Hamlib (libhamlib) on a
// worker thread, delivering readings to the UI thread via the dispatcher.
// The Hamlib RIG handle is only ever touched on the worker thread — opening
// (which can block, e.g. a network rig with no listener) included.
class RigController {
public:
    // Called on the UI thread with the latest frequency (MHz) and mode name.
    std::function<void(double mhz, const std::string& mode)> onUpdate;
    // Called on the UI thread when an attempted connection finishes; on failure
    // `error` is set. rig_open() can block, so it runs on the worker thread and
    // the outcome is reported here — start() never blocks the UI thread.
    std::function<void(bool ok, const std::string& error)> onConnectResult;

    explicit RigController(IUiDispatcher& ui);
    ~RigController();
    RigController(const RigController&)            = delete;
    RigController& operator=(const RigController&) = delete;

    // Begins connecting (model is a Hamlib model id; 1 == RIG_MODEL_DUMMY) and
    // polling every pollMs. Returns immediately; the blocking rig_open() runs on
    // the worker thread and success/failure is delivered via onConnectResult.
    // `device` may be empty for model 1 / network rigs.
    void start(int model, const std::string& device, int pollMs);
    void stop();

    // Queue a VFO frequency change (MHz); applied by the worker on its next
    // poll tick, so the Hamlib handle stays worker-thread-only. No-op if the
    // rig isn't running.
    void setFrequency(double mhz);

    bool isRunning() const { return running_.load(); }
    const std::string& lastError() const { return lastError_; }

private:
    void worker();
    void deliverUpdate();  // UI thread

    IUiDispatcher&     ui_;
    void* rig_ = nullptr;  // opaque RIG* (kept void* to keep hamlib out of the header)
    std::thread        thread_;
    std::atomic<bool>  running_{false};
    int                pollMs_ = 500;
    int                pendingModel_ = 1;     // connection params, read by worker
    std::string        pendingDevice_;

    std::mutex  mutex_;
    double      mhz_       = 0.0;
    std::string mode_;
    bool        hasUpdate_ = false;
    double      pendingFreqMhz_ = 0.0;   // requested VFO freq, applied by worker
    bool        hasPendingFreq_ = false;

    // Liveness token: posted closures hold a weak_ptr to it and bail if the
    // controller was destroyed before they ran on the UI thread.
    std::shared_ptr<bool> alive_ = std::make_shared<bool>(true);
    std::string           lastError_;
};
