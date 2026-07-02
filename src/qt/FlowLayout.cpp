// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2025 Adrian Scripcă (YO6SSW)

#include "FlowLayout.h"

#include <QWidget>

FlowLayout::FlowLayout(QWidget* parent, int margin, int hSpacing, int vSpacing)
    : QLayout(parent), hSpace_(hSpacing), vSpace_(vSpacing) {
  setContentsMargins(margin, margin, margin, margin);
}

FlowLayout::~FlowLayout() {
  QLayoutItem* item;
  while ((item = takeAt(0))) delete item;
}

void FlowLayout::addItem(QLayoutItem* item) { items_.append(item); }

int FlowLayout::horizontalSpacing() const {
  return hSpace_ >= 0 ? hSpace_
                      : smartSpacing(QStyle::PM_LayoutHorizontalSpacing);
}
int FlowLayout::verticalSpacing() const {
  return vSpace_ >= 0 ? vSpace_
                      : smartSpacing(QStyle::PM_LayoutVerticalSpacing);
}

int FlowLayout::count() const { return items_.size(); }
QLayoutItem* FlowLayout::itemAt(int index) const { return items_.value(index); }
QLayoutItem* FlowLayout::takeAt(int index) {
  return (index >= 0 && index < items_.size()) ? items_.takeAt(index) : nullptr;
}

Qt::Orientations FlowLayout::expandingDirections() const { return {}; }
bool FlowLayout::hasHeightForWidth() const { return true; }
int FlowLayout::heightForWidth(int width) const {
  return doLayout(QRect(0, 0, width, 0), true);
}

void FlowLayout::setGeometry(const QRect& rect) {
  QLayout::setGeometry(rect);
  doLayout(rect, false);
}

QSize FlowLayout::sizeHint() const { return minimumSize(); }

QSize FlowLayout::minimumSize() const {
  QSize size;
  for (const QLayoutItem* item : items_)
    size = size.expandedTo(item->minimumSize());
  const QMargins m = contentsMargins();
  return size + QSize(m.left() + m.right(), m.top() + m.bottom());
}

int FlowLayout::doLayout(const QRect& rect, bool testOnly) const {
  int left, top, right, bottom;
  getContentsMargins(&left, &top, &right, &bottom);
  const QRect effective = rect.adjusted(left, top, -right, -bottom);
  int x = effective.x();
  int y = effective.y();
  int lineHeight = 0;

  for (QLayoutItem* item : items_) {
    const QSize hint = item->sizeHint();
    int nextX = x + hint.width() + horizontalSpacing();
    if (nextX - horizontalSpacing() > effective.right() && lineHeight > 0) {
      x = effective.x();
      y = y + lineHeight + verticalSpacing();
      nextX = x + hint.width() + horizontalSpacing();
      lineHeight = 0;
    }
    if (!testOnly) item->setGeometry(QRect(QPoint(x, y), hint));
    x = nextX;
    lineHeight = qMax(lineHeight, hint.height());
  }
  return y + lineHeight - rect.y() + bottom;
}

int FlowLayout::smartSpacing(QStyle::PixelMetric pm) const {
  QObject* p = parent();
  if (!p) return -1;
  if (p->isWidgetType())
    return static_cast<QWidget*>(p)->style()->pixelMetric(
        pm, nullptr, static_cast<QWidget*>(p));
  return static_cast<QLayout*>(p)->spacing();
}
