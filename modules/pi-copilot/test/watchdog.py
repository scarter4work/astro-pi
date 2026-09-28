#!/usr/bin/env python3
# PI Copilot self-test harness: section watchdog (plan item 1, thist-hang-investigation.md).
#
# run-selftest.sh backgrounds this alongside the foreground PixInsight run. It
# finds the PI process for our reserved test slot, checks (once) that the
# module PI actually mapped is the one we asked for via -m= (item 3), then
# polls the two section-timing files the JS driver and the C++ module both
# keep rewriting ($PICOPILOT_SELFTEST_JS_TIMINGS / _SECTION_TIMINGS -- see
# SelfTestTiming.h and selftest.js's jsMark()) for how long the CURRENTLY OPEN
# section has been open. When that exceeds its budget (section-budgets.json,
# a generous multiple of a real measured baseline, or an explicit cap), it
# captures an all-thread gdb backtrace (symbol names only -- NEVER disassemble
# PixInsight; see ~/.claude/CLAUDE.md) and an Xvfb screenshot, kills PI, writes
# a fail-marker JSON for run-selftest.sh to report, and exits.
#
# This replaces "sit and wait for the 900s timeout with no evidence" (the
# T-hist "Out of memory" modal hang) with "fail loudly, fast, with a stack".

import argparse
import json
import os
import signal
import subprocess
import sys
import time


def log(msg):
    print("WATCHDOG: %s" % msg, file=sys.stderr, flush=True)


def read_json(path):
    try:
        with open(path) as f:
            return json.load(f)
    except (OSError, ValueError):
        return None


def find_pi_pid(slot):
    """Scan /proc for the real PixInsight binary (not the .sh wrapper) running
    our reserved -n=<slot>. Mirrors the matching the thist/watch.sh prototype
    used, but parses argv from /proc/<pid>/cmdline instead of a cmdline regex,
    so it doesn't care about exact spacing."""
    needle = "-n=%s" % slot
    for entry in os.listdir("/proc"):
        if not entry.isdigit():
            continue
        pid = int(entry)
        try:
            with open("/proc/%s/cmdline" % pid, "rb") as f:
                raw = f.read()
        except OSError:
            continue
        if not raw:
            continue
        argv = raw.decode("utf-8", "replace").split("\0")
        if not argv or not argv[0]:
            continue
        if not argv[0].endswith("/PixInsight"):
            continue
        if needle in argv:
            return pid, argv
    return None, None


def pid_alive(pid):
    try:
        os.kill(pid, 0)
    except OSError:
        return False
    return True


def module_so_from_argv(argv):
    for a in argv:
        if a.startswith("-m="):
            return a[3:]
    return None


def picopilot_mappings(pid, basename):
    """Realpaths of every file named <basename> mapped in /proc/<pid>/maps
    (None if the process is gone)."""
    try:
        with open("/proc/%s/maps" % pid) as f:
            maps = f.read()
    except OSError:
        return None
    found = set()
    for line in maps.splitlines():
        parts = line.split(None, 5)
        if len(parts) == 6 and os.path.basename(parts[5]) == basename:
            found.add(os.path.realpath(parts[5]))
    return found


def check_module_load(pid, argv, expected_so_cli, timeout_s):
    """Item 3 (made strict 2026-09-27): the -m= dev module must be mapped, and
    NO other file named PICopilot-pxm.so may ever be. The old check took the
    FIRST PICopilot-pxm.so line in /proc/<pid>/maps, but on a slot that did
    PixInsight's first-run module scan both the installed
    /opt/PixInsight/bin/PICopilot-pxm.so and the dev build are mapped, the
    installed one ~70 ms earlier (measured) -- so the verdict depended on
    where a 0.5 s poll landed: the intermittent "module-load mismatch".
    harness-lib.sh now seeds the slot so the installed one is never loaded;
    this check stays loud in case anything reintroduces it.
    Returns (ok, reason, expected_realpath, foreign_realpaths)."""
    expected = (argv and module_so_from_argv(argv)) or expected_so_cli
    if not expected:
        return False, "no -m= module to verify", None, []
    expected_real = os.path.realpath(expected)
    basename = os.path.basename(expected_real)
    deadline = time.time() + timeout_s
    while time.time() < deadline:
        found = picopilot_mappings(pid, basename)
        if found is None:
            return True, None, expected_real, []   # process already gone; the run's own exit status reports that
        foreign = sorted(found - {expected_real})
        if foreign:
            return False, "module-mismatch", expected_real, foreign
        if expected_real in found:
            return True, None, expected_real, []
        time.sleep(0.1)
    return False, "module-not-loaded", expected_real, []


