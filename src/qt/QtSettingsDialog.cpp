#include "QtSettingsDialog.h"

#include <QCheckBox>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QPushButton>
#include <QSpinBox>
#include <QStackedWidget>
#include <QVBoxLayout>

#include <algorithm>

namespace {
QString qstr(const std::string& s) { return QString::fromStdString(s); }
std::string sstr(const QLineEdit* e) { return e->text().toStdString(); }
// Fall back to `def` when the field is left blank (host fields must not be empty).
std::string sstrOr(const QLineEdit* e, const char* def) {
    const std::string s = e->text().toStdString();
    return s.empty() ? std::string(def) : s;
}
}  // namespace

QFormLayout* QtSettingsDialog::addPage(const QString& name) {
    auto* page = new QWidget;
    auto* form = new QFormLayout(page);
    stack_->addWidget(page);
    list_->addItem(name);
    return form;
}

QtSettingsDialog::QtSettingsDialog(const Settings& s, QWidget* parent)
    : QDialog(parent), seed_(s) {
    setWindowTitle("Settings");
    resize(560, 460);

    list_  = new QListWidget;
    list_->setMaximumWidth(160);
    stack_ = new QStackedWidget;
    connect(list_, &QListWidget::currentRowChanged, stack_, &QStackedWidget::setCurrentIndex);

    // --- Network ---
    {
        auto* f = addPage("Network");
        udpPort_ = new QSpinBox; udpPort_->setRange(1, 65535); udpPort_->setValue(s.udpPort);
        f->addRow("UDP listen port:", udpPort_);
        f->addRow(new QLabel("Receives QSOs broadcast by WSJT-X and similar (default 2237)."));
    }

    // --- Rig ---
    {
        auto* f = addPage("Rig");
        rigModel_ = new QSpinBox; rigModel_->setRange(1, 99999); rigModel_->setValue(s.rigModel);
        rigDevice_ = new QLineEdit(qstr(s.rigDevice));
        rigPoll_ = new QSpinBox; rigPoll_->setRange(50, 60000); rigPoll_->setValue(s.rigPollMs);
        rigAuto_ = new QCheckBox("Connect to rig at startup"); rigAuto_->setChecked(s.rigAutoConnect);
        f->addRow("Hamlib model id:", rigModel_);
        f->addRow("Device / host:", rigDevice_);
        f->addRow("Poll interval (ms):", rigPoll_);
        f->addRow(rigAuto_);
    }

    // --- DX Cluster ---
    {
        auto* f = addPage("DX Cluster");
        dxHost_ = new QLineEdit(qstr(s.dxHost));
        dxPort_ = new QSpinBox; dxPort_->setRange(1, 65535); dxPort_->setValue(s.dxPort);
        dxLogin_ = new QLineEdit(qstr(s.dxLogin));
        dxAuto_ = new QCheckBox("Connect at startup"); dxAuto_->setChecked(s.dxAutoConnect);
        f->addRow("Host:", dxHost_);
        f->addRow("Port:", dxPort_);
        f->addRow("Login callsign:", dxLogin_);
        f->addRow(dxAuto_);
    }

    // --- LoTW ---
    {
        auto* f = addPage("LoTW");
        lotwUser_ = new QLineEdit(qstr(s.lotwUser));
        lotwPass_ = new QLineEdit(qstr(s.lotwPassword)); lotwPass_->setEchoMode(QLineEdit::Password);
        lotwStation_ = new QLineEdit(qstr(s.lotwStation));
        tqslPath_ = new QLineEdit(qstr(s.tqslPath));
        f->addRow("LoTW username:", lotwUser_);
        f->addRow("LoTW password:", lotwPass_);
        f->addRow("Station location:", lotwStation_);
        f->addRow("tqsl path:", tqslPath_);
    }

    // --- QRZ ---
    {
        auto* f = addPage("QRZ");
        qrzUser_ = new QLineEdit(qstr(s.qrzUser));
        qrzPass_ = new QLineEdit(qstr(s.qrzPassword)); qrzPass_->setEchoMode(QLineEdit::Password);
        f->addRow("QRZ.com username:", qrzUser_);
        f->addRow("QRZ.com password:", qrzPass_);
    }

    // --- CW Keyer ---
    {
        auto* f = addPage("CW Keyer");
        keyerHost_ = new QLineEdit(qstr(s.keyerHost));
        keyerPort_ = new QSpinBox; keyerPort_->setRange(1, 65535); keyerPort_->setValue(s.keyerPort);
        keyerSpeed_ = new QSpinBox; keyerSpeed_->setRange(0, 60); keyerSpeed_->setValue(s.keyerSpeed);
        f->addRow("cwdaemon host:", keyerHost_);
        f->addRow("Port:", keyerPort_);
        f->addRow("Speed (wpm, 0=default):", keyerSpeed_);
        for (int i = 0; i < 9; ++i) {
            keyerMsgs_[i] = new QLineEdit(qstr(s.keyerMessages[i]));
            f->addRow(QString("F%1 message:").arg(i + 1), keyerMsgs_[i]);
        }
        f->addRow(new QLabel("Tokens: %CALL% %NAME% %QTH% %RST% (from the entry form)."));
    }

    // --- Paddle ---
    {
        auto* f = addPage("Paddle");
        paddleHost_ = new QLineEdit(qstr(s.paddleHost));
        paddlePort_ = new QSpinBox; paddlePort_->setRange(1, 65535); paddlePort_->setValue(s.paddlePort);
        paddleWpm_ = new QSpinBox; paddleWpm_->setRange(1, 99); paddleWpm_->setValue(s.paddleWpm);
        paddleIambicB_ = new QCheckBox("Iambic B (default: iambic A)"); paddleIambicB_->setChecked(s.paddleIambicB);
        paddleAutospace_ = new QCheckBox("Autospace (enforce inter-character spacing)"); paddleAutospace_->setChecked(s.paddleAutospace);
        paddleSidetone_ = new QCheckBox("Local sidetone"); paddleSidetone_->setChecked(s.paddleSidetone);
        paddleTone_ = new QSpinBox; paddleTone_->setRange(100, 2000); paddleTone_->setSuffix(" Hz"); paddleTone_->setValue(s.paddleToneHz);
        paddleLevel_ = new QSpinBox; paddleLevel_->setRange(0, 100); paddleLevel_->setValue(s.paddleLevel);
        paddleDevice_ = new QLineEdit(qstr(s.paddleSidetoneDevice));
        paddleDevice_->setPlaceholderText("ALSA playback device, e.g. default");
        paddleMute_ = new QCheckBox("Mute rig audio while keying"); paddleMute_->setChecked(s.paddleMuteAudio);
        f->addRow("Host:", paddleHost_);
        f->addRow("Port:", paddlePort_);
        f->addRow("Speed (wpm):", paddleWpm_);
        f->addRow(paddleIambicB_);
        f->addRow(paddleAutospace_);
        f->addRow(paddleSidetone_);
        f->addRow("Tone:", paddleTone_);
        f->addRow("Volume (0–100):", paddleLevel_);
        f->addRow("Sidetone device:", paddleDevice_);
        f->addRow(paddleMute_);
    }

    // --- Audio ---
    {
        auto* f = addPage("Audio");
        audioHost_ = new QLineEdit(qstr(s.audioHost));
        audioPort_ = new QSpinBox; audioPort_->setRange(1, 65535); audioPort_->setValue(s.audioPort);
        audioRate_ = new QComboBox;
        for (int r : {8000, 12000, 16000, 24000, 48000})
            audioRate_->addItem(QString::number(r), r);
        audioRate_->setCurrentIndex(std::max(0, audioRate_->findData(s.audioSampleRate)));
        audioChan_ = new QSpinBox; audioChan_->setRange(1, 2); audioChan_->setValue(s.audioChannels);
        audioDevice_ = new QLineEdit(qstr(s.audioDevice));
        audioDevice_->setPlaceholderText("ALSA playback device, e.g. default");
        f->addRow("Host:", audioHost_);
        f->addRow("Port:", audioPort_);
        f->addRow("Sample rate:", audioRate_);
        f->addRow("Channels:", audioChan_);
        f->addRow("Playback device:", audioDevice_);
        f->addRow(new QLabel("Sample rate and channels must match the cwsd `audio` section."));
    }

    // --- Skimmer ---
    {
        auto* f = addPage("Skimmer");
        skGate_ = new QSpinBox; skGate_->setRange(-30, 30); skGate_->setSuffix(" dB"); skGate_->setValue(s.skimmerGate);
        skMinSnr_ = new QSpinBox; skMinSnr_->setRange(0, 40); skMinSnr_->setSuffix(" dB"); skMinSnr_->setValue(s.skimmerMinSnr);
        skKnownOnly_ = new QCheckBox("Paranoid: only surface DB-confirmed calls"); skKnownOnly_->setChecked(s.skimmerKnownOnly);
        skBwNormDb_ = new QSpinBox; skBwNormDb_->setRange(0, 24); skBwNormDb_->setSuffix(" dB/oct"); skBwNormDb_->setValue(s.skimmerBwNormDb);
        skBwNormRef_ = new QSpinBox; skBwNormRef_->setRange(100, 6000); skBwNormRef_->setSuffix(" Hz"); skBwNormRef_->setValue(s.skimmerBwNormRefHz);
        skBwOffset_ = new QSpinBox; skBwOffset_->setRange(-30, 30); skBwOffset_->setSuffix(" dB"); skBwOffset_->setValue(s.skimmerBwOffsetDb);
        f->addRow("Detection gate:", skGate_);
        f->addRow("Min per-channel SNR:", skMinSnr_);
        f->addRow(skKnownOnly_);
        f->addRow("Waterfall BW norm:", skBwNormDb_);
        f->addRow("BW norm reference:", skBwNormRef_);
        f->addRow("Waterfall trim:", skBwOffset_);
        f->addRow(new QLabel("BW norm dims the waterfall as the rig's IF filter narrows."));
    }

    auto* bb = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Apply |
                                    QDialogButtonBox::Cancel);
    connect(bb->button(QDialogButtonBox::Apply), &QPushButton::clicked, this,
            [this]() { emit applied(result()); });
    connect(bb, &QDialogButtonBox::accepted, this, [this]() { emit applied(result()); accept(); });
    connect(bb, &QDialogButtonBox::rejected, this, &QDialog::reject);

    auto* top = new QHBoxLayout;
    top->addWidget(list_);
    top->addWidget(stack_, 1);
    auto* root = new QVBoxLayout(this);
    root->addLayout(top, 1);
    root->addWidget(bb);

    list_->setCurrentRow(0);
}

