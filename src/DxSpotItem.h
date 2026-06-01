#pragma once

#include "DxSpot.h"

#include <glibmm/object.h>
#include <glibmm/refptr.h>

// A Glib::Object wrapper around a DxSpot so it can live in a Gio::ListStore and
// be displayed by Gtk::ColumnView (mirrors QsoItem).
class DxSpotItem : public Glib::Object {
public:
    DxSpot spot;

    static Glib::RefPtr<DxSpotItem> create(const DxSpot& s) {
        return Glib::make_refptr_for_instance<DxSpotItem>(new DxSpotItem(s));
    }

protected:
    explicit DxSpotItem(const DxSpot& s)
        : Glib::ObjectBase(typeid(DxSpotItem)), spot(s) {}
};
