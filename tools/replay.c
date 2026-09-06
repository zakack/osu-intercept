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
 * The question it exists to answer: does ordinary alt-tapping ever arm the
 * `deep` latch? Arming needs BOTH keys on the backplate in one report, and
 * alt-tapping's defining shape is one finger leaving as the other arrives -
 * so in principle it never can. In practice that depends on how hard the
 * trace's author bottoms out and how wide `bottom_out_mm` is, which is a
 * matter of finger timing and travel depth rather than of hitting any
 * notes: a trace recorded against a metronome answers it as well as one
 * from a map. An alt-tap trace that reports ZERO latches is the acceptance
 * test for the whole feature.
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
    unsigned  presses;          /* virtual key-downs the kernel passes on */
    unsigned  dropped;          /* no-change writes the kernel swallows */
    long long first_engage_us;
} run_t;

static run_t      g_run;
static int        g_verbose;
static long long  g_now_us;

/* The virtual keyboard as the REST OF THE SYSTEM sees it, not as the daemon
 * writes it.
 *
 * A write to /dev/uinput is not an event. It goes through the kernel's
 * input_handle_event(), which for EV_KEY drops any 0/1 whose value already
 * matches the key's current state - only autorepeat (2) bypasses that. The
 * daemon leans on this on purpose: socd_apply's S_RELEASE/S_PRESS arm writes
 * an up/down pair unconditionally, and analog_socd_edge documents that when
 * the dipping key is the one whose virtual stepped aside as the latch armed,
 * BOTH halves land on keys already in that state and the kernel eats them.
 * That is called out there as "the common case, not a corner" - it is every
 * lift out of a rock on one side.
 *
 * Counting raw writes therefore over-reports presses against what a reader of
 * the virtual device actually receives, and it over-reports them precisely in
 * the gesture this branch exists for. So mirror the filter here: replay is
 * only worth anything if its number is the number you could have measured off
 * the device. */
static unsigned char g_vkey[KEY_MAX + 1];

static void replay_emit(unsigned type, unsigned code, int value) {
    if (type != EV_KEY || code > KEY_MAX)
        return;
    if (value == 2)                     /* autorepeat bypasses the state test */
        return;
    if (!!g_vkey[code] == !!value) {    /* no change: the kernel drops it */
        g_run.dropped++;
        return;
    }
    g_vkey[code] = value ? 1 : 0;
    if (value)
        g_run.presses++;
}

static void replay_run(const trace_t *tr, const oid_config_t *cfg,
                       run_t *out) {
    analog_dev_t ad;
    analog_dev_stub(&ad);
    memset(&g_run, 0, sizeof(g_run));
    memset(g_vkey, 0, sizeof(g_vkey));   /* the sweep re-runs this pass */
    g_run.first_engage_us = -1;
    g_log_quiet = 1;                     /* -v lists the latches instead */

    for (size_t i = 0; i < tr->n; i++) {
        int   was = ad.deep;
        g_now_us = tr->f[i].us;

        /* The daemon's own per-report path, not a restatement of it - the
         * ordering inside analog_report is load-bearing and a copy here
         * would be free to drift while still passing replay's own checks. */
        float depth[POOL_MAX] = { tr->f[i].mm[0], tr->f[i].mm[1] };
        /* The trace's own timestamp, so the placement path sees real
         * inter-report spacing rather than a synthetic constant. */
        analog_report(&ad, cfg, depth, (uint64_t)tr->f[i].us * 1000ull);

        if (!was && ad.deep) {
            g_run.engagements++;
            if (g_run.first_engage_us < 0)
                g_run.first_engage_us = tr->f[i].us;
            if (g_verbose)
                printf("  latched at %8.3fs   k1=%.2fmm  k2=%.2fmm\n",
                       tr->f[i].us / 1e6, (double)tr->f[i].mm[0],
                       (double)tr->f[i].mm[1]);
        }
    }
    g_log_quiet = 0;
    *out = g_run;
}

/* ------------------------------------------------------------------------ */
/* fidelity: did the trace get every report the daemon did?                  */
/* ------------------------------------------------------------------------ */

