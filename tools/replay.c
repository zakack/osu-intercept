#define _GNU_SOURCE
/*
 * replay - run a recorded travel trace through the real analog state
 * machine, offline.
 *
 * Records come from `doubletapd -T`, which is passive: it opens the
 * keyboard's hidraw node and nothing else, so whoever recorded played on
 * their own setup with their own keyboard behaving normally.
 *
 * This includes doubletapd.c directly and stubs only the uinput writes, so
 * every decision here is made by the same code the daemon runs - not a
 * reimplementation that could drift from it.
 *
 * The question it exists to answer: does ordinary alternate tapping ever
 * engage the SOCD regime? That needs both keys past socd_depth_mm and both
 * still emitting-pressed at the same instant, which is a matter of finger
 * timing and travel depth rather than of hitting any notes - so a trace
 * recorded against a metronome answers it as well as one from a map.
 *
 *   build:  cmake --build build --target replay
 *   usage:  ./build/replay [-c CONFIG] TRACE.csv
 */
#include <libevdev/libevdev.h>
#include <libevdev/libevdev-uinput.h>

static void replay_emit(unsigned type, unsigned code, int value);
#define libevdev_uinput_write_event(d, t, c, v) replay_emit((t), (c), (v))

#define main doubletapd_main
#include "doubletapd.c"
#undef main

/* ------------------------------------------------------------------------ */
/* trace                                                                     */
/* ------------------------------------------------------------------------ */

typedef struct {
    long long us;
    float     mm[2];
} frame_t;

typedef struct {
    frame_t *f;
    size_t   n, cap;
    float    travel_mm;   /* from the trace header, 0 if absent */
} trace_t;

static int trace_load(const char *path, trace_t *tr) {
    FILE *fp = strcmp(path, "-") ? fopen(path, "r") : stdin;
    if (!fp) {
        fprintf(stderr, "replay: %s: %s\n", path, strerror(errno));
        return -1;
    }
    char line[256];
    memset(tr, 0, sizeof(*tr));

    while (fgets(line, sizeof(line), fp)) {
        if (line[0] == '#') {
            sscanf(line, "# travel_mm=%f", &tr->travel_mm);
            continue;
        }
        long long us;
        float     a, b;
        if (sscanf(line, "%lld,%f,%f", &us, &a, &b) != 3)
            continue;                       /* header row, blank, junk */
        if (tr->n == tr->cap) {
            size_t cap = tr->cap ? tr->cap * 2 : 4096;
            frame_t *g = realloc(tr->f, cap * sizeof(*g));
            if (!g) { fprintf(stderr, "replay: oom\n"); return -1; }
            tr->f = g;
            tr->cap = cap;
        }
        tr->f[tr->n].us    = us;
        tr->f[tr->n].mm[0] = a;
        tr->f[tr->n].mm[1] = b;
        tr->n++;
    }
    if (fp != stdin) fclose(fp);
    if (!tr->n) {
        fprintf(stderr, "replay: %s: no frames\n", path);
        return -1;
    }
    return 0;
}

/* ------------------------------------------------------------------------ */
/* one pass over the trace, through the daemon's own functions               */
/* ------------------------------------------------------------------------ */

typedef struct {
    unsigned  engagements;
    unsigned  presses;          /* virtual key-downs emitted */
    long long first_engage_us;
} run_t;

static run_t      g_run;
static int        g_verbose;
static long long  g_now_us;

static void replay_emit(unsigned type, unsigned code, int value) {
    (void)code;
    if (type == EV_KEY && value == 1)
        g_run.presses++;
}

static void replay_run(const trace_t *tr, const oid_config_t *cfg,
                       run_t *out) {
    analog_dev_t ad;
    memset(&ad, 0, sizeof(ad));
    memset(&g_run, 0, sizeof(g_run));
    g_run.first_engage_us = -1;

    for (size_t i = 0; i < tr->n; i++) {
        int   edges[2][2], ne[2];
        int   was = ad.regime;
        g_now_us = tr->f[i].us;

        for (int k = 0; k < 2; k++)
            ne[k] = analog_key_feed(&ad.keys, k, tr->f[i].mm[k],
                                    &cfg->analog, ad.regime, edges[k]);
        analog_regime_update(&ad, cfg, tr->f[i].mm);
        for (int k = 0; k < 2; k++)
            for (int e = 0; e < ne[k]; e++)
                analog_socd_edge(&ad, k == 0, edges[k][e], cfg);

        if (!was && ad.regime) {
            g_run.engagements++;
            if (g_run.first_engage_us < 0)
                g_run.first_engage_us = tr->f[i].us;
            if (g_verbose)
                printf("  engaged at %8.3fs   k1=%.2fmm  k2=%.2fmm\n",
                       tr->f[i].us / 1e6, (double)tr->f[i].mm[0],
                       (double)tr->f[i].mm[1]);
        }
    }
    *out = g_run;
}

/* ------------------------------------------------------------------------ */
/* playstyle: where the peaks actually land                                  */
/* ------------------------------------------------------------------------ */

#define NBUCKET 14

