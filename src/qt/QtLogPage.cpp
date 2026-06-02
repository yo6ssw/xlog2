#include "QtLogPage.h"

#include "Bands.h"

#include <QCheckBox>
#include <QComboBox>
#include <QGridLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QPushButton>
#include <QSortFilterProxyModel>
#include <QTableView>
#include <QVBoxLayout>

#include "StrUtil.h"

namespace {
// Stable column ids in logical (model) order — identical to the gtkmm view's
// ids, so a saved layout is interchangeable between the two backends.
const char* const kColIds[] = {
    "date", "on", "off", "call", "country", "cont", "band", "mode", "freq",
    "rst_s", "rst_r", "name", "qth", "loc", "pwr", "qsl", "lotw", "cqz", "comment"};
constexpr int kColCount = static_cast<int>(sizeof(kColIds) / sizeof(kColIds[0]));

int logicalForId(const std::string& id) {
    for (int i = 0; i < kColCount; ++i)
        if (id == kColIds[i])
            return i;
    return -1;
}
}  // namespace

QtLogPage::QtLogPage(QWidget* parent) : QWidget(parent), presenter_(*this) {
    // Bridge the presenter's shell-facing hooks to Qt signals.
    presenter_.onChanged    = [this]() { emit changed(); };
    presenter_.onStatus     = [this](const std::string& s) { emit status(QString::fromStdString(s)); };
    presenter_.onLookupCall = [this](const std::string& c) { emit lookupCall(QString::fromStdString(c)); };
    presenter_.onSendCw     = [this](const std::string& t) { emit sendCw(QString::fromStdString(t)); };
    presenter_.onAbortCw    = [this]() { emit abortCw(); };

    buildUi();
    presenter_.start();  // populate rows + reset the form (widgets now exist)
}

static QLineEdit* addField(QGridLayout* grid, const QString& label, int row, int col) {
    auto* l = new QLabel(label);
    l->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
    auto* e = new QLineEdit;
    grid->addWidget(l, row, col * 2);
    grid->addWidget(e, row, col * 2 + 1);
    return e;
}

