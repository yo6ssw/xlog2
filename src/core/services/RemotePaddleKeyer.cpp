// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2025 Adrian Scripcă (YO6SSW)

#include "RemotePaddleKeyer.h"

#include "RemoteKeyProtocol.h"

#ifdef __ANDROID__
// Android has no PipeWire/rtkit: the sidetone uses AAudio (NDK low-latency
// audio) and the realtime bump falls back to a plain SCHED_FIFO attempt (see
// below).
#include <aaudio/AAudio.h>
#else
#include <pipewire/pipewire.h>
#include <pipewire/version.h>  // PW_CHECK_VERSION
#include <spa/param/audio/format-utils.h>

#include "Rtkit.h"
#endif

#include <netdb.h>
#include <pthread.h>
#include <sched.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <deque>
#include <mutex>
#include <thread>
#include <vector>

namespace {

constexpr int kKeepaliveMs =
    100;  // must stay well under cwsd's silence_ms (default 250)

// Raise the calling thread to real-time (SCHED_FIFO) so its tight poll cadence
// is honored even under system load — this bounds the worst-case latency
// between a paddle close and the first key edge (element *timing* is already
// schedule-locked, so this only affects initial reaction). Best-effort: without
// CAP_SYS_NICE / RLIMIT_RTPRIO it stays at normal priority and keying still
// works, just with more wakeup jitter. A modest priority preempts ordinary
// tasks while staying below the audio/kernel critical RT threads. Returns true
// if the bump was applied.
//
// Two paths: first a direct SCHED_FIFO request (works when the process has an
// rtprio limit, e.g. via limits.conf or CAP_SYS_NICE); if that is denied — the
// common desktop case — fall back to the rtkit D-Bus broker, which grants RT
// per-thread without any limits.conf change (the same route PipeWire takes).
bool raiseToRealtime() {
  sched_param sp{};
  int prio = sched_get_priority_min(SCHED_FIFO) + 5;
  const int maxPrio = sched_get_priority_max(SCHED_FIFO);
  if (prio > maxPrio) prio = maxPrio;
  sp.sched_priority = prio;
  if (pthread_setschedparam(pthread_self(), SCHED_FIFO, &sp) == 0) return true;
#ifdef __ANDROID__
  return false;  // no rtkit on Android; element timing is schedule-locked
                 // anyway
#else
  return platform::makeThreadRealtime(
      prio);  // desktop fallback via RealtimeKit
#endif
}

// Resolve + open a connected UDP socket. connect() pins the peer so send()
// needs no address; sending is fire-and-forget (no recv path).
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

// Native-PipeWire sidetone generator. A click-free (ramped-envelope) sine is
// synthesized directly in the realtime process() callback, gated by an external
// key-state atomic — there is no input ring, the callback IS the source.
//
// Crucially it requests a *graph-friendly* quantum via PW_KEY_NODE_LATENCY
// rather than the minimum: the old ALSA "default" path opened a 6 ms buffer
// through the pipewire-alsa shim, which dragged the whole graph to its smallest
// quantum and chronically xran, glitching every other stream on the sink
// (notably the rig-audio stream). A native node at ~10 ms keeps the feel tight
// without destabilizing the graph.
#ifndef __ANDROID__
struct PwSidetone {
  static constexpr unsigned kRate = 48000;

  pw_thread_loop* loop = nullptr;
  pw_stream* stream = nullptr;
  std::atomic<bool>* toneOn = nullptr;  // key state, owned by the keyer

  double phase = 0.0;  // oscillator phase, carried across callbacks
  double gain = 0.0;   // current envelope (0..1), ramped per sample
  double phaseInc = 0.0;
  double gainStep = 0.0;
  double amp = 0.0;

  bool init(int toneHz, int levelPct, int latencyFrames,
            const std::string& target, std::atomic<bool>* tone,
            std::string& err);
  void shutdown();

