// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2025 Adrian Scripcă (YO6SSW)

#include "QtMapPanel.h"

#include <QFormLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPaintEvent>
#include <QPainter>
#include <QVBoxLayout>
#include <cmath>

// --- the drawing surface -----------------------------------------------------

class MapView : public QWidget {
 public:
  explicit MapView(QWidget* parent = nullptr) : QWidget(parent) {
    setMinimumSize(240, 140);
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
    coast_ = geo::loadCoastline(geo::defaultCoastlinePath());
  }

  void setEndpoints(std::optional<geo::LatLon> from,
                    std::optional<geo::LatLon> to) {
    from_ = from;
    to_ = to;
    path_.clear();
    if (from_ && to_) path_ = geo::greatCircle(*from_, *to_, 128);
    update();
  }

 protected:
  void paintEvent(QPaintEvent*) override {
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing, true);

    // Centred 2:1 map rectangle.
    const double w = width(), h = height();
    mapW_ = std::min(w, h * 2.0);
    mapH_ = mapW_ / 2.0;
    ox_ = (w - mapW_) / 2.0;
    oy_ = (h - mapH_) / 2.0;
    const QRectF mapRect(ox_, oy_, mapW_, mapH_);

    p.fillRect(rect(), QColor(0x10, 0x18, 0x24));
    p.fillRect(mapRect, QColor(0x16, 0x2a, 0x3f));  // ocean

    // Graticule every 30°.
    p.setPen(QColor(0x2c, 0x44, 0x5e));
    for (int lon = -180; lon <= 180; lon += 30) {
      const double x = ox_ + (lon + 180.0) / 360.0 * mapW_;
      p.drawLine(QPointF(x, oy_), QPointF(x, oy_ + mapH_));
    }
    for (int lat = -90; lat <= 90; lat += 30) {
      const double y = oy_ + (90.0 - lat) / 180.0 * mapH_;
      p.drawLine(QPointF(ox_, y), QPointF(ox_ + mapW_, y));
    }

    // Coastline.
    p.setPen(QPen(QColor(0x6f, 0x9d, 0x74), 1.0));
    for (const auto& poly : coast_) drawPolyline(p, poly);

    // Great-circle path.
    if (!path_.empty()) {
      p.setPen(QPen(QColor(0xff, 0xb3, 0x4d), 2.0));
      drawPolyline(p, path_);
    }
    // Endpoints.
    if (from_) drawDot(p, *from_, QColor(0x4d, 0xc3, 0xff));
    if (to_) drawDot(p, *to_, QColor(0xff, 0x6b, 0x6b));
  }

 private:
  QPointF project(geo::LatLon ll) const {
    const geo::XY xy = geo::equirect(ll);
    return QPointF(ox_ + xy.x * mapW_, oy_ + xy.y * mapH_);
  }
  // Draw a lat/lon polyline, breaking the stroke where it wraps the
  // antimeridian.
  void drawPolyline(QPainter& p, const std::vector<geo::LatLon>& poly) const {
    QPointF prev;
    bool have = false;
    for (const auto& ll : poly) {
      const QPointF cur = project(ll);
      if (have && std::abs(cur.x() - prev.x()) <= mapW_ / 2.0)
        p.drawLine(prev, cur);
      prev = cur;
      have = true;
    }
  }
  void drawDot(QPainter& p, geo::LatLon ll, const QColor& c) const {
    const QPointF pt = project(ll);
    p.setPen(Qt::NoPen);
    p.setBrush(c);
    p.drawEllipse(pt, 4.0, 4.0);
  }

  std::vector<std::vector<geo::LatLon>> coast_;
  std::vector<geo::LatLon> path_;
  std::optional<geo::LatLon> from_, to_;
  double mapW_ = 0, mapH_ = 0, ox_ = 0, oy_ = 0;
};

// --- the panel ---------------------------------------------------------------

QtMapPanel::QtMapPanel(QWidget* parent) : QWidget(parent) {
  map_ = new MapView;

  fromEdit_ = new QLineEdit;
  fromEdit_->setPlaceholderText("my grid");
  toEdit_ = new QLineEdit;
  toEdit_->setPlaceholderText("their grid");
  info_ = new QLabel;
  info_->setTextInteractionFlags(Qt::TextSelectableByMouse);

  auto* form = new QHBoxLayout;
  form->addWidget(new QLabel("From:"));
  form->addWidget(fromEdit_, 1);
  form->addWidget(new QLabel("To:"));
  form->addWidget(toEdit_, 1);

  auto* root = new QVBoxLayout(this);
  root->setContentsMargins(4, 4, 4, 4);
  root->addLayout(form);
  root->addWidget(map_, 1);
  root->addWidget(info_);

  connect(fromEdit_, &QLineEdit::textChanged, this, [this]() {
    if (!loading_) recompute();
  });
  connect(toEdit_, &QLineEdit::textChanged, this, [this]() {
    if (!loading_) recompute();
  });
  recompute();
}

void QtMapPanel::setFrom(const std::string& grid) {
  loading_ = true;
  fromEdit_->setText(QString::fromStdString(grid));
  loading_ = false;
  recompute();
}

void QtMapPanel::setTo(const std::string& grid) {
  loading_ = true;
  toEdit_->setText(QString::fromStdString(grid));
  loading_ = false;
  recompute();
}

void QtMapPanel::recompute() {
  const auto from = geo::maidenheadToLatLon(fromEdit_->text().toStdString());
  const auto to = geo::maidenheadToLatLon(toEdit_->text().toStdString());
  map_->setEndpoints(from, to);
  if (from && to) {
    const double km = geo::distanceKm(*from, *to);
    const double br = geo::bearingDeg(*from, *to);
    info_->setText(QString("%1 km   %2°")
                       .arg(static_cast<long>(km + 0.5))
                       .arg(static_cast<long>(br + 0.5)));
  } else {
    info_->setText("Enter two valid grid locators.");
  }
}
