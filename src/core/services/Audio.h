#pragma once

#include "IUiDispatcher.h"

#include <atomic>
#include <functional>
#include <memory>
#include <string>
#include <thread>

// The cwsd audio stream a client subscribes to. sampleRate/channels must match
// the server's `audio` config (they size the Opus decoder and the ALSA device);
// device is the local ALSA *playback* device.
struct AudioStreamConfig {
    bool        enabled    = false;
    std::string host       = "127.0.0.1";  // cwsd audio_stream_server host
    int         port       = 7355;         // UDP port to subscribe to
    int         sampleRate = 48000;        // opus rate: 8000/12000/16000/24000/48000
    int         channels   = 1;
    std::string device     = "default";    // ALSA playback device
};

// Forward declarations so the alsa/opus headers stay confined to the .cpp.
typedef struct _snd_pcm snd_pcm_t;
struct OpusDecoder;

// Subscribes to a cwsd "audio_stream_server" Opus-over-UDP rig-audio stream and
// plays it back through an ALSA device. cwsd has no configured target: a client
// subscribes by sending any datagram to the port and stays subscribed by
// continuing to send (a periodic keepalive); silent clients are dropped. This
// client mirrors that — its worker sends a small keepalive every ~2 s.
//
// A worker thread owns the blocking POSIX socket, the Opus decoder and the ALSA
// playback handle; status changes are marshalled to the UI thread via the
// injected dispatcher, so onStatus always fires on the UI thread. A self-pipe
// wakes the worker promptly on stop().
//
// Wire format (server -> client): a 4-byte big-endian sequence number followed
// by a raw Opus packet (the same as cwsd's audio_stream_server).
class AudioStreamClient {
public:
    std::function<void(const std::string&)> onStatus;  // streaming state/errors

    explicit AudioStreamClient(IUiDispatcher& ui) : ui_(ui) {}
    ~AudioStreamClient();
    AudioStreamClient(const AudioStreamClient&)            = delete;
    AudioStreamClient& operator=(const AudioStreamClient&) = delete;

    void start(const AudioStreamConfig& cfg);
    void stop();
    bool isStreaming() const { return running_.load(); }

private:
    void worker(AudioStreamConfig cfg);
    void wake();  // poke the self-pipe so the worker re-checks its stop flag
    void postStatus(const std::string& s);

    IUiDispatcher&    ui_;
    int               wake_[2] = {-1, -1};
    std::atomic<bool> running_{false};
    std::thread       thread_;

    // Liveness token for posted closures (see DxCluster/RigController). Recreated
    // on each start so a late callback from a previous session is dropped.
    std::shared_ptr<bool> alive_ = std::make_shared<bool>(true);
};
