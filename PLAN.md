# Analog input and rapid trigger, gated by a `deep` latch

Implementation plan for `feature/analog-deep-latch`. Written before the code;
kept afterwards as the rationale record, because most of what follows is
*why not* rather than *what*.

## 1. The problem

`doubletapd`'s two useful modes each serve one play pattern and wreck the
other, and today you pick one at config time for the whole session.

**`socd: toggle` is right for rocking** — both fingers planted on k1/k2,
rocking between them — where it gives gap-free alternation.

**`socd: toggle` is wrong for alt-tapping**, the traditional style of
alternating discrete taps between the two keys. At speed the taps overlap: k2
lands before k1 has come back up. The toggle reads that overlap as a rock and
answers the k1 release with `S_RELEASE` — *re-pressing v1*, a note the player
never struck. Every overlapped pair costs a spurious click, and the `act`
inversion it leaves behind carries into everything after it.

**`socd: off` is right for alt-tapping** — k1/k2 remap one-to-one, overlap is
harmless, every tap is exactly one note — **and useless for rocking**, since
nothing alternates.

### What is *not* the problem

One key held deep while the other is lightly pressed or tapped. This is a
non-issue and always was. It is worth stating in the negative because two
prior branches were built entirely around solving it, and that is why they
were abandoned:

- **`feature/analog-input`** — hidraw plumbing plus a *press gate* (relative /
  depth / off) whose whole job was to decide whether a shallow press
  "deserved" to steal from a deep one. Excised wholesale on the successor
  branch (`a2a9a76`).
