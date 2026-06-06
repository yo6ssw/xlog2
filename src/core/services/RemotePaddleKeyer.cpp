#include "RemotePaddleKeyer.h"

#include "RemoteKeyProtocol.h"

#include <alsa/asoundlib.h>

#include <netdb.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <deque>
#include <thread>
#include <vector>

namespace {

constexpr int kKeepaliveMs = 100;  // must stay well under cwsd's silence_ms (default 250)

// Resolve + open a connected UDP socket. connect() pins the peer so send() needs
// no address; sending is fire-and-forget (no recv path).
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

}  // namespace

RemotePaddleKeyer::~RemotePaddleKeyer() {
    onStatus = nullptr;  // the owning view may already be gone at shutdown
    stop();
}

void RemotePaddleKeyer::start(const RemotePaddleConfig& cfg) {
    stop();
    if (cfg.host.empty() || cfg.port <= 0 || cfg.port > 65535) {
        postStatus("Paddle keyer: invalid host/port.");
        return;
    }
    if (cfg.wpm <= 0) {
        postStatus("Paddle keyer: invalid speed.");
        return;
    }
    alive_ = std::make_shared<bool>(true);
    dit_.store(false);
    dah_.store(false);
    toneOn_.store(false);
    running_.store(true);
    thread_ = std::thread(&RemotePaddleKeyer::worker, this, cfg);
    if (cfg.sidetone)
        sidetoneThread_ = std::thread(&RemotePaddleKeyer::sidetoneWorker, this, cfg);
}

void RemotePaddleKeyer::stop() {
    const bool wasRunning = running_.exchange(false);
    if (thread_.joinable())
        thread_.join();
    if (sidetoneThread_.joinable())
        sidetoneThread_.join();
    if (wasRunning)
        postStatus("Paddle keyer: stopped.");
}

