# gdb (attached by test/quit-in-turn.sh). One line per event in $QUIT_IN_TURN_LOG, each followed by its
# backtrace:
#   stop in-tool=<0|1> in-keep=<0|1>          JourneyService::Stop() (OnUnload), and whether it ran inside a
#                                             Copilot tool (e_Poll_Timer's tool loop) or ★ Keep's flow
#   teardown                                  JourneyService::Teardown (the deferred destruction)
#   start-request after-stop=<0|1>            PICopilotInterface::StartRequest (a new model request)
#   keeper-keep after-stop=<0|1>              KeeperExporter::Keep (a keep and its write-up)
#   interface-destroyed in-turn=<0|1>         ~PICopilotInterface, and whether e_Poll_Timer / the keep flow
#                                             was still on the stack (it would resume in a freed object)
import os
import gdb

LOG = os.environ["QUIT_IN_TURN_LOG"]
state = {"stopped": False}


def note(line):
    bt = gdb.execute("bt 90", to_string=True)
    with open(LOG, "a") as f:
        f.write(line + "\n" + bt + "\n")
    return bt


def on_stack(bt, *names):
    return 1 if any(n in bt for n in names) else 0


class Bp(gdb.Breakpoint):
    def __init__(self, spec, fn):
        super().__init__(spec)
        self.fn = fn

    def stop(self):
        self.fn()
        return False


def stop():
    bt = gdb.execute("bt 90", to_string=True)
    tool = on_stack(bt, "pcl::ExecuteTool") and on_stack(bt, "PICopilotInterface::e_Poll_Timer")
    keep = on_stack(bt, "PICopilotInterface::KeepJourneyOfView")
    with open(LOG, "a") as f:
        f.write("stop in-tool=%d in-keep=%d\n%s\n" % (tool, keep, bt))
    state["stopped"] = True


gdb.execute("set breakpoint pending on")
Bp("pcl::JourneyService::Stop", stop)
Bp("pcl::JourneyService::Teardown", lambda: note("teardown"))
Bp("pcl::PICopilotInterface::StartRequest", lambda: note("start-request after-stop=%d" % state["stopped"]))
Bp("pcl::KeeperExporter::Keep", lambda: note("keeper-keep after-stop=%d" % state["stopped"]))
def interface_destroyed():
    bt = gdb.execute("bt 90", to_string=True)
    in_turn = on_stack(bt, "PICopilotInterface::e_Poll_Timer", "PICopilotInterface::KeepJourneyOfView")
    with open(LOG, "a") as f:
        f.write("interface-destroyed in-turn=%d\n%s\n" % (in_turn, bt))


Bp("pcl::PICopilotInterface::~PICopilotInterface", interface_destroyed)
with open(LOG, "a") as f:
    f.write("armed\n")
