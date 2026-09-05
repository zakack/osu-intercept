#!/usr/bin/env python3
"""
e2e - drive doubletapd end to end through synthetic keyboards.

This is the outside-in counterpart to tools/regimetest.c. regimetest calls
the daemon's functions directly; this one runs the real binary, feeds it
real evdev events from a uinput device, and reads what comes out of the
daemon's own virtual keyboard. It is the only thing that exercises the
grab, the epoll loop, auto-discovery and the uinput write path as a whole.

    ./build/doubletapd must exist:  cmake --build build
    needs python-evdev            :  pacman -S python-evdev
    needs membership of the `input` group (for /dev/uinput)

    usage: tools/e2e.py [--keep-going] [--daemon PATH]

The synthetic source device deliberately carries KEY_1..KEY_5 and KEY_A and
NOTHING ELSE. A running doubletap.service auto-grabs any keyboard that
advertises all of ITS configured keys, and would take the device away from
the daemon under test - which shows up as every assertion emitting nothing
at all, not as an error. Keeping the pool away from the usual suspects
(KEY_Z/KEY_X/KEY_C) lets this run without stopping the user's session.
"""

import argparse
import glob
import os
import shutil
import subprocess
import sys
import tempfile
import time

try:
    from evdev import UInput, InputDevice, ecodes as e
except ImportError:
    sys.exit("e2e: python-evdev is not installed (pacman -S python-evdev)")

POOL = [e.KEY_1, e.KEY_2, e.KEY_3, e.KEY_4]
POOL_NAMES = ["KEY_1", "KEY_2", "KEY_3", "KEY_4"]
V1, V2 = e.KEY_F13, e.KEY_F14
OUT_NAME = "doubletap e2e virtual out"
SRC_NAME = "doubletap e2e synthetic src"

# The daemon settles a hotplug through inotify plus a udev-permission retry,
# so give the initial reconcile room; the per-step waits only have to cover
# one trip through epoll.
SETTLE, STEP = 1.3, 0.12

CONFIG = """\
socd: {socd}
keys:
  pool: [{pool}]
  v1: KEY_F13
  v2: KEY_F14
audio: {{ enabled: false }}
uinput: {{ name: "{out}" }}
"""


