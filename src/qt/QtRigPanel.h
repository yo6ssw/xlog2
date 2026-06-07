#pragma once

#include <QWidget>

#include <array>
#include <string>

class QLabel;
class QPushButton;
class QButtonGroup;

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

signals:
    void stepFrequency(double hz);  // signed Hz nudge
    void setFilter(int n);          // IF-filter slot 1..3

private:
    void updateFilterButtons(int filter);

    QLabel* freqLabel_ = nullptr;
    QLabel* modeLabel_ = nullptr;
    QButtonGroup* filterGroup_ = nullptr;
    std::array<QPushButton*, 3> filterButtons_{nullptr, nullptr, nullptr};

    bool updatingFilter_ = false;  // suppress emission during programmatic toggles
    int  filter_         = 0;
};
