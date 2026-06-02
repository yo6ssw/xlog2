#include "QtDxClusterPanel.h"

#include "Bands.h"
#include "Dxcc.h"
#include "FlowLayout.h"

#include <QHeaderView>
#include <QHBoxLayout>
#include <QLineEdit>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QStandardItemModel>
#include <QTableView>
#include <QTimer>
#include <QVBoxLayout>
#include <QWidget>

#include <algorithm>
#include <cmath>
#include <vector>

namespace {
constexpr std::chrono::minutes kSpotterTtl{5};
constexpr double kFreqToleranceKHz = 0.2;  // +/- 200 Hz

QString formatKHz(double khz) { return QString::number(khz, 'f', 1); }
}  // namespace

QtDxClusterPanel::QtDxClusterPanel(QWidget* parent) : QWidget(parent) {
    buildUi();
    // Prune expired spotters periodically so quiet entries disappear even
    // without new traffic.
    auto* timer = new QTimer(this);
    connect(timer, &QTimer::timeout, this, [this]() { rebuild(); });
    timer->start(5000);
}

void QtDxClusterPanel::buildUi() {
    auto* outer = new QVBoxLayout(this);
    outer->setContentsMargins(4, 4, 4, 4);
    outer->setSpacing(4);

    // --- control row: connect, command, send ---
    auto* controls = new QHBoxLayout;
    connectButton_ = new QPushButton("Connect");
    connect(connectButton_, &QPushButton::clicked, this, [this]() { emit connectToggle(); });
    controls->addWidget(connectButton_);
    command_ = new QLineEdit;
    command_->setPlaceholderText("cluster command (e.g. sh/dx 20, set/filter)…");
    connect(command_, &QLineEdit::returnPressed, this, &QtDxClusterPanel::onSend);
    controls->addWidget(command_, 1);
    auto* send = new QPushButton("Send");
    connect(send, &QPushButton::clicked, this, &QtDxClusterPanel::onSend);
    controls->addWidget(send);
    outer->addLayout(controls);

    // --- band filter chips (wrap to multiple rows, like the gtkmm FlowBox) ---
    auto* chipsWidget = new QWidget;
    auto* chipsFlow = new FlowLayout(chipsWidget, 0, 4, 4);
    buildBandChips(chipsFlow);
    // Let the surrounding vertical layout grow the chip area's height as rows
    // wrap, instead of clipping it to one line.
    QSizePolicy sp = chipsWidget->sizePolicy();
    sp.setHeightForWidth(true);
    sp.setVerticalPolicy(QSizePolicy::Minimum);
    chipsWidget->setSizePolicy(sp);
    outer->addWidget(chipsWidget);

    // --- band map table ---
    model_ = new QStandardItemModel(this);
    model_->setHorizontalHeaderLabels(
        {"Freq", "DX", "Band", "Spotters", "Entity", "Cont"});
    table_ = new QTableView;
    table_->setModel(model_);
    table_->setSelectionBehavior(QAbstractItemView::SelectRows);
    table_->setSelectionMode(QAbstractItemView::SingleSelection);
    table_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    table_->verticalHeader()->setVisible(false);
    table_->horizontalHeader()->setStretchLastSection(true);
    connect(table_, &QTableView::doubleClicked, this, [this](const QModelIndex& idx) {
        auto* first = model_->item(idx.row(), 0);
        if (first)
            emit activateSpot(first->data(Qt::UserRole).toString(),
                              first->data(Qt::UserRole + 1).toDouble());
    });
    outer->addWidget(table_, 1);

    // --- raw telnet console ---
    console_ = new QPlainTextEdit;
    console_->setReadOnly(true);
    console_->setMinimumHeight(90);
    QFont mono("monospace");
    mono.setStyleHint(QFont::Monospace);
    console_->setFont(mono);
    outer->addWidget(console_);
}

void QtDxClusterPanel::buildBandChips(QLayout* into) {
    for (const auto& band : bands::names()) {
        auto* chip = new QPushButton(QString::fromStdString(band));
        chip->setCheckable(true);
        const std::string b = band;
        connect(chip, &QPushButton::toggled, this, [this, b](bool on) {
            if (on)
                activeBands_.insert(b);
            else
                activeBands_.erase(b);
            rebuild();
        });
        into->addWidget(chip);
    }
}

void QtDxClusterPanel::addSpot(const DxSpot& spot) {
    if (spot.dxCall.empty() || spot.freqKHz <= 0.0)
        return;

    // Group spots for the same callsign within +/-200 Hz of an existing entry.
    Key key{};
    bool found = false;
    for (const auto& [k, e] : entries_) {
        if (e.dxCall == spot.dxCall &&
            std::abs(e.freqKHz - spot.freqKHz) <= kFreqToleranceKHz) {
            key = k;
            found = true;
            break;
        }
    }
    if (!found)
        key = std::make_pair(std::lround(spot.freqKHz * 10.0), spot.dxCall);

    Entry& e = entries_[key];
    if (!found) {
        e.freqKHz = spot.freqKHz;
        e.dxCall  = spot.dxCall;
        e.band    = spot.band;
    }
    const std::string who = spot.spotter.empty() ? "?" : spot.spotter;
    e.spotters[who] = SpotterInfo{Clock::now(), spot.comment, spot.timeUtc};
    rebuild();
}

