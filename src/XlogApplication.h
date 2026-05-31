#pragma once

#include <gtkmm/application.h>

// The Gtk::Application for xlog2. Owns application lifetime and creates the
// main window on activation.
class XlogApplication : public Gtk::Application {
protected:
    XlogApplication();

public:
    static Glib::RefPtr<XlogApplication> create();

protected:
    void on_activate() override;
};