  static void onProcess(void* data);
};

void PwSidetone::onProcess(void* data) {
  auto* st = static_cast<PwSidetone*>(data);
  pw_buffer* pb = pw_stream_dequeue_buffer(st->stream);
  if (pb == nullptr) return;
  spa_buffer* buf = pb->buffer;
  auto* dst = static_cast<std::int16_t*>(buf->datas[0].data);
  if (dst == nullptr) {
    pw_stream_queue_buffer(st->stream, pb);
    return;
  }
  const std::uint32_t stride = sizeof(std::int16_t);  // mono
  const std::uint32_t maxFrames = buf->datas[0].maxsize / stride;
  std::uint32_t n =
#if PW_CHECK_VERSION(0, 3, 49)
      pb->requested ? static_cast<std::uint32_t>(pb->requested) : maxFrames;
#else
      maxFrames;  // pw_buffer::requested added in PipeWire 0.3.49 (not on 22.04)
#endif
  if (n > maxFrames) n = maxFrames;

  for (std::uint32_t i = 0; i < n; ++i) {
    const bool on = st->toneOn->load(
        std::memory_order_relaxed);  // per-sample for low latency
    if (on && st->gain < 1.0)
      st->gain = std::min(1.0, st->gain + st->gainStep);
    else if (!on && st->gain > 0.0)
      st->gain = std::max(0.0, st->gain - st->gainStep);
    dst[i] = static_cast<std::int16_t>(
        st->gain > 0.0 ? std::sin(st->phase) * st->gain * st->amp : 0.0);
    st->phase += st->phaseInc;
    if (st->phase >= 2.0 * M_PI) st->phase -= 2.0 * M_PI;
  }

  buf->datas[0].chunk->offset = 0;
  buf->datas[0].chunk->stride = static_cast<std::int32_t>(stride);
  buf->datas[0].chunk->size = n * stride;
  pw_stream_queue_buffer(st->stream, pb);
}

bool PwSidetone::init(int toneHz, int levelPct, int latencyFrames,
                      const std::string& target, std::atomic<bool>* tone,
                      std::string& err) {
  static std::once_flag pwInitOnce;
  std::call_once(pwInitOnce, [] { pw_init(nullptr, nullptr); });

  toneOn = tone;
  phase = 0.0;
  gain = 0.0;
  phaseInc = 2.0 * M_PI * toneHz / kRate;
  gainStep = 1.0 / (kRate * 0.005);          // 5 ms attack/decay envelope
  amp = (levelPct / 100.0) * 0.6 * 32767.0;  // headroom below full scale

  // Value-initialized then assigned (rather than a designated initializer) so
  // the unused callbacks are zeroed without tripping
  // -Wmissing-field-initializers.
  static const pw_stream_events streamEvents = [] {
    pw_stream_events e{};
    e.version = PW_VERSION_STREAM_EVENTS;
    e.process = PwSidetone::onProcess;
    return e;
  }();

  loop = pw_thread_loop_new("xlog2-sidetone", nullptr);
  if (loop == nullptr) {
    err = "pw_thread_loop_new failed";
    return false;
  }

  // Requested node quantum. Default (512 ≈ 10.7 ms) is graph-friendly; the
  // standalone tool lowers it for tighter feel since it owns the graph alone.
  if (latencyFrames < 16) latencyFrames = 16;
  const std::string latency =
      std::to_string(latencyFrames) + "/" + std::to_string(kRate);
  pw_properties* props = pw_properties_new(
      PW_KEY_MEDIA_TYPE, "Audio", PW_KEY_MEDIA_CATEGORY, "Playback",
      PW_KEY_MEDIA_ROLE, "Production", PW_KEY_APP_NAME, "xlog2",
      PW_KEY_NODE_NAME, "xlog2-sidetone", PW_KEY_NODE_DESCRIPTION,
      "xlog2 CW sidetone", PW_KEY_NODE_LATENCY, latency.c_str(), nullptr);
  if (!target.empty() && target != "default")
    pw_properties_set(props, PW_KEY_TARGET_OBJECT, target.c_str());

  stream = pw_stream_new_simple(pw_thread_loop_get_loop(loop), "xlog2-sidetone",
                                props, &streamEvents, this);
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
  info.channels = 1;
  info.rate = kRate;
  info.position[0] = SPA_AUDIO_CHANNEL_MONO;
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

void PwSidetone::shutdown() {
  if (loop != nullptr) pw_thread_loop_stop(loop);
  if (stream != nullptr) {
    pw_stream_destroy(stream);
    stream = nullptr;
  }
  if (loop != nullptr) {
    pw_thread_loop_destroy(loop);
    loop = nullptr;
  }
}
using Sidetone = PwSidetone;

#else  // __ANDROID__

// AAudio sidetone: the Android analogue of PwSidetone. A low-latency output
// stream whose data callback synthesises the same click-free (ramped-envelope)
// sine gated by the key-state atomic. Same public shape (init/shutdown) so
// sidetoneWorker() is platform-agnostic.
struct AaSidetone {
  AAudioStream* stream = nullptr;
  std::atomic<bool>* toneOn = nullptr;  // key state, owned by the keyer
  int rate = 48000;
  int channels = 1;
  double phase = 0.0;
  double gain = 0.0;
  double phaseInc = 0.0;
  double gainStep = 0.0;
  double amp = 0.0;

  static aaudio_data_callback_result_t onData(AAudioStream*, void* userData,
                                              void* audioData,
                                              std::int32_t numFrames) {
    auto* st = static_cast<AaSidetone*>(userData);
    auto* dst = static_cast<std::int16_t*>(audioData);
    for (std::int32_t f = 0; f < numFrames; ++f) {
      const bool on = st->toneOn->load(
          std::memory_order_relaxed);  // per-frame for low latency
      if (on && st->gain < 1.0)
        st->gain = std::min(1.0, st->gain + st->gainStep);
      else if (!on && st->gain > 0.0)
        st->gain = std::max(0.0, st->gain - st->gainStep);
      const auto s = static_cast<std::int16_t>(
          st->gain > 0.0 ? std::sin(st->phase) * st->gain * st->amp : 0.0);
      for (int c = 0; c < st->channels; ++c) dst[f * st->channels + c] = s;
      st->phase += st->phaseInc;
      if (st->phase >= 2.0 * M_PI) st->phase -= 2.0 * M_PI;
    }
    return AAUDIO_CALLBACK_RESULT_CONTINUE;
  }

  bool init(int toneHz, int levelPct, int /*latencyFrames*/,
            const std::string& /*target*/, std::atomic<bool>* tone,
            std::string& err) {
    toneOn = tone;
    phase = 0.0;
    gain = 0.0;
    amp = (levelPct / 100.0) * 0.6 * 32767.0;

    AAudioStreamBuilder* b = nullptr;
    if (AAudio_createStreamBuilder(&b) != AAUDIO_OK) {
      err = "AAudio_createStreamBuilder failed";
      return false;
    }
    AAudioStreamBuilder_setFormat(b, AAUDIO_FORMAT_PCM_I16);
    AAudioStreamBuilder_setChannelCount(b, 1);
    AAudioStreamBuilder_setSampleRate(b, rate);
    AAudioStreamBuilder_setPerformanceMode(b,
                                           AAUDIO_PERFORMANCE_MODE_LOW_LATENCY);
    AAudioStreamBuilder_setDataCallback(b, onData, this);
    const aaudio_result_t r = AAudioStreamBuilder_openStream(b, &stream);
    AAudioStreamBuilder_delete(b);
    if (r != AAUDIO_OK || stream == nullptr) {
      err = AAudio_convertResultToText(r);
      stream = nullptr;
      return false;
    }
    // The device may open at a different rate/channel count than requested;
    // read the actuals so the tone frequency and envelope stay correct.
    rate = AAudioStream_getSampleRate(stream);
    channels = AAudioStream_getChannelCount(stream);
    if (rate <= 0) rate = 48000;
    if (channels < 1) channels = 1;
    phaseInc = 2.0 * M_PI * toneHz / rate;
    gainStep = 1.0 / (rate * 0.005);  // 5 ms attack/decay envelope

    const aaudio_result_t rs = AAudioStream_requestStart(stream);
    if (rs != AAUDIO_OK) {
      err = AAudio_convertResultToText(rs);
      AAudioStream_close(stream);
      stream = nullptr;
      return false;
    }
    return true;
  }

  void shutdown() {
    if (stream != nullptr) {
      AAudioStream_requestStop(stream);
      AAudioStream_close(stream);
      stream = nullptr;
    }
  }
};
using Sidetone = AaSidetone;

#endif  // __ANDROID__

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
    sidetoneThread_ =
        std::thread(&RemotePaddleKeyer::sidetoneWorker, this, cfg);
}

void RemotePaddleKeyer::stop() {
  const bool wasRunning = running_.exchange(false);
  if (thread_.joinable()) thread_.join();
  if (sidetoneThread_.joinable()) sidetoneThread_.join();
  if (wasRunning) postStatus("Paddle keyer: stopped.");
}

void RemotePaddleKeyer::worker(RemotePaddleConfig cfg) {
  const bool realtime =
      raiseToRealtime();  // best-effort; keeps the poll cadence reliable

  // Local-only mode (the xlog2-paddle practice tool) runs the element generator
  // and sidetone but opens no socket and sends nothing — no remote keying.
  int fd = -1;
  if (!cfg.localOnly) {
    postStatus("Paddle keyer: connecting to " + cfg.host + ":" +
               std::to_string(cfg.port) + "…");
    std::string err;
    fd = connectUdp(cfg.host, cfg.port, err);
    if (fd < 0) {
      postStatus("Paddle keyer: connect failed — " + err);
      running_.store(false);
      return;
    }
  }

  using clock = std::chrono::steady_clock;
  const auto sessionStart = clock::now();
  // A per-session id so cwsd re-anchors its playout clock on (re)start.
  const auto startNs = std::chrono::duration_cast<std::chrono::nanoseconds>(
                           sessionStart.time_since_epoch())
                           .count();
  const std::uint16_t sessionId = static_cast<std::uint16_t>(startNs);

  // Element timing. A dit is 1200/wpm ms by the PARIS standard; a dah is 3
  // dits; the inter-element gap is 1 dit.
  const std::uint64_t ditUs = 1200000ull / static_cast<std::uint64_t>(cfg.wpm);
  const std::uint64_t dahUs = 3 * ditUs;
  const std::uint64_t gapUs = ditUs;

  // Transmit hangs on after the last key-up so the audio mute bridges normal
  // character/word spacing instead of flapping per element (a PTT-hang
  // analogue). The tail length is operator-configurable; 0 releases the mute at
  // key-up.
  const std::uint64_t hangUs =
      static_cast<std::uint64_t>(std::max(0, cfg.muteTailMs)) * 1000ull;
  bool transmitting = false;
  std::uint64_t lastKeyUpUs = 0;

  std::deque<remotekey::Edge> history;  // recent edges, capped at kMaxEdges
  auto nowUs = [&]() -> std::uint64_t {
    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::microseconds>(clock::now() -
                                                              sessionStart)
            .count());
  };

