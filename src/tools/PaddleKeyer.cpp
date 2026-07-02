// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2025 Adrian Scripcă (YO6SSW)

// xlog2-paddle: a standalone iambic-paddle practice keyer extracted from xlog2.
//
// It reuses xlog2's RemotePaddleKeyer in *local-only* mode: the iambic element
// generator and the click-free local sidetone run exactly as in the full app,
// but nothing is keyed over the network — there is no cwsd connection. The
// window just exposes the paddle settings (speed, iambic type, autospace) plus
// the sidetone controls, so you can practise feel and timing with only audio
// coming out.
//
// Paddle input: a real USB HID paddle (the companion firmware) if present, and
// the
// `[` / `]` keys as a built-in dit / dah simulator while the keyer is active.

#include <QApplication>
#include <QCheckBox>
#include <QCloseEvent>
#include <QComboBox>
#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QLabel>
#include <QPushButton>
#include <QSlider>
#include <QVBoxLayout>
#include <QWidget>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <system_error>

#include "HidPaddleInput.h"
#include "IniFile.h"
#include "QtDispatcher.h"
#include "RemotePaddleKeyer.h"

namespace {

// Settings persist in their own ini, separate from the main app's layout.ini,
// so the two never clobber each other. Mirrors xlog2's $XDG_CONFIG_HOME/xlog2
// layout.
std::string configPath() {
  const char* xdg = std::getenv("XDG_CONFIG_HOME");
  std::string base;
  if (xdg && *xdg) {
    base = xdg;
  } else {
    const char* home = std::getenv("HOME");
    base = std::string(home ? home : ".") + "/.config";
  }
  return base + "/xlog2/paddle.ini";
}

}  // namespace

class PaddleWindow : public QWidget {
 public:
  PaddleWindow() : keyer_(dispatcher_), hid_(dispatcher_) {
    setWindowTitle("xlog2 Paddle Keyer");

    loadSettings();  // populate cfg_ before the widgets read their initial
                     // values

    wpm_ = new QSlider(Qt::Horizontal);
    wpm_->setRange(5, 60);
    wpm_->setValue(cfg_.wpm);
    wpmLabel_ = new QLabel;
    wpmLabel_->setMinimumWidth(56);
    auto setWpmLabel = [this](int v) {
      wpmLabel_->setText(QString::number(v) + " wpm");
    };
    setWpmLabel(cfg_.wpm);
    connect(wpm_, &QSlider::valueChanged, this, setWpmLabel);
    auto* speedRow = new QHBoxLayout;
    speedRow->addWidget(wpm_);
    speedRow->addWidget(wpmLabel_);

    iambic_ = new QComboBox;
    iambic_->addItem("Iambic A");   // index 0
    iambic_->addItem("Iambic B");   // index 1
    iambic_->addItem("Ultimatic");  // index 2 — last-pressed memory
    iambic_->setCurrentIndex(cfg_.ultimatic ? 2 : (cfg_.iambicB ? 1 : 0));

    autospace_ = new QCheckBox("Enforce inter-character spacing");
    autospace_->setChecked(cfg_.autospace);

    auto* keyerBox = new QGroupBox("Keyer");
    auto* keyerForm = new QFormLayout(keyerBox);
    keyerForm->addRow("Speed:", speedRow);
    keyerForm->addRow("Mode:", iambic_);
    keyerForm->addRow("Autospace:", autospace_);

    toneHz_ = new QSlider(Qt::Horizontal);
    toneHz_->setRange(300, 1200);
    toneHz_->setValue(cfg_.toneHz);
    toneHzLabel_ = new QLabel;
    toneHzLabel_->setMinimumWidth(56);
    auto setToneHzLabel = [this](int v) {
      toneHzLabel_->setText(QString::number(v) + " Hz");
    };
    setToneHzLabel(cfg_.toneHz);
    connect(toneHz_, &QSlider::valueChanged, this, setToneHzLabel);
    auto* pitchRow = new QHBoxLayout;
    pitchRow->addWidget(toneHz_);
    pitchRow->addWidget(toneHzLabel_);

    level_ = new QSlider(Qt::Horizontal);
    level_->setRange(0, 100);
    level_->setValue(cfg_.level);
    levelLabel_ = new QLabel;
    levelLabel_->setMinimumWidth(56);
    auto setLevelLabel = [this](int v) {
      levelLabel_->setText(QString::number(v) + " %");
    };
    setLevelLabel(cfg_.level);
    connect(level_, &QSlider::valueChanged, this, setLevelLabel);
    auto* volumeRow = new QHBoxLayout;
    volumeRow->addWidget(level_);
    volumeRow->addWidget(levelLabel_);

    auto* toneBox = new QGroupBox("Sidetone");
    auto* toneForm = new QFormLayout(toneBox);
    toneForm->addRow("Pitch:", pitchRow);
    toneForm->addRow("Volume:", volumeRow);

    startStop_ = new QPushButton("Start");
    startStop_->setCheckable(true);

    status_ = new QLabel("Stopped.");
    status_->setWordWrap(true);

    auto* hint = new QLabel(
        "While active, key with the <b>[</b> (dit) and <b>]</b> (dah) keys, "
        "or a connected USB paddle. Audio only — no signal is transmitted.");
    hint->setWordWrap(true);

    auto* root = new QVBoxLayout(this);
    root->addWidget(keyerBox);
    root->addWidget(toneBox);
    root->addWidget(startStop_);
    root->addWidget(hint);
    root->addWidget(status_);
    root->addStretch();

    keyer_.onStatus = [this](const std::string& s) {
      status_->setText(QString::fromStdString(s));
    };
    hid_.onStatus = [this](const std::string& s) {
      status_->setText(QString::fromStdString(s));
    };
    // HID contacts fire on the worker thread; setDit/setDah are lock-free
    // atomics.
    hid_.onDit = [this](bool p) { keyer_.setDit(p); };
    hid_.onDah = [this](bool p) { keyer_.setDah(p); };

    connect(startStop_, &QPushButton::toggled, this, &PaddleWindow::onToggle);
    // Live-apply any setting change while running by restarting the keyer.
    auto restart = [this] {
      if (keyer_.isActive()) start();
    };
    connect(wpm_, &QSlider::valueChanged, this, restart);
    connect(toneHz_, &QSlider::valueChanged, this, restart);
    connect(level_, &QSlider::valueChanged, this, restart);
    connect(iambic_, qOverload<int>(&QComboBox::currentIndexChanged), this,
            restart);
    connect(autospace_, &QCheckBox::toggled, this, restart);

    qApp->installEventFilter(this);  // for the `[` / `]` paddle simulation

    startStop_->setChecked(
        true);  // autostart the keyer on launch (fires onToggle)
  }

