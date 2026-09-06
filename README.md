# doubletap

A SOCD cleaner for Linux — snap tap style key
handling as a daemon, turning a "rocking" two-finger pattern into
rapid-fire, gap-free key alternation on any keyboard.

![demo](docs/demo.gif)

`doubletapd` exclusively grabs your keyboard(s) at the evdev level — before
Xorg/Wayland ever see the events — applies a SOCD (Simultaneous Opposing
Cardinal Directions) state machine to two configurable keys, and re-emits
everything through a single virtual uinput keyboard. All other keys pass
through untouched. It also plays a click sound through PipeWire on every
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
  - `off` — no SOCD cleaning: k1/k2 are simply remapped to v1/v2, and the
    audio click still plays on each press.
- **Kernel-level grab** — devices are grabbed exclusively via
  `EVIOCGRAB`, so the raw (uncleaned) events never leak to the compositor
  or the game. Works identically under Xorg and Wayland.
- **Auto-discovery + hotplug** — by default every keyboard-shaped device
  advertising both configured keys is grabbed; unplugged keyboards are
  dropped and re-grabbed on replug (inotify-driven). You can also pin an
  explicit device list.
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
# both k1 and k2. To pin specific keyboards, use stable by-id paths:
# devices:
#   - /dev/input/by-id/usb-Your_Keyboard-event-kbd

socd: toggle          # or "snappy" / "analog" / "off" ("on" = "toggle")

keys:                 # physical k1/k2 -> virtual v1/v2
  k1: KEY_Z           # symbolic KEY_* names or numeric codes
  k2: KEY_X
  v1: KEY_Z
  v2: KEY_X

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
#     bottom_out_mm: 0.05
```

After editing, restart the daemon:

```sh
systemctl --user restart doubletap
```

## Analog mode

Analog keyboards report *how far* each key is pressed, not just whether it
is down. `socd: analog` uses that to run **both** play patterns in one
session, instead of making you pick one at config time.

The two patterns want opposite things:

| pattern | what it is | wants |
| --- | --- | --- |
| **alt-tapping** | alternating discrete taps between k1 and k2 | `off` |
| **rocking** | both fingers planted, rocking between the keys | `toggle` |

And each mode wrecks the other pattern. `toggle` is wrong for alt-tapping
because at speed the taps overlap — k2 lands before k1 has come back up —
and the toggle reads that overlap as a rock:

> It answers the k1 release by **re-pressing v1**, a note you never struck.
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

So: `deep` **arms** when both keys are on the backplate in one report, and
k1/k2 switch to the toggle. It **disarms** the moment either key comes back
up past `actuation_mm`, and they switch back to the plain remap. Until it
arms, the two keys are completely independent.

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
they bottomed. The first key to *dip* out of the latch is what binds the
toggle's active key, and from there you get exactly one beat per lift.

Lifting out of a rock costs one extra note, where the plain remap wants the
still-held key's virtual back down. That is the cheaper of the two
mistakes; the alternative strands the hold.

### Rapid trigger

Rapid trigger follows the same shape as Wootility's: `press_mm` and
`release_mm` are reversal distances, and — as with a full release always
releasing — **bottoming out always presses**, however small the down-travel
(`bottom_out_mm`).

Unlike Wootility's, it runs **two profiles**, because tapping and riding
are different gestures that want different numbers. Which one applies is
decided by whether `deep` is armed — not by any depth:

| state | profile |
| --- | --- |
| not armed (tapping) | `press_mm` / `release_mm` |
| armed (riding) | `deep_press_mm` / `deep_release_mm` |

Tying it to the gesture rather than a depth is what keeps beats even. An
earlier version picked the profile from the anchor against a threshold, so
a rock that overshot that line silently switched to the tapping profile and
re-pressed part-way up instead of waiting for the backplate — the beat
landed early on exactly the rocks that went high. Amplitude changing the
rule is a limp, not a threshold.

Either deep value may be `off`. **`deep_press_mm: off` makes the backplate
the only thing that re-presses while riding** — no amount of down-travel
alone will do it — which is the setting Wootility caps at 2.5 mm and never
lets you reach. `deep_release_mm: off` is the mirror, holding the key until
a full release the way Keychron's bottom dead zone does; note that a rock
then emits nothing at all, since its beats come from the release/re-press
pair.

Leave the deep values unset and they mirror the tapping ones, which is the
single-profile behaviour you had before.

### Recording and replaying a session

`-T` writes the travel of k1/k2 to stdout, one line per hardware report,
until Ctrl-C. Like `-A` it is completely passive — it opens the keyboard's
hidraw node and nothing else, so there is no grab, no virtual device, and
nothing in the input path. Whoever records plays on their own setup with
their own keyboard behaving exactly as it normally does.

```sh
./build/doubletapd -T > session.csv     # play, then Ctrl-C
cmake --build build --target replay
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

## Running manually

```
usage: doubletapd [-h] [-A] [-c CONFIG] [-i DIR]

options:
    -h          show this help and exit
    -A          analog monitor: print live key travel depth and exit
                (for picking thresholds; grabs nothing)
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
2. **Filter** — a per-device radio-button state machine tracks `k1`, `k2`,
   and whichever virtual key is currently active. Pressing the second key
   while the first is held releases the first virtual key and presses the
   second ("release-then-press", always separated by `SYN_REPORT`s).
   Releasing a key while the other is still held re-presses the other
   virtual key (in `toggle` mode; `snappy` only does this when the active
   key was released). In `off` mode the state machine is bypassed entirely
   and k1/k2 are remapped one-to-one to v1/v2. In `analog` mode the digital
   k1/k2 events are dropped and the same state machine is driven instead by
   press/release edges synthesized from travel depth, read from the
   keyboard's analog hidraw interface on the same epoll loop.
3. **Re-emit** — everything flows out through one uinput virtual keyboard
   with a full keyboard-wide key set, so hotplugged keyboards with unusual
   keys still work. Non-k1/k2 events are mirrored verbatim.
4. **Click** — each virtual key-down triggers the WAV sample on a PipeWire
   realtime thread; overlapping triggers restart the sample from the top.

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
