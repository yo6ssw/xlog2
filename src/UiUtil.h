// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2025 Adrian Scripcă (YO6SSW)

#pragma once

#include <gtkmm.h>

#include <string>
#include <vector>

#include "StrUtil.h"
#include "TimeUtil.h"

// Small UI helpers shared by LogPage and MainWindow.
namespace ui {

// Gtk::Widget has no "set all margins" call.
inline void setMargin(Gtk::Widget& w, int m) {
  w.set_margin_start(m);
  w.set_margin_end(m);
  w.set_margin_top(m);
  w.set_margin_bottom(m);
}

inline std::string entryText(const Gtk::Entry& e) { return e.get_text().raw(); }

inline std::string dropdownText(const Gtk::DropDown& d,
                                const Glib::RefPtr<Gtk::StringList>& model) {
  const guint i = d.get_selected();
  if (i == GTK_INVALID_LIST_POSITION) return {};
  return model->get_string(i).raw();
}

inline void setDropdown(Gtk::DropDown& d,
                        const Glib::RefPtr<Gtk::StringList>& model,
                        const std::string& value) {
  for (guint i = 0; i < model->get_n_items(); ++i) {
    if (model->get_string(i).raw() == value) {
      d.set_selected(i);
      return;
    }
  }
  d.set_selected(GTK_INVALID_LIST_POSITION);
}

inline std::string utcNow(const char* fmt) { return timeutil::utcNow(fmt); }

inline std::vector<std::string> splitSemicolons(const std::string& s) {
  return strutil::splitSemicolons(s);
}

inline Glib::RefPtr<Gio::ListStore<Gtk::FileFilter>> makeFilters(
    const Glib::ustring& name, const std::vector<Glib::ustring>& patterns) {
  auto filter = Gtk::FileFilter::create();
  filter->set_name(name);
  for (const auto& p : patterns) filter->add_pattern(p);
  auto list = Gio::ListStore<Gtk::FileFilter>::create();
  list->append(filter);
  return list;
}

}  // namespace ui
