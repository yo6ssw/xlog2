// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2025 Adrian Scripcă (YO6SSW)

#pragma once

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_set>
#include <vector>

#include "IUiDispatcher.h"

// What the skimmer analyses. sampleRate/channels must match the PCM that is fed
// in (i.e. the rig-audio stream's). The display + decode are confined to the
// audio passband [minHz, maxHz] — CW lives in the low few kHz of rig audio.
struct SkimmerConfig {
  int sampleRate = 48000;
  int channels = 1;
  double minHz = 250.0;
  double maxHz = 4000.0;  // CW lives in the low audio; nothing above is decoded
};

// A toolkit-neutral "CW Skimmer": fed mono rig audio, it runs a sliding STFT to
// produce a waterfall and, on every active carrier in the passband at once,
// tracks the keying envelope and decodes Morse (à la VE3NEA's CW Skimmer,
// scaled to a single audio passband rather than wideband IQ). All DSP runs on a
// private worker thread; results are marshalled to the UI thread via the
// injected dispatcher, so the callbacks always fire on the UI thread.
//
// PCM arrives via pushPcm() from whoever owns the audio (AudioStreamClient's
// worker). The skimmer copies the samples and returns immediately, so it never
// stalls audio playback. A channel is identified by a stable integer id (its
// FFT bin); the UI keeps one row per id, updated as text is decoded and dropped
// when the carrier goes idle.
class CwSkimmer {
 public:
  // One waterfall line, newest. `mags` holds `cols` intensities in [0,1],
  // left = minHz .. right = maxHz. Posted a few tens of times a second.
  std::function<void(const std::vector<float>& mags, double minHz,
                     double maxHz)>
      onWaterfall;
  // A decoded channel appeared or changed: stable id, audio pitch (Hz),
  // estimated speed (wpm), the rolling decoded text, and the best callsign
  // guess so far ("" if none). First fires once the channel has decoded text.
  std::function<void(int id, double hz, int wpm, const std::string& text,
                     const std::string& call)>
      onChannel;
  // A previously-reported channel went idle and was dropped.
  std::function<void(int id)> onChannelRemoved;

  explicit CwSkimmer(IUiDispatcher& ui) : ui_(ui) {}
  ~CwSkimmer();
  CwSkimmer(const CwSkimmer&) = delete;
  CwSkimmer& operator=(const CwSkimmer&) = delete;

  void start(const SkimmerConfig& cfg);
  void stop();
  bool isRunning() const { return running_.load(); }

  // Detection gating level, in dB above the default. 0 keeps the normal
  // sensitivity; raising it requires stronger signals to spawn a channel and be
  // copied (suppresses noise/ghosts); lowering it catches weaker signals.
  // Thread-safe; the worker reads it each frame, so changes take effect live
  // and survive a stop()/start(). Persisted by the shell.
  void setGate(float db) { gateDb_.store(db, std::memory_order_relaxed); }

  // Minimum per-channel SNR (dB) — the channel's keyed power vs its own tracked
  // noise — for it to be shown and kept. A noise burst that spawns a channel
  // (a spurious 'E'/'T') has low SNR, so raising this rejects them; a real
  // signal sits well above its noise. Thread-safe; live; persisted by the
  // shell.
  void setMinSnr(float db) { minSnrDb_.store(db, std::memory_order_relaxed); }

  // Load a Super-Check-Partial master-callsign list (one call per line, e.g.
  // MASTER.SCP) used to validate and correct decoded callsigns — the single
  // biggest accuracy lever in VE3NEA's CW Skimmer. A decoded call that matches
  // the list is "known"; one a single edit away from exactly one list entry is
  // corrected to it; otherwise it stays an unvalidated guess. Returns the
  // number of calls loaded (0 if the file is absent/unreadable). Thread-safe.
  std::size_t loadCallsignDb(const std::string& path);
  bool hasCallsignDb() const;

