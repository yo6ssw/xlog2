#pragma once

#include "Qso.h"

#include <QAbstractTableModel>

#include <vector>

// Read-only table model backing the log view. Mirrors the gtkmm ColumnView's
// columns; the presenter pushes rows via setRows(), and the view maps a clicked
// row back to its QSO id.
class QsoTableModel : public QAbstractTableModel {
    Q_OBJECT
public:
    using QAbstractTableModel::QAbstractTableModel;

    void setRows(const std::vector<Qso>& rows) {
        beginResetModel();
        rows_ = rows;
        endResetModel();
    }

    long idAt(int row) const {
        return (row >= 0 && row < static_cast<int>(rows_.size())) ? rows_[row].id : 0;
    }

    int rowCount(const QModelIndex& parent = {}) const override {
        return parent.isValid() ? 0 : static_cast<int>(rows_.size());
    }
    int columnCount(const QModelIndex& parent = {}) const override {
        return parent.isValid() ? 0 : kColumnCount;
    }

    QVariant headerData(int section, Qt::Orientation o, int role) const override {
        if (role != Qt::DisplayRole || o != Qt::Horizontal)
            return {};
        static const char* titles[kColumnCount] = {
            "Date", "On", "Off", "Call", "Country", "Cont", "Band", "Mode",
            "Freq", "RST S", "RST R", "Name", "QTH", "Loc", "Pwr", "QSL",
            "LoTW", "CQ", "Comment"};
        return QString::fromUtf8(titles[section]);
    }

    QVariant data(const QModelIndex& idx, int role) const override {
        if (!idx.isValid() || role != Qt::DisplayRole)
            return {};
        const Qso& q = rows_[idx.row()];
        switch (idx.column()) {
            case 0:  return QString::fromStdString(q.date);
            case 1:  return QString::fromStdString(q.time_on);
            case 2:  return QString::fromStdString(q.time_off);
            case 3:  return QString::fromStdString(q.call);
            case 4:  return QString::fromStdString(q.country);
            case 5:  return QString::fromStdString(q.continent);
            case 6:  return QString::fromStdString(q.band);
            case 7:  return QString::fromStdString(q.mode);
            case 8:  return QString::fromStdString(q.freq);
            case 9:  return QString::fromStdString(q.rst_sent);
            case 10: return QString::fromStdString(q.rst_rcvd);
            case 11: return QString::fromStdString(q.name);
            case 12: return QString::fromStdString(q.qth);
            case 13: return QString::fromStdString(q.locator);
            case 14: return QString::fromStdString(q.power);
            case 15: return QString::fromStdString(
                         (q.qsl_sent.empty() ? "-" : q.qsl_sent) + "/" +
                         (q.qsl_rcvd.empty() ? "-" : q.qsl_rcvd));
            case 16: return QString::fromUtf8(q.lotw_rcvd == "Y" ? "✓"
                                            : q.lotw_sent == "Y" ? "↑" : "-");
            case 17: return QString::fromStdString(q.cq_zone);
            case 18: return QString::fromStdString(q.comment);
        }
        return {};
    }

private:
    static constexpr int kColumnCount = 19;
    std::vector<Qso> rows_;
};
