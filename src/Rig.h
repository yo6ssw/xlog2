#pragma once

#include <glibmm/dispatcher.h>

#include <atomic>
#include <functional>
#include <mutex>
#include <string>
#include <thread>

// Polls a radio for its frequency and mode using Hamlib (libhamlib) on a
// worker thread, delivering readings to the UI thread via Glib::Dispatcher.
// The Hamlib RIG handle is only ever touched from the controller's own
// thread of control (start/stop on the UI thread, polling on the worker).
class RigController {
public:
    // Called on the UI thread with the latest frequency (MHz) and mode name.
    std::function<void(double mhz, const std::string& mode)> onUpdate;

    RigController();
    ~RigController();
    RigController(const RigController&)            = delete;
    RigController& operator=(const RigController&) = delete;

    // Opens the rig (model is a Hamlib model id; 1 == RIG_MODEL_DUMMY) and
    // starts polling every pollMs. Returns false with lastError() set on
    // failure. `device` may be empty for model 1 / network rigs.
    bool start(int model, const std::string& device, int pollMs);
    void stop();

    bool isRunning() const { return running_.load(); }
    const std::string& lastError() const { return lastError_; }

private:
    void worker();
    void onDispatch();  // UI thread

    void* rig_ = nullptr;  // opaque RIG* (kept void* to keep hamlib out of the header)
    std::thread        thread_;
    std::atomic<bool>  running_{false};
    int                pollMs_ = 500;

    std::mutex  mutex_;
    double      mhz_       = 0.0;
    std::string mode_;
    bool        hasUpdate_ = false;

    Glib::Dispatcher dispatcher_;
    std::string      lastError_;
};
