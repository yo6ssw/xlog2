#include "QtCwSkimmerPanel.h"

#include <QCheckBox>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QImage>
#include <QLabel>
#include <QPainter>
#include <QSignalBlocker>
#include <QSlider>
#include <QStandardItemModel>
#include <QTableView>
#include <QTimer>
#include <QVBoxLayout>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <deque>
#include <map>
#include <vector>

namespace {

// Intensity (0..1) -> an "inferno"-style waterfall colormap: a dark, cool noise
// floor that recedes, ramping through purple/red/orange/yellow to white. The
// colour stops are packed into the upper range (most of the travel happens above
// 0.55) so differences in power among strong signals read as clear colour steps,
// not just "bright vs brighter". A gamma >1 first dims the low end (noise).
QRgb heat(float v) {
    v = std::pow(std::clamp(v, 0.0f, 1.0f), 1.4f);
    constexpr int N = 8;
    static const float pos[N]    = {0.00f, 0.35f, 0.55f, 0.68f, 0.78f, 0.86f, 0.93f, 1.00f};
    static const float col[N][3] = {
        {  0,   0,   0},   // black
        { 35,  10,  80},   // indigo (noise floor)
        {110,  25, 110},   // purple
        {180,  35,  85},   // red-magenta
        {225,  60,  40},   // red
        {245, 120,  20},   // orange
        {252, 190,  55},   // yellow
        {255, 255, 235},   // white (peak)
    };
    int i = 0;
    while (i < N - 2 && v > pos[i + 1]) ++i;
    const float t = (v - pos[i]) / (pos[i + 1] - pos[i]);
    const float r = col[i][0] + t * (col[i + 1][0] - col[i][0]);
    const float g = col[i][1] + t * (col[i + 1][1] - col[i][1]);
    const float b = col[i][2] + t * (col[i + 1][2] - col[i][2]);
    return qRgb(int(r), int(g), int(b));
}

}  // namespace

// A self-scrolling spectrogram. The newest line is at the top; on each new row
// the image scrolls down one pixel. Callsigns for the live channels are painted
// over the top edge at their frequency column.
class SkimmerWaterfall : public QWidget {
public:
    explicit SkimmerWaterfall(QWidget* parent = nullptr) : QWidget(parent) {
        setMinimumHeight(120);
        setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
        // Rows arrive in bursts: the skimmer worker emits several per audio chunk
        // and they reach us as a clump of queued UI posts, so scrolling on arrival
        // lurches multiple pixels then stalls. Buffer the rows and drain them on a
        // steady ~60 Hz timer (≤1 px/frame, mild catch-up on backlog) so the
        // waterfall scrolls smoothly regardless of arrival jitter.
        scrollTimer_ = new QTimer(this);
        scrollTimer_->setTimerType(Qt::PreciseTimer);
        connect(scrollTimer_, &QTimer::timeout, this, [this] { drain(); });
        scrollTimer_->start(16);
    }

    void addRow(const std::vector<float>& mags, double minHz, double maxHz) {
        minHz_ = minHz;
        maxHz_ = maxHz;
        if (mags.empty())
            return;
        pending_.push_back(mags);
        // Bound latency under a sustained overrun: never hold more than a screen's
        // worth of backlog — drop the oldest rather than fall ever further behind.
        while (static_cast<int>(pending_.size()) > kHistory)
            pending_.pop_front();
    }

private:
    // Advance the spectrogram by one buffered row (scroll down, write it on top).
    void scrollOne(const std::vector<float>& mags) {
        const int cols = static_cast<int>(mags.size());
        if (cols <= 0)
            return;
        if (img_.width() != cols) {
            img_ = QImage(cols, kHistory, QImage::Format_RGB32);
            img_.fill(Qt::black);  // QImage is uninitialised; clear so the not-yet-
                                   // filled scrollback shows black, not stale memory
        }
        const int bpl = img_.bytesPerLine();
        for (int y = img_.height() - 1; y > 0; --y)
            std::memcpy(img_.scanLine(y), img_.scanLine(y - 1), bpl);
        QRgb* top = reinterpret_cast<QRgb*>(img_.scanLine(0));
        for (int x = 0; x < cols; ++x)
            top[x] = heat(mags[x]);
    }

