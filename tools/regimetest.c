#define _GNU_SOURCE
/*
 * regimetest - assertions over the analog regime and its mode transitions.
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
static int  fails;

static void test_emit(unsigned type, unsigned code, int value) {
    if (type != EV_KEY) return;                    /* ignore SYN */
    size_t n = strlen(log_buf);
    snprintf(log_buf + n, sizeof log_buf - n, "%s%s%c",
             n ? " " : "", code == V1 ? "v1" : "v2", value ? '+' : '-');
}

static void reset_log(void) { log_buf[0] = '\0'; }

static void expect_log(const char *what, const char *want) {
    if (strcmp(log_buf, want) != 0) {
        printf("  FAIL %-48s emitted [%s] want [%s]\n",
               what, log_buf, want);
        fails++;
    } else {
        printf("  ok   %-48s [%s]\n", what, want[0] ? want : "nothing");
    }
    reset_log();
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
    int   was_deep[2] = { ad.keys.key[0].deep, ad.keys.key[1].deep };

    for (int k = 0; k < 2; k++)
        ne[k] = analog_key_feed(&ad.keys, k, d[k], &cfg.analog, edges[k]);
    analog_regime_pre(&ad, &cfg);
    for (int k = 0; k < 2; k++)
        for (int e = 0; e < ne[k]; e++)
            analog_socd_edge(&ad, k == 0, edges[k][e], &cfg);
    analog_regime_post(&ad, &cfg, was_deep, ne);
}

static void fresh(void) {
    memset(&ad, 0, sizeof ad);
    reset_log();
}

int main(void) {
    memset(&cfg, 0, sizeof cfg);
    cfg.socd = SOCD_ANALOG;
    cfg.v1   = V1;
    cfg.v2   = V2;
    /* the numbers this was tuned against: 60HE+, wobble style */
    cfg.analog.travel_mm     = 3.5f;
    cfg.analog.actuation_mm  = 0.10f;
    cfg.analog.release_mm    = 0.08f;
    cfg.analog.socd_depth_mm = 3.3f;
    cfg.analog.rt_enabled    = 1;
    cfg.analog.rt_press_mm   = 0.40f;
    cfg.analog.rt_release_mm = 0.10f;
    cfg.analog.bottom_out_mm = 0.08f;      /* bottomed at >= 3.42mm */
    cfg.analog.rt_deep_mm         = 3.3f;
    cfg.analog.rt_deep_press_mm   = 0.0f;  /* off: backplate only */
    cfg.analog.rt_deep_release_mm = 0.10f;

    /* The case that motivated the rule: a slider held on k1 while the next
     * circle is tapped on k2. Both keys reach the floor, so "both deep"
     * alone would engage here and release the slider mid-hold. */
    puts("A. slider into a circle: never engages, hold survives");
    fresh();
    sample(3.50f, 0.00f);
    expect_log("k1 pressed for the slider", "v1+");
    expect_int("  k1 deep", ad.keys.key[0].deep, 1);
    sample(3.50f, 3.50f);
    expect_log("k2 taps the next circle - plain remap", "v2+");
    expect_int("  BOTH deep, yet regime stays off", ad.regime, 0);
    expect_int("  slider still held", ad.socd.k1, 1);
    sample(3.50f, 0.00f);
    expect_log("k2 released", "v2-");
    expect_int("  still off", ad.regime, 0);
    sample(3.50f, 3.50f);
    expect_log("another circle on k2", "v2+");
    expect_int("  still off", ad.regime, 0);
    sample(0.00f, 0.00f);
    expect_log("slider ends, both up", "v1- v2-");

    /* Same opening as A - k1 already parked deep - but this time the
     * parked finger starts rocking. That edge is the whole signal. */
    puts("B. wobble from a parked finger: engages once BOTH rock");
    fresh();
    sample(3.50f, 0.00f);
    expect_log("k1 parked at the floor", "v1+");
    sample(3.50f, 3.50f);
    expect_log("k2 joins it deep - nothing yet", "v2+");
    expect_int("  regime off", ad.regime, 0);
    sample(3.50f, 3.35f);
    expect_log("k2 rocks up - plain release", "v2-");
    expect_int("  one finger moving is not a wobble", ad.regime, 0);
    sample(3.50f, 3.50f);
    expect_log("k2 rocks back - plain re-press", "v2+");
    expect_int("  still off (k1 has not moved)", ad.regime, 0);
    sample(3.35f, 3.50f);
    expect_log("k1 rocks too: plain release, THEN engage", "v1-");
    expect_int("  regime on", ad.regime, 1);
    sample(3.50f, 3.50f);
    expect_log("k1 rocks back - toggle steals", "v2- v1+");

    puts("C. the wobble keeps rocking under the toggle");
    sample(3.50f, 3.35f);
    expect_log("k2 rocks up", "v1- v2+");
    sample(3.50f, 3.50f);
    expect_log("k2 rocks back", "v2- v1+");
    expect_int("  still engaged", ad.regime, 1);

    puts("D. lifting a finger out ends it, no second note");
    sample(0.02f, 3.50f);
    /* Two v1- writes: the reconciliation releases it, then k1's own
     * release is routed through SOCD_OFF over an already-up key. The
     * input core drops a no-change key event, so userspace sees one.
     * What matters is that no PRESS is emitted. */
    expect_log("k1 leaves; k2 still held", "v1- v1-");
    expect_int("  regime off", ad.regime, 0);
    expect_int("  rocked latches cleared", ad.rocked[0] + ad.rocked[1], 0);

    puts("E. fast alternation never engages");
    fresh();
    sample(3.50f, 0.00f);
    expect_log("k1 bottoms out", "v1+");
    sample(3.35f, 0.00f);
    expect_log("k1 lifts - rapid trigger releases it", "v1-");
    expect_int("  k1 latched deep but not live", ad.keys.key[0].deep, 1);
    sample(3.35f, 3.50f);
    expect_log("k2 bottoms out while k1 is still latched", "v2+");
    expect_int("  BOTH deep, regime stays off", ad.regime, 0);
    sample(0.02f, 3.50f);
    expect_log("k1 leaves", "");
    expect_int("  still off", ad.regime, 0);

    printf("\n%s\n", fails ? "FAILURES ABOVE" : "all assertions passed");
    return fails ? 1 : 0;
}
