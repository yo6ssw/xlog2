// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2025 Adrian Scripcă (YO6SSW)

#pragma once

#include "Geo.h"

#include <QWidget>

#include <optional>
#include <string>
#include <vector>

class QLineEdit;
class QLabel;
class MapView;  // defined in the .cpp (plain QWidget, no moc needed)

// Qt world-map panel: an equirectangular map (bundled coastline) with a
// great-circle line drawn between two Maidenhead locators, above From/To entry
// fields and a distance/bearing readout. The shell calls setFrom() with the
// operator's locator and setTo() with the selected QSO's locator; the user can
// also override either field by typing.
class QtMapPanel : public QWidget {
    Q_OBJECT
public:
    explicit QtMapPanel(QWidget* parent = nullptr);

    void setFrom(const std::string& grid);  // operator QTH (from settings)
    void setTo(const std::string& grid);     // selected QSO / form locator

private:
    void recompute();  // re-parse both fields, update the map + readout

    QLineEdit* fromEdit_ = nullptr;
    QLineEdit* toEdit_   = nullptr;
    QLabel*    info_     = nullptr;
    MapView*   map_      = nullptr;
    bool       loading_  = false;  // suppress recompute while setting text
};
