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
  - `analog` — `toggle`, but driven by how far the keys are actually
    pressed rather than by one fixed actuation point. Requires an analog
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
#   socd_depth_mm: 1.5
```

After editing, restart the daemon:

```sh
systemctl --user restart doubletap
```

## Analog mode

Analog keyboards report *how far* each key is pressed, not just whether it
is down. `socd: analog` uses that to fix a specific failure of every
fixed-threshold SOCD cleaner:

> While single-tapping one key at speed, the finger resting on the other key
> dips a fraction of a millimetre past actuation. A fixed threshold cannot
> tell that from a deliberate press, so the toggle fires — costing two
> spurious notes (the steal, then the revert on the way back up) and
> inverting which key is active for everything after it.

`socd_depth_mm` decides **when the state machine engages at all**. When a
second key goes down while the key *already held* is riding deeper than
that, the toggle engages; otherwise the keys stay independent and an
incidental overlap costs nothing — which is what you want for ordinary
alternate tapping. Set it around where you actually ride the keys.

Only the **held** key's depth is tested. The key going down has barely
travelled when its press fires, so requiring depth of it too would tie
engagement to wherever a rapid-trigger re-press happens to land: reliable
while `socd_depth_mm` sits well above that, erratic as it approaches.

Once engaged it holds until neither key is deep — until they come back to
the top. It deliberately does **not** track the emitted keys, since rapid
trigger lifts and re-presses those constantly and a moment where both are
up is part of the rock, not the end of it.

Separately, while one key *is* held deep, a press on the other has to earn
the right to take over from it. `analog.gate` decides how:

| mode | rule |
| --- | --- |
| `depth` (default) | reach `gate_depth_mm` or emit nothing |
| `off` | no gating; plain analog toggle plus rapid trigger |

The threshold exists to stop an accidental dip — or a keycap magnet
bouncing from the shock of a hard hit on the *other* key, which is a real
effect on Hall-effect boards — from stealing the active key mid-wobble.
Set it just above where those land and no higher.

The daemon logs every press it suppresses, with the depth it reached, so
you can tell whether the gate is earning its keep or eating real input.

Note what the gate does to a press it *doesn't* reject: it **withholds** it
until the threshold is met. So every millimetre of `gate_depth_mm` is
latency added to a re-press, and a wobble where some beats lift far enough
to be re-gated and others don't will fire those beats at different times —
an even finger rhythm coming out as a limp. Keep `gate_depth_mm` shallow,
just above an accidental dip; it is a separate field from `socd_depth_mm`
for exactly this reason, since that one wants to be deep.

Rapid trigger follows the same shape as Wootility's: `press_mm` and
`release_mm` are reversal distances, and — as with a full release always
releasing — **bottoming out always presses**, however small the down-travel
(`bottom_out_mm`). That matters if you set a large `press_mm` and treat the
backplate as your neutral position when wobbling.

Once a press reaches `socd_depth_mm` it stays eligible until the key returns
all the way up past `release_mm`, so easing off mid-roll does not demote it
— the same idea as Wootility's *Continuous Rapid Trigger*. A software rapid
trigger comes along for the ride, since the daemon is computing actuation
itself either way.

### Picking thresholds

Run the analog monitor and watch your own travel depth:

```sh
doubletapd -A
```

It prints live depth per key and the peak depth of each press, grabs
nothing, and creates no virtual device — safe to run alongside a live
daemon. Set `socd_depth_mm` comfortably below how deep you actually ride
the keys and comfortably above an accidental dip.

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
