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

static void test_emit(unsigned type, unsigned code, int value) {
    if (type != EV_KEY) return;                    /* ignore SYN */
    int k = (code == V1) ? 0 : 1;
    if (vstate[k] == value) return;                /* no-change: dropped */
    vstate[k] = value;
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

static void fresh(void) {
    memset(&ad, 0, sizeof ad);
    vstate[0] = vstate[1] = 0;
    reset_log();
}


int main(void) {
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
    cfg.analog.rt_deep_press_mm   = 0.0f;  /* riding: backplate only */
    cfg.analog.rt_deep_release_mm = 0.15f;
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
    expect_int("  act not yet bound", ad.act_bound, 0);

    puts("D. the first key to dip binds act - dip on the key that kept it");
    sample(3.85f, 3.60f);
    expect_log("k2 lifts off the floor: one beat, onto v1", "v2- v1+");
    expect_clicks("  one click", "c1");
    expect_int("  act bound", ad.act_bound, 1);

    puts("E. rocking: one beat per lift, alternating");
    sample(3.85f, 3.85f);
    expect_log("k2 returns to the floor", "v1- v2+");
    expect_clicks("", "c2");
    sample(3.60f, 3.85f);
    expect_log("k1 lifts", "v2- v1+");
    expect_clicks("", "c1");

    puts("F. disarms when either key passes actuation on the way up");
    sample(0.90f, 3.85f);
    expect_log("k1 past 1.00mm: k2's hold is restored", "v1- v2+");
    expect_int("  disarmed", ad.deep, 0);
    expect_int("  k2 still held, nothing stranded", vstate[1], 1);

    puts("G. re-arms when both come back to the floor");
    sample(3.85f, 3.85f);
    expect_log("k1 plants again", "v2- v1+");
    expect_int("  armed again", ad.deep, 1);

    puts("H. the first dip landing on the key that stepped aside is silent");
    /* The mirror of D. That key's virtual is already up and the other's is
     * already down, so the toggle's revert is two no-change writes - and
     * the beat it would have claimed already landed under SOCD_OFF when
     * that key bottomed. One beat per lift, not two. */
    fresh();
    sample(3.85f, 0.00f);
    expect_log("k1 planted", "v1+");
    sample(3.85f, 2.00f);
    expect_log("k2 pressed", "v2+");
    sample(3.85f, 3.85f);
    expect_log("arms; v1 steps aside", "v1-");
    sample(3.60f, 3.85f);
    expect_log("k1 - the key that stepped aside - dips first", "");
    expect_clicks("  and no phantom click", "");
    expect_int("  act bound to it anyway", ad.act_bound, 1);

    puts("I. a hard tap short of the floor never arms it");
    fresh();
    sample(3.50f, 0.00f);
    expect_log("k1 pressed hard, 87.5% of travel", "v1+");
    sample(3.50f, 3.50f);
    expect_log("k2 too - still short of 3.80", "v2+");
    expect_int("  not armed", ad.deep, 0);

    puts("J. riding keeps the deep profile - an overshoot does not beat early");
    /* The bug the two profiles fix: picking the profile from a depth meant
     * a rock that overshot the line silently switched to the TAPPING
     * profile, so the return stroke re-pressed after rt_press_mm (0.30mm)
     * instead of waiting for the backplate. The beat landed early on
     * exactly the rocks that went high - amplitude changing the rule. */
    fresh();
    sample(3.85f, 0.00f);
    expect_log("k1 planted", "v1+");
    sample(3.85f, 2.00f);
    expect_log("k2 pressed", "v2+");
    sample(3.85f, 3.85f);
    expect_log("arms", "v1-");
    sample(3.85f, 3.20f);
    expect_log("k2 overshoots well clear of the backplate", "v2- v1+");
    expect_int("  still riding", ad.deep, 1);
    sample(3.85f, 3.55f);
    expect_log("+0.35mm back down: the tapping profile would fire here", "");
    sample(3.85f, 3.82f);
    expect_log("reaches the backplate - the beat lands there", "v1- v2+");
    expect_clicks("  one click, on time", "c2");

    printf("\n%s\n", fails ? "FAILURES ABOVE" : "all assertions passed");
    return fails ? 1 : 0;
}
