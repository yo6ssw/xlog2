#pragma once

#include "Settings.h"

#include <QDialog>

#include <array>

class QLineEdit;
class QSpinBox;
class QCheckBox;
class QComboBox;
class QListWidget;
class QStackedWidget;
class QFormLayout;

// The consolidated preferences dialog (Edit ▸ Settings): a category list on the
// left switching a stacked page per component on the right. It edits only the
// scalar *config* fields of Settings; runtime/view state (panel dock/visibility,
// the enable toggles, lotwLastDownload) is left to the menus and preserved.
//
// Seeded from the current Settings; result() returns a copy with the config
// fields overwritten from the widgets. The owning window applies the result
// (restarting any running service) — the dialog itself touches no services.
// Apply emits applied(result()); Ok emits applied(result()) then accepts.
class QtSettingsDialog : public QDialog {
    Q_OBJECT
public:
    explicit QtSettingsDialog(const Settings& s, QWidget* parent = nullptr);

    // The edited settings: a copy of the seed with the config subset replaced.
    Settings result() const;

signals:
    void applied(const Settings& edited);

private:
    // Build a category page (a QWidget with a QFormLayout) and register it under
    // `name` in the sidebar + stack. Returns the form to fill with addRow().
    QFormLayout* addPage(const QString& name);

    Settings seed_;  // preserves the fields the dialog does not edit

    QListWidget*    list_  = nullptr;
    QStackedWidget* stack_ = nullptr;

    // --- network ---
    QSpinBox* udpPort_ = nullptr;
    // --- rig ---
    QSpinBox*  rigModel_ = nullptr;
    QLineEdit* rigDevice_ = nullptr;
    QSpinBox*  rigPoll_ = nullptr;
    QCheckBox* rigAuto_ = nullptr;
    // --- dx cluster ---
    QLineEdit* dxHost_ = nullptr;
    QSpinBox*  dxPort_ = nullptr;
    QLineEdit* dxLogin_ = nullptr;
    QCheckBox* dxAuto_ = nullptr;
    // --- lotw ---
    QLineEdit* lotwUser_ = nullptr;
    QLineEdit* lotwPass_ = nullptr;
    QLineEdit* lotwStation_ = nullptr;
    QLineEdit* tqslPath_ = nullptr;
    // --- qrz ---
    QLineEdit* qrzUser_ = nullptr;
    QLineEdit* qrzPass_ = nullptr;
    // --- keyer ---
    QLineEdit* keyerHost_ = nullptr;
    QSpinBox*  keyerPort_ = nullptr;
    QSpinBox*  keyerSpeed_ = nullptr;
    std::array<QLineEdit*, 9> keyerMsgs_{};
    // --- paddle ---
    QLineEdit* paddleHost_ = nullptr;
    QSpinBox*  paddlePort_ = nullptr;
    QSpinBox*  paddleWpm_ = nullptr;
    QCheckBox* paddleIambicB_ = nullptr;
    QCheckBox* paddleAutospace_ = nullptr;
    QCheckBox* paddleSidetone_ = nullptr;
    QSpinBox*  paddleTone_ = nullptr;
    QSpinBox*  paddleLevel_ = nullptr;
    QLineEdit* paddleDevice_ = nullptr;
    QCheckBox* paddleMute_ = nullptr;
    // --- audio ---
    QLineEdit* audioHost_ = nullptr;
    QSpinBox*  audioPort_ = nullptr;
    QComboBox* audioRate_ = nullptr;
    QSpinBox*  audioChan_ = nullptr;
    QLineEdit* audioDevice_ = nullptr;
    // --- skimmer ---
    QSpinBox*  skGate_ = nullptr;
    QSpinBox*  skMinSnr_ = nullptr;
    QCheckBox* skKnownOnly_ = nullptr;
    QSpinBox*  skBwNormDb_ = nullptr;
    QSpinBox*  skBwNormRef_ = nullptr;
    QSpinBox*  skBwOffset_ = nullptr;
};
