# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## What this is

`doubletapd` is a single-file C11 daemon that acts as a SOCD (Simultaneous
Opposing Cardinal Directions) cleaner for two keyboard keys. It exclusively
grabs evdev keyboard devices at the kernel level (before Xorg/Wayland see
them) — either an explicit config list or, by default, every
keyboard-shaped device advertising both configured keys, with
inotify-driven hotplug in both cases — applies a SOCD state machine to two
configured physical keys — "toggle" (last-input + reverting toggle; "on"
is a synonym), "snappy" (last input wins, no latching), "analog" (toggle,
but driven by travel depth read from an analog keyboard), or "off" (no
cleaning, just the k1/k2 -> v1/v2 remap), selected by the top-level
`socd` config field — and re-emits all input through a
single uinput virtual keyboard. It also plays a click sound via PipeWire on
every virtual keypress. Built for rhythm games (osu!) where fast key
alternation needs a "rocking" input pattern instead of naive SOCD handling.

The entire implementation lives in `doubletapd.c` (~2850 lines) — there is
no multi-module structure.

## Build

```sh
cmake -S . -B build
cmake --build build
```

Produces `build/doubletapd`. Requires pkg-config-visible dev packages for
`libpipewire-0.3`, `libevdev`, and `yaml-0.1` (libyaml).

There is no test suite, linter, or CI config in this repo — validate changes
by building and manually exercising the daemon (see Running below).

## Running

The daemon is designed to run as an unprivileged user in the `input` group:
that covers read access to `/dev/input/event*`, and the packaged udev rule
(`70-doubletap-uinput.rules`) opens `/dev/uinput` to the same group. It
must NOT run as a root system service — PipeWire is a per-user session
daemon, so audio only works from inside a user session. The systemd unit
(`doubletap.service.in`, configured by CMake) is a *user* unit:
`systemctl --user enable --now doubletap`.

```sh
./build/doubletapd -c config.yaml
```

Without `-c`, the daemon looks for
`$XDG_CONFIG_HOME/doubletap/config.yaml` (`~/.config` if unset), then
falls back to the installed default (`/usr/share/doubletap/config.yaml`;
CMake bakes the real prefix in via `DEF_CONFIG`/`DEF_WAV` compile
definitions). See `config.yaml` for the schema: `devices` (optional list of
`/dev/input/by-id/*` paths; omitted or `auto` means auto-discovery), `socd`
(`toggle` default, `on` as a synonym, `snappy`, `analog`, or `off`), `keys`
(k1/k2 physical -> v1/v2 virtual, symbolic `KEY_*` names or numeric codes),
`audio` (enabled + wav path), `uinput`
(virtual device name), `analog` (analog-mode thresholds; see below). After
editing config, restart via
`systemctl --user restart doubletap`. The `-i DIR` flag overrides the
scanned/watched device directory (default `/dev/input`) — mainly for
integration testing against a directory of symlinks to synthetic uinput
nodes. The `-A` flag runs the analog monitor (live travel depth, no grab,
no uinput device) for picking thresholds and discovering HID usage ids.
`-T` is the same passivity with machine-readable output: CSV of k1/k2
travel per report, for replaying a session offline. `tools/replay.c`
consumes it — built on demand with `cmake --build build --target replay`,
excluded from `all`, and NOT installed. It `#include`s `doubletapd.c` and
stubs only the uinput writes, so it exercises the daemon's real state
machine rather than a copy that could drift; keep it that way.
`tools/regimetest.c` is the same trick as a unit test — assertions over the
`deep` latch and its mode transitions, run with `./build/regimetest`; case K
drives `analog_drain` itself from encoded HID reports over a pipe, since
every other case (and `replay`) calls the front-end directly and would not
catch a fault in the read/decode path. `tests/*.csv` are recorded `-T`
sessions named `<style>_<name>_Nphys_Mvirt.csv`, where N is the physical
presses played and M the virtual presses the daemon must emit — `replay`
prints M, so the filename is the expected result. The `tap_*` traces must
report ZERO latches; the `toggle_*` traces must latch and must emit MORE
virtual presses than physical. Both
tools build as part of `all`, deliberately: they `#include doubletapd.c`, and
that guarantee is worthless if the binaries can go stale.

## Architecture