 protected:
  void closeEvent(QCloseEvent* e) override {
    saveSettings();
    QWidget::closeEvent(e);
  }

  // App-wide filter: while keying, `[`/`]` drive dit/dah and are consumed so
  // the brackets aren't typed; otherwise events pass through untouched.
  bool eventFilter(QObject* obj, QEvent* ev) override {
    if (keyer_.isActive() &&
        (ev->type() == QEvent::KeyPress || ev->type() == QEvent::KeyRelease)) {
      auto* ke = static_cast<QKeyEvent*>(ev);
      if (!ke->isAutoRepeat() && (ke->key() == Qt::Key_BracketLeft ||
                                  ke->key() == Qt::Key_BracketRight)) {
        const bool down = ev->type() == QEvent::KeyPress;
        if (ke->key() == Qt::Key_BracketLeft)
          keyer_.setDit(down);
        else
          keyer_.setDah(down);
        return true;
      }
    }
    return QWidget::eventFilter(obj, ev);
  }

 private:
  void onToggle(bool on) {
    if (on) {
      start();
      startStop_->setText("Stop");
    } else {
      hid_.stop();
      keyer_.stop();
      startStop_->setText("Start");
    }
  }

  void start() {
    cfg_.localOnly = true;  // never key over the network
    cfg_.sidetone = true;
    // No other PipeWire streams to protect here, so use a small quantum (~2.7
    // ms) for the tightest sidetone feel — far below the main app's graph-safe
    // 512.
    cfg_.sidetoneLatencyFrames = 128;
    cfg_.wpm = wpm_->value();
    cfg_.iambicB = iambic_->currentIndex() == 1;
    cfg_.ultimatic = iambic_->currentIndex() == 2;
    cfg_.autospace = autospace_->isChecked();
    cfg_.toneHz = toneHz_->value();
    cfg_.level = level_->value();
    keyer_.start(cfg_);
    hid_.start();  // also accept a USB HID paddle, if present
  }

  // Read persisted settings into cfg_ (called before the widgets initialise, so
  // they pick the values up). Missing keys keep the in-code defaults.
  void loadSettings() {
    IniFile ini;
    if (!ini.loadFromFile(configPath())) return;
    cfg_.wpm = ini.getInt("paddle", "wpm", cfg_.wpm);
    cfg_.iambicB = ini.getBool("paddle", "iambic_b", cfg_.iambicB);
    cfg_.ultimatic = ini.getBool("paddle", "ultimatic", cfg_.ultimatic);
    cfg_.autospace = ini.getBool("paddle", "autospace", cfg_.autospace);
    cfg_.toneHz = ini.getInt("paddle", "tone_hz", cfg_.toneHz);
    cfg_.level = ini.getInt("paddle", "level", cfg_.level);
  }

  // Persist the current control values. Loads first so any unrelated keys
  // survive.
  void saveSettings() {
    IniFile ini;
    ini.loadFromFile(configPath());
    ini.setInt("paddle", "wpm", wpm_->value());
    ini.setBool("paddle", "iambic_b", iambic_->currentIndex() == 1);
    ini.setBool("paddle", "ultimatic", iambic_->currentIndex() == 2);
    ini.setBool("paddle", "autospace", autospace_->isChecked());
    ini.setInt("paddle", "tone_hz", toneHz_->value());
    ini.setInt("paddle", "level", level_->value());

    std::error_code ec;
    std::filesystem::create_directories(
        std::filesystem::path(configPath()).parent_path(), ec);
    std::ofstream(configPath()) << ini.toString();
  }

  QtDispatcher dispatcher_;
  RemotePaddleConfig cfg_;
  RemotePaddleKeyer keyer_;
  HidPaddleInput hid_;

  QSlider* wpm_ = nullptr;
  QLabel* wpmLabel_ = nullptr;
  QComboBox* iambic_ = nullptr;
  QCheckBox* autospace_ = nullptr;
  QSlider* toneHz_ = nullptr;
  QLabel* toneHzLabel_ = nullptr;
  QSlider* level_ = nullptr;
  QLabel* levelLabel_ = nullptr;
  QPushButton* startStop_ = nullptr;
  QLabel* status_ = nullptr;
};

int main(int argc, char* argv[]) {
  QApplication app(argc, argv);
  QApplication::setApplicationName("xlog2-paddle");

  PaddleWindow window;
  window.show();
  return app.exec();
}
