#pragma once

#include <gtkmm.h>

#include <ctime>
#include <string>
#include <vector>

// Small UI helpers shared by LogPage and MainWindow.
namespace ui {

// Gtk::Widget has no "set all margins" call.
inline void setMargin(Gtk::Widget& w, int m) {
    w.set_margin_start(m);
    w.set_margin_end(m);
    w.set_margin_top(m);
    w.set_margin_bottom(m);
}

inline std::string entryText(const Gtk::Entry& e) {
    return e.get_text().raw();
}

inline std::string dropdownText(const Gtk::DropDown& d,
                                const Glib::RefPtr<Gtk::StringList>& model) {
    const guint i = d.get_selected();
    if (i == GTK_INVALID_LIST_POSITION)
        return {};
    return model->get_string(i).raw();
}

inline void setDropdown(Gtk::DropDown& d, const Glib::RefPtr<Gtk::StringList>& model,
                        const std::string& value) {
    for (guint i = 0; i < model->get_n_items(); ++i) {
        if (model->get_string(i).raw() == value) {
            d.set_selected(i);
            return;
        }
    }
    d.set_selected(GTK_INVALID_LIST_POSITION);
}

inline std::string utcNow(const char* fmt) {
    std::time_t t = std::time(nullptr);
    std::tm tm{};
    gmtime_r(&t, &tm);
    char buf[32];
    std::strftime(buf, sizeof(buf), fmt, &tm);
    return buf;
}

inline std::vector<std::string> splitSemicolons(const std::string& s) {
    std::vector<std::string> out;
    std::string cur;
    for (char c : s) {
        if (c == ';') {
            if (!cur.empty())
                out.push_back(cur);
            cur.clear();
        } else {
            cur += c;
        }
    }
    if (!cur.empty())
        out.push_back(cur);
    return out;
}

inline Glib::RefPtr<Gio::ListStore<Gtk::FileFilter>> makeFilters(
    const Glib::ustring& name, const std::vector<Glib::ustring>& patterns) {
    auto filter = Gtk::FileFilter::create();
    filter->set_name(name);
    for (const auto& p : patterns)
        filter->add_pattern(p);
    auto list = Gio::ListStore<Gtk::FileFilter>::create();
    list->append(filter);
    return list;
}

} // namespace ui
