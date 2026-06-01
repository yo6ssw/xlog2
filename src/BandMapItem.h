#pragma once

#include <glibmm/object.h>
#include <glibmm/refptr.h>

#include <string>

// One row of the band map: a (frequency, DX) entry aggregated from all current
// spots for that pair. `count` is the number of live spotters and
// `spottersTooltip` the multi-line list shown when hovering the count cell.
struct BandMapRow {
    double      freqKHz = 0.0;
    std::string dxCall;
    std::string band;
    std::string comment;          // from the most recent spotter
    std::string timeUtc;          // from the most recent spotter
    int         count = 0;
    std::string spottersTooltip;
};

// Glib::Object wrapper so a BandMapRow can live in a Gio::ListStore / ColumnView.
class BandMapItem : public Glib::Object {
public:
    BandMapRow row;

    static Glib::RefPtr<BandMapItem> create(const BandMapRow& r) {
        return Glib::make_refptr_for_instance<BandMapItem>(new BandMapItem(r));
    }

protected:
    explicit BandMapItem(const BandMapRow& r)
        : Glib::ObjectBase(typeid(BandMapItem)), row(r) {}
};
