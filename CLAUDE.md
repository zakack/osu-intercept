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

The entire implementation lives in `doubletapd.c` (~2450 lines) — there is
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
   front-end: it turns depth into press/release edges using `actuation_mm`
   plus a two-profile software rapid trigger — the anchor (`rt_extreme`)
   whose profile follows the regime — `press_mm`/`release_mm` while
   tapping, `deep_press_mm`/`deep_release_mm` while riding, either of which
   may be `off`. k1/k2 -> HID usage
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
- The wobble is a GESTURE, and the backplate defines it. It starts when
  BOTH keys are bottomed at the same instant and ends when either stops
  being committed. `deep` is that commitment latch: set when a `live` key
  reaches `travel_mm - bottom_out_mm`, cleared only by rule 1 (a full
  release past `release_mm`). So `bottom_out_mm` is load-bearing — it is
  not merely a rapid-trigger corner case — and the config check refuses a
  zero value.
- It deliberately does NOT distinguish a slider held on one key from a
  wobble starting on both. At the instant the second key reaches the floor
  those are the same observation — same depth, velocity and dwell — and the
  information separating them does not exist yet. Every attempt to find it
  in one sample has failed and will; what the backplate buys instead is a
  trigger a player can aim, being a hard physical stop. The harness asserts
  this trade explicitly so it stays deliberate.
- The regime is resolved once per sample in `analog_regime_update`, called
  from `analog_drain` after both keys are fed and BEFORE that sample's
  edges are applied. Both directions need that ordering: engaging first
  lets the arriving key's press take over under the toggle immediately
  (with no churn — only one virtual key is down at that point, the press
  that completes the pair not yet applied), and lapsing first lets the
  release that ends a wobble land as a plain release instead of being
  reverted by a toggle on its way out.
- The rapid-trigger profile is picked by the REGIME, never by a depth.
  `analog_key_feed` takes it as an argument. An earlier revision selected
  on the anchor against a threshold, which re-decided mid-gesture: a rock
  that overshot the line silently switched to the tapping profile, so its
  return stroke re-pressed after `press_mm` instead of waiting for the
  backplate, and the beat landed early on exactly the rocks that went high.
  Amplitude changing the rule is a limp, not a threshold. The regime lags
  by one sample here and that is harmless — the sample that engages is a
  press stroke, where no reversal test runs, and the sample that lapses is
  a full release, which rule 1 handles before either profile matters.
- The lapse does NOT test `live`. Rapid trigger lifts and re-presses the
  emitted keys constantly, so a moment where one is up is mid-rock, not the
  end of the gesture; only `deep` clearing ends it.
- Changing regime under held fingers MUST reconcile the virtual keys, which
  is what `analog_regime_set` does: the two modes disagree about what should
  be down (OFF wants one virtual key per held physical key, the toggle wants
  exactly one), and flipping the flag without settling that strands a key.
  The current picture comes from `socd`, which reflects edges already
  APPLIED; the target picture comes from the front-end's `live`, which is
  this sample's truth. Using `socd` for the target presses a key for a
  finger that left on this very sample and releases it again when the edge
  lands — a phantom note on the way out of a wobble.
- Reconciliation only ever RELEASES, never presses. `deep` does not clear
  until `release_mm`, so a finger leaving is still inside the regime while
  it rises: its rapid-trigger release fires at `deep_release_mm` off the
  floor and the reverting toggle answers by pressing the OTHER key — the
  one being lifted. Pressing the still-held key here to complete OFF's
  picture would fire a SECOND note for the same departure. One beat per
  lift. The still-held key's virtual is left up until that key next moves,
  where the plain remap presses it again. No threshold fixes this: the rt
  release fires within `deep_release_mm` of the floor, while the regime
  lasts until the key clears `release_mm`, so a departing finger is always
  still inside it when that edge lands. The only alternative is snappy
  release semantics, which halves the wobble's note rate — a feel change,
  not a fix.
- Engagement must never be evaluated at a press edge, however tempting. The
  edge fires at `actuation_mm` or at a rapid-trigger reversal, before the
  key has travelled, so any test of the incoming key's depth there hinges
  on where re-presses happen to land. The per-sample check exists precisely
  so the second key can reach the backplate on its own schedule.
- Gating the *emission* of a press is not a substitute for gating the
  regime: two presses that both reach `socd_apply` still trigger the toggle,
  which is what turns an incidental overlap into an extra keypress.
- Regime state keys off `deep` and `live`, NOT off `socd.k1`/`socd.k2`.
  Those track edges already applied and lag the front-end by a sample.
- In analog mode the digital k1/k2 of the analog keyboard MUST be dropped
  (`in->analog`, set by matching vendor/product in `input_try_open`), or
  every press is emitted twice — once from evdev and once from analog.
- `analog_key_feed` must keep `live` in sync with the edges it actually
  emits; `socd_apply` assumes strict press/release alternation per key.
- A zero `rt_deep_press_mm`/`rt_deep_release_mm` means that zone's edge is
  DISABLED, not that any motion triggers it. The config parser rejects a
  literal `0` and requires `off`, because the two readings are opposites.
  With `deep_press_mm` off, `bottom_out_mm` is the only remaining re-press
  and the config check refuses to let both be disabled.
- The deep values are resolved from the tapping values in
  `analog_config_resolve` before any validation, so everything downstream
  reads concrete numbers and an unconfigured daemon runs one profile.
- `release_mm` is DERIVED, never configured: `analog_config_resolve` sets
  it to `actuation_mm * ANALOG_RELEASE_RATIO`. A fixed offset (Keychron's
  `actn_pt - 3`) cannot work here because actuation ranges from 0.1mm to
  most of the travel, and a subtraction suiting one end goes negative at
  the other. A warning fires if the derived value lands under
  `ANALOG_RELEASE_FLOOR`, where the firmware stops reporting at all.
- `analog_key_feed` decides edges on `actuation_mm`/`release_mm` and the
  rapid trigger ALONE. It does not inspect the other key at all; the only
  cross-key decision in the analog path lives in `analog_socd_edge`. An
  earlier revision gated presses on the other key's depth to filter a
  resting finger's dip — that filtered on depth, but a deliberate wobble and
  an incidental overlap during alternate tapping are identical in depth, so
  it could not separate them. Anything reaching for that problem again wants
  direction or dwell, not a depth threshold, and belongs in
  `analog_socd_edge` where the regime is decided.

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
