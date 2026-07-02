// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2025 Adrian Scripcă (YO6SSW)

#pragma once

#include <glibmm/object.h>
#include <glibmm/property.h>
#include <glibmm/refptr.h>
#include <glibmm/ustring.h>

// One row of the skimmer decode list (a decoded CW channel). Every field is a
// Glib property so the cells update in place as more characters arrive — the
// row widget is not recreated, so a selected row survives its text changing.
class SkimmerItem : public Glib::Object {
public:
    int    id = 0;   // stable channel id (FFT bin); used to find the row to update
    double hz = 0.0; // audio pitch, for ordering rows by frequency

    Glib::Property<Glib::ustring> freq;
    Glib::Property<Glib::ustring> wpm;
    Glib::Property<Glib::ustring> text;
    Glib::Property<Glib::ustring> call;

    static Glib::RefPtr<SkimmerItem> create() {
        return Glib::make_refptr_for_instance<SkimmerItem>(new SkimmerItem());
    }

protected:
    SkimmerItem()
        : Glib::ObjectBase(typeid(SkimmerItem)),
          freq(*this, "freq", {}),
          wpm(*this, "wpm", {}),
          text(*this, "text", {}),
          call(*this, "call", {}) {}
};
