// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2025 Adrian Scripcă (YO6SSW)

#include "Rig.h"

#include <hamlib/rig.h>

#include <chrono>

namespace {
RIG* asRig(void* p) { return static_cast<RIG*>(p); }

// The Hamlib passband width (Hz) for an IF-filter slot of a given mode:
// 1 = wide, 2 = normal, 3 = narrow. Returns 0 if the backend doesn't define it.
pbwidth_t passbandForFilter(RIG* rig, rmode_t mode, int filter) {
  switch (filter) {
    case 1:
      return rig_passband_wide(rig, mode);
    case 2:
      return rig_passband_normal(rig, mode);
    case 3:
      return rig_passband_narrow(rig, mode);
    default:
      return 0;
  }
}

// Classify the current passband width into an IF-filter slot by nearest match
// to the mode's wide/normal/narrow widths. 0 if the rig reports no widths.
int filterFromWidth(RIG* rig, rmode_t mode, pbwidth_t width) {
  const pbwidth_t cand[3] = {rig_passband_wide(rig, mode),
                             rig_passband_normal(rig, mode),
                             rig_passband_narrow(rig, mode)};
  int best = 0;
  pbwidth_t bestDiff = 0;
  for (int i = 0; i < 3; ++i) {
    if (cand[i] <= 0) continue;
    const pbwidth_t diff = width > cand[i] ? width - cand[i] : cand[i] - width;
    if (best == 0 || diff < bestDiff) {
      best = i + 1;
      bestDiff = diff;
    }
  }
  return best;
}
}  // namespace

RigController::RigController(IUiDispatcher& ui) : ui_(ui) {
  // Keep Hamlib quiet unless something goes wrong.
  rig_set_debug(RIG_DEBUG_ERR);
}

RigController::~RigController() { stop(); }

void RigController::start(int model, const std::string& device, int pollMs) {
  stop();
  lastError_.clear();
  {
    // Re-evaluate power support for the new connection (latched once a rig
    // answers; see the worker's get_powerstat handling).
    std::lock_guard<std::mutex> lock(mutex_);
    powerSupported_ = false;
    powerOn_ = false;
    hasPower_ = false;
    pendingPower_ = -1;
  }
  pendingModel_ = model;
  pendingDevice_ = device;
  pollMs_ = pollMs > 0 ? pollMs : 500;
  running_.store(true);
  // rig_open() can block for the connect timeout (e.g. a network rig with no
  // listener, or a slow serial port); do it on the worker so start() returns
  // immediately and the UI thread keeps running.
  run_ = std::make_shared<Run>();
  thread_ = std::thread(&RigController::worker, this, run_);
}

void RigController::stop() {
  running_.store(false);

  // Decide, under the run's lock, whether the worker can be joined quickly or
  // must be abandoned. If it has reached the poll loop (`opened`), the loop
  // exits within a poll slice and closes the rig, so join() is fast. If it is
  // still inside the blocking rig_open(), joining would stall shutdown until
  // the connection times out — so mark it abandoned and detach instead; the
  // worker then self-cleans via the shared Run without touching us.
  bool detach = false;
  if (run_) {
    std::lock_guard<std::mutex> lock(run_->mutex);
    if (!run_->opened) {
      run_->abandoned = true;
      detach = true;
    }
  }
  if (thread_.joinable()) {
    if (detach)
      thread_.detach();
    else
      thread_.join();  // the worker closes the rig as its last action
  }
  run_.reset();

  // Fallback: a rig handle with no worker to close it (defensive).
  if (rig_) {
    rig_close(asRig(rig_));
    rig_cleanup(asRig(rig_));
    rig_ = nullptr;
  }
}

void RigController::setFrequency(double mhz) {
  if (!running_.load() || mhz <= 0.0) return;
  std::lock_guard<std::mutex> lock(mutex_);
  pendingFreqMhz_ = mhz;
  hasPendingFreq_ = true;
}

void RigController::stepFrequency(double hz) {
  if (!running_.load() || hz == 0.0) return;
  std::lock_guard<std::mutex> lock(mutex_);
  pendingStepHz_ += hz;
}

void RigController::setFilter(int n) {
  if (!running_.load() || n < 1 || n > 3) return;
  std::lock_guard<std::mutex> lock(mutex_);
  pendingFilter_ = n;
}

void RigController::setMode(const std::string& mode) {
  if (!running_.load() || mode.empty()) return;
  std::lock_guard<std::mutex> lock(mutex_);
  pendingMode_ = mode;
}