// Build the stable cells for a new entry and insert the row at `pos`. The
// mutable cells (count + tooltip) are filled separately by applyMutable.
void QtDxClusterPanel::insertRow(int pos, const Entry& e) {
    std::string entity, continent;
    if (const dxcc::Info* info = dxcc::lookup(e.dxCall)) {
        entity    = info->entity;
        continent = info->continent;
    }
    auto* freq = new QStandardItem(formatKHz(e.freqKHz));
    freq->setData(QString::fromStdString(e.dxCall), Qt::UserRole);
    freq->setData(e.freqKHz / 1000.0, Qt::UserRole + 1);  // MHz for activation
    QList<QStandardItem*> row = {
        freq,
        new QStandardItem(QString::fromStdString(e.dxCall)),
        new QStandardItem(QString::fromStdString(e.band)),
        new QStandardItem,  // count — filled by applyMutable
        new QStandardItem(QString::fromStdString(entity)),
        new QStandardItem(QString::fromStdString(continent)),
    };
    model_->insertRow(pos, row);
}

// Update only the cells that change as spotters come and go: the spotter count
// and its tooltip, plus the latest comment/time stashed on the DX cell. The row
// widgets are reused, so a hovered or selected row is never disturbed.
void QtDxClusterPanel::applyMutable(int row, const Entry& e, Clock::time_point now) {
    std::vector<const std::pair<const std::string, SpotterInfo>*> sorted;
    sorted.reserve(e.spotters.size());
    for (const auto& kv : e.spotters)
        sorted.push_back(&kv);
    std::sort(sorted.begin(), sorted.end(),
              [](auto* a, auto* b) { return a->second.time > b->second.time; });

    QString tip;
    for (const auto* p : sorted) {
        const auto mins = std::chrono::duration_cast<std::chrono::minutes>(
                              now - p->second.time).count();
        if (!tip.isEmpty())
            tip += '\n';
        tip += QString("%1  (%2m ago)").arg(QString::fromStdString(p->first)).arg(mins);
    }
    const std::string comment = sorted.empty() ? std::string{} : sorted.front()->second.comment;
    const std::string timeUtc = sorted.empty() ? std::string{} : sorted.front()->second.timeUtc;

    if (auto* count = model_->item(row, 3)) {
        const QString text = QString::number(sorted.size());
        if (count->text() != text)
            count->setText(text);
        if (count->toolTip() != tip)
            count->setToolTip(tip);
    }
    if (auto* dx = model_->item(row, 1)) {
        dx->setData(QString::fromStdString(comment), Qt::UserRole);
        dx->setData(QString::fromStdString(timeUtc), Qt::UserRole + 1);
    }
}

void QtDxClusterPanel::rebuild() {
    const auto now = Clock::now();

    // Discard spotters older than the TTL; drop entries left with none.
    for (auto it = entries_.begin(); it != entries_.end();) {
        auto& spotters = it->second.spotters;
        for (auto s = spotters.begin(); s != spotters.end();) {
            if (now - s->second.time > kSpotterTtl)
                s = spotters.erase(s);
            else
                ++s;
        }
        if (spotters.empty())
            it = entries_.erase(it);
        else
            ++it;
    }

    // The target set of visible rows, in (frequency, call) order (entries_ is
    // already keyed that way), after applying the band filter.
    std::vector<Key> desired;
    desired.reserve(entries_.size());
    for (const auto& [key, e] : entries_)
        if (activeBands_.empty() || activeBands_.count(e.band))
            desired.push_back(key);
    const std::set<Key> desiredSet(desired.begin(), desired.end());

    // 1) Remove rows no longer wanted (bottom-up so indices stay valid).
    for (int r = static_cast<int>(rowKeys_.size()) - 1; r >= 0; --r) {
        if (!desiredSet.count(rowKeys_[r])) {
            model_->removeRow(r);
            rowKeys_.erase(rowKeys_.begin() + r);
        }
    }

    // 2) Walk the target order; rowKeys_ is now a subsequence of `desired`, so a
    // mismatch at i means desired[i] is new and gets inserted there. Matches are
    // updated in place.
    for (int i = 0; i < static_cast<int>(desired.size()); ++i) {
        const Key& key = desired[i];
        const Entry& e = entries_.at(key);
        if (i < static_cast<int>(rowKeys_.size()) && rowKeys_[i] == key) {
            applyMutable(i, e, now);
        } else {
            insertRow(i, e);
            rowKeys_.insert(rowKeys_.begin() + i, key);
            applyMutable(i, e, now);
        }
    }
}

void QtDxClusterPanel::addLine(const std::string& line) {
    console_->appendPlainText(QString::fromStdString(line));
}

void QtDxClusterPanel::setConnected(bool connected) {
    connected_ = connected;
    connectButton_->setText(connected ? "Disconnect" : "Connect");
}

void QtDxClusterPanel::onSend() {
    const QString cmd = command_->text();
    if (cmd.isEmpty())
        return;
    emit sendCommand(cmd);
    command_->clear();
}