    // Steady-cadence consumer: one row per tick normally; advance a couple when a
    // burst has piled up so the backlog drains without a visible multi-pixel jump.
    void drain() {
        if (pending_.empty())
            return;
        int steps = pending_.size() > 8 ? 2 : 1;
        for (; steps > 0 && !pending_.empty(); --steps) {
            scrollOne(pending_.front());
            pending_.pop_front();
        }
        update();
    }

public:

    // labels: x-position (Hz) -> callsign text, drawn along the top.
    void setLabels(std::map<double, QString> labels) {
        labels_ = std::move(labels);
        update();
    }

protected:
    void paintEvent(QPaintEvent*) override {
        QPainter p(this);
        p.fillRect(rect(), Qt::black);
        if (!img_.isNull())
            p.drawImage(rect(), img_, img_.rect());
        if (maxHz_ <= minHz_)
            return;
        const double span = maxHz_ - minHz_;
        // Frequency grid + axis labels every 500 Hz.
        p.setPen(QColor(255, 255, 255, 40));
        QFont f = p.font();
        f.setPointSizeF(f.pointSizeF() - 1);
        p.setFont(f);
        for (int hz = static_cast<int>(std::ceil(minHz_ / 500) * 500); hz < maxHz_; hz += 500) {
            const int x = static_cast<int>((hz - minHz_) / span * width());
            p.setPen(QColor(255, 255, 255, 35));
            p.drawLine(x, 0, x, height());
            p.setPen(QColor(180, 180, 180));
            p.drawText(x + 2, height() - 3, QString::number(hz));
        }
        // Callsign tags over the traces they were decoded from.
        for (const auto& [hz, call] : labels_) {
            if (hz < minHz_ || hz > maxHz_)
                continue;
            const int x = static_cast<int>((hz - minHz_) / span * width());
            const QRect box(x + 2, 1, p.fontMetrics().horizontalAdvance(call) + 6, 15);
            p.fillRect(box, QColor(0, 0, 0, 160));
            p.setPen(QColor(120, 255, 120));
            p.drawText(box.adjusted(3, 0, 0, 0), Qt::AlignVCenter, call);
        }
    }

private:
    static constexpr int kHistory = 400;  // scrollback in rows
    QImage  img_;
    double  minHz_ = 0.0, maxHz_ = 0.0;
    std::map<double, QString> labels_;
    std::deque<std::vector<float>> pending_;  // rows awaiting steady playout
    QTimer* scrollTimer_ = nullptr;
};

// -----------------------------------------------------------------------------

QtCwSkimmerPanel::QtCwSkimmerPanel(QWidget* parent) : QWidget(parent) {
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);

    waterfall_ = new SkimmerWaterfall;
    layout->addWidget(waterfall_, 3);

    // Detection-gate slider: higher = stronger signals only (suppress noise/ghosts).
    auto* gateRow = new QHBoxLayout;
    gateRow->setContentsMargins(4, 2, 4, 2);
    gateRow->addWidget(new QLabel("Gate"));
    gate_ = new QSlider(Qt::Horizontal);
    gate_->setRange(-12, 24);   // dB offset; 0 = default sensitivity
    gate_->setValue(0);
    gateRow->addWidget(gate_, 1);
    gateLabel_ = new QLabel("0 dB");
    gateLabel_->setMinimumWidth(40);
    gateRow->addWidget(gateLabel_);
    connect(gate_, &QSlider::valueChanged, this, [this](int v) {
        gateLabel_->setText(QString("%1 dB").arg(v));
        emit gateChanged(v);
    });
    layout->addLayout(gateRow);

    // Per-channel minimum-SNR slider: rejects low-SNR channels (spurious E/T).
    auto* snrRow = new QHBoxLayout;
    snrRow->setContentsMargins(4, 0, 4, 2);
    snrRow->addWidget(new QLabel("Min SNR"));
    snr_ = new QSlider(Qt::Horizontal);
    snr_->setRange(0, 30);   // dB; 0 = no per-channel gating
    snr_->setValue(0);
    snrRow->addWidget(snr_, 1);
    snrLabel_ = new QLabel("0 dB");
    snrLabel_->setMinimumWidth(40);
    snrRow->addWidget(snrLabel_);
    connect(snr_, &QSlider::valueChanged, this, [this](int v) {
        snrLabel_->setText(QString("%1 dB").arg(v));
        emit minSnrChanged(v);
    });
    layout->addLayout(snrRow);

    // Paranoid: only show channels whose callsign is confirmed in the master DB.
    knownOnly_ = new QCheckBox("Show only calls in database");
    knownOnly_->setEnabled(false);   // until a DB is loaded (see setCallDbInfo)
    connect(knownOnly_, &QCheckBox::toggled, this,
            [this](bool on) { emit knownOnlyChanged(on); });
    layout->addWidget(knownOnly_);

    model_ = new QStandardItemModel(0, 4, this);
    model_->setHorizontalHeaderLabels({"Freq", "WPM", "Text", "Call"});
    table_ = new QTableView;
    table_->setModel(model_);
    table_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    table_->setSelectionBehavior(QAbstractItemView::SelectRows);
    table_->verticalHeader()->setVisible(false);
    table_->horizontalHeader()->setStretchLastSection(false);
    table_->setColumnWidth(0, 70);
    table_->setColumnWidth(1, 50);
    table_->horizontalHeader()->setSectionResizeMode(2, QHeaderView::Stretch);
    layout->addWidget(table_, 2);

    setMinimumWidth(260);
}

