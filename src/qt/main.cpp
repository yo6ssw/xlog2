// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2025 Adrian Scripcă (YO6SSW)

#include <QApplication>

#include "QtMainWindow.h"

int main(int argc, char* argv[]) {
  QApplication app(argc, argv);
  QApplication::setApplicationName("xlog2");
  // No setApplicationDisplayName: the window sets its own full title.

  QtMainWindow window;
  window.show();
  return app.exec();
}
