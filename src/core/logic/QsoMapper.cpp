// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2025 Adrian Scripcă (YO6SSW)

#include "QsoMapper.h"

#include "StrUtil.h"

namespace qsomap {

Qso fromForm(const FormData& f, long id, const dxccderive::Fields& dxcc) {
  Qso q;
  q.id = id;
  q.date = f.date;
  q.time_on = f.time_on;
  q.time_off = f.time_off;
  q.call = strutil::toUpper(f.call);
  q.band = f.band;
  q.mode = f.mode;
  q.freq = f.freq;
  q.rst_sent = f.rst_sent;
  q.rst_rcvd = f.rst_rcvd;
  q.name = f.name;
  q.qth = f.qth;
  q.locator = strutil::toUpper(f.locator);
  q.power = f.power;
  q.qsl_sent = f.qsl_sent ? "Y" : "N";
  q.qsl_rcvd = f.qsl_rcvd ? "Y" : "N";
  q.comment = f.comment;

  q.country = dxcc.country;
  q.cq_zone = dxcc.cq_zone;
  q.itu_zone = dxcc.itu_zone;
  q.continent = dxcc.continent;
  return q;
}

FormData toForm(const Qso& q) {
  FormData f;
  f.date = q.date;
  f.time_on = q.time_on;
  f.time_off = q.time_off;
  f.call = q.call;
  f.band = q.band;
  f.mode = q.mode;
  f.freq = q.freq;
  f.rst_sent = q.rst_sent;
  f.rst_rcvd = q.rst_rcvd;
  f.name = q.name;
  f.qth = q.qth;
  f.locator = q.locator;
  f.power = q.power;
  f.comment = q.comment;
  f.qsl_sent = q.qsl_sent == "Y";
  f.qsl_rcvd = q.qsl_rcvd == "Y";
  return f;
}

dxccderive::Fields dxccOf(const Qso& q) {
  return {q.country, q.cq_zone, q.itu_zone, q.continent};
}

}  // namespace qsomap