Everything is in `doubletapd.c`, organized into clearly delimited
sections (search for the `/* --- */` banner comments):

1. **Config** (`load_config` and friends) — parses YAML via libyaml's
   document API (not the streaming API) into an `oid_config_t`. Key codes
   accept either symbolic names (resolved via
   `libevdev_event_code_from_name`) or raw integers.

2. **Input devices** (`input_try_open`/`input_close`/`reconcile_devices`) —
   devices are opened, wrapped in `libevdev` handles, and exclusively
   grabbed (`LIBEVDEV_GRAB`) so events don't leak to the rest of the
   system. `reconcile_devices` is the single (re)open path, run at startup
   and again on every inotify event under the input dir: in explicit mode
   it retries any configured path not currently open; in auto-discovery
   mode it scans `event*` nodes and grabs those passing `auto_grab_ok`
   (has both k1 and k2, no `EV_REL`/`EV_ABS`, not `BUS_VIRTUAL`). In BOTH
   modes `is_doubletap_output` refuses any device whose uniq is
   `"doubletap"` or whose name matches the configured uinput name —
   grabbing our own output would be an instant feedback loop. If any key
   is physically down at open time (`any_key_down` — e.g. the Enter that
   launched the daemon), the grab is *deferred*: the fd stays open
   ungrabbed (`in->grabbed == 0`), `drain_device` discards its events
   (the system still receives them directly), and the grab completes as
   soon as the device reports all keys up — grabbing mid-press would
   swallow the release and leave the raw key stuck. Open devices
   are deduped by `st_rdev` (a node reached via two symlinks is grabbed
   once) and tracked in a `dev_list_t` of stable heap pointers (epoll user
   data points at the entries).

3. **Virtual uinput device** (`build_virtual`) — a single synthetic
   keyboard is created with a fixed keyboard-wide key set (every `KEY_*`
   code, skipping the `BTN_*` pointer/gamepad ranges) plus the two
   configured virtual keys (v1/v2). The set is fixed rather than a union
   of source devices because uinput capabilities are immutable after
   creation and hotplugged keyboards may carry codes the startup set
   lacked. All non-k1/k2 events are mirrored through verbatim.

4. **Radio-button state machine** (`process_event`) — the core logic. Each
   input device tracks its own `k1`/`k2`/`act` (which virtual key is
   currently "on") state independently. The transition is computed as
   `state = k1 + k2 + event.value` after updating k1/k2, giving four cases
   (`S_NONE`, `S_RELEASE`, `S_SINGLE`, `S_PRESS`) that decide whether to
   emit a virtual key-down, a release, or an release-then-press pair (the
   "reverting toggle" that recreates a keypress when the active key is
   released while the other is still held). The `S_RELEASE` case is
   mode-dependent: `SOCD_TOGGLE` reverts no matter which of the two held
   keys was released (latching), while `SOCD_SNAPPY` ("last input wins")
   only reverts when the *active* key is released — releasing the
   already-suppressed key emits nothing. Autorepeat (`value == 2`) is
   ignored to avoid corrupting state. `SOCD_OFF` bypasses the state
   machine before any of this: k1/k2 are remapped one-to-one to v1/v2
   (autorepeat included), presses still return 1 for the audio trigger,
   and `k1`/`k2` are still tracked so `release_stuck` works.

5. **Analog input** (`analog_*`, `hid_map_*`) — optional path used only by
   `socd: analog`. Analog keyboards report per-key travel on a
   vendor-defined HID interface (usage page `0xFF53` v2 / `0xFF54` v1); the
   kernel makes no evdev node for a vendor usage page, so it exists purely
   as `/dev/hidrawN` — a pollable fd that joins the same epoll set, and one
   `EVIOCGRAB` has no bearing on. No vendor SDK and no kernel driver is
   involved. `analog_open` finds the node by matching the Wooting VID in
   sysfs `uevent` then scanning `report_descriptor` for the vendor usage
   page. `analog_decode` unpacks 16 slots per report. **A released key
   vanishes from the report rather than reporting a final zero**, so rest is
   inferred from absence — `analog_drain` rebuilds the full depth of k1/k2
   each frame rather than tracking deltas. `analog_key_feed` is the per-key
   front-end, and it runs one of two rules depending on the `deep` latch.
   TAPPING uses `actuation_mm` plus a software rapid trigger anchored on
   `rt_extreme` (`press_mm`/`release_mm`). RIDING uses nothing but the
   backplate: a key is down while it is against the floor and up as soon as
   it leaves, no rapid trigger at all, since riding bottoms out every stroke
   by definition and every note in a burst should land on the same physical
   line. `analog_deep_update` owns the latch (arm: both keys on the
   backplate in one report; disarm: BOTH off it — one finger lifting clear
   is a stroke, not an exit), and `analog_deep_set` reconciles across the
   mode change, release-only in both directions, parking both front ends on
   the way out.
   k1/k2 -> HID usage
   mapping is derived from the keyboard's own keymap via `EVIOCGKEYCODE_V2`
   (hid-input stores the HID usage as the scancode), not a hardcoded table.

