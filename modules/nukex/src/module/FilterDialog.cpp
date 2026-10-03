// NukeX -- teach NukeX an unrecognised filter name
// Copyright (c) 2026 Scott Carter. MIT License.

#include "FilterDialog.h"

#include "nukex/io/filter_alias.hpp"

#include <set>

namespace pcl {

using nukex::QSolveLine;

FilterDialog::FilterDialog(const std::string& filter_name)
    : filter_name_(filter_name)
{
    SetWindowTitle("Unrecognised filter");

    title_.SetText(String("NukeX does not recognise FILTER = '")
                   + filter_name_.c_str() + "'.");
    hint_.SetText("Which emission lines does it pass?");

    ha_.SetText("H-alpha  (656 nm)");
    oiii_.SetText("OIII  (501 nm)");
    sii_.SetText("SII  (672 nm)");

    // Hb is not offered on purpose, and saying why is cheaper than fielding
    // the question. At 486 nm it lands on the same blue and green photosites
    // as OIII, so a solve carrying both has no unique answer. A quad-band
    // filter is described by its other three lines.
    hb_note_.SetText("H-beta is not listed: on a colour sensor it cannot be "
                     "separated from OIII.");

    for (CheckBox* c : { &ha_, &oiii_, &sii_ })
        c->OnClick((pcl::Button::click_event_handler)&FilterDialog::OnToggle, *this);

    save_.SetText("Save and re-stack");
    cancel_.SetText("Cancel");
    save_.OnClick((pcl::Button::click_event_handler)&FilterDialog::OnSaveClick, *this);
    cancel_.OnClick((pcl::Button::click_event_handler)&FilterDialog::OnCancelClick, *this);

    buttons_.Add(save_);
    buttons_.AddSpacing(8);
    buttons_.Add(cancel_);
    buttons_.AddStretch();

    root_.Add(title_);
    root_.AddSpacing(6);
    root_.Add(hint_);
    root_.AddSpacing(6);
    root_.Add(ha_);
    root_.Add(oiii_);
    root_.Add(sii_);
    root_.AddSpacing(6);
    root_.Add(hb_note_);
    root_.AddSpacing(6);
    root_.Add(status_);
    root_.AddSpacing(8);
    root_.Add(buttons_);

    SetSizer(root_);
    Revalidate();
    AdjustToContents();
    SetFixedSize();
}

void FilterDialog::Revalidate() {
    std::set<QSolveLine> lines;
    if (ha_.IsChecked())   lines.insert(QSolveLine::Ha);
    if (oiii_.IsChecked()) lines.insert(QSolveLine::OIII);
    if (sii_.IsChecked())  lines.insert(QSolveLine::SII);

    const std::string canonical = nukex::canonical_for_lines(lines);

    if (lines.empty()) {
        status_.SetText("Tick at least one line.");
    } else if (canonical.empty()) {
        // The only reachable case today is Ha+SII without OIII.
        status_.SetText("NukeX has no calibrated filter for that combination. "
                        "Add OIII, or untick one.");
    } else {
        status_.SetText(String("Will be stacked as ") + canonical.c_str() + ".");
    }
    save_.Enable(!canonical.empty());
}

void FilterDialog::OnToggle(Button&, bool) {
    Revalidate();
}

void FilterDialog::OnSaveClick(Button&, bool) {
    std::set<QSolveLine> lines;
    if (ha_.IsChecked())   lines.insert(QSolveLine::Ha);
    if (oiii_.IsChecked()) lines.insert(QSolveLine::OIII);
    if (sii_.IsChecked())  lines.insert(QSolveLine::SII);

    result_.canonical = nukex::canonical_for_lines(lines);
    if (result_.canonical.empty()) {
        // Save is disabled in this state, so reaching here would mean the
        // button and the validation disagreed. Refuse rather than write a
        // filter name the database does not carry.
        Revalidate();
        return;
    }
    result_.saved = true;
    Ok();
}

void FilterDialog::OnCancelClick(Button&, bool) {
    result_.saved = false;
    Cancel();
}

FilterTeachResult FilterDialog::Run() {
    Execute();
    return result_;
}

} // namespace pcl
