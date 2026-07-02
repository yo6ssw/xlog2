// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2025 Adrian Scripcă (YO6SSW)

#pragma once

#include <glibmm/object.h>
#include <glibmm/refptr.h>

#include "Qso.h"

// A Glib::Object wrapper around a Qso so it can live in a Gio::ListStore and
// be displayed by Gtk::ColumnView.
class QsoItem : public Glib::Object {
 public:
  Qso qso;

  static Glib::RefPtr<QsoItem> create(const Qso& q) {
    return Glib::make_refptr_for_instance<QsoItem>(new QsoItem(q));
  }

 protected:
  explicit QsoItem(const Qso& q) : Glib::ObjectBase(typeid(QsoItem)), qso(q) {}
};