6. **Event loop** (`run_loop`/`drain_device`) — a single-threaded
   `epoll`-based loop multiplexes all grabbed devices plus an inotify fd
   watching the input dir (and its `by-id`/`by-path` subdirs) with
   `IN_CREATE | IN_ATTRIB | IN_MOVED_TO`; `IN_ATTRIB` matters because a
   hotplugged node is typically root-only until udev applies the
   input-group permissions, so the first open gets `EACCES` and the chmod
   retriggers the reconcile pass. Devices are drained with
   `libevdev_next_event` (handling `LIBEVDEV_READ_STATUS_SYNC`
   dropped-event resync) until `EAGAIN`. A device is dropped from the poll
   set on error/HUP without killing the daemon — releasing its active
   virtual key first if one was held (`release_stuck`) — and the loop
   keeps running with zero devices, waiting for hotplug (it only aborts at
   startup if nothing opened AND inotify is unavailable).

7. **Audio** (`wav_load`, `audio_init`, `on_process`, `audio_trigger`) — a
   hand-rolled WAV reader (16/24/32-bit PCM) loads the click sample into a
   float buffer up front. Playback runs on a dedicated PipeWire thread loop
   (`pw_thread_loop`); `on_process` is the realtime audio callback and only
   touches shared state through `atomic_*` operations (`playing`, `pending`,
   `reset`, `frame_pos`) since it runs on PipeWire's RT thread while
   `audio_trigger` is called from the main epoll thread. Overlapping
   triggers restart playback from frame 0 rather than queuing.

`main()` wires these together: parse args -> load config -> attempt
`SCHED_FIFO` realtime priority (best-effort, warns and falls back on
failure) -> best-effort audio init (`mlockall` if audio is enabled, to
avoid page faults in the RT callback) -> build the virtual device (safe
before any grab: discovery skips it via `is_doubletap_output`) -> open the
analog interface if `socd: analog` (before the first reconcile, so grabbed
keyboards already know their k1/k2 are analog-driven; a failure here just
warns and falls back to `toggle`) -> install
SIGINT/SIGTERM handlers -> run the event loop (which opens/grabs devices
via the initial reconcile pass and handles hotplug thereafter) -> tear
down in reverse order.

## Key invariants to preserve when editing the state machine

- `socd_apply` is the single SOCD core, shared by both input paths: the
  evdev path (`process_event`) feeds it real key events, the analog path
  (via `analog_socd_edge`) feeds it edges synthesized from travel depth.
  Adding a behavior to one path and not the other is almost always a bug.
  It takes the mode as an argument rather than reading `cfg->socd`, which is
  what lets the analog path run a shallow overlap as a plain remap.
- The `deep` latch is the ONLY thing that selects between the two modes,
  and both halves of its rule are pure depth tests on the CURRENT sample:
  it ARMS when BOTH keys are at `travel_mm - bottom_out_mm`, and DISARMS
  when BOTH are off it. No per-key commitment latch, no counter, no
  comparison of one key's depth against the other's.
- The failure being fixed is ALT-TAPPING OVERLAP, not a resting finger.
  At speed, alternating taps overlap in the middle of travel; a plain
  toggle reads that as a rock and answers the outgoing key's release by
  re-pressing its virtual key — a note nobody struck. What makes the latch
  work is that alt-tapping never puts both keys on the floor in the same
  report, because its defining shape is one finger leaving as the other
  arrives. `tools/regimetest.c` case B asserts exactly this.
