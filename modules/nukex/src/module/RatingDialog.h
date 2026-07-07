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
    // filter_class: nukex::FilterClass identity code (nukex/core/filter.hpp) --
    // UNKNOWN=0, BROADBAND_L=1, BROADBAND_RGB=2, BROADBAND_OSC=3,
    // NARROWBAND_SINGLE=4, DUAL_NB_OSC=5.
    // Color axis is shown iff the class is a colour-capable broadband
    // mosaic/OSC frame: BROADBAND_RGB(2) or BROADBAND_OSC(3). Mono, narrowband,
    // and dual-NB OSC runs hide the color slider.
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
