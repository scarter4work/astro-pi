// NukeX v4 — Phase 8 rating popup
// Copyright (c) 2026 Scott Carter. MIT License.

#include "RatingDialog.h"

namespace pcl {

static void init_signed_slider(HorizontalSlider& s) {
    s.SetRange(-2, 2);
    s.SetValue(0);
    s.SetMinWidth(120);
}

RatingDialog::RatingDialog(int filter_class) : filter_class_(filter_class) {
    SetWindowTitle("Rate last NukeX run");

    title_.SetText("Nudge what felt off, then Save.");

    brightness_label_.SetText("Brightness  (dim <-> bright)");
    init_signed_slider(brightness_);

    saturation_label_.SetText("Saturation  (washed <-> pumped)");
    init_signed_slider(saturation_);

    color_label_.SetText("Color  (cool <-> warm)");
    init_signed_slider(color_);

    star_bloat_label_.SetText("Stars  (tight <-> bloated)");
    init_signed_slider(star_bloat_);

    overall_label_.SetText("Overall  (1-5)");
    overall_.SetRange(1, 5);
    overall_.SetValue(3);

    dont_show_again_.SetText("Don't show after Execute");

    save_.SetText("Save");
    skip_.SetText("Skip");
    save_.OnClick((pcl::Button::click_event_handler)&RatingDialog::OnSaveClick, *this);
    skip_.OnClick((pcl::Button::click_event_handler)&RatingDialog::OnSkipClick, *this);

    buttons_.Add(save_);
    buttons_.AddSpacing(8);
    buttons_.Add(skip_);
    buttons_.AddStretch();

    root_.Add(title_);
    root_.AddSpacing(8);
    root_.Add(brightness_label_); root_.Add(brightness_);
    root_.Add(saturation_label_); root_.Add(saturation_);

    // Hide color axis for mono / narrowband.
    if (filter_class_ == 1 /* Bayer_RGB */) {
        root_.Add(color_label_); root_.Add(color_);
    }

    root_.Add(star_bloat_label_); root_.Add(star_bloat_);
    root_.Add(overall_label_);    root_.Add(overall_);
    root_.AddSpacing(8);
    root_.Add(dont_show_again_);
    root_.AddSpacing(8);
    root_.Add(buttons_);

    SetSizer(root_);
    AdjustToContents();
    SetFixedSize();
}

void RatingDialog::OnSaveClick(Button&, bool) {
    result_.saved            = true;
    result_.dont_show_again  = dont_show_again_.IsChecked();
    result_.brightness       = brightness_.Value();
    result_.saturation       = saturation_.Value();
    if (filter_class_ == 1) result_.color = color_.Value();
    result_.star_bloat       = star_bloat_.Value();
    result_.overall          = overall_.Value();
    Ok();
}

void RatingDialog::OnSkipClick(Button&, bool) {
    result_.saved = false;
    result_.dont_show_again = dont_show_again_.IsChecked();
    Cancel();
}

RatingResult RatingDialog::Run() {
    Execute();
    return result_;
}

} // namespace pcl
