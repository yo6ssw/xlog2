#include "QtMainWindow.h"

#include <QApplication>

int main(int argc, char* argv[]) {
    QApplication app(argc, argv);
    QApplication::setApplicationName("xlog2");
    // No setApplicationDisplayName: the window sets its own full title.

    QtMainWindow window;
    window.show();
    return app.exec();
}
