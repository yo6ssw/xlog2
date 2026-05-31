#pragma once

#include "Qso.h"

#include <glibmm/object.h>
#include <glibmm/refptr.h>

// A Glib::Object wrapper around a Qso so it can live in a Gio::ListStore and
// be displayed by Gtk::ColumnView.
class QsoItem : public Glib::Object {
public:
    Qso qso;

    static Glib::RefPtr<QsoItem> create(const Qso& q) {
        return Glib::make_refptr_for_instance<QsoItem>(new QsoItem(q));
    }

protected:
    explicit QsoItem(const Qso& q)
        : Glib::ObjectBase(typeid(QsoItem)), qso(q) {}
};