- One key held deep while the other is lightly pressed or tapped is NOT a
  failure mode and never was. Two earlier branches
  (`feature/analog-input`, `feature/analog-split-rt`) were built around
  solving it — relative-depth gates, absolute-depth gates, five successive
  reformulations of "what arms the regime" — and were abandoned for it.
  Do not reintroduce a depth-comparison gate between k1 and k2.
- WHILE RIDING, THE BACKPLATE IS THE SWITCH. Rule 0 in `analog_key_feed`
  returns before any of the tapping logic: a key is down while it is
  against the floor and up as soon as it leaves, and rapid trigger does not
  run at all. That is what riding already is — you bottom out every stroke —
  so every note in a burst lands on the same hard physical line. No
  hysteresis is needed or wanted: a hard stop cannot be hovered on, the same
  property that makes the latch aimable. `actuation_mm`, `release_mm`,
  `press_mm` and `release_mm` are TAPPING-ONLY thresholds.
- ONE finger lifting clear does NOT disarm. Only both leaving the floor
  does. The earlier rule (either key back past `actuation_mm`) looked
  reasonable and was wrong: in a burst a finger routinely comes right off
  the switch while the other stays planted, which tore the latch down
  mid-gesture and sent the next note through the TAPPING profile at
  actuation. Measured on `tests/toggle_5burst_3phys_5virt.csv`: k2 sat at
  3.5000mm throughout while k1 lifted to 0.0000mm, and note 4 landed 11.9ms
  early at 0.25mm instead of at 3.44mm. Two thresholds inside one burst is
  jitter, and "how completely you lift" is an amplitude-driven rule of
  exactly the kind this design rejects everywhere else. `regimetest` cases
  F and G pin it.
- `bottom_out_mm` is load-bearing and now serves three roles by deliberate
  choice: the tapping bottom-out re-press, the latch threshold, AND the
  only threshold in force while riding. The config check refuses a zero
  value and warns above 10% of travel, where a firm tap starts to reach it
  and alt-tapping could arm the latch.
- The config check still requires `actuation_mm` to sit above the
  backplate, though no longer because actuation disarms the latch: an
  actuation line at or past the floor means a key could reach the backplate
  without ever having actuated, so the plain remap would never press it.
- The latch is resolved once per sample in `analog_deep_update`, called from
  `analog_drain` after both keys are fed and BEFORE that sample's edges are
  applied. Both directions need that ordering. Disarming first is the one
  that bites: with both keys leaving the floor in the SAME report, routing
  the edges first gives `S_RELEASE` then `S_NONE` — `v2↑ v1↓ v1↑`, a
  phantom press on the way out — while disarming first reconciles to `v2↑`
  and lets both release edges land as no-change writes.
- RECONCILIATION IS RELEASE-ONLY IN BOTH DIRECTIONS, and that is what the
  disarm rule buys. ARMING goes from OFF's two keys down to the toggle's
  one, so planting is silent. DISARMING goes from the toggle's one to none:
  under "both off the floor" there is by definition nobody still riding, so
  there is no survivor to strand and nothing to press. The older rule could
  fire with the other finger planted deep, so it HAD to press to avoid
  stranding that hold — at the cost of a second note for one lift. Nothing
  here presses, so nothing here clicks.
- DISARMING MUST PARK BOTH FRONT ENDS: `live = 0` with `rt_extreme` left at
  the current depth. A key can be well down at disarm (it need only have
  left the backplate) and the tapping profile is about to judge it against
  `actuation_mm`. Clearing `live` alone lets rule 3 read that depth as a
  FRESH actuation and press a finger on its way up — a phantom note landing
  exactly where a slider ends. Leaving `live` set is no better: its eventual
  release emits an up with no matching down. The parked state is rule 2's
  "engaged but not live", which emits nothing while rising and re-presses
  only on a real stroke. `regimetest` case H3 is the slider pattern.
- `act` needs NO lazy binding, and that is a consequence of the backplate
  rule rather than a separate fix. Arming leaves the picture in the
  toggle's canonical form — exactly one virtual down, `act` naming it
  truthfully — and every later edge is a plain backplate crossing, so
  `up(act)` is always a real release and `down(!act)` always a real press,
  whichever finger moves. An earlier design released one virtual at arming
  and then rebound `act` to the first key to DIP; when that was the other
  key, `act` named a key whose virtual was already up and the handover
  wrote up(already-up) + down(already-down), which the kernel dropped. That
  cost one note per rock. Do not reintroduce `act_bound`, and do not add a
  cycle to repair a no-op that can no longer happen.