- **`feature/analog-split-rt`** — a rewrite with a two-profile rapid trigger,
  an offline replay tool, and a unit-test harness. Its commit log
  (`63f7ee8` → `c1a4ca7` → `7478c88` → `483a3ec`) is five reformulations of one
  question — *what arms the SOCD regime?* — each still phrased in terms of
  relative depth between the two keys. Its README enshrined the wrong premise
  ("the finger resting on the other key dips a fraction of a millimetre past
  actuation"); its `regimetest` case F treated a bottomed tap against a held
  key as an "accepted trade".

The HID layer underneath both was never the failure and is worth every line.
**Never reintroduce a depth-comparison gate between k1 and k2.**

## 2. The rule

An analog keyboard (Wooting 60HE+, `31E3:1320`) reports per-key travel depth.
That supplies a discriminator the digital path cannot have: **planting both
keys on the backplate is the rock, and nothing else is.** Alt-tapping never
puts both fingers at the floor at once — its defining shape is one finger
leaving as the other arrives.

> **`deep`** — a latching boolean, per analog device.
> **Arms** when *both* keys are at ≥95% of travel in one sample.
> While armed, k1/k2 run the `toggle` machine.
> **Disarms** the moment *either* key comes back above `actuation_mm`,
> returning to `off`.

Hysteresis is enormous by construction — arm at 3.80 mm, disarm at 1.00 mm on
the 4 mm defaults — so there is no chatter and no debounce counter. "One
consecutive sample" *is* the instantaneous test; if a counter is ever wanted
it wraps the arm test and nothing else moves.

## 3. Salvage

`feature/analog-split-rt` shares merge-base `70dd32b` with `main`, and
`git diff 70dd32b main` is empty — `main` is a no-op merge over it. The branch
therefore merges with zero conflicts. This is a merge, not a port.

| Salvaged | Verdict |
| --- | --- |
| hidraw open/probe/decode (`analog_open`, `analog_probe_fmt`, `analog_decode`, v1/v2 report formats) | keep as-is |
| HID-usage resolution (`hid_map_build`, `hid_map_from_any_keyboard`, `analog.hid_k1/k2`) | keep as-is |
| `analog_dev_open`/`_close`/`analog_reconcile` (hotplug recovery, `in->analog` de-duplication) | keep as-is |
| Split SOCD core: `socd_state_t` + `socd_apply(state, is_k1, value, mode, cfg)` on *synthesized* edges | **the key enabler** — keep |
| `socd_virtual_state()` mode-differ | keep, and lean on it harder |
| `analog_key_feed` rapid trigger, two profiles, bottom-out-always-presses | keep, minus its rule 4 |
| `-A` monitor, `-T` trace, `tools/replay.c`, `tools/regimetest.c`, CMake targets | keep; rewrite assertions |

Nothing is taken from `feature/analog-input`.

## 4. State machine changes (`doubletapd.c`)

### 4a. Simplify `analog_key_t`

The latch rules are pure depth tests on the current frame, so the per-key
commitment latch is dead weight. Delete rule 4 of `analog_key_feed` and the
field it writes:

```c
typedef struct {
    int   live;        /* an edge has been emitted; key counts as pressed */
    float rt_extreme;  /* local extreme for rapid trigger, mm */
} analog_key_t;                       /* `deep` removed */
```

`bottomed` stays inside `analog_key_feed` — it still drives the
"bottoming out always presses" rapid-trigger rule.

### 4b. Rename

`analog_dev_t`: `regime` → `deep`; add `act_bound`. Keep `last_press` — it no
longer picks `act`, only which virtual key survives the latch.
`analog_regime_set/_update` → `analog_deep_set/_update`; `analog_key_feed`'s
`riding` parameter is fed `ad->deep`.

### 4c. `analog_deep_update()` — the arming rule

```c
static void analog_deep_update(analog_dev_t *ad, const oid_config_t *cfg,
                               const float *depth) {
    const analog_config_t *a = &cfg->analog;
    float floor_mm = a->travel_mm - a->bottom_out_mm;   /* the backplate */

    if (ad->deep) {
        /* Either finger back past actuation ends it - the player has left
         * the rock and is tapping again. */
        if (depth[0] < a->actuation_mm || depth[1] < a->actuation_mm)
            analog_deep_set(ad, 0, cfg);
        return;
    }
    /* Both against the backplate in ONE frame. Alt-tapping cannot satisfy
     * this: its defining shape is one finger leaving as the other arrives,
     * so the two are never both at the floor together. */
    if (depth[0] >= floor_mm && depth[1] >= floor_mm)
        analog_deep_set(ad, 1, cfg);
}
```

`analog_drain` keeps its existing ordering, which was hard-won on the salvaged
branch and is still correct:

```
feed both keys  ->  analog_deep_update()  ->  apply this frame's edges
```

Feeding first makes `keys[k].live` this frame's truth, which the
reconciliation targets. Updating `deep` before the edges means the release
that ends a rock routes through `SOCD_OFF` rather than being reverted by the
toggle on its way out. The riding/tapping profile therefore lags `deep` by one
sample; harmless, and the explanatory comment stays.

### 4d. `analog_deep_set()` — reconciling the two pictures

The two modes disagree about what should be held: `OFF` wants a virtual key
per held physical key, the toggle wants exactly one. Flipping the flag without
settling that strands a key. `analog_deep_set` computes both pictures via
`socd_virtual_state()` and emits only the difference. Disarming at
`actuation_mm` (rather than the old `release_mm`) means the mode can flip
while a finger is still resting on a key, so this machinery matters *more*
under the new rules, not less.

- **Arming**: `socd.act` = the more recently pressed key (`last_press`,
  falling back to the other if it already lifted). Then `act_bound = 0`. The
  diff is release-only by construction (`{1,1}` → one down), so "silent at the
  latch" needs no special case.
- **Disarming**: `act_bound = 0`. The diff may include a **press**, restoring
  the still-held key's virtual (salvaged branch's `7aa192d`); preserve it.
- Delete the stale "Reconciliation only ever RELEASES" comment — `7aa192d`
  made it false.

### 4e. Deferred `act` binding

