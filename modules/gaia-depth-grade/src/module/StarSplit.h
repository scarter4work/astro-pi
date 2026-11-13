// The star / starless split (plan Task 16; port of the PJSR splitStars): StarXTerminator through
// ProcessInstance, or two layers the user supplies (StarNet2 or any other tool).
#ifndef __GDG_StarSplit_h
#define __GDG_StarSplit_h

#include "core/Log.h"

#include <pcl/ImageWindow.h>
#include <pcl/View.h>

namespace pcl
{

/// Review Focus 5, verbatim from splitStars.
extern const char* const kStarXTerminatorRequired;

/// Whether the StarXTerminator process is installed. In test builds (GDG_PROBES) the
/// environment variable GDG_TEST_HIDE_SXT=1 makes it report false (probe
/// split_requires_sxt_message); release builds never read it.
bool StarXTerminatorInstalled();

/// Runs StarXTerminator on `work`'s main view (output_stars = true, unscreen = true: a linear
/// stars image a screen blend recombines), which leaves the starless image in `work`, and
/// returns the new stars window, found by diffing the open windows (not by its name). Throws
/// pcl::Error: kStarXTerminatorRequired when it is not installed, "StarXTerminator failed on
/// <id>", or "StarXTerminator produced no new stars window" (any other new window is closed).
ImageWindow SplitWithStarXTerminator( ImageWindow& work, const gdg::Log& log );

/// The view `id` as a supplied layer (`what`: "stars" or "starless"): it must exist and have
/// the target's width, height and nominal channel count. Throws pcl::Error naming the problem.
View SuppliedLayer( const String& id, const char* what, int width, int height, int channels );

} // namespace pcl

#endif // __GDG_StarSplit_h