- `last_press` decides which virtual SURVIVES arming, and it must stay the
  most recently pressed key. The second key presses at `actuation_mm` and
  the latch arms when it reaches the backplate, so only that travel
  separates its key-down from the arming release — 19ms and 24ms in the
  recorded traces. Keeping the first-planted key instead emits a key-up
  19ms after the key-down a note just landed on, short enough for a game to
  miss the press. The older virtual has been down an order of magnitude
  longer and costs nothing to release. It no longer affects whether a beat
  lands, but it decides which voice sounds first in a burst, so it must
  stay deterministic. `regimetest` case L pins it in both directions.
- The `S_RELEASE` transition at the first crossing needs no forcing. Both
  keys are still tracked as held (`SOCD_OFF` maintains `socd.k1`/`k2` too),
  so `state == k1 + k2 + value == 1` computes it in the existing machine.
  The disarm release falls out the same way.
- Arming must never be evaluated at a press edge, however tempting. The
  edge fires at `actuation_mm`, before the key has travelled, so any test of
  the incoming key's depth there hinges on where presses happen to land. The
  per-sample check exists precisely so the second key can reach the
  backplate on its own schedule.
- Gating the *emission* of a press is not a substitute for gating the
  mode: two presses that both reach `socd_apply` still trigger the toggle,
  which is what turns an incidental overlap into an extra keypress.
- Latch state keys off the sample's depths and `live`, NOT off
  `socd.k1`/`socd.k2`. Those track edges already applied and lag the
  front-end by a sample.
- In analog mode the digital k1/k2 of the analog keyboard MUST be dropped
  (`in->analog`, set by matching vendor/product in `input_try_open`), or
  every press is emitted twice — once from evdev and once from analog.
- `analog_key_feed` must keep `live` in sync with the edges it actually
  emits; `socd_apply` assumes strict press/release alternation per key.
- `release_mm` is DERIVED, never configured: `analog_config_resolve` sets
  it to `actuation_mm * ANALOG_RELEASE_RATIO`. A fixed offset (Keychron's
  `actn_pt - 3`) cannot work here because actuation ranges from 0.1mm to
  most of the travel, and a subtraction suiting one end goes negative at
  the other. A warning fires if the derived value lands under
  `ANALOG_RELEASE_FLOOR`, where the firmware stops reporting at all.
- `analog_key_feed` decides TAPPING edges on `actuation_mm`/`release_mm`
  and the rapid trigger ALONE, and RIDING edges on the backplate alone. It
  does not inspect the other key in either mode; the only cross-key
  decision in the analog path lives in `analog_socd_edge`. An earlier
  revision gated presses on the other key's depth to filter a resting
  finger's dip. That was solving a non-problem, and it filtered on depth,
  where a deliberate rock and an incidental overlap are identical. Do not
  bring it back.
- KNOWN RESIDUAL, accepted: armed with one finger resting on a bottomed
  key, taps on the other that never reach the floor register nothing. It
  requires having planted both first, it clears the moment the resting
  finger leaves the floor, and during a real burst every stroke bottoms out
  by definition. Untested in play as of this writing.

## Key invariants to preserve when editing `process_event`

- `k1`/`k2`/`act` state lives in a `socd_state_t` owned by each input
  source (embedded in `input_dev_t`, and in `analog_dev_t`), not global —
  multiple physical keyboards are tracked independently even though they
  share one virtual output device.
- Every emitted `EV_KEY` write must be followed by an `EV_SYN`/`SYN_REPORT`
  before the next state-changing write, otherwise userspace sees coalesced
  events.
- `MSC_SCAN` events are dropped rather than mirrored; all other non-k1/k2
  events pass through untouched.
- The daemon must never grab a doubletap output device
  (`is_doubletap_output`, keyed on uinput uniq `"doubletap"` and the
  configured device name) — every path that opens an input device has to
  keep going through this guard, in explicit mode too, or emitted events
  feed straight back in as input (infinite feedback loop).