void QtCwSkimmerPanel::setGate(int db) {
    QSignalBlocker block(gate_);   // restore from settings without re-emitting
    gate_->setValue(db);
    gateLabel_->setText(QString("%1 dB").arg(db));
}

void QtCwSkimmerPanel::setMinSnr(int db) {
    QSignalBlocker block(snr_);
    snr_->setValue(db);
    snrLabel_->setText(QString("%1 dB").arg(db));
}

void QtCwSkimmerPanel::setKnownOnly(bool on) {
    QSignalBlocker block(knownOnly_);
    knownOnly_->setChecked(on);
}

void QtCwSkimmerPanel::setCallDbInfo(bool loaded, std::size_t count) {
    knownOnly_->setEnabled(loaded);
    knownOnly_->setText(loaded
        ? QString("Show only calls in database (%1)").arg(count)
        : QString("Show only calls in database (none loaded)"));
}

void QtCwSkimmerPanel::addWaterfall(const std::vector<float>& mags, double minHz,
                                    double maxHz) {
    waterfall_->addRow(mags, minHz, maxHz);
}

void QtCwSkimmerPanel::updateChannel(int id, double hz, int wpm,
                                     const std::string& text, const std::string& call) {
    const QString freq = QString::number(static_cast<int>(std::lround(hz))) + " Hz";
    auto it = rows_.find(id);
    if (it == rows_.end()) {
        auto* freqItem = new QStandardItem(freq);
        freqItem->setData(hz, Qt::UserRole);  // numeric key for frequency ordering
        QList<QStandardItem*> cells = {freqItem,
                                       new QStandardItem(QString::number(wpm)),
                                       new QStandardItem(QString::fromStdString(text)),
                                       new QStandardItem(QString::fromStdString(call))};
        // Insert so the table stays ordered by frequency (a channel's frequency
        // never changes, so rows only move on insert/remove — never on update).
        int pos = model_->rowCount();
        for (int r = 0; r < model_->rowCount(); ++r)
            if (model_->item(r, 0)->data(Qt::UserRole).toDouble() > hz) { pos = r; break; }
        model_->insertRow(pos, cells);
        rows_[id] = freqItem;
    } else {
        // Update the cells in place (freq is fixed; row keeps its sorted slot).
        const int row = it->second->row();
        model_->item(row, 1)->setText(QString::number(wpm));
        model_->item(row, 2)->setText(QString::fromStdString(text));
        model_->item(row, 3)->setText(QString::fromStdString(call));
    }

    // Refresh the waterfall callsign tags from the current channel set.
    std::map<double, QString> labels;
    for (int r = 0; r < model_->rowCount(); ++r) {
        const QString c = model_->item(r, 3)->text();
        if (!c.isEmpty()) {
            const double f = model_->item(r, 0)->text().split(' ').first().toDouble();
            labels[f] = c;
        }
    }
    waterfall_->setLabels(std::move(labels));
}

void QtCwSkimmerPanel::removeChannel(int id) {
    auto it = rows_.find(id);
    if (it == rows_.end())
        return;
    model_->removeRow(it->second->row());
    rows_.erase(it);
}

void QtCwSkimmerPanel::clear() {
    model_->removeRows(0, model_->rowCount());
    rows_.clear();
    waterfall_->setLabels({});
}
