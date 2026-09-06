# doubletap

A SOCD cleaner for Linux — snap tap style key
handling as a daemon, turning a "rocking" two-finger pattern into
rapid-fire, gap-free key alternation on any keyboard.

![demo](docs/demo.gif)

`doubletapd` exclusively grabs your keyboard(s) at the evdev level — before
Xorg/Wayland ever see the events — applies a SOCD (Simultaneous Opposing
Cardinal Directions) state machine to a *pool* of configurable physical keys
(two by default, up to 16) that all drive the same two virtual keys, and
re-emits everything through a single virtual uinput keyboard. All other keys
pass through untouched. It also plays a click sound through PipeWire on every
virtual keypress, so you get tactile-style audio feedback even on the
re-pressed key.

## Features

- **Four SOCD modes**, selected by the `socd` config field:
  - `toggle` (default; `on` is a synonym) — *reverting toggle*: when both
    keys are held, the most recent press wins; releasing **either** key
    re-presses the other virtual key. Rocking your fingers between the two
    keys produces clean, gap-free alternation.
  - `snappy` — *last input wins* ("Snappy Tappy"): the most recent press
    wins, but only releasing the **active** key falls back to the still-held
    one; releasing the already-suppressed key does nothing.
  - `analog` — runs `off` and `toggle` in the *same session*, switching
    between them on how far the keys are actually pressed: plant both keys
    on the backplate and you get the toggle for rocking, tap and you get
    the plain remap. Adds a software rapid trigger. Requires an analog
    keyboard; see [Analog mode](#analog-mode).
  - `off` — no SOCD cleaning: the pool keys are simply remapped to v1/v2,
    and the audio click still plays on each press. With a pool larger than
    two this also drops the sticky rule, so repeated taps of one key
    alternate v1/v2 instead of repeating.
- **Kernel-level grab** — devices are grabbed exclusively via
  `EVIOCGRAB`, so the raw (uncleaned) events never leak to the compositor
  or the game. Works identically under Xorg and Wayland.
- **Key pool** — the physical side is a pool of up to 16 keys, all driving
  the same two virtual keys; which of the two a key gets is decided per
  press, not fixed in the config. Any sequence of distinct pool keys comes
  out as clean alternation. A two-key pool is the default and behaves
  exactly as the old fixed `k1`/`k2` pair. See [Key pool](#key-pool).
- **Auto-discovery + hotplug** — by default every keyboard-shaped device
  advertising *every* pool key is grabbed; unplugged keyboards are dropped
  and re-grabbed on replug (inotify-driven). You can also pin an explicit
  device list.
- **Multiple keyboards** — each physical keyboard gets its own independent
  SOCD state, all funneled into one virtual output device.
- **Audio click** — separate WAV samples (16/24/32-bit PCM) played via PipeWire on
  every virtual key-down, from a dedicated realtime audio thread.
- **Low latency** — single-threaded epoll loop, best-effort `SCHED_FIFO`
  realtime scheduling, and `mlockall` when audio is enabled.

The whole thing is a single-file C11 daemon (`doubletapd.c`).

## Requirements

- Linux with evdev + uinput (any remotely modern kernel)
- [libevdev](https://www.freedesktop.org/wiki/Software/libevdev/)
- [libyaml](https://pyyaml.org/wiki/LibYAML)
- [PipeWire](https://pipewire.org/) (`libpipewire-0.3`)
- CMake ≥ 3.10, a C11 compiler, and `pkg-config` to build

## Installation

### Arch Linux

Install `doubletap-git` with your AUR-helper of choice.

```sh
yay -S doubletap-git
```

### From source

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build
sudo cmake --install build
```

This installs the `doubletapd` binary, the default config and click sample
(`/usr/share/doubletap/`), a systemd **user** unit, and a udev rule that
opens `/dev/uinput` to the `input` group.

## Setup

`doubletapd` runs as an **unprivileged user service** — not as root. (It
must not: PipeWire is a per-user session daemon, so a root system service
would have no audio.)

1. Add yourself to the `input` group (covers read access to
   `/dev/input/event*`; the packaged udev rule opens `/dev/uinput` to the
   same group), then log out and back in:

   ```sh
   sudo usermod -aG input $USER
   ```

2. Copy the example config and edit it to taste:

   ```sh
   mkdir -p ~/.config/doubletap
   cp /usr/share/doubletap/config.yaml ~/.config/doubletap/
   ```

3. Enable the service:

   ```sh
   systemctl --user enable --now doubletap
   ```

Optional: for `SCHED_FIFO` realtime scheduling, grant your user realtime
privileges (on Arch, install `realtime-privileges` and join the `realtime`
group). The daemon warns and falls back gracefully without it.

## Configuration

Config is read from `~/.config/doubletap/config.yaml`
(`$XDG_CONFIG_HOME` respected), falling back to the installed default.
See the extensively commented [`config.yaml`](config.yaml) for the full
schema. The short version:

```yaml
# Omit `devices` (or set it to "auto") to grab every real keyboard that has
# every pool key. To pin specific keyboards, use stable by-id paths:
# devices:
#   - /dev/input/by-id/usb-Your_Keyboard-event-kbd

socd: toggle          # or "snappy" / "analog" / "off" ("on" = "toggle")

keys:                 # physical pool -> virtual v1/v2
  k1: KEY_Z           # symbolic KEY_* names or numeric codes
  k2: KEY_X
  v1: KEY_Z
  v2: KEY_X
  # pool: [KEY_Z, KEY_X, KEY_C, KEY_V]   # instead of k1/k2, up to 16
  # latch: [KEY_Z, KEY_X]                # analog only; defaults to the
                                         # first two pool keys

audio:
  enabled: true
  wav: /usr/share/doubletap/click.wav   # any 16/24/32-bit PCM WAV
  gain: 1.0
  # wav_v1: /usr/share/doubletap/click.wav # optional: separate
  # wav_v2: /usr/share/doubletap/clack.wav # sound samples and
  # gain_v1: 1.0                           # gains for v1 & v2
  # gain_v2: 0.8

uinput:
  name: "doubletap virtual keyboard"

# analog:             # only used by `socd: analog` — see below
#   actuation_mm: 1.0
#   rapid_trigger:
#     bottom_out_mm: 0.2
```

After editing, restart the daemon:

```sh
systemctl --user restart doubletap
```

### Key pool

`v1`/`v2` — the two keys the daemon actually emits — are fixed. The physical
side is not: `keys.pool` takes anything from two to sixteen keys, and every
one of them drives those same two virtual keys. `k1`/`k2` are just the
two-entry spelling of `pool`, and remain the default.

Which of the two a key gets is decided per press, in this order:

1. **Sticky.** A key reuses the virtual it drove last time, whenever that one
   is free. So a key tapped on its own always gives the same virtual — and a
   two-key pool behaves exactly as `k1`/`k2` always did.
2. **Otherwise the free one.** This is what makes any sequence of distinct
   keys alternate: `Z X C V` comes out `v1 v2 v1 v2`, and so does `C V C V`.
3. **If both are held** — a third key down at once — the newcomer alternates
   off the last press and *shares* that virtual. It still counts as a press,
   so the toggle fires for it; its release does nothing while the key sharing
   with it is still down.

Pool keys are **consumed**: a pool key emits only `v1`/`v2`, never its own
code. Everything outside the pool passes through untouched. Auto-discovery
grabs a keyboard only if it advertises *every* pool key — the pool alternates
across all of it, so a board carrying only part of it would alternate against
keys it cannot see.

## Analog mode

Analog keyboards report *how far* each key is pressed, not just whether it
is down. `socd: analog` uses that to run **both** play patterns in one
session, instead of making you pick one at config time.

The two patterns want opposite things:

| pattern | what it is | wants |
| --- | --- | --- |
| **alt-tapping** | alternating discrete taps between the two keys | `off` |
| **rocking** | both fingers planted, rocking between the keys | `toggle` |

And each mode wrecks the other pattern. `toggle` is wrong for alt-tapping
because at speed the taps overlap — the second key lands before the first has
come back up — and the toggle reads that overlap as a rock:

> It answers the outgoing key's release by **re-pressing its virtual key**, a
> note you never struck.
> Every overlapped pair costs a spurious click, and the inversion it leaves
> behind carries into everything after it.

`off` is right for alt-tapping and useless for rocking, since nothing
alternates.

### The `deep` latch

Travel depth supplies a discriminator that a fixed threshold cannot have:

> **Planting both keys on the backplate is the rock, and nothing else is.**

Alt-tapping never puts both fingers on the floor at the same instant — its
defining shape is one finger leaving as the other arrives, however fast you
play and however much the presses overlap in the middle of travel. Rocking,
by contrast, *begins* by planting both; you cannot start one any other way.

So: `deep` **arms** when both latch keys are on the backplate in one report,
and the pool switches to the toggle. It **disarms** when **both** of them are
back off the backplate, and the keys switch back to the plain remap. Until it
arms, they are completely independent.

One finger lifting clear is *not* an exit. In a burst a finger routinely comes
right off the switch while the other stays planted; ending the latch there
would push the next note through the tapping profile at `actuation_mm` and
land it early. Two thresholds inside one burst is a limp, and "how completely
you lift" is exactly the kind of amplitude-driven rule this design rejects
everywhere else.

The pair that arms it is `keys.latch`, defaulting to the first two pool keys.
With a larger pool every key is still read by travel — so the daemon's own
rapid trigger governs all of them, rather than half of them running the
firmware's actuation instead — but only the latch pair can arm the latch, and
the rest go **silent** for the duration of a rock. While it is armed, the two
latch keys own both virtuals.

Because the backplate is a hard physical stop, this is a trigger you can
*aim*: "plant both keys to start rocking" is an instruction you can
actually follow, unlike "cross some depth in the middle of travel". The
threshold is `rapid_trigger.bottom_out_mm` — the single most important
number in the analog block, since it defines the backplate for both the
latch and rapid trigger.

Note what this deliberately does **not** try to do: it never compares one
key's depth against the other's. One key held deep while the other is
lightly pressed or tapped is not a problem and is left entirely alone.

#### What you hear

Arming is **silent**. It releases one virtual key so the toggle's
one-key-down invariant holds, and emits no press and no click — planting
your fingers is not a note, and the notes for both keys already landed when
they bottomed. It leaves exactly one virtual key down, correctly named, so
every later crossing of the backplate is a plain handover: one beat per lift,
whichever finger moves.

Disarming is **silent too**, and that is what the "both off the floor" rule
buys. Since nobody is still riding by the time it fires, there is no hold to
strand and so nothing to press. An earlier rule could disarm with the other
finger planted deep, and had to re-press to avoid stranding it — one extra
note for one lift. That cost is gone.

### Rapid trigger

Rapid trigger follows the same shape as Wootility's: `press_mm` and
`release_mm` are reversal distances, and — as with a full release always
releasing — **bottoming out always presses**, however small the down-travel
(`bottom_out_mm`).

It is **tapping-only**, though. Which of the two rules a key runs is decided
by whether `deep` is armed — not by any depth:

| state | rule |
| --- | --- |
| not armed (tapping) | `actuation_mm`, plus rapid trigger on `press_mm` / `release_mm` |
| armed (riding) | the backplate alone — down against the floor, up as soon as it leaves |

**While riding, the backplate *is* the switch**, and rapid trigger does not
run at all. That is what riding already is — you bottom out every stroke — so
every note in a burst lands on the same hard physical line instead of at
whatever depth a reversal distance happened to fall. No hysteresis is needed:
a hard stop cannot be hovered on, the same property that makes the latch
aimable. Making the backplate the only thing that re-presses is the setting
Wootility caps at 2.5 mm and never lets you reach.

Tying the rule to the gesture rather than a depth is what keeps beats even.
An earlier version picked a profile from the anchor against a threshold, so a
rock that overshot that line silently switched to the tapping profile and
re-pressed part-way up instead of waiting for the backplate — the beat landed
early on exactly the rocks that went high. Amplitude changing the rule is a
limp, not a threshold.

`actuation_mm` and the `rapid_trigger` distances therefore only ever apply to
tapping. The `deep_press_mm` / `deep_release_mm` keys of earlier versions are
gone; the daemon warns and ignores them if it finds them in a config.

### Recording and replaying a session

`-T` writes the travel of the **latch pair** to stdout, one line per hardware
report, until Ctrl-C. It stays two columns wide (`us,k1_mm,k2_mm`) whatever
the pool's size — the rest of the pool cannot arm the latch, and keeping the
format fixed is what lets the recorded traces stay comparable.

Like `-A` it is completely passive — it opens the keyboard's hidraw node and
nothing else, so there is no grab, no virtual device, and nothing in the
input path. Whoever records plays on their own setup with their own keyboard
behaving exactly as it normally does.

```sh
./build/doubletapd -T > session.csv     # play, then Ctrl-C
./build/replay session.csv
```

`replay` runs the recording back through the daemon's own analog state
machine — it `#include`s `doubletapd.c` and stubs only the uinput writes, so
it cannot drift from what the daemon actually does. It reports how many
times `deep` armed, how many virtual presses were emitted, a histogram of
how deep the keypresses actually went, and a sweep of latches against
`bottom_out_mm` so you can see how the backplate's width changes it.

**A trace of your own alt-tapping is the acceptance test.** Record one and
check that it reports *zero* latches:

```sh
./build/doubletapd -T > alttap.csv      # alt-tap a stream at speed
./build/replay alttap.csv               # "0 deep latches" is the answer you want
```

If it arms even once, your `bottom_out_mm` is too wide for how hard you
bottom out — the sweep shows how the count moves with it. On synthetic
traces the latch never arms during alternating taps at any speed down to 40
notes/second; it starts only once you hold each key longer than the gap
between notes, which is the point where you have stopped tapping and
started riding. The depth
histogram is the other half of that picture: a player who never presses
past half travel cannot arm it at all, whatever their timing does.

### Picking thresholds

Run the analog monitor and watch your own travel depth:

```sh
doubletapd -A
```

It prints live depth per key and the peak depth of each press, grabs
nothing, and creates no virtual device — safe to run alongside a live
daemon. Set `bottom_out_mm` so the backplate starts just above where your
fingers actually rest when riding.

It also prints each key's **HID usage id** alongside its name. The daemon
derives those from the keyboard's own keymap, so you should not need them —
but if a key comes out wrong you can pin them with `analog.hid`, a list
parallel to `keys.pool` (`analog.hid_k1` / `hid_k2` are its two-key
spelling).

### Requirements and caveats

- A Wooting keyboard (the analog interface is read directly from
  `/dev/hidraw*`; no vendor SDK or kernel driver is involved). Other analog
  boards are not supported yet.
- Read access to the analog hidraw node. The `70-wooting.rules` udev rules
  shipped with Wootility grant this via `uaccess`, which covers a graphical
  session; outside one, add a group-based rule.
- **Turn off the keyboard's own rapid trigger and SOCD** (Snappy Tappy /
  Rappy Snappy). The daemon does both itself, and on-board versions fight
  it.
- If no analog device is found, the daemon logs a warning and falls back to
  the digital `toggle` behaviour rather than failing.
- One known residual: *while the latch is armed*, with one finger resting on
  a bottomed key, taps on the other that never reach the floor register
  nothing — the backplate is the only switch during a rock. It clears the
  moment the resting finger leaves the floor, and during a real burst every
  stroke bottoms out by definition, so it is accepted; it is untested in
  play as of this writing.

## Running manually

```
usage: doubletapd [-h] [-A|-T] [-c CONFIG] [-i DIR]

options:
    -h          show this help and exit
    -A          analog monitor: print live key travel depth and exit
                (for picking thresholds; grabs nothing)
    -T          trace k1/k2 travel as CSV on stdout until Ctrl-C
                (grabs nothing; replay it with the `replay` tool)
    -c CONFIG   path to YAML config
    -i DIR      directory to scan/watch for event devices
                (default /dev/input; mainly for testing)
```

Handy for trying config changes before restarting the service:

```sh
./build/doubletapd -c config.yaml
```

## How it works

1. **Grab** — keyboards are opened via libevdev and exclusively grabbed, so
   nothing else on the system sees their raw events. The daemon refuses to
   grab its own virtual output (that would be an instant feedback loop).
2. **Assign a slot** — the pool layer picks which of the two virtual keys an
   incoming pool key drives (sticky → free → share off the last press). The
   state machine below it has always had two *slots*, not two keys; with the
   default two-key pool there is one key per slot and the distinction never
   shows.
3. **Filter** — a per-device radio-button state machine tracks the two slots
   and whichever virtual key is currently active. Pressing into the second
   slot while the first is held releases the first virtual key and presses
   the second ("release-then-press", always separated by `SYN_REPORT`s).
   Releasing a key while the other slot is still held re-presses the other
   virtual key (in `toggle` mode; `snappy` only does this when the active
   key was released). In `off` mode the state machine is bypassed entirely
   and each slot is remapped one-to-one to v1/v2. In `analog` mode the
   digital events for every pool key on that keyboard are dropped and the
   same state machine is driven instead by press/release edges synthesized
   from travel depth, read from the keyboard's analog hidraw interface on
   the same epoll loop.
4. **Re-emit** — everything flows out through one uinput virtual keyboard
   with a full keyboard-wide key set, so hotplugged keyboards with unusual
   keys still work. Non-pool events are mirrored verbatim.
5. **Click** — each virtual key-down triggers the WAV sample on a PipeWire
   realtime thread; overlapping triggers restart the sample from the top.

## Development

Two test tools `#include doubletapd.c` outright, so they exercise the
daemon's real state machine rather than a copy that could drift. Both build
as part of `all`, deliberately — the guarantee is worthless if the binaries
can go stale.

- `build/regimetest` — assertions over the `deep` latch, its mode
  transitions, and the slot layer (alternation, third-key sharing, arming
  eviction, latch-pair canonicalisation, autorepeat).
- `build/replay` — replays a `-T` trace through the analog front end, and
  sweeps `bottom_out_mm` so you can see how the backplate's width moves the
  latch count.

`tools/e2e.py` is the outside-in counterpart: it runs the real binary against
synthetic uinput keyboards via `-i DIR` and asserts on what comes out of the
daemon's own virtual device, so it is the only thing covering the grab, the
epoll loop, auto-discovery and the uinput write path as a whole.

```sh
python3 tools/e2e.py          # --keep-going to run past the first failure
```

It needs `python-evdev` and membership of the `input` group. If *every*
assertion comes back empty, check `pgrep -af doubletapd` — a running
`doubletap.service` auto-grabs any keyboard advertising its configured keys,
and will take the synthetic device away from the daemon under test.

## Fair play

doubletapd rewrites your input below the game's view: what the game
receives is not literally what your fingers did. Some games, anti-cheat
systems, and tournament rulesets prohibit SOCD-style input handling —
Valve banned the equivalent keyboard-firmware features (Razer Snap Tap,
Wooting SOCD) from CS2 in 2024, and rhythm game communities have their own
rules on input assistance. Check the rules of whatever you're playing
before using this. You are responsible for how you use it.

## License

[MIT](LICENSE.md)
