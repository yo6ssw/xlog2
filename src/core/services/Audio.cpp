// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2025 Adrian Scripcă (YO6SSW)

#include "Audio.h"

#include <netdb.h>
#include <opus/opus.h>
#include <pipewire/pipewire.h>
#include <poll.h>
#include <spa/param/audio/format-utils.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <functional>
#include <mutex>
#include <vector>

namespace {

constexpr std::size_t kHeaderBytes = 4;   // big-endian sequence number
constexpr std::size_t kMaxPacket = 1500;  // matches the server's datagram cap
constexpr int kMaxFrame = 5760;     // 120 ms at 48 kHz, the largest opus frame
constexpr int kKeepaliveMs = 2000;  // resubscribe well within cwsd's timeout
constexpr int kMaxConceal = 10;     // cap concealed frames per gap; a larger
                                    // jump is a restart/long dropout, not loss

// Drift-compensating playout buffer. The rig's capture clock and the local DAC
// clock are independent crystals: even with zero packet loss they drift, so a
// fixed cushion slowly drains (consumer faster -> underrun) or fills (producer
// faster -> overrun), each an audible click. We steer the ring fill toward a
// target every tick: top up with Opus PLC when it runs low (also covers jitter
// gaps before they underrun), and drop a frame when a smoothed measure runs
// high.
constexpr int kTargetMs = 150;  // cushion to steer the ring fill toward
constexpr int kLowMs = 80;      // below this -> expand (insert PLC) to target
constexpr int kHighMs = 230;    // smoothed fill above this -> compress (drop)
constexpr int kTickMs = 10;     // max poll wait while playing, so the buffer
                                // is serviced even when no packet arrives
constexpr int kMaxInsertPerTick = 8;  // bound PLC top-up per tick (~160 ms cap)
constexpr double kFillEmaAlpha =
    0.01;  // ~2 s smoothing for the compress decision,
           // so transient jitter bursts are not trimmed

std::uint32_t readBe32(const std::uint8_t* p) {
  return (static_cast<std::uint32_t>(p[0]) << 24) |
         (static_cast<std::uint32_t>(p[1]) << 16) |
         (static_cast<std::uint32_t>(p[2]) << 8) |
         static_cast<std::uint32_t>(p[3]);
}

// Resolve + open a connected UDP socket to the server. connect() pins the peer
// so recv() only yields its datagrams and send() needs no address (keepalives).
int connectUdp(const std::string& host, int port, std::string& err) {
  addrinfo hints{};
  hints.ai_family = AF_UNSPEC;
  hints.ai_socktype = SOCK_DGRAM;
  addrinfo* res = nullptr;
  const int rc =
      ::getaddrinfo(host.c_str(), std::to_string(port).c_str(), &hints, &res);
  if (rc != 0 || !res) {
    err = ::gai_strerror(rc);
    return -1;
  }
  int fd = -1;
  for (addrinfo* ai = res; ai; ai = ai->ai_next) {
    fd = ::socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
    if (fd < 0) continue;
    if (::connect(fd, ai->ai_addr, ai->ai_addrlen) == 0) break;
    ::close(fd);
    fd = -1;
  }
  ::freeaddrinfo(res);
  if (fd < 0) err = std::strerror(errno);
  return fd;
}

// PipeWire playback backend. A native pw_stream fed from a lock-free SPSC ring:
// the decode worker is the single producer, PipeWire's realtime process()
// callback the single consumer. The ring decouples the (bursty, network-paced)
// producer from the (steady, device-clock-paced) consumer; the worker steers
// its fill for drift.
struct PwBackend {
  pw_thread_loop* loop = nullptr;
  pw_stream* stream = nullptr;
  int channels = 1;

  // Interleaved int16 samples; capacity is a power of two so index masking
  // wraps. head/tail are free-running 32-bit counters (their unsigned
  // difference is the occupancy); the producer owns head, the consumer owns
  // tail.
  std::vector<std::int16_t> ring;
  std::uint32_t mask = 0;
  std::atomic<std::uint32_t> head{0};
  std::atomic<std::uint32_t> tail{0};

