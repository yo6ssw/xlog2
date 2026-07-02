// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2025 Adrian Scripcă (YO6SSW)

#pragma once

#include <atomic>
#include <functional>
#include <memory>
#include <string>
#include <thread>

#include "IUiDispatcher.h"

// The cwsd audio stream a client subscribes to. sampleRate/channels must match
// the server's `audio` config (they size the Opus decoder and the PipeWire
// stream); device, when not "default", is a PipeWire target node
// (PW_KEY_TARGET_OBJECT).
struct AudioStreamConfig {
  bool enabled = false;
  std::string host = "127.0.0.1";  // cwsd audio_stream_server host
  int port = 7355;                 // UDP port to subscribe to
  int sampleRate = 8000;           // opus rate: 8000/12000/16000/24000/48000
  int channels = 1;
  std::string device = "default";  // "default" sink, or a PipeWire node name
};

// Forward declaration so the pipewire/opus headers stay confined to the .cpp.
struct OpusDecoder;

// Subscribes to a cwsd "audio_stream_server" Opus-over-UDP rig-audio stream and
// plays it back through a native PipeWire stream. cwsd has no configured
// target: a client subscribes by sending any datagram to the port and stays
// subscribed by continuing to send (a periodic keepalive); silent clients are
// dropped. This client mirrors that — its worker sends a small keepalive every
// ~2 s.
//
// A worker thread owns the blocking POSIX socket and the Opus decoder, and
// feeds a lock-free ring drained by PipeWire's realtime callback (a
// drift-compensating playout buffer absorbs jitter and clock skew). Status
// changes are marshalled to the UI thread via the injected dispatcher, so
// onStatus always fires there. A self-pipe wakes the worker promptly on stop().
//
// Wire format (server -> client): a 4-byte big-endian sequence number followed
// by a raw Opus packet (the same as cwsd's audio_stream_server).
class AudioStreamClient {
 public:
  std::function<void(const std::string&)> onStatus;  // streaming state/errors
  // Running count of decoded+played audio frames, posted ~once a second while
  // streaming — a live "the stream is working" indicator for the UI.
  std::function<void(unsigned long framesDecoded)> onStats;
  // A tap on the decoded PCM (int16, interleaved), fired straight from the
  // worker thread for every datagram — including while muted, so the CW skimmer
  // keeps copying the band during transmit even though the output is silenced.
  // Set it before start() and leave it set for the client's lifetime; the sink
  // (e.g. CwSkimmer::pushPcm) must be cheap and thread-safe.
  std::function<void(const int16_t* samples, int frames, int channels,
                     int sampleRate)>
      onPcm;

  explicit AudioStreamClient(IUiDispatcher& ui) : ui_(ui) {}
  ~AudioStreamClient();
  AudioStreamClient(const AudioStreamClient&) = delete;
  AudioStreamClient& operator=(const AudioStreamClient&) = delete;

  void start(const AudioStreamConfig& cfg);
  void stop();
  bool isStreaming() const { return running_.load(); }

  // Mute playback without unsubscribing: the worker keeps receiving + decoding
  // (so it stays subscribed and the decoder stays in sync) and still feeds the
  // onPcm tap, but plays silence to the output device. Used to silence the
  // rig-audio stream while transmitting (see RemotePaddleKeyer) without
  // starving the CW skimmer.
  void setMuted(bool m) { muted_.store(m, std::memory_order_relaxed); }

 private:
  void worker(AudioStreamConfig cfg);
  void wake();  // poke the self-pipe so the worker re-checks its stop flag
  void postStatus(const std::string& s);
  void postStats(unsigned long frames);

  IUiDispatcher& ui_;
  int wake_[2] = {-1, -1};
  std::atomic<bool> running_{false};
  std::atomic<bool> muted_{false};
  std::thread thread_;

  // Liveness token for posted closures (see DxCluster/RigController). Recreated
  // on each start so a late callback from a previous session is dropped.
  std::shared_ptr<bool> alive_ = std::make_shared<bool>(true);
};
