#pragma once

#include "IUiDispatcher.h"

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

// What the skimmer analyses. sampleRate/channels must match the PCM that is fed
// in (i.e. the rig-audio stream's). The display + decode are confined to the
// audio passband [minHz, maxHz] — CW lives in the low few kHz of rig audio.
struct SkimmerConfig {
    int    sampleRate = 48000;
    int    channels   = 1;
    double minHz      = 250.0;
    double maxHz      = 4000.0;   // CW lives in the low audio; nothing above is decoded
};

// A toolkit-neutral "CW Skimmer": fed mono rig audio, it runs a sliding STFT to
// produce a waterfall and, on every active carrier in the passband at once,
// tracks the keying envelope and decodes Morse (à la VE3NEA's CW Skimmer, scaled
// to a single audio passband rather than wideband IQ). All DSP runs on a private
// worker thread; results are marshalled to the UI thread via the injected
// dispatcher, so the callbacks always fire on the UI thread.
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
    std::function<void(const std::vector<float>& mags, double minHz, double maxHz)> onWaterfall;
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
    CwSkimmer(const CwSkimmer&)            = delete;
    CwSkimmer& operator=(const CwSkimmer&) = delete;

    void start(const SkimmerConfig& cfg);
    void stop();
    bool isRunning() const { return running_.load(); }

    // Feed decoded PCM (int16, interleaved). Called from the audio worker thread;
    // downmixes to mono and queues it. No-op when not running or when the rate
    // does not match the configured one.
    void pushPcm(const std::int16_t* samples, int frames, int channels, int sampleRate);

private:
    void worker(SkimmerConfig cfg);
    void postWaterfall(std::vector<float> mags, double minHz, double maxHz);
    void postChannel(int id, double hz, int wpm, std::string text, std::string call);
    void postRemoved(int id);

    IUiDispatcher&    ui_;
    std::atomic<bool> running_{false};
    std::thread       thread_;

    std::mutex              mu_;
    std::condition_variable cv_;
    std::vector<float>      queue_;   // mono samples awaiting analysis
    int                     rate_ = 0;  // configured rate (guards pushPcm)

    // Liveness token for posted closures (see the other services): recreated on
    // each start so a late callback from a previous session is dropped.
    std::shared_ptr<bool> alive_ = std::make_shared<bool>(true);
};