  // Status sink (set by the worker) so stream errors reach the UI. Called from
  // the PipeWire loop thread, never the RT process callback.
  std::function<void(const std::string&)> status;

  bool init(int rate, int ch, const std::string& target, std::string& err);
  void shutdown();

  // Producer (worker thread).
  std::uint32_t pushSamples(const std::int16_t* src, std::uint32_t n) {
    const std::uint32_t cap = mask + 1;
    const std::uint32_t h = head.load(std::memory_order_relaxed);
    const std::uint32_t t = tail.load(std::memory_order_acquire);
    std::uint32_t toWrite = std::min(n, cap - (h - t));
    for (std::uint32_t i = 0; i < toWrite; ++i) ring[(h + i) & mask] = src[i];
    head.store(h + toWrite, std::memory_order_release);
    return toWrite;  // < n means the ring was full (samples dropped)
  }
  long occupancyFrames() const {
    const std::uint32_t h = head.load(std::memory_order_relaxed);
    const std::uint32_t t = tail.load(std::memory_order_acquire);
    return static_cast<long>((h - t) / static_cast<std::uint32_t>(channels));
  }

  // Consumer (RT process callback) — lock-free, no allocation.
  std::uint32_t popSamples(std::int16_t* dst, std::uint32_t n) {
    const std::uint32_t h = head.load(std::memory_order_acquire);
    const std::uint32_t t = tail.load(std::memory_order_relaxed);
    std::uint32_t toRead = std::min(n, h - t);
    for (std::uint32_t i = 0; i < toRead; ++i) dst[i] = ring[(t + i) & mask];
    tail.store(t + toRead, std::memory_order_release);
    return toRead;
  }