The forced `S_RELEASE` needs no forcing. At the latch both keys are still
tracked as held (`socd.k1 == socd.k2 == 1`, maintained by `SOCD_OFF` too), so
the first edge after it — necessarily a release, since a live key can only
release — computes `state == 1 == S_RELEASE` in the existing machine. What
remains is *which* key `act` names when it does: the first one to dip.

```c
static int analog_socd_edge(analog_dev_t *ad, int is_k1, int value,
                            const oid_config_t *cfg) {
    /* cfg->socd is SOCD_ANALOG here; socd_apply special-cases only OFF and
     * SNAPPY, so ANALOG runs the toggle path. Deliberate, not accidental. */
    int mode = ad->deep ? cfg->socd : SOCD_OFF;

    /* Snapshot BEFORE the act binding below. `before` has to describe the
     * virtual keys as actually written, and `act` is what names them - bind
     * first and this snapshot describes the picture for the NEW act, which
     * is the inverse of reality in exactly the case the gate exists for.
     * Do not "simplify" this by moving it down. */
    int held[2] = { ad->socd.k1, ad->socd.k2 };
    int before[2];
    socd_virtual_state(held, ad->socd.act, mode, before);

    /* The first key to dip out of the latch owns the output, so its release
     * is the one the toggle reverts. Deferring this is the point - at the
     * latch both keys are on the floor and nothing yet says which finger is
     * leaving. */
    if (ad->deep && !ad->act_bound && value == 0) {
        ad->socd.act  = is_k1;
        ad->act_bound = 1;
    }
    if (value == 1)
        ad->last_press = is_k1 ? 0 : 1;

    int voice = socd_apply(&ad->socd, is_k1, value, mode, cfg);

    int after[2];
    held[0] = ad->socd.k1; held[1] = ad->socd.k2;
    socd_virtual_state(held, ad->socd.act, mode, after);

    /* Suppress the click when the "press" was a no-change event the input
     * core drops - a beat you hear but the game never sees. */
    if (voice != VOICE_NONE &&
        !(before[voice - 1] == 0 && after[voice - 1] == 1))
        voice = VOICE_NONE;

    return voice;
}
```

The click gate is the common case, not a corner: ride k1, plant k2 (the latch
keeps v2 and releases v1), then lift k1. `S_RELEASE` emits `up(v1)` over an
already-up key and `down(v2)` over an already-down key — both swallowed by the
kernel's input core — yet `socd_apply` still reports `VOICE_V2`. That silence
is correct: the beat for that key already landed under `SOCD_OFF` when it
bottomed, and a second one is the doubled note `f81cdbc` removed. Every real
transition (`S_SINGLE`, `S_PRESS`, a rocking `S_RELEASE`, an `OFF` remap) still
changes the picture and still clicks.

## 5. Config surface

No new YAML keys. `bottom_out_mm` serves two jobs by deliberate choice: the
rapid-trigger backplate *and* the `deep` latch line.

- `bottom_out_mm` default **0.05 → 0.20 mm**. On the 4 mm default travel that
  puts the backplate at 3.80 mm = **95.0% of travel**. It also shifts the
  salvaged branch's rapid trigger slightly (bottom-out re-presses fire a touch
  earlier); that is the accepted cost of one knob serving both roles.
- `release_mm` stays derived (`actuation_mm * 0.8`) and stays the full-release
  reset inside `analog_key_feed`. It is no longer what ends the latch —
  `actuation_mm` is.
- `analog_config_check()` keeps every existing check; the
  actuation-below-backplate check is now load-bearing for the latch and its
  message should say so. Add a warning when `bottom_out_mm > 0.10 * travel_mm`
  — a latch line under 90% of travel starts to be reachable by a hard tap,
  which is what would let alt-tapping arm it.

## 6. Tests (`tools/regimetest.c`)