void QtLogPage::buildUi() {
    auto* outer = new QVBoxLayout(this);

    // Search field (hidden until "Find").
    search_ = new QLineEdit;
    search_->setPlaceholderText("Search log — call, name, QTH, locator, comment, band, mode…");
    search_->setClearButtonEnabled(true);
    search_->hide();
    outer->addWidget(search_);

    // Log table.
    model_ = new QsoTableModel(this);
    proxy_ = new QSortFilterProxyModel(this);
    proxy_->setSourceModel(model_);
    proxy_->setFilterKeyColumn(-1);  // match against every column
    proxy_->setFilterCaseSensitivity(Qt::CaseInsensitive);
    table_ = new QTableView;
    table_->setModel(proxy_);
    table_->setSelectionBehavior(QAbstractItemView::SelectRows);
    table_->setSelectionMode(QAbstractItemView::SingleSelection);
    table_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    table_->setSortingEnabled(true);
    table_->verticalHeader()->setVisible(false);
    auto* header = table_->horizontalHeader();
    header->setStretchLastSection(true);
    header->setSectionsMovable(true);  // drag to reorder
    // Right-click the header to toggle column visibility (parity with the gtkmm
    // header context menu).
    header->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(header, &QWidget::customContextMenuRequested, this,
            [this, header](const QPoint& pos) {
                QMenu menu;
                for (int i = 0; i < kColCount; ++i) {
                    auto* a = menu.addAction(
                        model_->headerData(i, Qt::Horizontal, Qt::DisplayRole).toString());
                    a->setCheckable(true);
                    a->setChecked(!header->isSectionHidden(i));
                    connect(a, &QAction::toggled, this,
                            [header, i](bool on) { header->setSectionHidden(i, !on); });
                }
                menu.exec(header->mapToGlobal(pos));
            });
    outer->addWidget(table_, 1);

    connect(search_, &QLineEdit::textChanged, this,
            [this](const QString& t) { proxy_->setFilterFixedString(t); });
    connect(table_->selectionModel(), &QItemSelectionModel::selectionChanged, this,
            [this]() { onSelectionChanged(); });

    // Entry form.
    auto* box = new QGroupBox("QSO entry");
    auto* grid = new QGridLayout(box);

    date_    = addField(grid, "Date (UTC)",     0, 0);
    timeOn_  = addField(grid, "Time on (UTC)",  0, 1);
    timeOff_ = addField(grid, "Time off (UTC)", 0, 2);
    auto* nowButton = new QPushButton("Now");
    grid->addWidget(nowButton, 0, 6);

    call_ = addField(grid, "Call", 1, 0);
    auto* lookupButton = new QPushButton("QRZ");
    lookupButton->setToolTip("Look up this callsign on QRZ.com");
    grid->addWidget(lookupButton, 1, 6);

    auto* bandLabel = new QLabel("Band");
    bandLabel->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
    band_ = new QComboBox;
    grid->addWidget(bandLabel, 1, 2);
    grid->addWidget(band_, 1, 3);
    auto* modeLabel = new QLabel("Mode");
    modeLabel->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
    mode_ = new QComboBox;
    grid->addWidget(modeLabel, 1, 4);
    grid->addWidget(mode_, 1, 5);
    band_->addItem("");
    for (const auto& b : bands::names()) band_->addItem(QString::fromStdString(b));
    mode_->addItem("");
    for (const auto& m : bands::modes()) mode_->addItem(QString::fromStdString(m));

    freq_    = addField(grid, "Freq MHz", 2, 0);
    rstSent_ = addField(grid, "RST sent", 2, 1);
    rstRcvd_ = addField(grid, "RST rcvd", 2, 2);
    name_    = addField(grid, "Name",     3, 0);
    qth_     = addField(grid, "QTH",      3, 1);
    locator_ = addField(grid, "Locator",  3, 2);
    power_   = addField(grid, "Power W",  4, 0);
    qslSent_ = new QCheckBox("QSL sent");
    qslRcvd_ = new QCheckBox("QSL rcvd");
    grid->addWidget(qslSent_, 4, 3);
    grid->addWidget(qslRcvd_, 4, 5);

    auto* commentLabel = new QLabel("Comment");
    commentLabel->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
    comment_ = new QLineEdit;
    grid->addWidget(commentLabel, 5, 0);
    grid->addWidget(comment_, 5, 1, 1, 5);

    dxccLabel_ = new QLabel;
    grid->addWidget(dxccLabel_, 6, 1, 1, 5);
    dupeLabel_ = new QLabel;
    dupeLabel_->setStyleSheet("color: #b00;");
    grid->addWidget(dupeLabel_, 7, 0, 1, 6);

    addButton_    = new QPushButton("Add QSO");
    deleteButton_ = new QPushButton("Delete");
    auto* clearButton = new QPushButton("Clear");
    deleteButton_->setEnabled(false);
    auto* buttons = new QHBoxLayout;
    buttons->addStretch();
    buttons->addWidget(clearButton);
    buttons->addWidget(deleteButton_);
    buttons->addWidget(addButton_);
    grid->addLayout(buttons, 8, 0, 1, 7);

    // Keyer bar.
    auto* keyerBar = new QHBoxLayout;
    keyerBar->addWidget(new QLabel("Keyer:"));
    for (int i = 0; i < 9; ++i) {
        auto* b = new QPushButton(QString("F%1").arg(i + 1));
        b->setEnabled(false);
        connect(b, &QPushButton::clicked, this, [this, i]() { presenter_.onSendCwClicked(i); });
        cwButtons_[i] = b;
        keyerBar->addWidget(b);
    }
    auto* stopButton = new QPushButton("Stop");
    connect(stopButton, &QPushButton::clicked, this, [this]() { presenter_.onAbortCwClicked(); });
    keyerBar->addWidget(stopButton);
    keyerBar->addStretch();
    grid->addLayout(keyerBar, 9, 0, 1, 7);

    outer->addWidget(box);

    // Wire user events to the presenter.
    connect(addButton_,    &QPushButton::clicked, this, [this]() { presenter_.onAddOrUpdate(); });
    connect(deleteButton_, &QPushButton::clicked, this, [this]() { presenter_.onDelete(); });
    connect(clearButton,   &QPushButton::clicked, this, [this]() { presenter_.onClear(); });
    connect(nowButton,     &QPushButton::clicked, this, [this]() { presenter_.onSetNow(); });
    connect(lookupButton,  &QPushButton::clicked, this, [this]() { presenter_.onLookupCallClicked(); });
    connect(call_, &QLineEdit::returnPressed, this, [this]() { presenter_.onAddOrUpdate(); });
    connect(call_, &QLineEdit::textChanged, this, [this]() { if (!loadingForm_) presenter_.onCallChanged(); });
    connect(freq_, &QLineEdit::textChanged, this, [this]() { if (!loadingForm_) presenter_.onFreqChanged(); });
    connect(date_, &QLineEdit::textChanged, this, [this]() { if (!loadingForm_) presenter_.onDupeKeyChanged(); });
    connect(band_, &QComboBox::currentIndexChanged, this, [this]() { if (!loadingForm_) presenter_.onDupeKeyChanged(); });
    connect(mode_, &QComboBox::currentIndexChanged, this, [this]() { if (!loadingForm_) presenter_.onDupeKeyChanged(); });
}

void QtLogPage::onSelectionChanged() {
    const auto sel = table_->selectionModel()->selectedRows();
    if (sel.isEmpty())
        return;
    const int sourceRow = proxy_->mapToSource(sel.first()).row();
    presenter_.onRowSelected(model_->idAt(sourceRow));
}

// --- ILogPageView ------------------------------------------------------------