  static void onProcess(void* data);
  static void onStateChanged(void* data, enum pw_stream_state old,
                             enum pw_stream_state state, const char* error);
};

void PwBackend::onProcess(void* data) {
  auto* be = static_cast<PwBackend*>(data);
  pw_buffer* pb = pw_stream_dequeue_buffer(be->stream);
  if (pb == nullptr) return;
  spa_buffer* buf = pb->buffer;
  auto* dst = static_cast<std::int16_t*>(buf->datas[0].data);
  if (dst == nullptr) {
    pw_stream_queue_buffer(be->stream, pb);
    return;
  }
  const std::uint32_t stride =
      sizeof(std::int16_t) * static_cast<std::uint32_t>(be->channels);
  const std::uint32_t maxFrames = buf->datas[0].maxsize / stride;
  std::uint32_t reqFrames =
      pb->requested ? static_cast<std::uint32_t>(pb->requested) : maxFrames;
  if (reqFrames > maxFrames) reqFrames = maxFrames;

  const std::uint32_t want =
      reqFrames * static_cast<std::uint32_t>(be->channels);
  const std::uint32_t got = be->popSamples(dst, want);
  if (got < want)  // underflow: the drift compensator should make this rare
    std::memset(dst + got, 0, (want - got) * sizeof(std::int16_t));

  buf->datas[0].chunk->offset = 0;
  buf->datas[0].chunk->stride = static_cast<std::int32_t>(stride);
  buf->datas[0].chunk->size = reqFrames * stride;
  pw_stream_queue_buffer(be->stream, pb);
}

void PwBackend::onStateChanged(void* data, enum pw_stream_state /*old*/,
                               enum pw_stream_state state, const char* error) {
  auto* be = static_cast<PwBackend*>(data);
  if (state == PW_STREAM_STATE_ERROR && be->status)
    be->status(std::string("Audio: PipeWire stream error: ") +
               (error ? error : "unknown"));
}

bool PwBackend::init(int rate, int ch, const std::string& target,
                     std::string& err) {
  // Value-initialized then assigned (rather than a designated initializer) so
  // the unused callbacks are zeroed without tripping
  // -Wmissing-field-initializers.
  static const pw_stream_events streamEvents = [] {
    pw_stream_events e{};
    e.version = PW_VERSION_STREAM_EVENTS;
    e.state_changed = PwBackend::onStateChanged;
    e.process = PwBackend::onProcess;
    return e;
  }();

  static std::once_flag pwInitOnce;
  std::call_once(pwInitOnce, [] { pw_init(nullptr, nullptr); });

  channels = ch;
  std::uint32_t cap = 1024;
  while (cap <
         static_cast<std::uint32_t>(rate) * static_cast<std::uint32_t>(ch))
    cap <<= 1;  // >= ~1 s of audio, power of two
  ring.assign(cap, 0);
  mask = cap - 1;
  head.store(0);
  tail.store(0);

  // Pre-fill the cushion before the loop runs so the first process() callbacks
  // play silence rather than underrunning while the first packets arrive.
  std::vector<std::int16_t> prime(
      static_cast<std::size_t>(rate) * kTargetMs / 1000 * ch, 0);
  pushSamples(prime.data(), static_cast<std::uint32_t>(prime.size()));

  loop = pw_thread_loop_new("xlog2-audio", nullptr);
  if (loop == nullptr) {
    err = "pw_thread_loop_new failed";
    return false;
  }

  pw_properties* props = pw_properties_new(
      PW_KEY_MEDIA_TYPE, "Audio", PW_KEY_MEDIA_CATEGORY, "Playback",
      PW_KEY_MEDIA_ROLE, "Music",  // monitoring, not comms: avoid ducking
      PW_KEY_APP_NAME, "xlog2", PW_KEY_NODE_NAME, "xlog2-rig-audio",
      PW_KEY_NODE_DESCRIPTION, "xlog2 rig audio", nullptr);
  if (!target.empty() && target != "default")
    pw_properties_set(props, PW_KEY_TARGET_OBJECT, target.c_str());

  stream = pw_stream_new_simple(pw_thread_loop_get_loop(loop),
                                "xlog2-rig-audio", props, &streamEvents, this);
  if (stream == nullptr) {
    err = "pw_stream_new_simple failed";
    pw_thread_loop_destroy(loop);
    loop = nullptr;
    return false;
  }

  std::uint8_t podBuf[1024];
  spa_pod_builder b = SPA_POD_BUILDER_INIT(podBuf, sizeof(podBuf));
  spa_audio_info_raw info{};
  info.format = SPA_AUDIO_FORMAT_S16;
  info.channels = static_cast<std::uint32_t>(ch);
  info.rate = static_cast<std::uint32_t>(rate);
  if (ch == 1) {
    info.position[0] = SPA_AUDIO_CHANNEL_MONO;
  } else {
    info.position[0] = SPA_AUDIO_CHANNEL_FL;
    info.position[1] = SPA_AUDIO_CHANNEL_FR;
  }
  const spa_pod* params[1];
  params[0] = spa_format_audio_raw_build(&b, SPA_PARAM_EnumFormat, &info);

  const auto flags = static_cast<pw_stream_flags>(PW_STREAM_FLAG_AUTOCONNECT |
                                                  PW_STREAM_FLAG_MAP_BUFFERS |
                                                  PW_STREAM_FLAG_RT_PROCESS);
  if (pw_stream_connect(stream, PW_DIRECTION_OUTPUT, PW_ID_ANY, flags, params,
                        1) < 0) {
    err = "pw_stream_connect failed";
    shutdown();
    return false;
  }
  if (pw_thread_loop_start(loop) < 0) {
    err = "pw_thread_loop_start failed";
    shutdown();
    return false;
  }
  return true;
}

void PwBackend::shutdown() {
  if (loop != nullptr)
    pw_thread_loop_stop(loop);  // quiesce the RT thread before tearing down
  if (stream != nullptr) {
    pw_stream_destroy(stream);
    stream = nullptr;
  }
  if (loop != nullptr) {
    pw_thread_loop_destroy(loop);
    loop = nullptr;
  }
}

}  // namespace

AudioStreamClient::~AudioStreamClient() {
  onStatus = nullptr;  // the owning view may already be gone at shutdown
  stop();              // joins the worker, so onPcm is no longer called
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
  muted_.store(false);  // a fresh stream starts audible
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
  if (wasRunning) postStatus("Audio: stopped.");
}

