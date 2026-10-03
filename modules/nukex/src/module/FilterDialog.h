// NukeX -- teach NukeX an unrecognised filter name
// Copyright (c) 2026 Scott Carter. MIT License.
//
// FITS FILTER values are whatever the capture software wrote, so no shipped
// table can list them all. When NukeX does not recognise one it refuses the
// batch rather than guessing, which is right -- guessing would mis-colour the
// result silently -- but it leaves the user stuck. This asks the one question
// that resolves it: which emission lines does the filter pass?
//
// It deliberately does NOT ask for wavelengths or passband widths. Those come
// from the shipped database once the lines are known, and most people do not
// have them to hand.
//
// All the logic lives in canonical_for_lines() in lib/io, which is unit
// tested. This file is only the window.

#ifndef __NukeX_FilterDialog_h
#define __NukeX_FilterDialog_h

#include <pcl/Dialog.h>
#include <pcl/Sizer.h>
#include <pcl/Label.h>
#include <pcl/CheckBox.h>
#include <pcl/PushButton.h>

#include <string>

namespace pcl {

struct FilterTeachResult {
    bool        saved = false;   // false when the user cancelled
    std::string canonical;       // e.g. "HaO3S2"; empty unless saved
};

class FilterDialog : public Dialog {
public:
    /// `filter_name` is the raw FITS FILTER value that stopped the batch.
    explicit FilterDialog(const std::string& filter_name);

    FilterTeachResult Run();

private:
    VerticalSizer   root_;
    Label           title_, hint_, hb_note_, status_;
    CheckBox        ha_, oiii_, sii_;
    HorizontalSizer buttons_;
    PushButton      save_, cancel_;

    std::string       filter_name_;
    FilterTeachResult result_;

    /// Re-reads the ticks, updates the status line and enables Save only when
    /// the set maps to a filter the database can actually solve. Ha+SII
    /// without OIII has no calibrated entry, so it is refused here rather than
    /// accepted and failed later.
    void Revalidate();

    void OnToggle(Button&, bool);
    void OnSaveClick(Button&, bool);
    void OnCancelClick(Button&, bool);
};

} // namespace pcl

#endif