void RemotePaddleKeyer::worker(RemotePaddleConfig cfg) {
    postStatus("Paddle keyer: connecting to " + cfg.host + ":" + std::to_string(cfg.port) + "…");

    std::string err;
    const int fd = connectUdp(cfg.host, cfg.port, err);
    if (fd < 0) {
        postStatus("Paddle keyer: connect failed — " + err);
        running_.store(false);
        return;
    }

    using clock = std::chrono::steady_clock;
    const auto sessionStart = clock::now();
    // A per-session id so cwsd re-anchors its playout clock on (re)start.
    const auto startNs =
        std::chrono::duration_cast<std::chrono::nanoseconds>(sessionStart.time_since_epoch()).count();
    const std::uint16_t sessionId = static_cast<std::uint16_t>(startNs);

    // Element timing. A dit is 1200/wpm ms by the PARIS standard; a dah is 3 dits;
    // the inter-element gap is 1 dit.
    const std::uint64_t ditUs = 1200000ull / static_cast<std::uint64_t>(cfg.wpm);
    const std::uint64_t dahUs = 3 * ditUs;
    const std::uint64_t gapUs = ditUs;

    // Transmit hangs on after the last key-up so the audio mute bridges normal
    // character/word spacing instead of flapping per element (a PTT-hang analogue).
    const std::uint64_t hangUs = std::max<std::uint64_t>(400000ull, 10 * ditUs);
    bool          transmitting = false;
    std::uint64_t lastKeyUpUs  = 0;

    std::deque<remotekey::Edge> history;  // recent edges, capped at kMaxEdges
    auto nowUs = [&]() -> std::uint64_t {
        return static_cast<std::uint64_t>(
            std::chrono::duration_cast<std::chrono::microseconds>(clock::now() - sessionStart).count());
    };

    auto nextKeepalive = clock::now();  // first packet (a session-reset keepalive) goes out at once
    bool firstPacket = true;
    auto sendPacket = [&]() {
        const std::uint8_t flags = firstPacket ? remotekey::kFlagSessionReset : 0;
        firstPacket = false;
        const std::vector<remotekey::Edge> edges(history.begin(), history.end());
        const auto bytes = remotekey::encode(sessionId, flags, edges);
        [[maybe_unused]] ssize_t s = ::send(fd, bytes.data(), bytes.size(), 0);
        nextKeepalive = clock::now() + std::chrono::milliseconds(kKeepaliveMs);
    };
    auto emitEdge = [&](std::uint64_t ts, std::uint8_t state) {
        const bool down = (state & remotekey::kKeyDown) != 0;
        // Drive the local sidetone from the same transition — instant feel, no
        // network round-trip.
        toneOn_.store(down, std::memory_order_relaxed);
        if (down) {
            if (!transmitting) { transmitting = true; postTransmit(true); }
        } else {
            lastKeyUpUs = ts;  // start the hang countdown (see the loop below)
        }
        history.push_back({ts, state});
        while (history.size() > remotekey::kMaxEdges)
            history.pop_front();
        sendPacket();  // every edge ships immediately, carrying recent history
    };

    // Element-generator state. iambic-A "memory": while sending one element the
    // opposite paddle is sampled, and that element is queued to follow.
    enum class Phase { Idle, Mark, Gap };
    Phase phase = Phase::Idle;
    bool  curDah = false;          // element currently being sent
    bool  memDit = false, memDah = false;
    std::uint64_t phaseEndUs = 0;

    // `at` is the *scheduled* start instant, not the polled clock: from Idle it is
    // the moment the paddle close was detected; mid-train it is the previous gap's
    // exact end. Emitting on the schedule (rather than on `now`) keeps the whole
    // element train phase-locked to the first press, so dit/dah/gap durations are
    // exact regardless of poll granularity or a scheduler stall — only the initial
    // detection carries real latency.
    auto startMark = [&](std::uint64_t at, bool dah) {
        curDah = dah;
        memDit = memDah = false;
        emitEdge(at, remotekey::kKeyDown);
        phase = Phase::Mark;
        phaseEndUs = at + (dah ? dahUs : ditUs);
    };

    postStatus("Paddle keyer: ready — streaming to " + cfg.host + ":" +
               std::to_string(cfg.port) + " at " + std::to_string(cfg.wpm) + " wpm.");

    while (running_.load()) {
        const std::uint64_t now = nowUs();
        const bool d = dit_.load(std::memory_order_relaxed);
        const bool h = dah_.load(std::memory_order_relaxed);

        switch (phase) {
            case Phase::Idle:
                // Start the squeezed element; if both are down, lead with the dit.
                if (d)
                    startMark(now, false);
                else if (h)
                    startMark(now, true);
                break;

            case Phase::Mark:
                if (curDah && d) memDit = true;        // opposite-paddle memory
                if (!curDah && h) memDah = true;
                if (now >= phaseEndUs) {
                    const std::uint64_t markEnd = phaseEndUs;  // exact, not polled
                    emitEdge(markEnd, 0);              // key up
                    phase = Phase::Gap;
                    phaseEndUs = markEnd + gapUs;
                }
                break;

            case Phase::Gap:
                if (curDah && d) memDit = true;
                if (!curDah && h) memDah = true;
                if (now >= phaseEndUs) {
                    const std::uint64_t gapEnd = phaseEndUs;   // exact, not polled
                    bool haveNext = true, nextDah = false;
                    if (curDah) {
                        if (d || memDit)       nextDah = false;
                        else if (h)            nextDah = true;
                        else                   haveNext = false;
                    } else {
                        if (h || memDah)       nextDah = true;
                        else if (d)            nextDah = false;
                        else                   haveNext = false;
                    }
                    if (haveNext)
                        startMark(gapEnd, nextDah);   // next element phase-locked to the gap end
                    else
                        phase = Phase::Idle;
                }
                break;
        }

        // End the transmit window once the key has been up past the hang time.
        if (transmitting && !toneOn_.load(std::memory_order_relaxed) &&
            now - lastKeyUpUs > hangUs) {
            transmitting = false;
            postTransmit(false);
        }

        // Keepalive while idle (also re-sends recent history, so a lost key-up edge
        // is recovered until it ages out of the window — cwsd's max-key-down
        // watchdog is the final backstop).
        if (clock::now() >= nextKeepalive)
            sendPacket();

        // Poll fast so a fresh paddle close (and a brief iambic squeeze) is caught
        // with minimal latency. Element *timing* no longer depends on this interval
        // — edges are emitted on the schedule above — so this only bounds how
        // quickly we react to the operator, not how clean the keying is.
        std::this_thread::sleep_for(std::chrono::microseconds(250));
    }

    // Leave cwsd in a safe state: a final key-up if we stopped mid-element.
    if (phase == Phase::Mark)
        emitEdge(nowUs(), 0);
    // Drop the transmit/mute state on the way out so the audio stream is restored.
    if (transmitting)
        postTransmit(false);

    ::close(fd);
}

