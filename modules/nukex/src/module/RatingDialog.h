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
    // filter_class: rating-DB schema v2 ints (1 BROADBAND_L, 2 BROADBAND_RGB,
    // 3 BROADBAND_OSC, 4 NARROWBAND_SINGLE, 5 DUAL_NB_OSC, 0 UNKNOWN).
    // The color axis is shown only when has_color_axis(filter_class).
    RatingDialog(int filter_class);
    static bool has_color_axis(int filter_class) { return filter_class == 2 || filter_class == 3; }

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
