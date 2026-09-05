#define _GNU_SOURCE
/*
 * regimetest - assertions over the `deep` latch and its mode transitions.
 *
 * Includes doubletapd.c and stubs only the uinput writes, so it exercises
 * the daemon's own state machine rather than a copy. Depths are fed one
 * sample at a time exactly as analog_drain feeds them.
 *
 *   build:  cmake --build build --target regimetest
 *   run:    ./build/regimetest
 */
#include <libevdev/libevdev.h>
#include <libevdev/libevdev-uinput.h>

static void test_emit(unsigned type, unsigned code, int value);
#define libevdev_uinput_write_event(d, t, c, v) test_emit((t), (c), (v))

#define main doubletapd_main
#include "doubletapd.c"
#undef main

#define V1 44   /* KEY_Z */
#define V2 45   /* KEY_X */

static char log_buf[512];
static char voice_log[128];
static int  fails;

/* What the kernel's input core would be holding for v1/v2. Writes that do
 * not change it are dropped there and never reach the game, so the harness
 * drops them too: every assertion below is about what a player actually
 * gets, not about what the daemon happened to write. */
static int vstate[2];

/* Counted here rather than by sampling vstate between reports: a handover
 * onto an already-down key cycles it (release then press) WITHIN one report,
 * so its rising edge is invisible to anything that only looks at the state
 * afterwards. That is the note case H is about, so the counter has to see
 * every transition as it is written. */
static unsigned vpresses;

static void test_emit(unsigned type, unsigned code, int value) {
    if (type != EV_KEY) return;                    /* ignore SYN */
    int k = (code == V1) ? 0 : 1;
    if (vstate[k] == value) return;                /* no-change: dropped */
    vstate[k] = value;
    if (value) vpresses++;
    size_t n = strlen(log_buf);
    snprintf(log_buf + n, sizeof log_buf - n, "%s%s%c",
             n ? " " : "", k == 0 ? "v1" : "v2", value ? '+' : '-');
}

/* Clicks, recorded from what analog_socd_edge returns. Note that a press
 * surviving the emit filter above and a click are equivalent by
 * construction - the click gate in analog_socd_edge suppresses a voice
 * exactly when the press it named was a no-change write - so the emit log
 * doubles as the click log. voice_log pins the gate itself directly. */
static void note_voice(int voice) {
    if (voice == VOICE_NONE) return;
    size_t n = strlen(voice_log);
    snprintf(voice_log + n, sizeof voice_log - n, "%s%s",
             n ? " " : "", voice == VOICE_V1 ? "c1" : "c2");
}

static void reset_log(void) { log_buf[0] = '\0'; voice_log[0] = '\0'; }

/* NB: the expect_* helpers below are deliberately side-effect free, and
 * sample() owns the reset. An earlier version cleared the logs inside
 * expect_log(), which wiped voice_log before expect_clicks() could read it
 * - so every "silent" click assertion passed without testing anything. */

static void expect_log(const char *what, const char *want) {
    if (strcmp(log_buf, want) != 0) {
        printf("  FAIL %-48s emitted [%s] want [%s]\n", what, log_buf, want);
        fails++;
    } else {
        printf("  ok   %-48s [%s]\n", what, want[0] ? want : "nothing");
    }
}

static void expect_clicks(const char *what, const char *want) {
    if (strcmp(voice_log, want) != 0) {
        printf("  FAIL %-48s clicked [%s] want [%s]\n", what, voice_log, want);
        fails++;
    } else {
        printf("  ok   %-48s [%s]\n", what, want[0] ? want : "silent");
    }
}

static void expect_int(const char *what, int got, int want) {
    if (got != want) {
        printf("  FAIL %-48s got %d want %d\n", what, got, want);
        fails++;
    } else {
        printf("  ok   %-48s\n", what);
    }
}

static oid_config_t cfg;
static analog_dev_t ad;

/* One analog report, exactly as analog_drain processes it. */
static void sample(float d0, float d1) {
    float d[2] = { d0, d1 };
    int   edges[2][2], ne[2];

    reset_log();

    for (int k = 0; k < 2; k++)
        ne[k] = analog_key_feed(&ad.keys, k, d[k], &cfg.analog,
                                ad.deep, edges[k]);
    analog_deep_update(&ad, &cfg, d);
    for (int k = 0; k < 2; k++)
        for (int e = 0; e < ne[k]; e++)
            note_voice(analog_socd_edge(&ad, k == 0, edges[k][e], &cfg));
}

