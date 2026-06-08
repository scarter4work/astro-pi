// NukeX v4 — Phase 8 rating popup
// Copyright (c) 2026 Scott Carter. MIT License.

#ifndef __NukeX_RatingDialog_h
#define __NukeX_RatingDialog_h

#include <pcl/Dialog.h>
#include <pcl/Sizer.h>
#include <pcl/Label.h>
#include <pcl/Slider.h>
#include <pcl/CheckBox.h>
#include <pcl/PushButton.h>
#include <pcl/SpinBox.h>

#include <optional>

namespace pcl {

// Result of a rating dialog session.
struct RatingResult {
    bool                saved = false;
    bool                dont_show_again = false;
    int                 brightness = 0;
    int                 saturation = 0;
    std::optional<int>  color;   // nullopt for mono / narrowband
    int                 star_bloat = 0;
    int                 overall = 3;
};

class RatingDialog : public Dialog {
public:
    // filter_class: 0 = LRGB_mono, 1 = Bayer_RGB, 2 = Narrowband_HaO3, 3 = Narrowband_S2O3.
    // Color axis is hidden for filter_class != 1.
    RatingDialog(int filter_class);

    RatingResult Run();

private:
    VerticalSizer root_;
    Label         title_;
    Label         brightness_label_, saturation_label_, color_label_, star_bloat_label_, overall_label_;
    HorizontalSlider brightness_, saturation_, color_, star_bloat_;
    SpinBox       overall_;
    CheckBox      dont_show_again_;
    HorizontalSizer buttons_;
    PushButton    save_, skip_;

    RatingResult result_;
    int          filter_class_;

    void OnSaveClick(Button&, bool);
    void OnSkipClick(Button&, bool);
};

} // namespace pcl

#endif