class Harness:
    """One daemon, one synthetic keyboard, torn down together."""

    def __init__(self, daemon, socd, caps=None):
        self.daemon, self.socd = daemon, socd
        self.caps = caps or [e.KEY_1, e.KEY_2, e.KEY_3, e.KEY_4, e.KEY_5, e.KEY_A]
        self.src = self.proc = self.virt = None
        self.dir = tempfile.mkdtemp(prefix="doubletap-e2e-")

    def __enter__(self):
        self.src = UInput({e.EV_KEY: self.caps}, name=SRC_NAME,
                          bustype=e.BUS_USB, vendor=0x1234, product=0x5678)
        time.sleep(0.4)
        indir = os.path.join(self.dir, "input")
        os.makedirs(indir)
        os.symlink(self.src.device.path, os.path.join(indir, "event0"))

        cfgpath = os.path.join(self.dir, "config.yaml")
        with open(cfgpath, "w") as f:
            f.write(CONFIG.format(socd=self.socd, out=OUT_NAME,
                                  pool=", ".join(POOL_NAMES)))

        self.proc = subprocess.Popen(
            [self.daemon, "-c", cfgpath, "-i", indir],
            stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
        time.sleep(SETTLE)
        return self

    def __exit__(self, *exc):
        if self.proc and self.proc.poll() is None:
            self.proc.terminate()
            try:
                self.proc.wait(timeout=5)
            except subprocess.TimeoutExpired:
                self.proc.kill()
        if self.src:
            self.src.close()
        shutil.rmtree(self.dir, ignore_errors=True)

    def log(self):
        if not self.proc:
            return ""
        try:
            return self.proc.communicate(timeout=5)[0] or ""
        except subprocess.TimeoutExpired:
            return ""

    def find_output(self):
        """The daemon's virtual keyboard, by the name this config gave it."""
        for path in sorted(glob.glob("/dev/input/event*")):
            try:
                dev = InputDevice(path)
            except OSError:
                continue
            if dev.name == OUT_NAME:
                self.virt = dev
                return dev
        return None

    def emit(self, code, value):
        self.src.write(e.EV_KEY, code, value)
        self.src.syn()
        time.sleep(0.05)

    def drain(self):
        """Virtual key transitions since the last call, as 'v1+ v2-' text."""
        out = []
        try:
            for ev in self.virt.read():
                if (ev.type == e.EV_KEY and ev.value in (0, 1)
                        and ev.code in (V1, V2)):
                    out.append(("v1" if ev.code == V1 else "v2")
                               + ("+" if ev.value else "-"))
        except BlockingIOError:
            pass
        return " ".join(out)


class Report:
    def __init__(self, keep_going):
        self.fails = 0
        self.keep_going = keep_going

    def check(self, label, got, want):
        if got == want:
            print(f"  ok   {label:46s} [{got or 'nothing'}]")
        else:
            self.fails += 1
            print(f"  FAIL {label:46s} [{got}]  want [{want}]")
            if not self.keep_going:
                raise AssertionError(label)


def suite_toggle(h, r):
    """Sticky assignment, alternation across the pool, and slot sharing."""
    def step(label, actions, want):
        for code, value in actions:
            h.emit(code, value)
        time.sleep(STEP)
        r.check(label, h.drain(), want)

    print("toggle: one key at a time")
    # Seeded by index, so each key has its own virtual and taps alternate
    # across the pool without any two keys ever being down together.
    for i, want in enumerate(["v1+ v1-", "v2+ v2-", "v1+ v1-", "v2+ v2-"]):
        step(f"tap {POOL_NAMES[i]}", [(POOL[i], 1), (POOL[i], 0)], want)
    step("tap KEY_1 again - sticky, same virtual",
         [(POOL[0], 1), (POOL[0], 0)], "v1+ v1-")

    print("toggle: overlapping, which is how it is played")
    step("1 down", [(POOL[0], 1)], "v1+")
    step("2 down - the toggle steals", [(POOL[1], 1)], "v1- v2+")
    step("1 up - reverting toggle", [(POOL[0], 0)], "v2- v1+")
    step("3 down - takes the free slot", [(POOL[2], 1)], "v1- v2+")
    step("2 up", [(POOL[1], 0)], "v2- v1+")
    step("3 up - last key released", [(POOL[2], 0)], "v1-")

    print("toggle: a third key with both slots held")
    step("1 down", [(POOL[0], 1)], "v1+")
    step("2 down", [(POOL[1], 1)], "v1- v2+")
    step("3 down - shares a slot, still a note", [(POOL[2], 1)], "v2- v1+")
    step("3 up - its slot-mate still holds it", [(POOL[2], 0)], "")
    step("1 up - now the slot empties", [(POOL[0], 0)], "v1- v2+")
    step("2 up", [(POOL[1], 0)], "v2-")

    step("a non-pool key passes through untouched",
         [(e.KEY_A, 1), (e.KEY_A, 0)], "")


def suite_off(h, r):
    """`off` drops the sticky rule once the pool is larger than two."""
    def step(label, actions, want):
        for code, value in actions:
            h.emit(code, value)
        time.sleep(STEP)
        r.check(label, h.drain(), want)

    print("off: repeated taps of ONE key alternate")
    step("tap KEY_1", [(POOL[0], 1), (POOL[0], 0)], "v1+ v1-")
    step("  tap it again - alternates", [(POOL[0], 1), (POOL[0], 0)], "v2+ v2-")
    step("  and back", [(POOL[0], 1), (POOL[0], 0)], "v1+ v1-")
    step("  KEY_3 follows the same alternation",
         [(POOL[2], 1), (POOL[2], 0)], "v2+ v2-")

    print("off: no SOCD cleaning - the two slots are independent")
    step("1 and 2 down", [(POOL[0], 1), (POOL[1], 1)], "v1+ v2+")
    step("  and up", [(POOL[0], 0), (POOL[1], 0)], "v1- v2-")


def test_discovery(daemon, r):
    """auto_grab_ok must require EVERY pool key, not merely some."""
    print("discovery: a board missing one pool key is not grabbed")
    partial = UInput({e.EV_KEY: [e.KEY_1, e.KEY_2, e.KEY_3, e.KEY_A]},
                     name="doubletap e2e partial", bustype=e.BUS_USB,
                     vendor=0x1234, product=0x9999)
    full = UInput({e.EV_KEY: [e.KEY_1, e.KEY_2, e.KEY_3, e.KEY_4, e.KEY_A]},
                  name="doubletap e2e full", bustype=e.BUS_USB,
                  vendor=0x1234, product=0x8888)
    tmp = tempfile.mkdtemp(prefix="doubletap-e2e-")
    proc = None
    try:
        time.sleep(0.4)
        indir = os.path.join(tmp, "input")
        os.makedirs(indir)
        os.symlink(partial.device.path, os.path.join(indir, "event0"))
        os.symlink(full.device.path, os.path.join(indir, "event1"))
        cfgpath = os.path.join(tmp, "config.yaml")
        with open(cfgpath, "w") as f:
            f.write(CONFIG.format(socd="toggle", out=OUT_NAME,
                                  pool=", ".join(POOL_NAMES)))
        proc = subprocess.Popen([daemon, "-c", cfgpath, "-i", indir],
                                stdout=subprocess.PIPE,
                                stderr=subprocess.STDOUT, text=True)
        time.sleep(SETTLE)
        proc.terminate()
        log = proc.communicate(timeout=5)[0] or ""
        grabbed = [l for l in log.splitlines() if "Opened and grabbed" in l]
        r.check("only the board with all four pool keys",
                f"{len(grabbed)} grabbed, full={any('full' in l for l in grabbed)}",
                "1 grabbed, full=True")
    finally:
        if proc and proc.poll() is None:
            proc.kill()
        partial.close()
        full.close()
        shutil.rmtree(tmp, ignore_errors=True)


def main():
    ap = argparse.ArgumentParser(description=__doc__.strip().splitlines()[0])
    ap.add_argument("--daemon", default="./build/doubletapd")
    ap.add_argument("--keep-going", action="store_true",
                    help="run every assertion instead of stopping at the first")
    args = ap.parse_args()

    if not os.access(args.daemon, os.X_OK):
        sys.exit(f"e2e: {args.daemon} is not executable - cmake --build build")
    if not os.access("/dev/uinput", os.W_OK):
        sys.exit("e2e: cannot write /dev/uinput - are you in the `input` group?")

    r = Report(args.keep_going)
    try:
        for socd, suite in (("toggle", suite_toggle), ("off", suite_off)):
            with Harness(args.daemon, socd) as h:
                if not h.find_output():
                    print(h.log(), file=sys.stderr)
                    sys.exit(f"e2e: daemon never created \"{OUT_NAME}\"")
                time.sleep(0.3)
                h.drain()                     # discard the startup dribble
                suite(h, r)
        test_discovery(args.daemon, r)
    except AssertionError:
        print("\nstopped at the first failure; --keep-going runs them all")

    print(f"\n{'FAILURES ABOVE' if r.fails else 'all assertions passed'}")
    return 1 if r.fails else 0


if __name__ == "__main__":
    sys.exit(main())
