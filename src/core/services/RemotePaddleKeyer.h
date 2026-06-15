#pragma once

#include "IUiDispatcher.h"

#include <atomic>
#include <functional>
#include <memory>
#include <string>
#include <thread>

// Operator-side client for cwsd's "remote_key" service: real paddle keying over
// the internet (see cwsd's remote_key_server). The iambic keyer runs *here*, on
// jitter-free local paddle input, and streams finished key edges (key-down /
// key-up) timestamped on a per-session monotonic clock; cwsd replays them onto
// the rig's key line behind a fixed playout delay, so network jitter never
// distorts the Morse. Contrast CwKeyer, which sends *text* for cwdaemon to key.
//
// Paddle input arrives from the UI thread (for testing, the `[` and `]` keys —
// see MainWindow / QtMainWindow); setDit()/setDah() are lock-free. A worker
// thread polls them every ~250 us and ticks the element generator, emitting edges
// as UDP datagrams (each carrying recent edges as loss-recovery history) and a
// keepalive while idle so cwsd keeps the session anchored. Edge timestamps come
// from the element *schedule*, not the poll clock, so the keying stays exact — the
// poll interval bounds only how fast we react to a fresh paddle close.
//
// Local sidetone gives the operator instant feel: a native PipeWire stream renders
// a click-free (ramped-envelope) sine, gated by the same key-down/up transitions
// as the streamed edges. It is local, so it sounds the moment the paddle closes —
// the on-air signal lags by cwsd's playout delay, but the feel does not. The
// sidetone is independent: if its sink is missing, keying still streams.
//
// Autospace (on by default): when a new character's first element is keyed within
// 3 dits of the previous element ending, its start is held to the 3-dit boundary so
// quickly-tapped letters get a clean inter-character space instead of running
// together. It applies only across the idle gap between characters, never to a
// continuing iambic squeeze.
//
// NOTE (scaffold): iambic memory gives iambic-A behaviour; full iambic-B is a TODO.
struct RemotePaddleConfig {
    bool        enabled = false;
    std::string host    = "127.0.0.1";  // cwsd remote_key_server host
    int         port    = 6790;         // cwsd remote_key UDP port
    int         wpm     = 20;           // keying speed
    bool        iambicB = false;        // false = iambic A (reserved; see header note)
    bool        autospace = true;       // enforce a full 3-dit inter-character space

    // local sidetone
    bool        sidetone = true;        // generate local audio feedback
    int         toneHz   = 600;         // sidetone frequency
    int         level    = 50;          // sidetone volume, 0..100
    std::string device   = "default";   // "default" sink, or a PipeWire node name
};

class RemotePaddleKeyer {
public:
    std::function<void(const std::string&)> onStatus;  // connection state / errors (UI thread)
    // Transmit on/off, with a hang so it doesn't flap between elements. The shell
    // uses this to mute the rig-audio stream while keying (semi-break-in): you'd
    // otherwise hear your own signal returning through the stream, delayed by
    // cwsd's playout buffer, fighting the instant local sidetone. Fires on the UI
    // thread.
    std::function<void(bool transmitting)> onTransmit;

    explicit RemotePaddleKeyer(IUiDispatcher& ui) : ui_(ui) {}
    ~RemotePaddleKeyer();
    RemotePaddleKeyer(const RemotePaddleKeyer&)            = delete;
    RemotePaddleKeyer& operator=(const RemotePaddleKeyer&) = delete;

    void start(const RemotePaddleConfig& cfg);
    void stop();
    bool isActive() const { return running_.load(); }

    // Paddle contacts, set from the UI thread. The worker samples these atomics.
    void setDit(bool pressed) { dit_.store(pressed, std::memory_order_relaxed); }
    void setDah(bool pressed) { dah_.store(pressed, std::memory_order_relaxed); }

private:
    void worker(RemotePaddleConfig cfg);
    void sidetoneWorker(RemotePaddleConfig cfg);
    void postStatus(const std::string& s);
    void postTransmit(bool on);

    IUiDispatcher&        ui_;
    std::atomic<bool>     dit_{false};
    std::atomic<bool>     dah_{false};
    std::atomic<bool>     toneOn_{false};   // key state for the sidetone thread
    std::atomic<bool>     running_{false};
    std::thread           thread_;
    std::thread           sidetoneThread_;

    // Liveness token for posted closures (mirrors AudioStreamClient): recreated on
    // each start so a late callback from a previous session is dropped.
    std::shared_ptr<bool> alive_ = std::make_shared<bool>(true);
};