// Renders a click-free sidetone to ALSA, gated by toneOn_ (the key state). Runs
// independently of the keying worker: a missing device just means no sidetone,
// never a stall in keying. Opened lazily and retried so startup never blocks.
void RemotePaddleKeyer::sidetoneWorker(RemotePaddleConfig cfg) {
    constexpr unsigned kRate  = 48000;
    constexpr int      kBlock = kRate / 100;            // 10 ms render blocks

    const int    toneHz   = cfg.toneHz > 0 ? cfg.toneHz : 600;
    const int    levelPct = cfg.level < 0 ? 0 : (cfg.level > 100 ? 100 : cfg.level);
    const double amp      = (levelPct / 100.0) * 0.6 * 32767.0;  // headroom below full scale
    const double phaseInc = 2.0 * M_PI * toneHz / kRate;
    const double gainStep = 1.0 / (kRate * 0.005);      // 5 ms attack/decay envelope

    snd_pcm_t* pcm = nullptr;
    bool       reportedMissing = false;
    double     phase = 0.0;
    double     gain  = 0.0;                             // current envelope, ramped per sample
    std::vector<std::int16_t> block(kBlock);

    while (running_.load()) {
        if (pcm == nullptr) {
            if (snd_pcm_open(&pcm, cfg.device.c_str(), SND_PCM_STREAM_PLAYBACK, 0) < 0 ||
                snd_pcm_set_params(pcm, SND_PCM_FORMAT_S16_LE, SND_PCM_ACCESS_RW_INTERLEAVED,
                                   1, kRate, 1, 20000 /* ~20 ms latency target */) < 0) {
                if (pcm) { snd_pcm_close(pcm); pcm = nullptr; }
                if (!reportedMissing) {
                    postStatus("Paddle keyer: sidetone device " + cfg.device + " unavailable.");
                    reportedMissing = true;
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(200));
                continue;
            }
            reportedMissing = false;
        }

        for (int i = 0; i < kBlock; ++i) {
            const bool on = toneOn_.load(std::memory_order_relaxed);  // per-sample for low latency
            if (on && gain < 1.0)       gain = std::min(1.0, gain + gainStep);
            else if (!on && gain > 0.0) gain = std::max(0.0, gain - gainStep);
            block[i] = static_cast<std::int16_t>(gain > 0.0 ? std::sin(phase) * gain * amp : 0.0);
            phase += phaseInc;
            if (phase >= 2.0 * M_PI)
                phase -= 2.0 * M_PI;
        }

        const snd_pcm_sframes_t w = snd_pcm_writei(pcm, block.data(), kBlock);
        if (w == -EPIPE) {
            snd_pcm_prepare(pcm);          // underrun: recover and keep going
        } else if (w < 0) {
            snd_pcm_close(pcm);
            pcm = nullptr;                 // reopen on the next iteration
        }
    }

    if (pcm != nullptr) {
        snd_pcm_drop(pcm);
        snd_pcm_close(pcm);
    }
}

void RemotePaddleKeyer::postStatus(const std::string& s) {
    ui_.post([this, w = std::weak_ptr<bool>(alive_), s]() {
        if (!w.expired() && onStatus) onStatus(s);
    });
}

void RemotePaddleKeyer::postTransmit(bool on) {
    ui_.post([this, w = std::weak_ptr<bool>(alive_), on]() {
        if (!w.expired() && onTransmit) onTransmit(on);
    });
}