/* ---------------------------------------------------------------------- *
 * analog_drain, driven by real encoded reports over a pipe.
 *
 * Everything above calls analog_key_feed / analog_deep_update /
 * analog_socd_edge directly, and so does tools/replay.c. That leaves
 * analog_drain - the read, the decode, and the depth rebuild that feeds all
 * three - as the one link in the analog chain nothing exercises, which is
 * exactly where a daemon that disagrees with replay would have to differ.
 * Case K closes that hole by running one gesture down both paths and
 * demanding the same output.
 *
 * A v2 report is 16 four-byte slots, [pad, key, packed, value], with the
 * 10-bit depth split across `value` and the top two bits of `packed`; a key
 * at rest is ABSENT rather than zero, which is the property analog_drain
 * rebuilds each frame from. Writes are 64 bytes, well under PIPE_BUF, so
 * each read() lifts exactly one report the way it does from hidraw.
 * ---------------------------------------------------------------------- */
static void encode_v2(unsigned char *b, const uint16_t *usage,
                      const float *depth, float travel) {
    int slot = 0;
    memset(b, 0, ANALOG_V2_REPORT);
    for (int k = 0; k < 2; k++) {
        if (depth[k] <= 0.0f) continue;          /* at rest: not reported */
        unsigned v10 = (unsigned)(depth[k] / travel * 1023.0f + 0.5f);
        if (v10 > 1023) v10 = 1023;
        b[slot * 4 + 1] = (unsigned char)(usage[k] & 0xFF);
        b[slot * 4 + 2] = (unsigned char)((v10 & 0x03) << 6);
        b[slot * 4 + 3] = (unsigned char)(v10 >> 2);
        slot++;
    }
}

static void fresh(void) {
    memset(&ad, 0, sizeof ad);
    vstate[0] = vstate[1] = 0;
    vpresses  = 0;
    reset_log();
}


