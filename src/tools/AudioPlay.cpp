// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2025 Adrian Scripcă (YO6SSW)

// xlog2-audioplay — a headless companion that reads xlog2's saved configuration
// and plays the cwsd rig-audio stream, nothing else. Useful for monitoring the
// rig (and exercising the audio path) without launching the full GUI.
//
// It reuses the exact same pieces the GUI uses: IniFile + Settings to read
// ~/.config/xlog2/layout.ini, and AudioStreamClient (native PipeWire playback,
// FEC/PLC loss recovery, drift-compensated jitter buffer). Run it, Ctrl-C to
// quit.

#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdlib>
#include <iostream>
#include <string>
#include <thread>

#include "Audio.h"
#include "IUiDispatcher.h"
#include "IniFile.h"
#include "Settings.h"

namespace {

std::atomic<bool> g_running{true};

void onSignal(int) { g_running.store(false); }

// AudioStreamClient marshals worker-thread callbacks through an IUiDispatcher.
// We have no UI thread, so just run each closure inline on the calling thread;
// the callbacks here only print, which is fine from the worker thread.
class InlineDispatcher : public IUiDispatcher {
 public:
  void post(std::function<void()> fn) override { fn(); }
};

// ~/.config/xlog2/layout.ini, honouring XDG_CONFIG_HOME — matching the GUI.
std::string configPath() {
  if (const char* xdg = std::getenv("XDG_CONFIG_HOME"); xdg && *xdg)
    return std::string(xdg) + "/xlog2/layout.ini";
  const char* home = std::getenv("HOME");
  return std::string(home ? home : ".") + "/.config/xlog2/layout.ini";
}

}  // namespace

int main() {
  const std::string path = configPath();
  IniFile ini;
  if (!ini.loadFromFile(path))
    std::cerr << "xlog2-audioplay: could not read " << path
              << " (using defaults)\n";

  const Settings s = Settings::load(ini);

  AudioStreamConfig cfg;
  cfg.host = s.audioHost;
  cfg.port = s.audioPort;
  cfg.sampleRate = s.audioSampleRate;
  cfg.channels = s.audioChannels;
  cfg.device = s.audioDevice;

  std::signal(SIGINT, onSignal);
  std::signal(SIGTERM, onSignal);

  InlineDispatcher dispatcher;
  AudioStreamClient client(dispatcher);
  client.onStatus = [](const std::string& msg) {
    std::cout << msg << std::endl;
  };

  std::cout << "xlog2-audioplay: " << cfg.host << ":" << cfg.port << " @ "
            << cfg.sampleRate << " Hz/" << cfg.channels << "ch -> "
            << (cfg.device.empty() ? "default" : cfg.device)
            << "  (Ctrl-C to quit)" << std::endl;

  client.start(cfg);
  while (g_running.load())
    std::this_thread::sleep_for(std::chrono::milliseconds(200));

  client.stop();
  return 0;
}
