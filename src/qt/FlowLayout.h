// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2025 Adrian Scripcă (YO6SSW)

#pragma once

#include <QLayout>
#include <QList>
#include <QRect>
#include <QStyle>

// A wrapping layout: items flow left-to-right and wrap to the next line when
// they run out of width (like gtkmm's Gtk::FlowBox). Qt ships this only as an
// example, so it's reproduced here for the DX-cluster band-filter chips.
class FlowLayout : public QLayout {
 public:
  explicit FlowLayout(QWidget* parent, int margin = -1, int hSpacing = -1,
                      int vSpacing = -1);
  ~FlowLayout() override;

  void addItem(QLayoutItem* item) override;
  int horizontalSpacing() const;
  int verticalSpacing() const;
  Qt::Orientations expandingDirections() const override;
  bool hasHeightForWidth() const override;
  int heightForWidth(int width) const override;
  int count() const override;
  QLayoutItem* itemAt(int index) const override;
  QLayoutItem* takeAt(int index) override;
  QSize minimumSize() const override;
  void setGeometry(const QRect& rect) override;
  QSize sizeHint() const override;

 private:
  int doLayout(const QRect& rect, bool testOnly) const;
  int smartSpacing(QStyle::PixelMetric pm) const;

  QList<QLayoutItem*> items_;
  int hSpace_;
  int vSpace_;
};
