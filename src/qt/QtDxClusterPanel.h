#pragma once

#include "DxSpot.h"

#include <QWidget>

#include <chrono>
#include <map>
#include <set>
#include <string>
#include <utility>
#include <vector>

class QTableView;
class QStandardItemModel;
class QPlainTextEdit;
class QLineEdit;
class QPushButton;
class QHBoxLayout;
class QLayout;

// Qt equivalent of the gtkmm DxClusterPanel: a band map that aggregates incoming
// spots by (frequency, DX call) — grouping reports within +/-200 Hz of the same
// callsign — and shows one row per pair (Freq / DX / Band / spotter count /
// Entity / Cont) ordered by frequency, with band-filter chips, a connect/command
// bar and a raw telnet console. Each spotter expires 5 minutes after its report;
// an entry disappears once its last spotter has expired.
class QtDxClusterPanel : public QWidget {
    Q_OBJECT
public:
    explicit QtDxClusterPanel(QWidget* parent = nullptr);

    void addSpot(const DxSpot& spot);
    void addLine(const std::string& line);  // raw text -> console
    void setConnected(bool connected);

signals:
    void activateSpot(const QString& call, double mhz);  // row double-clicked
    void sendCommand(const QString& cmd);
    void connectToggle();

private:
    using Clock = std::chrono::steady_clock;
    struct SpotterInfo {
        Clock::time_point time;
        std::string       comment;
        std::string       timeUtc;
    };
    struct Entry {
        double      freqKHz = 0.0;
        std::string dxCall;
        std::string band;
        std::map<std::string, SpotterInfo> spotters;  // spotter call -> latest
    };
    // Keyed by (freq in 0.1 kHz units, DX call) so iteration is ordered by
    // frequency then call.
    using Key = std::pair<long, std::string>;

    void buildUi();
    void buildBandChips(QLayout* into);
    void rebuild();   // prune expired spotters + reconcile the model incrementally
    void insertRow(int pos, const Entry& e);
    void applyMutable(int row, const Entry& e, Clock::time_point now);  // count + tooltip
    void onSend();

    std::map<Key, Entry>  entries_;
    std::vector<Key>      rowKeys_;      // display order of the model's rows
    std::set<std::string> activeBands_;  // empty = show all

    QStandardItemModel* model_  = nullptr;
    QTableView*         table_  = nullptr;
    QPlainTextEdit*     console_ = nullptr;
    QLineEdit*          command_ = nullptr;
    QPushButton*        connectButton_ = nullptr;
    bool                connected_ = false;
};