  // Paranoid mode: only surface channels whose callsign is confirmed in the DB
  // (no effect when no DB is loaded). Thread-safe; live; persisted by the
  // shell.
  void setKnownCallsOnly(bool on) {
    knownOnly_.store(on, std::memory_order_relaxed);
  }

  // Waterfall level compensation for the rig's IF filter. Narrowing the filter
  // brightens the waterfall even though the signal is unchanged: the AGC lifts
  // the surviving passband, and the percentile noise floor — taken across the
  // whole [minHz, maxHz] analysis band — collapses into the now-silent,
  // filtered-out region, so the displayed (signal - floor) widens. With
  // dbPerOctave > 0 the waterfall is dimmed by dbPerOctave dB for every octave
  // the live passband is narrower than refHz, holding the floor roughly put
  // across filter changes. dbPerOctave is empirical (it folds in both the AGC
  // rise and the floor collapse) — tune it to taste. Display only:
  // detection/decode are untouched. Thread-safe; live; survives stop()/start().
  // 0 disables.
  //
  // offsetDb is a constant trim applied to the whole waterfall *before* the
  // per-bandwidth dimming above (it is subtracted, so positive dims and
  // negative brightens) — a fixed baseline independent of the filter width. 0 =
  // unchanged.
  void setBandwidthNorm(int dbPerOctave, int refHz, int offsetDb) {
    bwNormDb_.store(dbPerOctave, std::memory_order_relaxed);
    bwNormRefHz_.store(refHz, std::memory_order_relaxed);
    bwOffsetDb_.store(offsetDb, std::memory_order_relaxed);
  }

  // The rig's current receive passband width (Hz), fed from RigController's
  // onFilter; 0 = unknown (no compensation applied). Thread-safe; live.
  void setFilterBandwidthHz(int hz) {
    filterBwHz_.store(hz, std::memory_order_relaxed);
  }

  // Feed decoded PCM (int16, interleaved). Called from the audio worker thread;
  // downmixes to mono and queues it. No-op when not running or when the rate
  // does not match the configured one.
  void pushPcm(const std::int16_t* samples, int frames, int channels,
               int sampleRate);

 private:
  void worker(SkimmerConfig cfg);
  void postWaterfall(std::vector<float> mags, double minHz, double maxHz);
  void postChannel(int id, double hz, int wpm, std::string text,
                   std::string call);
  void postRemoved(int id);

  IUiDispatcher& ui_;
  std::atomic<bool> running_{false};
  std::atomic<float> gateDb_{
      0.0f};  // detection gating offset (dB); see setGate
  std::atomic<float> minSnrDb_{
      0.0f};  // minimum per-channel SNR (dB); see setMinSnr
  std::atomic<bool> knownOnly_{
      false};                     // Paranoid: surface only DB-confirmed calls
  std::atomic<int> bwNormDb_{0};  // waterfall dim per octave of narrowing (dB)
  std::atomic<int> bwNormRefHz_{2800};  // passband width treated as 0 dB
  std::atomic<int> bwOffsetDb_{0};      // constant waterfall level trim (dB)
  std::atomic<int> filterBwHz_{0};  // live rig passband width, Hz (0 = unknown)
  std::thread thread_;

  std::mutex mu_;
  std::condition_variable cv_;
  std::vector<float> queue_;  // mono samples awaiting analysis
  int rate_ = 0;              // configured rate (guards pushPcm)

  // Master-callsign list (SCP). An immutable set behind a shared_ptr so the
  // worker can grab a reference cheaply while loadCallsignDb() can swap in a
  // new one under the mutex without disturbing an in-flight decode.
  std::shared_ptr<const std::unordered_set<std::string>> callDb_;
  mutable std::mutex dbMu_;

  // Liveness token for posted closures (see the other services): recreated on
  // each start so a late callback from a previous session is dropped.
  std::shared_ptr<bool> alive_ = std::make_shared<bool>(true);
};
