# gdb (attached by test/quit-mid-tick.sh). For every JourneyService::Stop() it
# records whether a journey tick was on the stack -- the proof that the test's
# Quit really ran OnUnload inside a tick (otherwise a clean exit proves nothing)
# -- and for every ~JourneyTracker() whether JourneyTracker::Tick was: the
# invariant the fix restores is that the tracker is never destroyed under its
# own tick (0.2.2.0 freed it there; the crash itself depends on what the freed
# memory holds by then). Output: $QUIT_MID_TICK_STOP_LOG, a line per event
# followed by its backtrace.
import os
import gdb

LOG = os.environ["QUIT_MID_TICK_STOP_LOG"]


def note(line, bt):
    with open(LOG, "a") as f:
        f.write(line + "\n" + bt + "\n")


class StopBp(gdb.Breakpoint):
    def stop(self):
        bt = gdb.execute("bt 80", to_string=True)
        # OnTick too: an optimised build may inline Tick into it (same translation unit).
        nested = "pcl::JourneyTracker::Tick" in bt or "pcl::JourneyService::OnTick" in bt
        note("stop tick-on-stack=%d" % (1 if nested else 0), bt)
        return False


class TrackerDtorBp(gdb.Breakpoint):
    def stop(self):
        bt = gdb.execute("bt 80", to_string=True)
        note("tracker-destroyed tick-on-stack=%d" % (1 if "pcl::JourneyTracker::Tick" in bt else 0), bt)
        return False


gdb.execute("set breakpoint pending on")
StopBp("pcl::JourneyService::Stop")
TrackerDtorBp("pcl::JourneyTracker::~JourneyTracker")
with open(LOG, "a") as f:
    f.write("armed\n")