FormData QtLogPage::formData() const {
    FormData f;
    f.date     = date_->text().toStdString();
    f.time_on  = timeOn_->text().toStdString();
    f.time_off = timeOff_->text().toStdString();
    f.call     = call_->text().toStdString();
    f.band     = band_->currentText().toStdString();
    f.mode     = mode_->currentText().toStdString();
    f.freq     = freq_->text().toStdString();
    f.rst_sent = rstSent_->text().toStdString();
    f.rst_rcvd = rstRcvd_->text().toStdString();
    f.name     = name_->text().toStdString();
    f.qth      = qth_->text().toStdString();
    f.locator  = locator_->text().toStdString();
    f.power    = power_->text().toStdString();
    f.comment  = comment_->text().toStdString();
    f.qsl_sent = qslSent_->isChecked();
    f.qsl_rcvd = qslRcvd_->isChecked();
    return f;
}

void QtLogPage::setFormData(const FormData& f) {
    loadingForm_ = true;
    date_->setText(QString::fromStdString(f.date));
    timeOn_->setText(QString::fromStdString(f.time_on));
    timeOff_->setText(QString::fromStdString(f.time_off));
    call_->setText(QString::fromStdString(f.call));
    freq_->setText(QString::fromStdString(f.freq));
    band_->setCurrentText(QString::fromStdString(f.band));
    mode_->setCurrentText(QString::fromStdString(f.mode));
    rstSent_->setText(QString::fromStdString(f.rst_sent));
    rstRcvd_->setText(QString::fromStdString(f.rst_rcvd));
    name_->setText(QString::fromStdString(f.name));
    qth_->setText(QString::fromStdString(f.qth));
    locator_->setText(QString::fromStdString(f.locator));
    power_->setText(QString::fromStdString(f.power));
    comment_->setText(QString::fromStdString(f.comment));
    qslSent_->setChecked(f.qsl_sent);
    qslRcvd_->setChecked(f.qsl_rcvd);
    loadingForm_ = false;
}

void QtLogPage::setCall(const std::string& s) { call_->setText(QString::fromStdString(s)); }
void QtLogPage::setFreq(const std::string& s) { freq_->setText(QString::fromStdString(s)); }
void QtLogPage::setBand(const std::string& s) { band_->setCurrentText(QString::fromStdString(s)); }

void QtLogPage::setRows(const std::vector<Qso>& qsos) {
    model_->setRows(qsos);
}

void QtLogPage::clearSelection() {
    table_->selectionModel()->clearSelection();
}

void QtLogPage::setDupeWarning(const std::string& msg, bool highlight) {
    dupeLabel_->setText(QString::fromStdString(msg));
    call_->setStyleSheet(highlight ? "background:#fdd;" : QString());
}

void QtLogPage::setDxccText(const std::string& s) {
    dxccLabel_->setText(QString::fromStdString(s));
}

void QtLogPage::setEditing(bool editing) {
    addButton_->setText(editing ? "Update QSO" : "Add QSO");
    deleteButton_->setEnabled(editing);
}

void QtLogPage::setCwButtons(const std::array<std::string, 9>& messages) {
    for (int i = 0; i < 9; ++i) {
        const bool has = !messages[i].empty();
        cwButtons_[i]->setEnabled(has);
        cwButtons_[i]->setToolTip(has ? QString::fromStdString(messages[i]) : "(no message set)");
    }
}

void QtLogPage::focusCall() { call_->setFocus(); }

void QtLogPage::showSearch() {
    search_->show();
    search_->setFocus();
}

// --- column layout -----------------------------------------------------------

void QtLogPage::applyColumnLayout(const IniFile& ini) {
    auto* h = table_->horizontalHeader();
    for (int i = 0; i < kColCount; ++i) {
        const std::string id = kColIds[i];
        if (ini.hasKey("width", id)) {
            const int w = ini.getInt("width", id, 0);
            if (w > 0)
                h->resizeSection(i, w);
        }
        if (ini.hasKey("visible", id))
            h->setSectionHidden(i, !ini.getBool("visible", id, true));
    }
    if (ini.hasKey("columns", "order")) {
        int target = 0;
        for (const auto& id : strutil::splitSemicolons(ini.getString("columns", "order"))) {
            const int logical = logicalForId(id);
            if (logical < 0)
                continue;
            const int cur = h->visualIndex(logical);
            if (cur >= 0 && cur != target)
                h->moveSection(cur, target);
            ++target;
        }
    }
}

void QtLogPage::storeColumnLayout(IniFile& ini) const {
    auto* h = table_->horizontalHeader();
    std::string order;
    for (int v = 0; v < kColCount; ++v) {
        const int logical = h->logicalIndex(v);
        if (logical < 0 || logical >= kColCount)
            continue;
        if (!order.empty())
            order += ';';
        order += kColIds[logical];
    }
    ini.setString("columns", "order", order);
    for (int i = 0; i < kColCount; ++i) {
        const int w = h->sectionSize(i);
        if (w > 0)
            ini.setInt("width", kColIds[i], w);
        ini.setBool("visible", kColIds[i], !h->isSectionHidden(i));
    }
}