int main(void) {
    g_log_quiet = 1;   /* the assertions are the output; not the latch log */
    memset(&cfg, 0, sizeof cfg);
    cfg.socd = SOCD_ANALOG;
    cfg.v1   = V1;
    cfg.v2   = V2;
    /* The daemon defaults, so the numbers here are the ones a player sees:
     * backplate at 3.80mm (95% of travel), latch disarms at 1.00mm. */
    cfg.analog.travel_mm     = 4.00f;
    cfg.analog.actuation_mm  = 1.00f;
    cfg.analog.rt_enabled    = 1;
    cfg.analog.rt_press_mm   = 0.30f;
    cfg.analog.rt_release_mm = 0.30f;
    cfg.analog.bottom_out_mm = 0.20f;      /* backplate at 3.80mm */
    /* No riding profile exists any more: while the latch is armed the
     * backplate is the switch and no rapid trigger runs. */
    analog_config_resolve(&cfg.analog);    /* release_mm follows actuation */

    /* ------------------------------------------------------------------ *
     * The feature exists to keep alt-tapping out of the toggle. Cases A
     * and B are therefore the ones that matter most; everything after
     * them is about behaving correctly once a rock has actually started.
     * ------------------------------------------------------------------ */

    puts("A. alt-tapping never arms it - taps clear of each other");
    fresh();
    sample(4.00f, 0.00f);
    expect_log("k1 taps", "v1+");
    sample(0.00f, 0.00f);
    expect_log("k1 leaves", "v1-");
    sample(0.00f, 4.00f);
    expect_log("k2 taps", "v2+");
    sample(0.00f, 0.00f);
    expect_log("k2 leaves", "v2-");
    expect_int("  never armed", ad.deep, 0);

    puts("B. alt-tapping never arms it - taps OVERLAPPING (the headline)");
    /* This is the shape that makes plain `toggle` misfire: k2 actuates
     * while k1 is still emitting-pressed, so the toggle steals, and then
     * k1's release is answered by re-pressing v1 - a note nobody struck.
     * Under the latch, k1 never reaches the floor at the same instant k2
     * does, so the whole exchange stays in SOCD_OFF and the k1 release is
     * just a release. */
    fresh();
    sample(4.00f, 0.00f);
    expect_log("k1 bottoms", "v1+");
    sample(3.85f, 1.20f);
    expect_log("k2 actuates while k1 is still pressed", "v2+");
    expect_int("  both virtual keys down (plain remap)", vstate[0] + vstate[1], 2);
    expect_int("  still not armed", ad.deep, 0);
    sample(3.20f, 3.85f);
    expect_log("k1 lifts: a plain release, NOT a toggle revert", "v1-");
    expect_clicks("  and no phantom beat", "");
    sample(0.50f, 3.85f);
    expect_log("k1 gone", "");
    expect_int("  never armed", ad.deep, 0);

    puts("C. planting both keys on the backplate arms it, silently");
    fresh();
    sample(3.85f, 0.00f);
    expect_log("k1 planted", "v1+");
    sample(3.85f, 2.00f);
    expect_log("k2 pressed but not yet down", "v2+");
    sample(3.85f, 3.85f);
    expect_log("k2 reaches the floor: arms, v1 steps aside", "v1-");
    expect_clicks("  arming itself is silent", "");
    expect_int("  armed", ad.deep, 1);
    expect_int("  exactly one virtual key held", vstate[0] + vstate[1], 1);

    puts("D. while riding, the backplate IS the switch - leaving it is a beat");
    /* No rapid trigger runs here and no reversal distance is consulted: the
     * only line that means anything while riding is the floor. */
    sample(3.85f, 3.60f);
    expect_log("k2 lifts off the floor: one beat, onto v1", "v2- v1+");
    expect_clicks("  one click", "c1");

    puts("E. rocking: one beat per crossing, alternating");
    sample(3.85f, 3.85f);
    expect_log("k2 returns to the floor", "v1- v2+");
    expect_clicks("", "c2");
    sample(3.60f, 3.85f);
    expect_log("k1 lifts", "v2- v1+");
    expect_clicks("", "c1");

    puts("F. a finger lifting CLEAR does not disarm - the other is still planted");
    /* The regression this rule exists for, from
     * tests/toggle_5burst_3phys_5virt.csv. The old rule disarmed as soon as
     * either key passed actuation, so a finger lifted clear mid-burst tore
     * the latch down while the other finger was still on the floor - and the
     * next note then fired off the TAPPING profile at actuation instead of
     * at the backplate. Two thresholds inside one burst is jitter. */
    sample(0.90f, 3.85f);
    expect_log("k1 past actuation - the old rule disarmed here", "");
    expect_int("  still armed: k2 never left the floor", ad.deep, 1);
    expect_int("  and its virtual is still held", vstate[0], 1);
    sample(0.00f, 3.85f);
    expect_log("k1 lifted clear off the switch entirely", "");
    expect_int("  still riding", ad.deep, 1);

    puts("G. and its return beats at the BACKPLATE, not at actuation");
    sample(1.50f, 3.85f);
    expect_log("k1 back past actuation: no beat here", "");
    sample(3.70f, 3.85f);
    expect_log("still short of the floor: still nothing", "");
    sample(3.85f, 3.85f);
    expect_log("reaches the floor - the beat lands there", "v1- v2+");
    expect_clicks("  on time", "c2");

    puts("H. disarms only when BOTH are off the floor, and releases only");
    sample(3.60f, 3.60f);
    expect_log("both leave the backplate", "v2-");
    expect_int("  disarmed", ad.deep, 0);
    expect_int("  nothing left held", vstate[0] + vstate[1], 0);
    expect_clicks("  and nothing pressed, so nothing clicked", "");
    /* Both keys are now PARKED: live cleared, rt_extreme still at 3.60. The
     * tapping profile judges against actuation_mm (1.00) from here, so a key
     * sitting this deep is exactly where a broken handoff shows up - clear
     * rt_extreme too and rule 3 reads 3.60mm as a fresh actuation and presses
     * a finger that is on its way up. Hold them there for a sample to prove
     * it does not, then bring one back down to prove it still can. */
    sample(3.60f, 3.60f);
    expect_log("held deep after the disarm: no fresh actuation", "");
    sample(3.85f, 3.60f);
    expect_log("k1 bottoms out again: recovers on a real stroke", "v1+");

    puts("H2. plant, plant, rock: two physical presses, three notes");
    /* The count tests/toggle_triple_2phys_3virt.csv is named for. */
    fresh();
    {
        static const float rock[][2] = {
            { 2.00f, 0.00f }, { 3.85f, 0.00f },   /* plant k1  */
            { 3.85f, 2.00f }, { 3.85f, 3.85f },   /* plant k2 -> arms */
            { 3.60f, 3.85f },                     /* rock: k1 leaves floor */
            { 0.00f, 3.85f }, { 0.00f, 0.00f },   /* lift out */
        };
        for (size_t i = 0; i < sizeof rock / sizeof rock[0]; i++)
            sample(rock[i][0], rock[i][1]);
        expect_int("three virtual presses from two physical",
                   (int)vpresses, 3);
    }

    puts("H3. a slider held through a full lift, ending when both leave");
    /* circle, circle, slider, circle. The hold has to survive one finger
     * coming clear off the switch, end when the planted finger lifts, and
     * NOT emit a phantom press for the finger that is on its way up. */
    fresh();
    sample(3.85f, 0.00f);
    expect_log("circle 1", "v1+");
    sample(3.85f, 2.00f);
    expect_log("circle 2", "v2+");
    sample(3.85f, 3.85f);
    expect_log("both plant: arms", "v1-");
    sample(3.60f, 3.85f);
    expect_log("k1 leaves the floor: the slider's note", "v2- v1+");
    sample(0.00f, 3.85f);
    expect_log("k1 lifts clear - the hold must survive", "");
    expect_int("  v1 still holding the slider", vstate[0], 1);
    sample(0.00f, 3.60f);
    expect_log("k2 lifts at the end of the track: slider ends", "v1-");
    expect_int("  disarmed", ad.deep, 0);
    expect_clicks("  no phantom press for the finger on its way up", "");
    sample(1.50f, 0.00f);
    expect_log("the final circle, tapped at actuation", "v1+");

    puts("I. a hard tap short of the floor never arms it");
    fresh();
    sample(3.50f, 0.00f);
    expect_log("k1 pressed hard, 87.5% of travel", "v1+");
    sample(3.50f, 3.50f);
    expect_log("k2 too - still short of 3.80", "v2+");
    expect_int("  not armed", ad.deep, 0);

    puts("J. while riding, depth short of the floor never beats");
    /* What the old two-profile rule was protecting, now structural. An
     * earlier design ran a second rapid-trigger profile while riding and
     * picked between the two by the latch; a rock that overshot re-pressed
     * after a reversal distance instead of at the floor, so the beat landed
     * early on exactly the rocks that went high. With the backplate as the
     * only line, amplitude cannot reach the decision at all - there is
     * nothing left to select between. */
    fresh();
    sample(3.85f, 0.00f);
    expect_log("k1 planted", "v1+");
    sample(3.85f, 2.00f);
    expect_log("k2 pressed", "v2+");
    sample(3.85f, 3.85f);
    expect_log("arms", "v1-");
    sample(3.85f, 3.60f);
    expect_log("k2 leaves the floor: beat", "v2- v1+");
    sample(3.85f, 2.00f);
    expect_log("k2 travels far further up: no second beat", "");
    sample(3.85f, 3.70f);
    expect_log("and back down, still short of the floor: nothing", "");
    sample(3.85f, 3.85f);
    expect_log("reaches the floor - the beat lands there, not before",
               "v1- v2+");
    expect_clicks("  one click, on time", "c2");

    puts("K. analog_drain agrees with the direct calls, report for report");
    {
        /* One gesture, two paths: the direct calls every other case (and
         * replay) makes, then the same depths encoded as hardware reports
         * and pushed through analog_drain. Any divergence here is a daemon
         * that does not do what replay predicts. */
        static const float gesture[][2] = {
            { 0.00f, 0.00f }, { 2.00f, 0.00f }, { 3.85f, 0.00f },
            { 3.85f, 2.00f }, { 3.85f, 3.85f },            /* arms */
            { 3.85f, 3.20f }, { 3.85f, 3.82f },            /* rock */
            { 3.20f, 3.85f }, { 3.85f, 3.85f },
            { 0.00f, 3.85f }, { 0.00f, 0.00f },            /* lift out */
        };
        size_t nstep = sizeof gesture / sizeof gesture[0];
        char   direct[512] = "", drained[512] = "";

        unsigned arm_direct = 0, arm_drain = 0;

        fresh();
        for (size_t i = 0; i < nstep; i++) {
            int was = ad.deep;
            sample(gesture[i][0], gesture[i][1]);
            if (!was && ad.deep) arm_direct++;
            size_t n = strlen(direct);
            snprintf(direct + n, sizeof direct - n, "%s%s",
                     n && log_buf[0] ? "|" : "", log_buf);
        }

        int fd[2];
        if (pipe(fd) != 0) {
            printf("  FAIL %-48s pipe: %s\n", "pipe for the drain path",
                   strerror(errno));
            fails++;
        } else {
            analog_dev_t dd;
            memset(&dd, 0, sizeof dd);
            dd.kind = EP_ANALOG;
            dd.fd   = fd[0];
            dd.fmt  = ANALOG_FMT_V2;
            dd.usage[0] = 0x1d;            /* KEY_Z */
            dd.usage[1] = 0x1b;            /* KEY_X */
            snprintf(dd.path, sizeof dd.path, "pipe");
            fcntl(fd[0], F_SETFL, O_NONBLOCK);
            vstate[0] = vstate[1] = 0;

            for (size_t i = 0; i < nstep; i++) {
                unsigned char rep[ANALOG_V2_REPORT];
                float         d[2] = { gesture[i][0], gesture[i][1] };
                encode_v2(rep, dd.usage, d, cfg.analog.travel_mm);
                reset_log();
                if (write(fd[1], rep, sizeof rep) != (ssize_t)sizeof rep) {
                    printf("  FAIL short write to the drain pipe\n");
                    fails++;
                    break;
                }
                int was = dd.deep;
                analog_drain(&dd, &cfg);
                if (!was && dd.deep) arm_drain++;
                size_t n = strlen(drained);
                snprintf(drained + n, sizeof drained - n, "%s%s",
                         n && log_buf[0] ? "|" : "", log_buf);
            }
            close(fd[0]);
            close(fd[1]);

            if (strcmp(direct, drained) != 0) {
                printf("  FAIL %-48s\n    direct [%s]\n    drain  [%s]\n",
                       "same virtual output either way", direct, drained);
                fails++;
            } else {
                printf("  ok   %-48s [%s]\n",
                       "same virtual output either way", direct);
            }
            /* Compare the two paths against the EXPECTED count, not just
             * against each other: a regression that made the latch
             * unarmable would break both identically and still pass an
             * equivalence-only check, quietly reducing this case to a test
             * of the plain remap. */
            expect_int("  direct path armed exactly once", (int)arm_direct, 1);
            expect_int("  drain path armed exactly once", (int)arm_drain, 1);
            expect_int("  and both ended disarmed", dd.deep | ad.deep, 0);
        }
    }

    puts("L. the key that plants SECOND keeps its virtual - last input wins");
    /* Arming has to drop one of two held keys, and it keeps the most recently
     * pressed one. Not cosmetic: the second key presses at actuation_mm and
     * the latch arms when it reaches the BACKPLATE, so only the travel
     * between those two thresholds separates its key-down from the arming
     * release - 19ms and 24ms in tests/toggle_5burst and tests/toggle_triple.
     * Keep the first-planted key instead and you emit a key-up 19ms after the
     * key-down a note just landed on, short enough for a game to miss the
     * press outright. The older virtual has been down an order of magnitude
     * longer (101ms, 116ms) and costs nothing to release.
     *
     * Since the redesign this no longer affects whether a beat lands - every
     * handover is real either way - but it still decides which voice sounds
     * first in a burst, so it must stay deterministic. */
    fresh();
    sample(3.85f, 0.00f);
    expect_log("k1 plants first", "v1+");
    sample(3.85f, 2.00f);
    expect_log("k2 pressed second", "v2+");
    sample(3.85f, 3.85f);
    expect_log("arms: the OLDER virtual steps aside", "v1-");
    expect_int("  the newer one keeps it", vstate[1], 1);

    /* Mirrored. Nothing privileges k1 over k2 anywhere in this path - the
     * rule is "planted second", not "k2". */
    fresh();
    sample(0.00f, 3.85f);
    expect_log("k2 plants first", "v2+");
    sample(2.00f, 3.85f);
    expect_log("k1 pressed second", "v1+");
    sample(3.85f, 3.85f);
    expect_log("arms: again the older one steps aside", "v2-");
    expect_int("  and again the newer keeps it", vstate[0], 1);

    printf("\n%s\n", fails ? "FAILURES ABOVE" : "all assertions passed");
    return fails ? 1 : 0;
}
