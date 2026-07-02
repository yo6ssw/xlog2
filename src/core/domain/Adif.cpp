// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2025 Adrian Scripcă (YO6SSW)

#include "Adif.h"

#include <algorithm>
#include <cctype>
#include <sstream>

namespace adif {

namespace {

std::string toLower(std::string s) {
  std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) {
    return static_cast<char>(std::tolower(c));
  });
  return s;
}

std::string toUpper(std::string s) {
  std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) {
    return static_cast<char>(std::toupper(c));
  });
  return s;
}

// "20250531" -> "2025-05-31"; passes through anything that is not 8 digits.
std::string dateFromAdif(const std::string& v) {
  if (v.size() != 8) return v;
  return v.substr(0, 4) + "-" + v.substr(4, 2) + "-" + v.substr(6, 2);
}

// "1432" or "143200" -> "14:32".
std::string timeFromAdif(const std::string& v) {
  if (v.size() < 4) return v;
  return v.substr(0, 2) + ":" + v.substr(2, 2);
}

// "2025-05-31" -> "20250531".
std::string dateToAdif(const std::string& v) {
  std::string out;
  for (char c : v)
    if (std::isdigit(static_cast<unsigned char>(c))) out += c;
  return out;
}

// "14:32" -> "1432".
std::string timeToAdif(const std::string& v) {
  std::string out;
  for (char c : v)
    if (std::isdigit(static_cast<unsigned char>(c))) out += c;
  return out;
}

void setField(Qso& q, const std::string& name, const std::string& value) {
  if (name == "qso_date")
    q.date = dateFromAdif(value);
  else if (name == "time_on")
    q.time_on = timeFromAdif(value);
  else if (name == "time_off")
    q.time_off = timeFromAdif(value);
  else if (name == "call")
    q.call = toUpper(value);
  else if (name == "band")
    q.band = toLower(value);
  else if (name == "mode")
    q.mode = toUpper(value);
  else if (name == "freq")
    q.freq = value;
  else if (name == "rst_sent")
    q.rst_sent = value;
  else if (name == "rst_rcvd")
    q.rst_rcvd = value;
  else if (name == "name")
    q.name = value;
  else if (name == "qth")
    q.qth = value;
  else if (name == "gridsquare")
    q.locator = toUpper(value);
  else if (name == "tx_pwr")
    q.power = value;
  else if (name == "qsl_sent")
    q.qsl_sent = toUpper(value);
  else if (name == "qsl_rcvd")
    q.qsl_rcvd = toUpper(value);
  else if (name == "lotw_qsl_sent")
    q.lotw_sent = toUpper(value);
  else if (name == "lotw_qslsdate")
    q.lotw_sent_date = dateFromAdif(value);
  else if (name == "lotw_qsl_rcvd")
    q.lotw_rcvd = toUpper(value);
  else if (name == "lotw_qslrdate")
    q.lotw_rcvd_date = dateFromAdif(value);
  else if (name == "country")
    q.country = value;
  else if (name == "cqz")
    q.cq_zone = value;
  else if (name == "ituz")
    q.itu_zone = value;
  else if (name == "cont")
    q.continent = toUpper(value);
  else if (name == "comment" || name == "notes")
    q.comment = value;
  // unknown fields are silently ignored
}

void writeField(std::ostringstream& os, const std::string& name,
                const std::string& value) {
  if (value.empty()) return;
  os << '<' << name << ':' << value.size() << '>' << value;
}

}  // namespace

std::vector<Qso> parse(const std::string& text) {
  std::vector<Qso> out;
  Qso cur;
  const size_t n = text.size();
  size_t i = 0;

  while (i < n) {
    // Skip everything that is not the start of a tag.
    if (text[i] != '<') {
      ++i;
      continue;
    }
    const size_t close = text.find('>', i);
    if (close == std::string::npos) break;

    // Tag body is NAME[:LENGTH[:TYPE]].
    const std::string tag = text.substr(i + 1, close - i - 1);
    i = close + 1;

    std::string name;
    size_t length = std::string::npos;  // npos => no value follows
    if (const size_t c1 = tag.find(':'); c1 == std::string::npos) {
      name = tag;
    } else {
      name = tag.substr(0, c1);
      const size_t c2 = tag.find(':', c1 + 1);
      const std::string lenStr = (c2 == std::string::npos)
                                     ? tag.substr(c1 + 1)
                                     : tag.substr(c1 + 1, c2 - c1 - 1);
      try {
        length = static_cast<size_t>(std::stoul(lenStr));
      } catch (...) {
        length = 0;
      }
    }

    const std::string lname = toLower(name);
    if (lname == "eoh") {  // end of header: discard collected fields
      cur = Qso{};
      continue;
    }
    if (lname == "eor") {  // end of record: flush
      if (!cur.call.empty()) out.push_back(cur);
      cur = Qso{};
      continue;
    }

    std::string value;
    if (length != std::string::npos && length > 0) {
      value = text.substr(i, std::min(length, n - i));
      i += value.size();
    }
    setField(cur, lname, value);
  }

  return out;
}

std::string write(const std::vector<Qso>& qsos) {
  std::ostringstream os;
  os << "ADIF export from xlog2\n";
  writeField(os, "ADIF_VER", "3.1.4");
  writeField(os, "PROGRAMID", "xlog2");
  os << "<EOH>\n";

  for (const auto& q : qsos) {
    writeField(os, "QSO_DATE", dateToAdif(q.date));
    writeField(os, "TIME_ON", timeToAdif(q.time_on));
    writeField(os, "TIME_OFF", timeToAdif(q.time_off));
    writeField(os, "CALL", q.call);
    writeField(os, "BAND", q.band);
    writeField(os, "MODE", q.mode);
    writeField(os, "FREQ", q.freq);
    writeField(os, "RST_SENT", q.rst_sent);
    writeField(os, "RST_RCVD", q.rst_rcvd);
    writeField(os, "NAME", q.name);
    writeField(os, "QTH", q.qth);
    writeField(os, "GRIDSQUARE", q.locator);
    writeField(os, "TX_PWR", q.power);
    writeField(os, "COUNTRY", q.country);
    writeField(os, "CQZ", q.cq_zone);
    writeField(os, "ITUZ", q.itu_zone);
    writeField(os, "CONT", q.continent);
    writeField(os, "QSL_SENT", q.qsl_sent);
    writeField(os, "QSL_RCVD", q.qsl_rcvd);
    writeField(os, "LOTW_QSL_SENT", q.lotw_sent);
    writeField(os, "LOTW_QSLSDATE", dateToAdif(q.lotw_sent_date));
    writeField(os, "LOTW_QSL_RCVD", q.lotw_rcvd);
    writeField(os, "LOTW_QSLRDATE", dateToAdif(q.lotw_rcvd_date));
    writeField(os, "COMMENT", q.comment);
    os << "<EOR>\n";
  }
  return os.str();
}

}  // namespace adif
