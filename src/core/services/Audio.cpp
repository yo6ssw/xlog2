#include "Audio.h"

#include <alsa/asoundlib.h>
#include <opus/opus.h>

#include <netdb.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <vector>

namespace {

constexpr std::size_t kHeaderBytes  = 4;     // big-endian sequence number
constexpr std::size_t kMaxPacket    = 1500;  // matches the server's datagram cap
constexpr int         kMaxFrame     = 5760;  // 120 ms at 48 kHz, the largest opus frame
constexpr int         kKeepaliveMs  = 2000;  // resubscribe well within cwsd's timeout

// Resolve + open a connected UDP socket to the server. connect() pins the peer so
// recv() only yields its datagrams and send() needs no address (keepalives).
int connectUdp(const std::string& host, int port, std::string& err) {
    addrinfo hints{};
    hints.ai_family   = AF_UNSPEC;
    hints.ai_socktype = SOCK_DGRAM;
    addrinfo* res = nullptr;
    const int rc = ::getaddrinfo(host.c_str(), std::to_string(port).c_str(), &hints, &res);
    if (rc != 0 || !res) {
        err = ::gai_strerror(rc);
        return -1;
    }
    int fd = -1;
    for (addrinfo* ai = res; ai; ai = ai->ai_next) {
        fd = ::socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
        if (fd < 0)
            continue;
        if (::connect(fd, ai->ai_addr, ai->ai_addrlen) == 0)
            break;
        ::close(fd);
        fd = -1;
    }
    ::freeaddrinfo(res);
    if (fd < 0)
        err = std::strerror(errno);
    return fd;
}

// Open ALSA playback for the stream's rate/channels. Returns nullptr on failure
// (a missing device must not block: it is retried on the worker).
snd_pcm_t* openPlayback(const AudioStreamConfig& cfg) {
    snd_pcm_t* pcm = nullptr;
    if (snd_pcm_open(&pcm, cfg.device.c_str(), SND_PCM_STREAM_PLAYBACK, 0) < 0)
        return nullptr;
    if (snd_pcm_set_params(pcm,
                           SND_PCM_FORMAT_S16_LE,
                           SND_PCM_ACCESS_RW_INTERLEAVED,
                           cfg.channels,
                           static_cast<unsigned>(cfg.sampleRate),
                           1,          // allow ALSA soft-resampling
                           100000) < 0 /* ~100 ms latency target */) {
        snd_pcm_close(pcm);
        return nullptr;
    }
    return pcm;
}

}  // namespace

AudioStreamClient::~AudioStreamClient() {
    onStatus = nullptr;  // the owning view may already be gone at shutdown
    stop();
}

void AudioStreamClient::wake() {
    if (wake_[1] >= 0) {
        const char b = 1;
        [[maybe_unused]] ssize_t w = ::write(wake_[1], &b, 1);
    }
}

void AudioStreamClient::start(const AudioStreamConfig& cfg) {
    stop();
    if (cfg.host.empty() || cfg.port <= 0 || cfg.port > 65535) {
        postStatus("Audio: invalid host/port.");
        return;
    }
    // Fresh liveness token so any straggling callback from a prior session is
    // ignored.
    alive_ = std::make_shared<bool>(true);
    if (::pipe(wake_) != 0) {
        postStatus("Audio: " + std::string(std::strerror(errno)));
        return;
    }
    running_.store(true);
    thread_ = std::thread(&AudioStreamClient::worker, this, cfg);
}

void AudioStreamClient::stop() {
    const bool wasRunning = running_.exchange(false);
    if (thread_.joinable()) {
        wake();
        thread_.join();
    }
    for (int* p : {&wake_[0], &wake_[1]}) {
        if (*p >= 0) {
            ::close(*p);
            *p = -1;
        }
    }
    if (wasRunning)
        postStatus("Audio: stopped.");
}