/* A trace is only a stand-in for the daemon if it holds every report the
 * daemon drained, and -T cannot promise that.
 *
 * Both read the same hidraw node, but hidraw hands each client its own ring
 * of 64 reports and overwrites the oldest when the client falls behind - no
 * error, no short read, nothing the tracer can notice. The daemon runs at
 * SCHED_FIFO 90; -T is an ordinary process, and if stdout is a terminal it
 * pays a write() per report on top. At the ~1kHz these boards report, 64
 * reports is 64ms of slack: one scheduling hiccup while a game is running and
 * the trace quietly loses a stretch the daemon saw in full.
 *
 * That is not a cosmetic hole. analog_key_feed carries `rt_extreme` from
 * sample to sample, so missing samples move where reversals fire, and
 * analog_deep_update wants both keys on the backplate in ONE report - drop
 * that report and the latch the daemon really armed never appears here.
 * A replay off a holey trace disagrees with the live daemon and looks like a
 * state-machine bug.
 *
 * The timestamps are the evidence, so read them. Cadence comes from the
 * median interval rather than the mean, which a single long pause would drag
 * anywhere. */
static int cmp_ll(const void *a, const void *b) {
    long long x = *(const long long *)a, y = *(const long long *)b;
    return x < y ? -1 : x > y ? 1 : 0;
}

static void report_gaps(const trace_t *tr, const oid_config_t *cfg) {
    if (tr->n < 3)
        return;

    size_t     nd = tr->n - 1;
    long long *d  = malloc(nd * sizeof(*d));
    if (!d)
        return;
    for (size_t i = 0; i < nd; i++)
        d[i] = tr->f[i + 1].us - tr->f[i].us;
    qsort(d, nd, sizeof(*d), cmp_ll);

    long long med = d[nd / 2];
    free(d);
    if (med <= 0)
        return;

    /* Four missed reports in a row is well past jitter and still far short of
     * the 64 it takes to be certain, so it errs toward telling you. The 5ms
     * floor keeps a slow-reporting board from tripping on ordinary spacing. */
    long long limit = med * 5;
    if (limit < 5000) limit = 5000;

    /* Only a gap with a key ENGAGED ACROSS IT is evidence of loss. The board
     * reports a key while it is off the rest position and stops when it is
     * not, so the long stretches between gestures - the player pausing
     * between one burst and the next - are silence the hardware never sent,
     * not reports the tracer missed. Counting those flags every ordinary
     * recording as damaged, which trains you to ignore the one warning that
     * would have mattered. */
    float least = cfg->analog.actuation_mm;
    if (cfg->analog.rt_enabled) {
        if (cfg->analog.rt_press_mm   > 0.0f && cfg->analog.rt_press_mm   < least)
            least = cfg->analog.rt_press_mm;
        if (cfg->analog.rt_release_mm > 0.0f && cfg->analog.rt_release_mm < least)
            least = cfg->analog.rt_release_mm;
    }

    unsigned  ngap = 0;
    long long worst = 0, lost = 0;
    for (size_t i = 0; i + 1 < tr->n; i++) {
        long long delta = tr->f[i + 1].us - tr->f[i].us;
        if (delta <= limit)
            continue;
        float a = tr->f[i].mm[0]     > tr->f[i].mm[1]
                ? tr->f[i].mm[0]     : tr->f[i].mm[1];
        float b = tr->f[i + 1].mm[0] > tr->f[i + 1].mm[1]
                ? tr->f[i + 1].mm[0] : tr->f[i + 1].mm[1];
        if (a < cfg->analog.release_mm || b < cfg->analog.release_mm)
            continue;                   /* idle either side: nothing was sent */

        /* Nor does a key merely being HELD prove a report was lost. The
         * board reports on change, so a finger resting on the backplate
         * sends nothing for as long as it stays there - the gaps in an
         * ordinary recording are mostly this. Loss only matters if travel
         * went unobserved across the gap, and only if enough of it went
         * unobserved to have changed a decision: every threshold the front
         * end tests is at least `least`, so a gap hiding less movement than
         * that could not have altered the outcome however long it lasted. */
        float m0 = fabsf(tr->f[i + 1].mm[0] - tr->f[i].mm[0]);
        float m1 = fabsf(tr->f[i + 1].mm[1] - tr->f[i].mm[1]);
        if ((m0 > m1 ? m0 : m1) < least)
            continue;
        ngap++;
        lost += delta / med - 1;
        if (delta > worst) worst = delta;
    }

    printf("\ntrace fidelity: %.0f Hz median (%.2fms between reports)\n",
           1e6 / (double)med, med / 1000.0);
    if (!ngap) {
        printf("  no unexplained gaps - every pause is idle or a held key\n");
        return;
    }
    printf("  %u gap%s over %.1fms with travel unaccounted for, worst "
           "%.1fms, ~%lld reports missing\n",
           ngap, ngap == 1 ? "" : "s", limit / 1000.0, worst / 1000.0, lost);
    printf("  WARNING: -T lost reports the daemon would have seen, so this\n"
           "  replay can disagree with the live daemon through no fault of\n"
           "  the state machine. Re-record with stdout redirected to a FILE\n"
           "  (not a terminal) and with less running alongside.\n");
}

