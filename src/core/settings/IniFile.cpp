// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2025 Adrian Scripcă (YO6SSW)

#include "IniFile.h"

#include <fstream>
#include <sstream>

namespace {

std::string trim(const std::string& s) {
  const auto first = s.find_first_not_of(" \t\r\n");
  if (first == std::string::npos) return {};
  const auto last = s.find_last_not_of(" \t\r\n");
  return s.substr(first, last - first + 1);
}

}  // namespace

bool IniFile::loadFromFile(const std::string& path) {
  std::ifstream in(path);
  if (!in) return false;

  std::string group;
  std::string line;
  while (std::getline(in, line)) {
    const std::string t = trim(line);
    if (t.empty() || t[0] == '#' || t[0] == ';') continue;
    if (t.front() == '[' && t.back() == ']') {
      group = t.substr(1, t.size() - 2);
      continue;
    }
    const auto eq = t.find('=');
    if (eq == std::string::npos) continue;
    const std::string key = trim(t.substr(0, eq));
    const std::string val = trim(t.substr(eq + 1));
    if (!key.empty()) data_[group][key] = val;
  }
  return true;
}

std::string IniFile::toString() const {
  std::ostringstream os;
  bool first = true;
  for (const auto& [group, keys] : data_) {
    if (!first) os << '\n';
    first = false;
    os << '[' << group << "]\n";
    for (const auto& [key, val] : keys) os << key << '=' << val << '\n';
  }
  return os.str();
}

bool IniFile::hasGroup(const std::string& group) const {
  return data_.find(group) != data_.end();
}

bool IniFile::hasKey(const std::string& group, const std::string& key) const {
  const auto g = data_.find(group);
  return g != data_.end() && g->second.find(key) != g->second.end();
}

std::string IniFile::getString(const std::string& group, const std::string& key,
                               const std::string& def) const {
  const auto g = data_.find(group);
  if (g == data_.end()) return def;
  const auto k = g->second.find(key);
  return k == g->second.end() ? def : k->second;
}

int IniFile::getInt(const std::string& group, const std::string& key,
                    int def) const {
  if (!hasKey(group, key)) return def;
  try {
    return std::stoi(getString(group, key));
  } catch (const std::exception&) {
    return def;
  }
}

bool IniFile::getBool(const std::string& group, const std::string& key,
                      bool def) const {
  if (!hasKey(group, key)) return def;
  const std::string v = getString(group, key);
  if (v == "true" || v == "1") return true;
  if (v == "false" || v == "0") return false;
  return def;
}

void IniFile::setString(const std::string& group, const std::string& key,
                        const std::string& value) {
  data_[group][key] = value;
}

void IniFile::setInt(const std::string& group, const std::string& key,
                     int value) {
  data_[group][key] = std::to_string(value);
}

void IniFile::setBool(const std::string& group, const std::string& key,
                      bool value) {
  data_[group][key] = value ? "true" : "false";
}