static void report_style(const trace_t *tr, const oid_config_t *cfg) {
    float    travel = cfg->analog.travel_mm;
    float    act    = cfg->analog.actuation_mm;
    float    rel    = cfg->analog.release_mm;
    unsigned bucket[NBUCKET];
    unsigned npress = 0;
    float    peak[2] = { 0, 0 };
    int      down[2] = { 0, 0 };
    double   sum = 0.0, deepest = 0.0;

    memset(bucket, 0, sizeof(bucket));

    for (size_t i = 0; i < tr->n; i++) {
        for (int k = 0; k < 2; k++) {
            float d = tr->f[i].mm[k];
            if (!down[k] && d >= act) { down[k] = 1; peak[k] = d; }
            else if (down[k] && d > peak[k]) peak[k] = d;
            else if (down[k] && d < rel) {
                int b = (int)(peak[k] / travel * NBUCKET);
                if (b < 0) b = 0;
                if (b >= NBUCKET) b = NBUCKET - 1;
                bucket[b]++;
                npress++;
                sum += peak[k];
                if (peak[k] > deepest) deepest = peak[k];
                down[k] = 0;
                peak[k] = 0;
            }
        }
    }

    printf("\nkeypress depth distribution (%u presses)\n", npress);
    if (!npress) { printf("  none\n"); return; }

    unsigned max = 1;
    for (int b = 0; b < NBUCKET; b++) if (bucket[b] > max) max = bucket[b];

    for (int b = 0; b < NBUCKET; b++) {
        float lo = travel * b / NBUCKET, hi = travel * (b + 1) / NBUCKET;
        int   w  = (int)((double)bucket[b] * 44.0 / max + 0.5);
        printf("  %4.2f-%4.2fmm %5u |%.*s%*s|%s\n",
               (double)lo, (double)hi, bucket[b], w,
               "############################################", 44 - w, "",
               (lo <= travel - cfg->analog.bottom_out_mm &&
                travel - cfg->analog.bottom_out_mm < hi)
                   ? "  <- backplate" : "");
    }
    printf("  mean peak %.2fmm, deepest %.2fmm, backplate at %.2fmm "
           "(%.0f%% of travel)\n",
           sum / npress, deepest,
           (double)(travel - cfg->analog.bottom_out_mm),
           (double)(travel - cfg->analog.bottom_out_mm) / travel * 100.0);
}

/* ------------------------------------------------------------------------ */

int main(int argc, char **argv) {
    const char *config_path = NULL;
    int         sweep       = 1;

    int opt;
    while ((opt = getopt(argc, argv, "c:vq")) != -1) {
        switch (opt) {
            case 'c': config_path = optarg; break;
            case 'v': g_verbose = 1;        break;
            case 'q': sweep = 0;            break;
            default:
            fprintf(stderr,
                    "usage: %s [-c CONFIG] [-v] [-q] TRACE.csv\n"
                    "  -v  list every engagement\n"
                    "  -q  skip the socd_depth_mm sweep\n", argv[0]);
            return EXIT_FAILURE;
        }
    }
    if (optind >= argc) {
        fprintf(stderr, "usage: %s [-c CONFIG] [-v] [-q] TRACE.csv\n",
                argv[0]);
        return EXIT_FAILURE;
    }

    trace_t tr;
    if (trace_load(argv[optind], &tr) != 0)
        return EXIT_FAILURE;

    if (!config_path)
        config_path = default_config_path();
    oid_config_t cfg;
    config_init(&cfg);
    if (load_config(config_path, &cfg) != 0)
        return EXIT_FAILURE;
    if (tr.travel_mm > 0.0f && tr.travel_mm != cfg.analog.travel_mm)
        fprintf(stderr, "replay: note: trace was recorded with travel_mm "
                        "%.3f, config says %.3f\n",
                (double)tr.travel_mm, (double)cfg.analog.travel_mm);

    double secs = (tr.f[tr.n - 1].us - tr.f[0].us) / 1e6;
    printf("%zu reports over %.1fs (%.0f Hz)\n",
           tr.n, secs, secs > 0 ? tr.n / secs : 0.0);

    run_t r;
    printf("\nwith the config as it stands:\n");
    replay_run(&tr, &cfg, &r);
    printf("  %u engagement%s, %u virtual presses emitted\n",
           r.engagements, r.engagements == 1 ? "" : "s", r.presses);
    if (r.engagements)
        printf("  first at %.3fs\n", r.first_engage_us / 1e6);

    report_style(&tr, &cfg);

    if (sweep) {
        printf("\nengagements vs bottom_out_mm (where the backplate starts)\n");
        float keep = cfg.analog.bottom_out_mm;
        static const float bo[] = { 0.02f, 0.05f, 0.08f, 0.10f, 0.15f,
                                    0.20f, 0.30f, 0.50f, 0.80f };
        for (size_t i = 0; i < sizeof(bo) / sizeof(bo[0]); i++) {
            cfg.analog.bottom_out_mm = bo[i];
            replay_run(&tr, &cfg, &r);
            printf("  %4.2fmm  (backplate at %5.2fmm)  %5u engagements\n",
                   (double)bo[i],
                   (double)(cfg.analog.travel_mm - bo[i]), r.engagements);
        }
        cfg.analog.bottom_out_mm = keep;
    }

    free(tr.f);
    config_free(&cfg);
    return EXIT_SUCCESS;
}