The harness `#include`s `doubletapd.c` and stubs only
`libevdev_uinput_write_event`, so it drives the daemon's real state machine.
Keep `sample()`, `expect_log()`, `expect_int()`, `fresh()`. Every existing
assertion A–F encodes the old premise and is replaced — case F in particular
asserted that a non-problem was a problem.

1. **Alt-tapping never arms — no overlap.** k1 down/up, then k2 down/up.
2. **Alt-tapping never arms — with overlap.** k2 bottoms while k1 is still
   held on its way up: the exact shape that makes plain `toggle` misfire.
   `deep == 0`, and the k1 release emits a plain release with **no `v2+`
   revert**. *The headline regression test for the whole feature.*
3. **Arms on the pair** — both at the backplate in one frame: `deep == 1`,
   exactly one virtual down, no press and no click.
4. **First dip binds `act`** — the kept key dips: one beat on the other.
5. **First dip on the released key** — nothing emitted, no phantom click.
6. **Steady rocking** — one beat per lift.
7. **Disarms at actuation** — the still-held key's virtual is restored,
   nothing stranded.
8. **Re-arms** — disarm without a full release, then back to the floor.
9. **Hard tap short of the floor never arms** — both held at 87.5%.
10. **Profiles** — a rock clear of the backplate keeps the *riding* profile;
    the beat lands at the backplate, not at `rt_press_mm`.

## 7. Verification

```sh
cmake -S . -B build && cmake --build build
./build/regimetest                     # expect "all assertions passed"
```

Live, on the analog board — all passive except the last:

```sh
./build/doubletapd -A                  # depths sane; k1/k2 usages resolve
./build/doubletapd -T > alttap.csv     # alt-tap a stream at speed, Ctrl-C
./build/replay alttap.csv              # THE number: latch count must be 0
./build/doubletapd -T > rock.csv       # plant and rock, Ctrl-C
./build/replay rock.csv                # latches once, one beat per lift
./build/doubletapd -c config.yaml      # real run
```

The alt-tap trace is the acceptance test. If it arms even once,
`bottom_out_mm` is too wide — `replay`'s sweep shows how the count moves with
it.

## 8. Known trade-offs

- **One knob, two jobs.** `bottom_out_mm` tunes the rapid-trigger backplate
  and the latch together; separating them means a second threshold.
- **Lifting out of a latch costs a second note** when `SOCD_OFF` wants the
  still-held key's virtual back down. The alternative is stranding the hold,
  which is worse.
- **A deliberately bottomed alt-tap pair could arm.** If two consecutive taps
  are both at ≥95% within the same 1 ms sample, the latch arms. `replay`
  measures this rather than assuming it, and `bottom_out_mm` is the dial. It
  is *not* to be defended against with a depth-comparison gate — that is the
  road both prior branches died on.

## 9. Validation against synthetic traces

Pending a real `-T` recording, the acceptance test was run against generated
traces (1000 Hz, 4.0 mm travel, 95% backplate):

| trace | latches | presses | note |
| --- | --- | --- | --- |
| 200 BPM alt-tap stream, both keys fully bottomed | **0** | 80 over 6 s | exactly one press per tap, no spurious notes |
| same with lazy 60 ms releases (heavy mid-travel overlap) | **0** | 80 (+1 boundary artifact) | overlap alone never arms it |
| plant both, rock at 13 Hz, lift out | **1** | 137 | one beat per lift |

Sweeping tap geometry finds the actual boundary. With a 22 ms down-stroke
and 25 ms up-stroke, the latch never arms at *any* note spacing down to 40
notes/sec while the per-key hold is 8-20 ms. It begins arming only once the
**hold time exceeds the note spacing** (40 ms hold at 35 ms spacing) - which
is the point at which you are no longer alternating taps but holding both
keys down, i.e. exactly the gesture the latch is meant to catch. The
discriminator lands on the right side of the line for structural reasons,
not by threshold tuning.

This does not replace recording your own hands; it establishes that the
mechanism separates the two patterns rather than merely being tuned to.
