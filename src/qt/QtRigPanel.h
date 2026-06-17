#pragma once

#include <QWidget>

#include <array>
#include <string>

class QLabel;
class QPushButton;
class QButtonGroup;
class QComboBox;

// Qt equivalent of the gtkmm RigPanel: the current VFO frequency in a large
// font, four tune buttons (<< / < / > / >> for -500 / -100 / +100 / +500 Hz),
// the current mode/passband, and an IF-filter selector (1 = wide, 2 = normal,
// 3 = narrow). QtMainWindow owns the RigController and feeds this panel via
// setState()/setConnected(); the panel emits stepFrequency()/setFilter().
class QtRigPanel : public QWidget {
    Q_OBJECT
public:
    explicit QtRigPanel(QWidget* parent = nullptr);

    void setState(double mhz, const std::string& mode, int pbwidthHz, int filter);
    void setConnected(bool connected);
    // Reflect the rig's power state; the power button is shown only when the
    // backend reports a power status (`supported`).
    void setPowerState(bool supported, bool on);

signals:
    void stepFrequency(double hz);  // signed Hz nudge
    void setFilter(int n);          // IF-filter slot 1..3
    void setPower(bool on);         // power on/off request
    void setAgc(bool on);           // AGC enable/disable request
    void setMode(const QString& mode);  // operating-mode change request

private:
    void updateFilterButtons(int filter);
    void updateModeCombo(const std::string& mode);

    QLabel* freqLabel_ = nullptr;
    QLabel* modeLabel_ = nullptr;
    QComboBox* modeCombo_ = nullptr;
    QPushButton* powerButton_ = nullptr;
    QPushButton* agcButton_ = nullptr;
    QButtonGroup* filterGroup_ = nullptr;
    std::array<QPushButton*, 3> filterButtons_{nullptr, nullptr, nullptr};

    bool updatingFilter_ = false;  // suppress emission during programmatic toggles
    bool updatingPower_  = false;  // suppress emission during programmatic toggles
    bool updatingMode_   = false;  // suppress emission during programmatic selects
    int  filter_         = 0;
};