void AudioStreamClient::worker(AudioStreamConfig cfg) {
    postStatus("Audio: connecting to " + cfg.host + ":" + std::to_string(cfg.port) + "…");

    std::string err;
    const int fd = connectUdp(cfg.host, cfg.port, err);
    if (fd < 0) {
        postStatus("Audio: connect failed — " + err);
        running_.store(false);
        return;
    }

    int derr = 0;
    OpusDecoder* dec = opus_decoder_create(cfg.sampleRate, cfg.channels, &derr);
    if (dec == nullptr || derr != OPUS_OK) {
        postStatus("Audio: opus decoder init failed: " + std::string(opus_strerror(derr)));
        ::close(fd);
        running_.store(false);
        return;
    }

    snd_pcm_t*           pcm = nullptr;
    bool                 playbackReported = false;
    std::vector<int16_t> pcmBuf(static_cast<std::size_t>(kMaxFrame) * cfg.channels);
    std::uint8_t         rx[kMaxPacket];

    using clock = std::chrono::steady_clock;
    auto nextKeepalive = clock::now();  // send one immediately to subscribe
    auto nextStats     = clock::now() + std::chrono::seconds(1);
    unsigned long framesDecoded = 0;

    postStatus("Audio: streaming from " + cfg.host + ":" + std::to_string(cfg.port) + ".");

    while (running_.load()) {
        const auto now = clock::now();
        if (now >= nextStats) {
            postStats(framesDecoded);  // periodic live counter, even while idle
            nextStats = now + std::chrono::seconds(1);
        }
        if (now >= nextKeepalive) {
            const std::uint8_t ka = 0;
            [[maybe_unused]] ssize_t s = ::send(fd, &ka, 1, 0);  // any datagram (re)subscribes
            nextKeepalive = now + std::chrono::milliseconds(kKeepaliveMs);
        }
        const auto until = std::chrono::duration_cast<std::chrono::milliseconds>(
                               std::min(nextKeepalive, nextStats) - now).count();
        const int timeoutMs = until > 0 ? static_cast<int>(until) : 0;

        pollfd fds[2] = {{fd, POLLIN, 0}, {wake_[0], POLLIN, 0}};
        const int rc = ::poll(fds, 2, timeoutMs);
        if (rc < 0) {
            if (errno == EINTR)
                continue;
            break;
        }
        if (fds[1].revents & POLLIN)
            break;  // stop() signalled
        if (!(fds[0].revents & POLLIN))
            continue;  // keepalive timeout, no audio yet

        // Lazily (re)open playback so a missing device never blocks startup and a
        // device that disappears mid-stream is retried.
        if (pcm == nullptr) {
            pcm = openPlayback(cfg);
            if (pcm == nullptr) {
                if (!playbackReported) {
                    postStatus("Audio: playback device " + cfg.device + " unavailable.");
                    playbackReported = true;
                }
                // Drain and discard so the socket buffer does not grow unbounded.
                while (::recv(fd, rx, sizeof(rx), MSG_DONTWAIT) > 0) {
                }
                continue;
            }
            postStatus("Audio: playing to " + cfg.device + ".");
            playbackReported = false;
        }

        // Drain every queued datagram, decoding and playing each.
        for (;;) {
            const ssize_t n = ::recv(fd, rx, sizeof(rx), MSG_DONTWAIT);
            if (n < 0)
                break;  // EAGAIN: queue drained
            if (n <= static_cast<ssize_t>(kHeaderBytes))
                continue;  // empty / header-only datagram
            const int frames = opus_decode(dec, rx + kHeaderBytes,
                                           static_cast<opus_int32>(n - kHeaderBytes),
                                           pcmBuf.data(), kMaxFrame, 0);
            if (frames < 0)
                continue;  // corrupt packet
            ++framesDecoded;
            const snd_pcm_sframes_t w = snd_pcm_writei(pcm, pcmBuf.data(), frames);
            if (w == -EPIPE) {
                snd_pcm_prepare(pcm);  // underrun: recover and keep going
            } else if (w < 0) {
                snd_pcm_close(pcm);
                pcm = nullptr;  // reopen on the next datagram
                break;
            }
        }
    }

    if (pcm != nullptr)
        snd_pcm_close(pcm);
    opus_decoder_destroy(dec);
    ::close(fd);
}

void AudioStreamClient::postStatus(const std::string& s) {
    ui_.post([this, w = std::weak_ptr<bool>(alive_), s]() {
        if (!w.expired() && onStatus) onStatus(s);
    });
}

void AudioStreamClient::postStats(unsigned long frames) {
    ui_.post([this, w = std::weak_ptr<bool>(alive_), frames]() {
        if (!w.expired() && onStats) onStats(frames);
    });
}