Settings QtSettingsDialog::result() const {
    Settings s = seed_;  // preserve the fields not edited here

    s.udpPort = udpPort_->value();

    s.rigModel = rigModel_->value();
    s.rigDevice = sstr(rigDevice_);
    s.rigPollMs = rigPoll_->value();
    s.rigAutoConnect = rigAuto_->isChecked();

    s.dxHost = sstr(dxHost_);
    s.dxPort = dxPort_->value();
    s.dxLogin = sstr(dxLogin_);
    s.dxAutoConnect = dxAuto_->isChecked();

    s.lotwUser = sstr(lotwUser_);
    s.lotwPassword = sstr(lotwPass_);
    s.lotwStation = sstr(lotwStation_);
    s.tqslPath = sstrOr(tqslPath_, "tqsl");

    s.qrzUser = sstr(qrzUser_);
    s.qrzPassword = sstr(qrzPass_);

    s.keyerHost = sstrOr(keyerHost_, "127.0.0.1");
    s.keyerPort = keyerPort_->value();
    s.keyerSpeed = keyerSpeed_->value();
    for (int i = 0; i < 9; ++i)
        s.keyerMessages[i] = sstr(keyerMsgs_[i]);

    s.paddleHost = sstrOr(paddleHost_, "127.0.0.1");
    s.paddlePort = paddlePort_->value();
    s.paddleWpm = paddleWpm_->value();
    s.paddleIambicB = paddleIambicB_->isChecked();
    s.paddleAutospace = paddleAutospace_->isChecked();
    s.paddleSidetone = paddleSidetone_->isChecked();
    s.paddleToneHz = paddleTone_->value();
    s.paddleLevel = paddleLevel_->value();
    s.paddleSidetoneDevice = sstrOr(paddleDevice_, "default");
    s.paddleMuteAudio = paddleMute_->isChecked();

    s.audioHost = sstrOr(audioHost_, "127.0.0.1");
    s.audioPort = audioPort_->value();
    s.audioSampleRate = audioRate_->currentData().toInt();
    s.audioChannels = audioChan_->value();
    s.audioDevice = sstrOr(audioDevice_, "default");

    s.skimmerGate = skGate_->value();
    s.skimmerMinSnr = skMinSnr_->value();
    s.skimmerKnownOnly = skKnownOnly_->isChecked();
    s.skimmerBwNormDb = skBwNormDb_->value();
    s.skimmerBwNormRefHz = skBwNormRef_->value();
    s.skimmerBwOffsetDb = skBwOffset_->value();

    return s;
}
