// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2025 Adrian Scripcă (YO6SSW)

#pragma once

#include <glibmm/object.h>
#include <glibmm/property.h>
#include <glibmm/propertyproxy.h>
#include <glibmm/refptr.h>
#include <glibmm/ustring.h>

#include <string>

// One row of the band map: a (frequency, DX) entry. The stable fields are set
// once when the row is created; `count` and `spotters` are Glib properties so
// the count cell can update in place (the row widget is not recreated, so a
// hovered/selected row is preserved while its spotter count changes).
class BandMapItem : public Glib::Object {
 public:
  double freqKHz = 0.0;
  std::string dxCall;
  std::string band;
  std::string entity;     // DXCC entity name (cty.dat)
  std::string continent;  // DXCC continent (cty.dat)
  std::string comment;    // most recent spotter's comment (used by activate)
  std::string timeUtc;    // most recent spotter's time (used by activate)

  Glib::Property<int> count;
  Glib::Property<Glib::ustring> spotters;  // tooltip: list of spotters

  static Glib::RefPtr<BandMapItem> create() {
    return Glib::make_refptr_for_instance<BandMapItem>(new BandMapItem());
  }

 protected:
  BandMapItem()
      : Glib::ObjectBase(typeid(BandMapItem)),
        count(*this, "count", 0),
        spotters(*this, "spotters", Glib::ustring{}) {}
};
