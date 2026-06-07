#pragma once

#include "ILogPageView.h"
#include "IniFile.h"
#include "LogPagePresenter.h"
#include "QsoTableModel.h"
#include "Qrz.h"

#include <QWidget>

#include <array>
#include <string>

class QLineEdit;
class QComboBox;
class QCheckBox;
class QPushButton;
class QLabel;
class QTableView;
class QSortFilterProxyModel;

// Qt Widgets view for one logbook tab. Mirrors the gtkmm LogPage: it owns a
// LogPagePresenter, implements ILogPageView for the presenter to drive, and
// forwards user events to it. Shell-facing presenter hooks are re-emitted as Qt
// signals for QtMainWindow to connect.
class QtLogPage : public QWidget, public ILogPageView {
    Q_OBJECT
public:
    explicit QtLogPage(QWidget* parent = nullptr);

    LogPagePresenter& presenter() { return presenter_; }

    // Logbook operations (delegate to the presenter).
    void newInMemory()                       { presenter_.newInMemory(); }
    bool openFile(const std::string& p)      { return presenter_.openFile(p); }
    bool saveAs(const std::string& p)        { return presenter_.saveAs(p); }
    int  importAdif(const std::string& t)    { return presenter_.importAdif(t); }
    int  importXlog(const std::string& t)    { return presenter_.importXlog(t); }
    std::string exportAdif() const           { return presenter_.exportAdif(); }
    const LogBook& logbook() const           { return presenter_.logbook(); }
    std::string path() const                 { return presenter_.path(); }
    bool isFileBacked() const                { return presenter_.isFileBacked(); }
    std::string title() const                { return presenter_.title(); }
    std::size_t qsoCount() const             { return presenter_.qsoCount(); }
    void addExternalQso(const Qso& q)        { presenter_.addExternalQso(q); }
    void setCwMessages(const std::array<std::string, 9>& m) { presenter_.setCwMessages(m); }
    void backfillDxcc()                      { presenter_.backfillDxcc(); }
    void beginSearch()                       { showSearch(); }

    // Shared column layout (order/width/visibility) persistence, using the same
    // [columns]/[width]/[visible] groups and stable column ids as the gtkmm view.
    void applyColumnLayout(const IniFile& ini);
    void storeColumnLayout(IniFile& ini) const;

    // --- ILogPageView ---
    FormData formData() const override;
    void setFormData(const FormData&) override;
    void setCall(const std::string&) override;
    void setFreq(const std::string&) override;
    void setBand(const std::string&) override;
    void setMode(const std::string&) override;
    void setRows(const std::vector<Qso>&) override;
    void clearSelection() override;
    void setDupeWarning(const std::string& msg, bool highlight) override;
    void setDxccText(const std::string&) override;
    void setEditing(bool editing) override;
    void setCwButtons(const std::array<std::string, 9>& messages) override;
    void focusCall() override;
    void showSearch() override;

signals:
    void changed();
    void status(const QString&);
    void lookupCall(const QString&);
    void sendCw(const QString&);
    void abortCw();

private:
    void buildUi();
    void onSelectionChanged();

    LogPagePresenter presenter_;

    QsoTableModel*         model_  = nullptr;
    QSortFilterProxyModel* proxy_  = nullptr;
    QTableView*            table_  = nullptr;
    QLineEdit*             search_ = nullptr;

    QLineEdit* date_; QLineEdit* timeOn_; QLineEdit* timeOff_;
    QLineEdit* call_; QLineEdit* freq_;
    QLineEdit* rstSent_; QLineEdit* rstRcvd_;
    QLineEdit* name_; QLineEdit* qth_; QLineEdit* locator_; QLineEdit* power_;
    QLineEdit* comment_;
    QComboBox* band_; QComboBox* mode_;
    QCheckBox* qslSent_; QCheckBox* qslRcvd_;
    QPushButton* addButton_; QPushButton* deleteButton_;
    QLabel* dupeLabel_; QLabel* dxccLabel_;
    std::array<QPushButton*, 9> cwButtons_{};

    bool loadingForm_ = false;
};
