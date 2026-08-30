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
        printf("  FAIL %-48s emitted [%s] want [%s]\n", what, log_buf, want);
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

    for (int k = 0; k < 2; k++)
        ne[k] = analog_key_feed(&ad.keys, k, d[k], &cfg.analog,
                                ad.regime, edges[k]);
    analog_regime_update(&ad, &cfg, d);
    for (int k = 0; k < 2; k++)
        for (int e = 0; e < ne[k]; e++)
            analog_socd_edge(&ad, k == 0, edges[k][e], &cfg);
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
    cfg.analog.travel_mm     = 3.5f;
    cfg.analog.actuation_mm  = 0.10f;
    cfg.analog.release_mm    = 0.08f;
    cfg.analog.rt_enabled    = 1;
    cfg.analog.rt_press_mm   = 0.08f;
    cfg.analog.rt_release_mm = 0.10f;
    cfg.analog.bottom_out_mm = 0.10f;      /* backplate at 3.40mm */
    cfg.analog.rt_deep_press_mm   = 0.0f;  /* riding: backplate only */
    cfg.analog.rt_deep_release_mm = 0.10f;

    puts("A. planting both fingers starts it, immediately");
    fresh();
    sample(3.50f, 0.00f);
    expect_log("k1 to the backplate", "v1+");
    expect_int("  k1 committed", ad.keys.key[0].deep, 1);
    expect_int("  regime off", ad.regime, 0);
    sample(3.50f, 3.50f);
    expect_log("k2 joins it: engages at once, toggle steals", "v1- v2+");
    expect_int("  regime on", ad.regime, 1);
    sample(3.50f, 3.38f);
    expect_log("rocking, no warm-up", "v2- v1+");
    sample(3.50f, 3.50f);
    expect_log("and back", "v1- v2+");

    /* The bug this design replaces: picking the profile from the anchor
     * meant an overshooting rock silently switched to the TAPPING profile,
     * so the return stroke re-pressed after rt_press_mm (0.08mm) instead
     * of waiting for the backplate. The beat landed early on exactly the
     * rocks that went high - amplitude changing the rule. */
    puts("B. a big rock keeps the riding profile (no early beat)");
    sample(3.50f, 3.00f);
    expect_log("k2 overshoots well clear of the backplate", "v2- v1+");
    expect_int("  still riding", ad.regime, 1);
    sample(3.50f, 3.10f);
    expect_log("+0.10 back down: tapping profile would fire here", "");
    sample(3.50f, 3.30f);
    expect_log("still short of the backplate", "");
    sample(3.50f, 3.45f);
    expect_log("reaches the backplate - beat lands there", "v1- v2+");

    puts("C. lifting a finger out ends it, no second note");
    sample(3.50f, 0.02f);
    /* Two writes: the reconciliation releases it, then k2's own release
     * routes through SOCD_OFF over an already-up key. The input core drops
     * a no-change key event, so userspace sees one. What matters is that
     * no PRESS is emitted. */
    expect_log("k2 leaves the switch", "v2- v2-");
    expect_int("  regime off", ad.regime, 0);
    expect_int("  k2 no longer committed", ad.keys.key[1].deep, 0);
    expect_int("  k1 still held", ad.socd.k1, 1);

    puts("D. deep but not bottomed never starts it");
    fresh();
    sample(3.50f, 0.00f);
    expect_log("k1 on the backplate", "v1+");
    sample(3.50f, 3.35f);
    expect_log("k2 pressed hard but short of 3.40", "v2+");
    expect_int("  regime stays off", ad.regime, 0);
    expect_int("  k2 not committed", ad.keys.key[1].deep, 0);

    puts("E. alternate tapping never starts it");
    fresh();
    sample(3.50f, 0.00f);
    expect_log("k1 bottoms out", "v1+");
    sample(3.35f, 0.00f);
    expect_log("k1 lifts - rapid trigger releases it", "v1-");
    sample(0.02f, 0.00f);
    expect_log("k1 gone", "");
    sample(0.00f, 3.50f);
    expect_log("k2 bottoms out after k1 left", "v2+");
    expect_int("  regime off", ad.regime, 0);

    puts("F. the accepted trade: a bottomed tap against a held slider");
    fresh();
    sample(3.50f, 0.00f);
    expect_log("slider held on k1", "v1+");
    sample(3.50f, 3.50f);
    expect_log("circle BOTTOMED on k2 - this does engage", "v1- v2+");
    expect_int("  regime on (known; avoidable by not bottoming)",
               ad.regime, 1);

    printf("\n%s\n", fails ? "FAILURES ABOVE" : "all assertions passed");
    return fails ? 1 : 0;
}
