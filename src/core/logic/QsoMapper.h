// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2025 Adrian Scripcă (YO6SSW)

#pragma once

#include "DxccDeriver.h"
#include "FormData.h"
#include "Qso.h"

// Converts between the entry-form DTO and the stored Qso record, applying the
// normalization the UI used to do inline (upper-casing call/locator, mapping
// the QSL check-boxes to "Y"/"N", attaching derived DXCC fields).
namespace qsomap {

// Build a Qso from the form. `id` is the row being edited (0 = new). `dxcc`
// carries the entity/zone fields derived from the callsign (see DxccDeriver).
Qso fromForm(const FormData& f, long id, const dxccderive::Fields& dxcc);

// Populate a form from a stored Qso (for loading a selected row into the form).
FormData toForm(const Qso& q);

// The DXCC fields stored on a record, used as the fallback when no cty.dat
// match exists for the current callsign.
dxccderive::Fields dxccOf(const Qso& q);

}  // namespace qsomap