/* Could this trace EVER have armed the latch, and by how much did it miss?
 *
 * Arming is one test - both keys at or past travel_mm - bottom_out_mm in a
 * SINGLE report - so the whole question reduces to one number: the largest
 * min(k1, k2) anywhere in the trace. Everything else about the trace is
 * irrelevant to it. If that number is under the backplate the latch was
 * unreachable, the daemon was right to stay on the plain remap, and no
 * amount of state-machine reading will explain the "missing" toggle - the
 * fingers, or the board's reported maximum, never got there.
 *
 * That last case is real and easy to miss: the firmware reports a fraction
 * of ITS travel, so if it saturates below the configured backplate the latch
 * cannot arm however hard you press. The number this prints is measured, so
 * it settles that without a guess. */
static void report_headroom(const trace_t *tr, const oid_config_t *cfg) {
    float floor_mm = cfg->analog.travel_mm - cfg->analog.bottom_out_mm;
    float best = 0.0f;

    for (size_t i = 0; i < tr->n; i++) {
        float lo = tr->f[i].mm[0] < tr->f[i].mm[1]
                 ? tr->f[i].mm[0] : tr->f[i].mm[1];
        if (lo > best) best = lo;
    }

    printf("  deepest both keys ever were together: %.2fmm "
           "(backplate %.2fmm)\n", (double)best, (double)floor_mm);
    if (best >= floor_mm)
        return;
    printf("  the latch was UNREACHABLE in this trace - short by %.2fmm. "
           "Either\n"
           "  the keys were never both bottomed in one report, or the board "
           "reports\n"
           "  less than full travel. bottom_out_mm would have to be >= "
           "%.2fmm to arm\n"
           "  on this recording.\n",
           (double)(floor_mm - best),
           (double)(cfg->analog.travel_mm - best));
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
                    "  -v  list every latch\n"
                    "  -q  skip the bottom_out_mm sweep\n", argv[0]);
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

    report_gaps(&tr, &cfg);

    run_t r;
    printf("\nwith the config as it stands:\n");
    replay_run(&tr, &cfg, &r);
    /* `presses` is what a reader of the virtual device receives; `dropped`
     * is the daemon's unconditional up/down writes that the kernel swallows
     * because the key was already in that state. Both are shown so a count
     * taken off the real device can be reconciled against this one. */
    printf("  %u deep latch%s, %u virtual presses emitted "
           "(%u no-change writes dropped by the kernel)\n",
           r.engagements, r.engagements == 1 ? "" : "es", r.presses,
           r.dropped);
    if (r.engagements)
        printf("  first at %.3fs\n", r.first_engage_us / 1e6);
    report_headroom(&tr, &cfg);

    report_style(&tr, &cfg);

    if (sweep) {
        printf("\ndeep latches vs bottom_out_mm (where the backplate "
               "starts)\n");
        float keep = cfg.analog.bottom_out_mm;
        static const float bo[] = { 0.02f, 0.05f, 0.08f, 0.10f, 0.15f,
                                    0.20f, 0.30f, 0.50f, 0.80f };
        for (size_t i = 0; i < sizeof(bo) / sizeof(bo[0]); i++) {
            cfg.analog.bottom_out_mm = bo[i];
            replay_run(&tr, &cfg, &r);
            printf("  %4.2fmm  (backplate at %5.2fmm, %4.1f%%)  %5u latches\n",
                   (double)bo[i],
                   (double)(cfg.analog.travel_mm - bo[i]),
                   (double)((cfg.analog.travel_mm - bo[i])
                            / cfg.analog.travel_mm * 100.0f),
                   r.engagements);
        }
        cfg.analog.bottom_out_mm = keep;
    }

    free(tr.f);
    config_free(&cfg);
    return EXIT_SUCCESS;
}