  auto nextKeepalive = clock::now();  // first packet (a session-reset
                                      // keepalive) goes out at once
  bool firstPacket = true;
  auto sendPacket = [&]() {
    if (cfg.localOnly) {  // no networking: just hold off the keepalive timer
      nextKeepalive = clock::now() + std::chrono::milliseconds(kKeepaliveMs);
      return;
    }
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
      if (!transmitting) {
        transmitting = true;
        postTransmit(true);
      }
    } else {
      lastKeyUpUs = ts;  // start the hang countdown (see the loop below)
    }
    history.push_back({ts, state});
    while (history.size() > remotekey::kMaxEdges) history.pop_front();
    sendPacket();  // every edge ships immediately, carrying recent history
  };

  // Element-generator state. iambic-A "memory": while sending one element the
  // opposite paddle is sampled, and that element is queued to follow. The Wait
  // phase is the autospace dwell: a new character's first element is latched
  // and held until the 3-dit inter-character boundary (see the Idle case
  // below).
  enum class Phase { Idle, Mark, Gap, Wait };
  Phase phase = Phase::Idle;
  bool curDah = false;  // element currently being sent
  bool memDit = false, memDah = false;
  bool pendDah = false;  // element latched during an autospace Wait
  std::uint64_t phaseEndUs = 0;

  // Ultimatic "last-pressed memory": tracked from paddle press *edges* so that
  // when both paddles are held the most-recently-pressed element wins (see the
  // Idle/Gap decisions below). prevD/prevH hold the previous poll's contact
  // state for edge detection; lastDah is the side of the latest press.
  bool prevD = false, prevH = false;
  bool lastDah = false;

  // `at` is the *scheduled* start instant, not the polled clock: from Idle it
  // is the moment the paddle close was detected; mid-train it is the previous
  // gap's exact end. Emitting on the schedule (rather than on `now`) keeps the
  // whole element train phase-locked to the first press, so dit/dah/gap
  // durations are exact regardless of poll granularity or a scheduler stall —
  // only the initial detection carries real latency.
  auto startMark = [&](std::uint64_t at, bool dah) {
    curDah = dah;
    memDit = memDah = false;
    emitEdge(at, remotekey::kKeyDown);
    phase = Phase::Mark;
    phaseEndUs = at + (dah ? dahUs : ditUs);
  };

  // Note the scheduling outcome so the operator can see whether the latency-
  // reducing real-time bump actually took effect (it needs rtprio privileges).
  const std::string prio =
      realtime ? "" : " (normal priority — grant rtprio for lower latency)";
  if (cfg.localOnly)
    postStatus("Paddle keyer: ready — local sidetone at " +
               std::to_string(cfg.wpm) + " wpm." + prio);
  else
    postStatus("Paddle keyer: ready — streaming to " + cfg.host + ":" +
               std::to_string(cfg.port) + " at " + std::to_string(cfg.wpm) +
               " wpm." + prio);

  while (running_.load()) {
    const std::uint64_t now = nowUs();
    const bool d = dit_.load(std::memory_order_relaxed);
    const bool h = dah_.load(std::memory_order_relaxed);

    // Detect fresh paddle presses. In ultimatic mode the latest press both wins
    // (lastDah) and is remembered (memDit/memDah) so a tap released before the
    // next decision is never dropped. Tracking is gated to ultimatic so iambic
    // timing is unchanged.
    if (cfg.ultimatic) {
      if (d && !prevD) {
        lastDah = false;
        memDit = true;
      }
      if (h && !prevH) {
        lastDah = true;
        memDah = true;
      }
    }
    prevD = d;
    prevH = h;

    switch (phase) {
      case Phase::Idle:
        // A fresh press begins a new character; if both are down, lead with
        // the dit. With autospace, hold that first element until a full
        // 3-dit inter-character space has elapsed since the previous element
        // ended (lastKeyUpUs), so letters tapped in quick succession don't
        // run together. The element is latched in Wait and emitted exactly on
        // the boundary, keeping the sidetone aligned with the on-air edge.
        if (d || h) {
          // Ultimatic: lead with the last-pressed paddle. Iambic: dit-priority.
          const bool dah = cfg.ultimatic ? lastDah : (h && !d);
          const std::uint64_t charBoundary = lastKeyUpUs + 3 * ditUs;
          if (cfg.autospace && lastKeyUpUs != 0 && now < charBoundary) {
            pendDah = dah;
            phase = Phase::Wait;
            phaseEndUs = charBoundary;
          } else {
            startMark(now, dah);
          }
        }
        break;

      case Phase::Wait:
        if (now >= phaseEndUs) startMark(phaseEndUs, pendDah);
        break;

      case Phase::Mark:
        if (curDah && d) memDit = true;  // opposite-paddle memory
        if (!curDah && h) memDah = true;
        if (now >= phaseEndUs) {
          const std::uint64_t markEnd = phaseEndUs;  // exact, not polled
          emitEdge(markEnd, 0);                      // key up
          phase = Phase::Gap;
          phaseEndUs = markEnd + gapUs;
        }
        break;

      case Phase::Gap:
        if (curDah && d) memDit = true;
        if (!curDah && h) memDah = true;
        if (now >= phaseEndUs) {
          const std::uint64_t gapEnd = phaseEndUs;  // exact, not polled
          bool haveNext = true, nextDah = false;
          if (cfg.ultimatic) {
            // Last-pressed wins: with both requested, repeat the latest
            // press; otherwise follow whichever is still requested.
            const bool ditReq = d || memDit;
            const bool dahReq = h || memDah;
            if (ditReq && dahReq)
              nextDah = lastDah;
            else if (dahReq)
              nextDah = true;
            else if (ditReq)
              nextDah = false;
            else
              haveNext = false;
          } else if (curDah) {
            if (d || memDit)
              nextDah = false;
            else if (h)
              nextDah = true;
            else
              haveNext = false;
          } else {
            if (h || memDah)
              nextDah = true;
            else if (d)
              nextDah = false;
            else
              haveNext = false;
          }
          if (haveNext)
            startMark(gapEnd,
                      nextDah);  // next element phase-locked to the gap end
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
    if (clock::now() >= nextKeepalive) sendPacket();

    // Poll fast so a fresh paddle close (and a brief iambic squeeze) is caught
    // with minimal latency. Element *timing* no longer depends on this interval
    // — edges are emitted on the schedule above — so this only bounds how
    // quickly we react to the operator, not how clean the keying is. With the
    // SCHED_FIFO bump above this short sleep stays accurate without starving
    // the system (a busy-spin would needlessly peg a core for no timing
    // benefit).
    std::this_thread::sleep_for(std::chrono::microseconds(100));
  }

  // Leave cwsd in a safe state: a final key-up if we stopped mid-element.
  if (phase == Phase::Mark) emitEdge(nowUs(), 0);
  // Drop the transmit/mute state on the way out so the audio stream is
  // restored.
  if (transmitting) postTransmit(false);

  if (fd >= 0) ::close(fd);
}

// Renders a click-free sidetone via a native PipeWire stream, gated by toneOn_
// (the key state). The audio is generated on PipeWire's realtime thread (see
// PwSidetone); this worker only owns the backend's lifetime and idles until
// stop. Runs independently of the keying worker: a missing sink just means no
// sidetone, never a stall in keying.
void RemotePaddleKeyer::sidetoneWorker(RemotePaddleConfig cfg) {
  const int toneHz = cfg.toneHz > 0 ? cfg.toneHz : 600;
  const int levelPct = cfg.level < 0 ? 0 : (cfg.level > 100 ? 100 : cfg.level);

  Sidetone tone;
  std::string err;
  if (!tone.init(toneHz, levelPct, cfg.sidetoneLatencyFrames, cfg.device,
                 &toneOn_, err)) {
    postStatus("Paddle keyer: sidetone unavailable — " + err);
    return;  // keying continues without local audio feedback
  }

  while (running_.load())
    std::this_thread::sleep_for(std::chrono::milliseconds(100));

  tone.shutdown();
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