void AudioStreamClient::worker(AudioStreamConfig cfg) {
  postStatus("Audio: connecting to " + cfg.host + ":" +
             std::to_string(cfg.port) + "…");

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
    postStatus("Audio: opus decoder init failed: " +
               std::string(opus_strerror(derr)));
    ::close(fd);
    running_.store(false);
    return;
  }

  PwBackend pw;
  pw.status = [this](const std::string& s) { postStatus(s); };
  std::string perr;
  if (!pw.init(cfg.sampleRate, cfg.channels, cfg.device, perr)) {
    postStatus("Audio: PipeWire init failed: " + perr);
    opus_decoder_destroy(dec);
    ::close(fd);
    running_.store(false);
    return;
  }

  std::vector<int16_t> pcmBuf(static_cast<std::size_t>(kMaxFrame) *
                              cfg.channels);
  std::uint8_t rx[kMaxPacket];

  using clock = std::chrono::steady_clock;
  auto nextKeepalive = clock::now();  // send one immediately to subscribe
  auto nextStats = clock::now() + std::chrono::seconds(1);
  unsigned long framesDecoded = 0;

  // Loss recovery: the server stamps each datagram with a monotonic sequence
  // and emits Opus in-band FEC. A gap means lost packets — recover the one just
  // before the new packet from its embedded FEC copy, and conceal any earlier
  // losses with Opus PLC, so element timing stays intact instead of skipping.
  bool haveSeq = false;
  std::uint32_t expectedSeq = 0;
  const int defaultFrame = std::max(1, cfg.sampleRate / 50);  // 20 ms fallback

  // Drift-compensation state. Watermarks are in frames (= samples/channel, the
  // unit the ring reports). avgBuffered is a slow EMA so the compress (drop)
  // decision tracks clock drift, not transient jitter bursts.
  const int targetFrames = cfg.sampleRate * kTargetMs / 1000;
  const int lowFrames = cfg.sampleRate * kLowMs / 1000;
  const int highFrames = cfg.sampleRate * kHighMs / 1000;
  int lastFrameSamples = defaultFrame;  // PLC frame size; tracks the stream
  double avgBuffered = -1.0;

  // Send one decoded/concealed frame to the skimmer tap and the playout ring.
  // The skimmer tap always gets the real audio (even while muted, so the
  // decoder keeps copying the band during transmit); muting only silences the
  // *output* device, zeroing the buffer after the tap has seen it.
  auto play = [&](int frames) {
    if (frames <= 0) return;
    if (onPcm) onPcm(pcmBuf.data(), frames, cfg.channels, cfg.sampleRate);
    if (muted_.load(std::memory_order_relaxed))
      std::fill_n(pcmBuf.data(),
                  static_cast<std::size_t>(frames) * cfg.channels, int16_t{0});
    pw.pushSamples(pcmBuf.data(),
                   static_cast<std::uint32_t>(frames) * cfg.channels);
  };

  // Expand: while the fill is below target, synthesize PLC frames to top it up.
  // Runs every tick (even with no packet), so a jitter gap or a drained ring is
  // refilled with extrapolated audio instead of underrunning into silence.
  auto topUp = [&]() {
    if (!haveSeq) return;  // need a warmed-up decoder before PLC is meaningful
    long b = pw.occupancyFrames();
    if (b >= lowFrames) return;
    for (int i = 0; i < kMaxInsertPerTick && b < targetFrames; ++i) {
      const int g =
          opus_decode(dec, nullptr, 0, pcmBuf.data(), lastFrameSamples, 0);
      if (g <= 0) break;
      play(g);
      b += g;
    }
  };

  postStatus("Audio: streaming from " + cfg.host + ":" +
             std::to_string(cfg.port) + ".");

  while (running_.load()) {
    const auto now = clock::now();
    if (now >= nextStats) {
      postStats(framesDecoded);  // periodic live counter, even while idle
      nextStats = now + std::chrono::seconds(1);
    }
    if (now >= nextKeepalive) {
      const std::uint8_t ka = 0;
      [[maybe_unused]] ssize_t s =
          ::send(fd, &ka, 1, 0);  // any datagram (re)subscribes
      nextKeepalive = now + std::chrono::milliseconds(kKeepaliveMs);
    }
    const auto until = std::chrono::duration_cast<std::chrono::milliseconds>(
                           std::min(nextKeepalive, nextStats) - now)
                           .count();
    int timeoutMs = until > 0 ? static_cast<int>(until) : 0;
    // Once audio is flowing, wake at least every kTickMs so the ring is
    // serviced (PLC top-up) during gaps between packets, not only on arrival.
    if (haveSeq && timeoutMs > kTickMs) timeoutMs = kTickMs;

    pollfd fds[2] = {{fd, POLLIN, 0}, {wake_[0], POLLIN, 0}};
    const int rc = ::poll(fds, 2, timeoutMs);
    if (rc < 0) {
      if (errno == EINTR) continue;
      break;
    }
    if (fds[1].revents & POLLIN) break;  // stop() signalled
    const bool haveData = (fds[0].revents & POLLIN);

    // Drain every queued datagram, decoding and playing each.
    if (haveData)
      for (;;) {
        const ssize_t n = ::recv(fd, rx, sizeof(rx), MSG_DONTWAIT);
        if (n < 0) break;  // EAGAIN: queue drained
        if (n <= static_cast<ssize_t>(kHeaderBytes))
          continue;  // empty / header-only datagram
        const std::uint32_t seq = readBe32(rx);
        const std::uint8_t* pkt = rx + kHeaderBytes;
        const auto len = static_cast<opus_int32>(n - kHeaderBytes);

        // Per-frame sample count of this packet; assume any lost frames matched
        // it (the server uses a constant frame_ms).
        int frameSamples = opus_decoder_get_nb_samples(dec, pkt, len);
        if (frameSamples <= 0) frameSamples = defaultFrame;

        // Conceal the gap between the last decoded packet and this one.
        if (haveSeq) {
          const std::int32_t diff =
              static_cast<std::int32_t>(seq - expectedSeq);
          if (diff < 0)
            continue;  // late/reordered or duplicate: already past it, drop
          int lost = diff;
          if (lost > kMaxConceal)
            lost = 0;  // restart/long dropout: resync without flooding PLC
          for (int i = 0; i < lost; ++i) {
            // The final missing frame (right before this packet) is recoverable
            // from this packet's in-band FEC; conceal earlier ones with PLC.
            const bool useFec = (i == lost - 1);
            const int g = useFec ? opus_decode(dec, pkt, len, pcmBuf.data(),
                                               frameSamples, 1)
                                 : opus_decode(dec, nullptr, 0, pcmBuf.data(),
                                               frameSamples, 0);
            play(g);
          }
        }

        const int frames =
            opus_decode(dec, pkt, len, pcmBuf.data(), kMaxFrame, 0);
        if (frames < 0)
          continue;  // corrupt packet (don't advance expectedSeq off a bad
                     // header)
        expectedSeq = seq + 1;
        haveSeq = true;
        lastFrameSamples = frameSamples;
        ++framesDecoded;

        // Compress: if the smoothed fill runs long (DAC slower than the rig's
        // capture clock, or latency accreted from jitter top-ups) drop this
        // frame to shed ~one frame of latency. The EMA keeps transient bursts
        // from triggering a drop; snapping the average to target after one
        // keeps drops isolated rather than clustered.
        const long b = pw.occupancyFrames();
        avgBuffered = avgBuffered < 0
                          ? b
                          : avgBuffered + (b - avgBuffered) * kFillEmaAlpha;
        if (avgBuffered > highFrames) {
          avgBuffered = targetFrames;
          continue;  // drop (decoder state already advanced)
        }
        play(frames);
      }

    // Maintain the cushion every tick, including when no packet arrived: expand
    // with PLC if the fill has run low (jitter gap or drained ring).
    topUp();
  }

  pw.shutdown();
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