void RigController::setPower(bool on) {
  if (!running_.load()) return;
  std::lock_guard<std::mutex> lock(mutex_);
  pendingPower_ = on ? 1 : 0;
}

void RigController::setAgc(bool on) {
  if (!running_.load()) return;
  std::lock_guard<std::mutex> lock(mutex_);
  pendingAgc_ = on ? 1 : 0;
}

void RigController::worker(std::shared_ptr<Run> run) {
  // Report the connection outcome on the UI thread. Only ever called while the
  // run is not abandoned (so the controller — hence ui_ — is still alive); the
  // posted closure additionally drops itself if the controller is gone by the
  // time it runs.
  auto report = [this](bool ok) {
    ui_.post([this, w = std::weak_ptr<bool>(alive_), ok, err = lastError_]() {
      if (!w.expired() && onConnectResult) onConnectResult(ok, err);
    });
  };

  RIG* rig = rig_init(static_cast<rig_model_t>(pendingModel_));
  if (!rig) {
    std::lock_guard<std::mutex> lock(run->mutex);
    if (run->abandoned) return;  // controller detached us; touch nothing
    lastError_ =
        "rig_init failed (unknown model " + std::to_string(pendingModel_) + ")";
    running_.store(false);
    report(
        false);  // under the lock so a racing stop() can't free us mid-report
    return;
  }
  if (!pendingDevice_.empty())
    rig_set_conf(rig, rig_token_lookup(rig, "rig_pathname"),
                 pendingDevice_.c_str());

  // We run our own polling thread, so disable Hamlib's background readers
  // (internal poll routine + async data handler) — otherwise they'd touch the
  // non-thread-safe RIG handle concurrently with our worker. Set before
  // rig_open(), where those threads would be started.
  rig_set_conf(rig, rig_token_lookup(rig, "poll_interval"), "0");
  rig_set_conf(rig, rig_token_lookup(rig, "async"), "0");

  const int rc = rig_open(rig);  // may block for the whole connect timeout

  // Hand off under the run's lock: stop() inspects `opened`/`abandoned` under
  // the same lock, so exactly one of {join, detach} happens and the worker
  // touches controller state only when it will be joined.
  {
    std::lock_guard<std::mutex> lock(run->mutex);
    if (run->abandoned) {
      // Detached during the open. Clean up only our local handle and exit
      // without referencing any controller member.
      if (rc == RIG_OK) rig_close(rig);
      rig_cleanup(rig);
      return;
    }
    if (rc != RIG_OK) {
      lastError_ = rigerror(rc);
      rig_cleanup(rig);
      running_.store(false);
      report(false);  // under the lock (see above)
      return;
    }
    rig_ = rig;
    run->opened = true;  // committed: stop() will join from here on
  }
  report(true);

  while (running_.load()) {
    // Apply any queued tune/step/filter requests before reading back state.
    double pending = 0.0;
    double stepHz = 0.0;
    int setFilt = 0;
    int setPow = -1;
    int setAgcTo = -1;
    std::string setModeName;
    {
      std::lock_guard<std::mutex> lock(mutex_);
      if (hasPendingFreq_) {
        pending = pendingFreqMhz_;
        hasPendingFreq_ = false;
      }
      stepHz = pendingStepHz_;
      pendingStepHz_ = 0.0;
      setFilt = pendingFilter_;
      pendingFilter_ = 0;
      setPow = pendingPower_;
      pendingPower_ = -1;
      setAgcTo = pendingAgc_;
      pendingAgc_ = -1;
      setModeName.swap(pendingMode_);
    }
    if (setAgcTo >= 0) {
      // Off = RIG_AGC_OFF; on = a normal AGC (MEDIUM is the IC-7300 default).
      value_t v{};
      v.i = setAgcTo ? RIG_AGC_MEDIUM : RIG_AGC_OFF;
      rig_set_level(asRig(rig_), RIG_VFO_CURR, RIG_LEVEL_AGC, v);
    }
    if (setPow >= 0) {
      rig_set_powerstat(asRig(rig_), setPow ? RIG_POWER_ON : RIG_POWER_OFF);
      // Reflect the commanded state immediately: a rig that has just been
      // switched OFF typically stops answering, so we must not wait for a
      // readback that may never arrive (and the readback below would then
      // leave the last-known state untouched, keeping this value).
      std::lock_guard<std::mutex> lock(mutex_);
      powerOn_ = (setPow != 0);
      hasPower_ = true;
    }
    if (pending > 0.0)
      rig_set_freq(asRig(rig_), RIG_VFO_CURR,
                   static_cast<freq_t>(pending * 1.0e6));
    if (stepHz != 0.0) {
      freq_t cur = 0;
      if (rig_get_freq(asRig(rig_), RIG_VFO_CURR, &cur) == RIG_OK) {
        const double next = static_cast<double>(cur) + stepHz;
        if (next > 0.0)
          rig_set_freq(asRig(rig_), RIG_VFO_CURR, static_cast<freq_t>(next));
      }
    }
    if (!setModeName.empty()) {
      const rmode_t nm = rig_parse_mode(setModeName.c_str());
      if (nm != RIG_MODE_NONE) {
        // Pin the VFO first for the same reason as the filter switch
        // below: via netrigctl RIG_VFO_CURR maps to VFOA while the remote
        // reports "Main", so a targetable set is needed for the command
        // to actually reach the rig. Passband 0 = the mode's normal width.
        rig_set_vfo(asRig(rig_), RIG_VFO_A);
        rig_set_mode(asRig(rig_), RIG_VFO_CURR, nm, RIG_PASSBAND_NORMAL);
      }
    }
    if (setFilt >= 1 && setFilt <= 3) {
      rmode_t cm = RIG_MODE_NONE;
      pbwidth_t cw = 0;
      if (rig_get_mode(asRig(rig_), RIG_VFO_CURR, &cm, &cw) == RIG_OK) {
        const pbwidth_t tw = passbandForFilter(asRig(rig_), cm, setFilt);
        // A filter switch only changes the passband width, not the mode.
        // hamlib's rig_set_mode short-circuits ("mode not changing, so
        // ignoring") and never sends the command when the mode is
        // unchanged AND the target VFO differs from the rig's current
        // VFO — which is always the case via netrigctl (RIG_VFO_CURR is
        // fixed up to VFOA while the remote reports its VFO as "Main").
        // Our 500 ms poll caches the mode, so the width change is then
        // dropped client-side and never reaches the rig. Pin the VFO
        // first so the set takes hamlib's targetable branch and is
        // actually transmitted — this is exactly what `rigctl` itself
        // does (V VFOA; M ...; V Main).
        if (tw > 0) {
          rig_set_vfo(asRig(rig_), RIG_VFO_A);
          rig_set_mode(asRig(rig_), RIG_VFO_CURR, cm, tw);
        }
      }
    }

    // Power status, read independently of the frequency: a rig that is off
    // won't report a frequency, but we still want its power state to reach
    // the panel (so the operator can turn it back on).
    powerstat_t ps = RIG_POWER_UNKNOWN;
    const int prc = rig_get_powerstat(asRig(rig_), &ps);
    {
      std::lock_guard<std::mutex> lock(mutex_);
      if (prc == RIG_OK) {
        // Latch support on the first rig that answers: the read fails
        // precisely when the rig is off, and dropping support then would
        // hide the very button needed to turn it back on.
        powerSupported_ = true;
        powerOn_ = (ps != RIG_POWER_OFF);
      }
      // On a failed read keep the last known state (set above by a command,
      // or by the previous successful read) rather than guessing.
      hasPower_ = true;
    }

    freq_t f = 0;
    rmode_t m = RIG_MODE_NONE;
    pbwidth_t w = 0;
    const int rc = rig_get_freq(asRig(rig_), RIG_VFO_CURR, &f);
    rig_get_mode(asRig(rig_), RIG_VFO_CURR, &m, &w);

    if (rc == RIG_OK) {
      const char* modeStr = rig_strrmode(m);
      const int filt = filterFromWidth(asRig(rig_), m, w);
      std::lock_guard<std::mutex> lock(mutex_);
      mhz_ = f / 1.0e6;
      mode_ = modeStr ? modeStr : "";
      pbwidthHz_ = static_cast<int>(w);
      filter_ = filt;
      hasUpdate_ = true;
    }
    ui_.post([this, w = std::weak_ptr<bool>(alive_)]() {
      if (!w.expired()) deliverUpdate();
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
  double mhz;
  std::string mode;
  int pbwidthHz;
  int filter;
  bool haveUpdate;
  bool havePower;
  bool powerSupported;
  bool powerOn;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    haveUpdate = hasUpdate_;
    havePower = hasPower_;
    mhz = mhz_;
    mode = mode_;
    pbwidthHz = pbwidthHz_;
    filter = filter_;
    powerSupported = powerSupported_;
    powerOn = powerOn_;
    hasUpdate_ = false;
    hasPower_ = false;
  }
  if (haveUpdate) {
    if (onUpdate) onUpdate(mhz, mode);
    if (onFilter) onFilter(pbwidthHz, filter);
  }
  if (havePower && onPower) onPower(powerSupported, powerOn);
}
