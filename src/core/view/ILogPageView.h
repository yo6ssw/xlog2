#pragma once

#include "FormData.h"
#include "Qso.h"

#include <array>
#include <string>
#include <vector>

// What LogPagePresenter needs from a logbook tab's view. A backend implements
// this with its own widgets (gtkmm Gtk::ColumnView + entries, or Qt
// QTableView + QLineEdits); the presenter never touches a widget. The view is
// passive: it raises user events by calling the presenter back, and otherwise
// only does what these methods say.
class ILogPageView {
public:
    virtual ~ILogPageView() = default;

    // --- entry form ---
    virtual FormData formData() const = 0;          // read all fields
    virtual void setFormData(const FormData&) = 0;  // write all fields
    // Targeted setters, used for live updates that must not disturb other fields
    // (rig polling, band auto-detect, DX spots).
    virtual void setCall(const std::string&) = 0;
    virtual void setFreq(const std::string&) = 0;
    virtual void setBand(const std::string&) = 0;

    // --- log list ---
    virtual void setRows(const std::vector<Qso>&) = 0;
    virtual void clearSelection() = 0;

    // --- indicators / button state ---
    virtual void setDupeWarning(const std::string& msg, bool highlight) = 0;
    virtual void setDxccText(const std::string&) = 0;
    // editing == a stored row is loaded: "Update QSO" label + Delete enabled.
    virtual void setEditing(bool editing) = 0;
    virtual void setCwButtons(const std::array<std::string, 9>& messages) = 0;

    // --- focus / navigation ---
    virtual void focusCall() = 0;
    virtual void showSearch() = 0;
};