def resolve_cap(budgets, source, section, floor_s, multiplier, default_cap):
    entry = (budgets or {}).get(source, {}).get(section)
    if entry is None:
        return default_cap, None
    if isinstance(entry, dict):
        if "cap" in entry:
            return float(entry["cap"]), entry.get("baseline")
        baseline = float(entry.get("baseline", 0.0))
        return max(floor_s, baseline * multiplier), baseline
    baseline = float(entry)
    return max(floor_s, baseline * multiplier), baseline


def parse_overrides(spec):
    out = {}
    if not spec:
        return out
    for part in spec.split(","):
        part = part.strip()
        if not part or "=" not in part:
            continue
        name, val = part.split("=", 1)
        try:
            out[name.strip()] = float(val.strip())
        except ValueError:
            pass
    return out


def read_environ(pid):
    try:
        with open("/proc/%s/environ" % pid, "rb") as f:
            raw = f.read()
    except OSError:
        return {}
    out = {}
    for chunk in raw.split(b"\0"):
        if b"=" in chunk:
            k, _, v = chunk.partition(b"=")
            out[k.decode("utf-8", "replace")] = v.decode("utf-8", "replace")
    return out


def capture_artifacts(pid, log_dir, gdb_timeout):
    os.makedirs(log_dir, exist_ok=True)
    stacks_path = os.path.join(log_dir, "stacks.txt")
    shot_path = os.path.join(log_dir, "screenshot.png")

    # All-thread backtrace, symbol names only. NEVER add -ex "disassemble" or
    # similar here -- the PixInsight EULA forbids disassembling the binary,
    # and a plain backtrace never needs to.
    gdb_cmd = [
        "gdb", "-batch", "-nx",
        "-ex", "set pagination off",
        "-ex", "set confirm off",
        "-ex", "set debuginfod enabled off",
        "-p", str(pid),
        "-ex", "thread apply all bt",
    ]
    try:
        with open(stacks_path, "wb") as out:
            subprocess.run(gdb_cmd, stdout=out, stderr=subprocess.STDOUT,
                            timeout=gdb_timeout, check=False)
    except Exception as e:  # noqa: BLE001 -- best-effort capture, never let this hide the real FAIL
        with open(stacks_path, "a") as out:
            out.write("\n[watchdog: gdb capture failed: %r]\n" % (e,))

    env = read_environ(pid)
    display = env.get("DISPLAY")
    if display:
        shot_env = dict(os.environ)
        shot_env["DISPLAY"] = display
        if env.get("XAUTHORITY"):
            shot_env["XAUTHORITY"] = env["XAUTHORITY"]
        try:
            subprocess.run(["import", "-window", "root", shot_path],
                            env=shot_env, timeout=30, check=False,
                            stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        except Exception as e:  # noqa: BLE001
            with open(os.path.join(log_dir, "screenshot-error.txt"), "w") as f:
                f.write("%r\n" % (e,))
    else:
        with open(os.path.join(log_dir, "screenshot-error.txt"), "w") as f:
            f.write("no DISPLAY found in /proc/%s/environ\n" % pid)

    return stacks_path, shot_path if os.path.exists(shot_path) else None


def kill_pi(pid):
    try:
        os.kill(pid, signal.SIGTERM)
    except OSError:
        return
    for _ in range(25):  # 5s
        if not pid_alive(pid):
            return
        time.sleep(0.2)
    try:
        os.kill(pid, signal.SIGKILL)
    except OSError:
        pass


def write_marker(path, payload):
    tmp = path + ".tmp"
    with open(tmp, "w") as f:
        json.dump(payload, f, indent=2)
    os.replace(tmp, path)


def fail_module(args, pid, reason, expected_real, actual):
    log_dir = os.path.join(args.log_dir_base, "%s-%d-%d" % (reason, int(time.time()), pid))
    os.makedirs(log_dir, exist_ok=True)
    detail_path = os.path.join(log_dir, "module-check.txt")
    with open(detail_path, "w") as f:
        f.write("reason: %s\nexpected (-m=, realpath): %s\nactual: %s\n" % (reason, expected_real, actual))
    # Marker MUST land before we kill PI: run-selftest.sh's foreground
    # `timeout ... "$PI" ...` unblocks the instant PI dies, and it then
    # kills this watchdog process almost immediately afterwards -- if the
    # marker were written after kill_pi(), that race can (and, measured,
    # does) reap us before write_marker() ever runs, so run-selftest.sh
    # sees a plain nonzero exit with no marker and reports the generic
    # timeout/FAIL instead of this specific, evidenced one.
    write_marker(args.fail_marker, {
        "reason": reason,
        "expected": expected_real,
        "actual": actual,
        "pid": pid,
        "log_dir": log_dir,
        "detail": detail_path,
    })
    kill_pi(pid)
    log("FAIL %s: expected %s, got %s" % (reason, expected_real, actual))
    return 0


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--slot", required=True)
    ap.add_argument("--module-so", default=None, help="fallback if -m= can't be read from argv")
    ap.add_argument("--js-timings", required=True)
    ap.add_argument("--section-timings", required=True)
    ap.add_argument("--budgets", required=True)
    ap.add_argument("--fail-marker", required=True)
    ap.add_argument("--log-dir-base", required=True)
    ap.add_argument("--poll-interval", type=float, default=2.0)
    ap.add_argument("--floor", type=float, default=60.0)
    ap.add_argument("--multiplier", type=float, default=5.0)
    ap.add_argument("--default-cap", type=float, default=90.0)
    ap.add_argument("--override", default="")
    ap.add_argument("--gdb-timeout", type=float, default=90.0)
    ap.add_argument("--module-load-timeout", type=float, default=90.0)
    ap.add_argument("--ok-marker", default=None,
                    help="written once the module-load (and isolation) checks passed")
    ap.add_argument("--xdg-data-home", default=None,
                    help="the private XDG_DATA_HOME the PI process must carry (data isolation)")
    ap.add_argument("--pid-discovery-timeout", type=float, default=120.0)
    args = ap.parse_args()

    budgets = read_json(args.budgets) or {}
    overrides = parse_overrides(args.override)

    log("waiting for PI (slot %s)..." % args.slot)
    deadline = time.time() + args.pid_discovery_timeout
    pid, argv = None, None
    while time.time() < deadline:
        pid, argv = find_pi_pid(args.slot)
        if pid is not None:
            break
        time.sleep(0.5)
    if pid is None:
        log("PI for slot %s never appeared within %ss; nothing to watch, exiting" %
            (args.slot, args.pid_discovery_timeout))
        return 0
    log("found PI pid=%d" % pid)

    if args.xdg_data_home:
        env = read_environ(pid)
        if env.get("XDG_DATA_HOME") != args.xdg_data_home:
            return fail_module(args, pid, "isolation-missing", args.xdg_data_home,
                               ["XDG_DATA_HOME=%r in the PI process" % env.get("XDG_DATA_HOME")])

    ok, reason, expected_real, foreign = check_module_load(pid, argv, args.module_so, args.module_load_timeout)
    if not ok:
        return fail_module(args, pid, reason, expected_real, foreign)
    log("module-load check: only %s is mapped" % expected_real)
    if args.ok_marker:
        write_marker(args.ok_marker, {"module": expected_real, "pid": pid,
                                      "xdgDataHome": read_environ(pid).get("XDG_DATA_HOME")})
    basename = os.path.basename(expected_real)

    tracked_key = None       # (section, source)
    tracked_since = None

    while True:
        if not pid_alive(pid):
            log("PI pid=%d gone; run finished, watchdog exiting" % pid)
            return 0

        found = picopilot_mappings(pid, basename)
        if found is not None and found - {expected_real}:
            return fail_module(args, pid, "module-mismatch", expected_real, sorted(found - {expected_real}))

        section_js = read_json(args.js_timings)
        section_mod = read_json(args.section_timings)
        open_mod = section_mod.get("open") if section_mod else None
        open_js = section_js.get("open") if section_js else None

        if open_mod:
            cur = (open_mod["section"], "module")
        elif open_js:
            cur = (open_js["section"], "js")
        else:
            cur = None

        now = time.monotonic()
        if cur != tracked_key:
            tracked_key = cur
            tracked_since = now

        if cur is not None:
            elapsed = now - tracked_since
            section, source = cur
            if section in overrides:
                cap = overrides[section]
                baseline = None
            else:
                cap, baseline = resolve_cap(budgets, source, section, args.floor, args.multiplier, args.default_cap)
            if elapsed > cap:
                log("section overrun: [%s] %r open %.1fs > cap %.1fs (baseline=%s) -- capturing evidence and killing pid=%d" %
                    (source, section, elapsed, cap, baseline, pid))
                ts = time.strftime("%Y%m%dT%H%M%S")
                log_dir = os.path.join(args.log_dir_base, "%s-slot%s-pid%d" % (ts, args.slot, pid))
                stacks_path, shot_path = capture_artifacts(pid, log_dir, args.gdb_timeout)
                # Marker MUST land before we kill PI -- see the identical note
                # in the module-mismatch branch above; killing PI first races
                # run-selftest.sh's own cleanup of this watchdog process
                # against write_marker() actually finishing (measured: it
                # lost that race on the very first end-to-end proof run).
                write_marker(args.fail_marker, {
                    "reason": "section-overrun",
                    "section": section,
                    "source": source,
                    "elapsed": elapsed,
                    "cap": cap,
                    "baseline": baseline,
                    "pid": pid,
                    "log_dir": log_dir,
                    "stacks": stacks_path,
                    "screenshot": shot_path,
                })
                kill_pi(pid)
                log("FAIL written to marker; artefacts in %s" % log_dir)
                return 0

        time.sleep(args.poll_interval)


if __name__ == "__main__":
    sys.exit(main())
