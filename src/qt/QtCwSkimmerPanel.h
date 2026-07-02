// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2025 Adrian Scripcă (YO6SSW)

#pragma once

#include <QPointer>
#include <QWidget>

#include <map>
#include <string>

class QTableView;
class QStandardItemModel;
class QStandardItem;
class QSlider;
class QLabel;
class QCheckBox;
class QPlainTextEdit;
class SkimmerWaterfall;  // defined in the .cpp (plain QWidget, no moc needed)

// Qt CW-Skimmer panel: a scrolling waterfall of the rig-audio passband with the
// decoded callsigns labelled on the active traces, above a table of every CW
// signal currently being copied (audio pitch / wpm / rolling text / callsign).
// QtMainWindow owns the CwSkimmer service and feeds this panel via
// addWaterfall()/updateChannel()/removeChannel().
class QtCwSkimmerPanel : public QWidget {
    Q_OBJECT
public:
    explicit QtCwSkimmerPanel(QWidget* parent = nullptr);

    void addWaterfall(const std::vector<float>& mags, double minHz, double maxHz);
    void updateChannel(int id, double hz, int wpm, const std::string& text,
                       const std::string& call);
    void removeChannel(int id);
    void clear();

    // Set a slider's position without emitting its change signal (restore from
    // settings).
    void setGate(int db);
    void setMinSnr(int db);
    void setKnownOnly(bool on);
    // Label the Paranoid checkbox with the loaded DB size, or disable it if none.
    void setCallDbInfo(bool loaded, std::size_t count);

signals:
    void gateChanged(int db);    // operator moved the detection-gate slider
    void minSnrChanged(int db);  // operator moved the per-channel min-SNR slider
    void knownOnlyChanged(bool on);  // operator toggled the "DB calls only" box

private:
    // Open a popup showing the channel's complete decoded text (more than the
    // table's rolling window), or raise its already-open window.
    void showChannelText(int id);

    SkimmerWaterfall*   waterfall_ = nullptr;
    QSlider*            gate_      = nullptr;
    QLabel*             gateLabel_ = nullptr;
    QSlider*            snr_       = nullptr;
    QLabel*             snrLabel_  = nullptr;
    QCheckBox*          knownOnly_ = nullptr;
    QTableView*         table_     = nullptr;
    QStandardItemModel* model_     = nullptr;
    // id -> the row's first item, so a row can be found (item->row()) and updated
    // in place as more characters arrive.
    std::map<int, QStandardItem*> rows_;
    // id -> the channel's full decoded text, reconstructed from the rolling
    // windows (the skimmer only keeps the last ~100 chars; we accumulate beyond).
    std::map<int, std::string> fullText_;
    // id -> the live text view of an open decode popup, refreshed in place as
    // more characters arrive (QPointer so a closed dialog reads back null).
    std::map<int, QPointer<QPlainTextEdit>> textViews_;
};
