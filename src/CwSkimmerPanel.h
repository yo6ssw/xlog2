// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2025 Adrian Scripcă (YO6SSW)

#pragma once

#include <gtkmm.h>

#include <cstdint>
#include <deque>
#include <map>
#include <string>
#include <vector>

#include "SkimmerItem.h"

// gtkmm CW-Skimmer panel: a Cairo waterfall of the rig-audio passband with the
// decoded callsigns labelled on the active traces, above a ColumnView listing
// every CW signal currently being copied (audio pitch / wpm / rolling text /
// callsign). MainWindow owns the CwSkimmer service and feeds this panel via
// addWaterfall()/updateChannel()/removeChannel(). It is a value member of the
// shell (like dxPanel_) so it survives being reparented between paned slots.
class CwSkimmerPanel : public Gtk::Box {
 public:
  CwSkimmerPanel();

  void addWaterfall(const std::vector<float>& mags, double minHz, double maxHz);
  void updateChannel(int id, double hz, int wpm, const std::string& text,
                     const std::string& call);
  void removeChannel(int id);
  void clear();

  // Set a control without emitting its signal (restore from settings).
  void setGate(int db);
  void setMinSnr(int db);
  void setKnownOnly(bool on);
  // Label the Paranoid check button with the loaded DB size, or disable if
  // none.
  void setCallDbInfo(bool loaded, std::size_t count);
  sigc::signal<void(int)>& signalGate() {
    return signalGate_;
  }  // moved the gate
  sigc::signal<void(int)>& signalMinSnr() {
    return signalMinSnr_;
  }  // moved min-SNR
  sigc::signal<void(bool)>& signalKnownOnly() {
    return signalKnownOnly_;
  }  // toggled DB-only

 private:
  Glib::RefPtr<Gtk::ColumnViewColumn> makeColumn(
      const Glib::ustring& title,
      std::function<Glib::ustring(const SkimmerItem&)> getter,
      bool expand = false);
  // Open a popup showing the channel's complete decoded text (more than the
  // list's rolling window), or present its already-open window.
  void showChannelText(int id);
  void onDrawWaterfall(const Cairo::RefPtr<Cairo::Context>& cr, int w, int h);
  void scrollOneRow(
      const std::vector<float>& mags);  // advance the image one row
  bool drainWaterfall();                // steady-cadence playout tick

  Gtk::DrawingArea waterfall_;
  Gtk::Scale* gateScale_ = nullptr;
  Gtk::Label* gateLabel_ = nullptr;
  Gtk::Scale* snrScale_ = nullptr;
  Gtk::Label* snrLabel_ = nullptr;
  Gtk::CheckButton* knownOnly_ = nullptr;
  bool updatingGate_ = false;  // suppress emission during a programmatic set
  bool updatingSnr_ = false;
  bool updatingKnown_ = false;
  Gtk::ColumnView columnView_;
  Glib::RefPtr<Gio::ListStore<SkimmerItem>> store_;
  Glib::RefPtr<Gtk::SingleSelection> selection_;
  std::map<int, Glib::RefPtr<SkimmerItem>> items_;  // id -> live row
  // id -> the channel's full decoded text, reconstructed from the rolling
  // windows (the skimmer only keeps the last ~100 chars; we accumulate beyond).
  std::map<int, std::string> fullText_;
  // id -> an open decode popup and its text view, refreshed in place as more
  // characters arrive. The window is heap-owned (deleted on close, GTK4 idiom).
  std::map<int, Gtk::Window*> textWindows_;
  std::map<int, Gtk::TextView*> textViews_;
  sigc::signal<void(int)> signalGate_;
  sigc::signal<void(int)> signalMinSnr_;
  sigc::signal<void(bool)> signalKnownOnly_;

  // Waterfall pixel history (cols_ wide, kHistory tall; RGB24, newest at top).
  static constexpr int kHistory = 400;
  int cols_ = 0;
  std::vector<uint32_t> pixels_;
  double minHz_ = 0.0, maxHz_ = 0.0;
  std::map<double, std::string> labels_;    // pitch Hz -> callsign tag
  std::deque<std::vector<float>> pending_;  // rows awaiting steady playout
};
