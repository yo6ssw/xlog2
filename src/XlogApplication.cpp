// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2025 Adrian Scripcă (YO6SSW)

#include "XlogApplication.h"

#include "MainWindow.h"

XlogApplication::XlogApplication()
    : Gtk::Application("ro.scripca.xlog2", Gio::Application::Flags::NONE) {}

Glib::RefPtr<XlogApplication> XlogApplication::create() {
    return Glib::make_refptr_for_instance<XlogApplication>(new XlogApplication());
}

void XlogApplication::on_activate() {
    set_accels_for_action("win.find", {"<Control>f"});

    auto* window = new MainWindow();
    add_window(*window);
    // Destroy the heap window when it is closed.
    window->signal_hide().connect([window]() { delete window; });
    window->present();
}
