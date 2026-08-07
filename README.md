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

- **Three SOCD modes**, selected by the `socd` config field:
  - `toggle` (default; `on` is a synonym) — *reverting toggle*: when both
    keys are held, the most recent press wins; releasing **either** key
    re-presses the other virtual key. Rocking your fingers between the two
    keys produces clean, gap-free alternation.
  - `snappy` — *last input wins* ("Snappy Tappy"): the most recent press
    wins, but only releasing the **active** key falls back to the still-held
    one; releasing the already-suppressed key does nothing.
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

socd: toggle          # or "snappy" / "off" ("on" = "toggle")

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
```

After editing, restart the daemon:

```sh
systemctl --user restart doubletap
```

## Running manually

```
usage: doubletapd [-h] [-c CONFIG] [-i DIR]

options:
    -h          show this help and exit
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
   and k1/k2 are remapped one-to-one to v1/v2.
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
