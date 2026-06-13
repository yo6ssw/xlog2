#pragma once

#include <QWidget>

#include <map>
#include <string>

class QTableView;
class QStandardItemModel;
class QStandardItem;
class QSlider;
class QLabel;
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

signals:
    void gateChanged(int db);    // operator moved the detection-gate slider
    void minSnrChanged(int db);  // operator moved the per-channel min-SNR slider

private:
    SkimmerWaterfall*   waterfall_ = nullptr;
    QSlider*            gate_      = nullptr;
    QLabel*             gateLabel_ = nullptr;
    QSlider*            snr_       = nullptr;
    QLabel*             snrLabel_  = nullptr;
    QTableView*         table_     = nullptr;
    QStandardItemModel* model_     = nullptr;
    // id -> the row's first item, so a row can be found (item->row()) and updated
    // in place as more characters arrive.
    std::map<int, QStandardItem*> rows_;
};
