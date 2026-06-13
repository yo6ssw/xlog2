#include "QtRigPanel.h"

#include <QButtonGroup>
#include <QFont>
#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QVBoxLayout>

#include <cmath>

namespace {
// Classic transceiver readout: MHz.kHz.tens-of-Hz, e.g. 14074000 Hz -> "14.074.00".
QString formatFreq(double mhz) {
    if (mhz <= 0.0)
        return QStringLiteral("—.———.——");
    const long hz      = std::lround(mhz * 1.0e6);
    const long mhzPart = hz / 1000000;
    const long khzPart = (hz / 1000) % 1000;
    const long tens    = (hz % 1000) / 10;
    return QString::asprintf("%ld.%03ld.%02ld", mhzPart, khzPart, tens);
}
}  // namespace

QtRigPanel::QtRigPanel(QWidget* parent) : QWidget(parent) {
    auto* layout = new QVBoxLayout(this);
    layout->setSpacing(6);

    // Power on/off, top-right. Shown only once the rig reports a power status.
    auto* powerRow = new QHBoxLayout;
    powerRow->addStretch();
    powerButton_ = new QPushButton(QString::fromUtf8("⏻"));
    powerButton_->setCheckable(true);
    powerButton_->setToolTip("Power the rig on/off");
    powerButton_->setFixedWidth(36);
    powerButton_->setVisible(false);
    connect(powerButton_, &QPushButton::clicked, this, [this](bool checked) {
        if (!updatingPower_)
            emit setPower(checked);
    });
    powerRow->addWidget(powerButton_);
    layout->addLayout(powerRow);

    // Frequency, large and centred.
    freqLabel_ = new QLabel(formatFreq(0.0));
    freqLabel_->setAlignment(Qt::AlignCenter);
    QFont f = freqLabel_->font();
    f.setPointSize(f.pointSize() + 16);
    f.setBold(true);
    freqLabel_->setFont(f);
    layout->addWidget(freqLabel_);

    modeLabel_ = new QLabel;
    modeLabel_->setAlignment(Qt::AlignCenter);
    modeLabel_->setEnabled(false);  // dim look
    layout->addWidget(modeLabel_);

    // Tune buttons: << < > >>  (-500 / -100 / +100 / +500 Hz).
    auto* tuneRow = new QHBoxLayout;
    tuneRow->addStretch();
    const struct { const char* label; double hz; const char* tip; } steps[] = {
        {"«", -500.0, "Down 500 Hz"},  // «
        {"‹", -100.0, "Down 100 Hz"},  // ‹
        {"›",  100.0, "Up 100 Hz"},    // ›
        {"»",  500.0, "Up 500 Hz"},    // »
    };
    for (const auto& s : steps) {
        auto* b = new QPushButton(QString::fromUtf8(s.label));
        b->setToolTip(s.tip);
        b->setFixedWidth(44);
        const double hz = s.hz;
        connect(b, &QPushButton::clicked, this, [this, hz]() { emit stepFrequency(hz); });
        tuneRow->addWidget(b);
    }
    tuneRow->addStretch();
    layout->addLayout(tuneRow);

    // IF-filter selector: 1 = wide, 2 = normal, 3 = narrow.
    auto* filterRow = new QHBoxLayout;
    filterRow->addStretch();
    filterRow->addWidget(new QLabel("Filter:"));
    filterGroup_ = new QButtonGroup(this);
    filterGroup_->setExclusive(true);
    const char* tips[3] = {"Filter 1 (wide)", "Filter 2 (normal)", "Filter 3 (narrow)"};
    for (int i = 0; i < 3; ++i) {
        auto* t = new QPushButton(QString::number(i + 1));
        t->setCheckable(true);
        t->setToolTip(tips[i]);
        t->setFixedWidth(36);
        const int n = i + 1;
        connect(t, &QPushButton::clicked, this, [this, n]() {
            if (!updatingFilter_)
                emit setFilter(n);
        });
        filterButtons_[i] = t;
        filterGroup_->addButton(t, n);
        filterRow->addWidget(t);
    }
    filterRow->addStretch();
    layout->addLayout(filterRow);

    // AGC enable/disable. Starts enabled (the rig's normal state); disabling keeps
    // signal levels proportional, which the CW skimmer wants. There is no readback,
    // so the toggle reflects the operator's intent.
    auto* agcRow = new QHBoxLayout;
    agcRow->addStretch();
    agcButton_ = new QPushButton("AGC");
    agcButton_->setCheckable(true);
    agcButton_->setChecked(true);
    agcButton_->setToolTip("Enable/disable the rig's AGC");
    connect(agcButton_, &QPushButton::clicked, this, [this](bool checked) {
        emit setAgc(checked);
    });
    agcRow->addWidget(agcButton_);
    agcRow->addStretch();
    layout->addLayout(agcRow);
    layout->addStretch();

    setConnected(false);
}

void QtRigPanel::updateFilterButtons(int filter) {
    updatingFilter_ = true;
    for (int i = 0; i < 3; ++i)
        if (filterButtons_[i])
            filterButtons_[i]->setChecked(filter == i + 1);
    updatingFilter_ = false;
}

void QtRigPanel::setState(double mhz, const std::string& mode, int pbwidthHz, int filter) {
    freqLabel_->setText(formatFreq(mhz));

    QString info = QString::fromStdString(mode);
    if (pbwidthHz > 0) {
        if (!info.isEmpty())
            info += "  •  ";
        info += QString::number(pbwidthHz) + " Hz";
    }
    modeLabel_->setText(info);

    filter_ = filter;
    updateFilterButtons(filter);
}

void QtRigPanel::setPowerState(bool supported, bool on) {
    if (!powerButton_)
        return;
    powerButton_->setVisible(supported);
    updatingPower_ = true;
    powerButton_->setChecked(on);
    updatingPower_ = false;
}

void QtRigPanel::setConnected(bool connected) {
    setEnabled(connected);
    if (!connected) {
        modeLabel_->setText("not connected");
        freqLabel_->setText(formatFreq(0.0));
        updateFilterButtons(0);
        if (powerButton_)
            powerButton_->setVisible(false);
    }
}
