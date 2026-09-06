#define _GNU_SOURCE
/*
 * mixtest - assertions over the audio voice pool, the trigger ring and the
 * summed output buffer.
 *
 * Includes doubletapd.c and calls the daemon's own trigger() and
 * audio_mix(), so it exercises the real mixer rather than a copy. There is
 * no PipeWire here at all: audio_mix takes a plain float buffer, which is
 * exactly why the mix was factored out of on_process.
 *
 * Triggers go in through trigger() rather than by poking audio.voice[]
 * directly. The ring IS the boundary between the input thread and the RT
 * thread, and a bug there is the kind that lives for six months.
 *
 *   build:  cmake --build build --target mixtest
 *   run:    ./build/mixtest
 */
#include <math.h>

#define main doubletapd_main
#include "doubletapd.c"
#undef main

static int fails;

static void expect_int(const char *what, long long got, long long want) {
    if (got == want) { printf("  ok    %s\n", what); return; }
    printf("  FAIL  %s: got %lld, want %lld\n", what, got, want);
    fails++;
}

static void expect_f(const char *what, float got, float want) {
    /* The mixer only multiplies and adds, so exactness is a fair ask for
     * everything except the resampler (which gets its own tolerance). */
    if (fabsf(got - want) <= 1e-6f) { printf("  ok    %s\n", what); return; }
    printf("  FAIL  %s: got %.9f, want %.9f\n", what, (double)got, (double)want);
    fails++;
}

/* ------------------------------------------------------------------------ */
/* Fixtures                                                                  */
/* ------------------------------------------------------------------------ */

#define SLEN 500              /* frames in a synthetic sample */
#define BUF  128              /* frames per render call */

static float store[AUDIO_NSAMPLES][SLEN * MIX_CHANNELS];
static float out[BUF * MIX_CHANNELS * 4];

/* Sample s, frame f, channel c -> a value unique to that triple, so any
 * mis-summed or mis-offset read shows up as a number nobody can produce by
 * accident. The three terms occupy separate decades (f contributes at most
 * 0.005 over SLEN frames, below the channel step) so no combination
 * collides, and the whole thing stays small enough that a legitimate sum of
 * two voices does not hit the output clamp and hide a real mismatch. */
static float cell(int s, size_t f, int c) {
    return (float)(s + 1) * 0.03f + (float)f * 0.00001f + (float)c * 0.007f;
}

/* Triggers still go in through the daemon's own audio_trigger - the ring IS
 * the thread boundary - but through wrappers that name the two intents.
 *
 * trigger() passes a zero timestamp, which mix_drain_triggers reads as "no
 * placement information" and puts at frame 0. That is deliberately what every
 * case below A..M asserts against: they predate sub-buffer placement and are
 * the regression gate on the mixer proper, so they must keep testing the
 * mixer and not the clock. Cases N and O are the placement path. */
static void trigger(int sample) {
    audio_trigger(sample, 0);
}

static void trigger_at(int sample, uint64_t t_ns) {
    audio_trigger(sample, t_ns);
}

/* Reset the whole audio subsystem: samples, pool, ring. Nothing here reaches
 * into internals the daemon does not own - it is the startup state. */
static void fresh(float gain0, float gain1) {
    float gains[AUDIO_NSAMPLES] = { gain0, gain1 };
    memset(&audio, 0, sizeof audio);
    memset(&trig,  0, sizeof trig);
    for (int s = 0; s < AUDIO_NSAMPLES; s++) {
        for (size_t f = 0; f < SLEN; f++)
            for (int c = 0; c < MIX_CHANNELS; c++)
                store[s][f * MIX_CHANNELS + c] = cell(s, f, c);
        audio.sample[s].samples    = store[s];
        audio.sample[s].num_frames = SLEN;
        audio.sample[s].gain       = gains[s];
    }
    audio_available = 1;
}

static void render(size_t nframes) {
    memset(out, 0x7f, sizeof out);   /* poison: silence must be written, not left */
    audio_mix(out, nframes, 0);
}

static int active_voices(void) {
    int n = 0;
    for (int i = 0; i < AUDIO_MAX_VOICES; i++)
        if (audio.voice[i].active) n++;
    return n;
}

int main(void) {
    puts("A. one trigger, one voice, exact output");
    fresh(1.0f, 1.0f);
    render(BUF);
    expect_int("nothing triggered: no voices", active_voices(), 0);
    expect_f("  and the buffer is silence, not poison", out[0], 0.0f);
    expect_f("  right to the end", out[BUF * MIX_CHANNELS - 1], 0.0f);

    trigger(0);
    render(BUF);
    expect_int("triggered: one voice", active_voices(), 1);
    expect_f("  first frame, left",  out[0], cell(0, 0, 0));
    expect_f("  first frame, right", out[1], cell(0, 0, 1));
    expect_f("  last frame of the buffer",
             out[(BUF - 1) * MIX_CHANNELS], cell(0, BUF - 1, 0));

    render(BUF);
    expect_f("second buffer picks up where the first left off",
             out[0], cell(0, BUF, 0));
    expect_f("  and its last frame", out[(BUF - 1) * MIX_CHANNELS],
             cell(0, 2 * BUF - 1, 0));

    puts("B. gain scales the voice, and it is the SAMPLE's gain");
    fresh(0.5f, 1.0f);
    trigger(0);
    render(BUF);
    expect_f("half gain", out[0], cell(0, 0, 0) * 0.5f);

    puts("C. overlapping triggers SUM - the regression this branch exists for");
    /* The old design restarted the stream from frame 0 on a second trigger,
     * so the first click's tail vanished. At 300 BPM the shipped 125ms click
     * was being cut at 40% on every note of a stream. Each trigger now owns
     * a voice, so the tail survives underneath the new note. */
    fresh(1.0f, 1.0f);
    trigger(0);
    render(BUF);                       /* first voice advances BUF frames */
    trigger(0);                  /* second tap, same key */
    render(BUF);
    expect_int("two voices, same sample", active_voices(), 2);
    expect_f("  the new note plus the OLD ONE'S TAIL",
             out[0], cell(0, BUF, 0) + cell(0, 0, 0));
    expect_f("  and later in the buffer",
             out[10 * MIX_CHANNELS],
             cell(0, BUF + 10, 0) + cell(0, 10, 0));

    puts("D. distinct samples mix rather than clobber");
    fresh(1.0f, 1.0f);
    trigger(0);
    trigger(1);
    render(BUF);
    expect_int("two voices", active_voices(), 2);
    expect_f("  v1 + v2", out[0], cell(0, 0, 0) + cell(1, 0, 0));
    expect_f("  on the right channel too", out[1],
             cell(0, 0, 1) + cell(1, 0, 1));

    puts("E. retirement EXACTLY on a buffer boundary");
    /* SLEN is a whole number of BUF-sized renders plus a remainder; drive to
     * the frame where remaining == nframes and assert the voice both fills
     * that buffer completely and is gone afterwards. Reading one frame past
     * the end or emitting one extra frame are the two classic off-by-ones,
     * and a sample shorter than a buffer catches neither. */
    fresh(1.0f, 1.0f);
    trigger(0);
    render(SLEN - BUF);                       /* leave exactly BUF frames */
    expect_int("still playing with one buffer to go", active_voices(), 1);
    render(BUF);
    expect_f("  the final buffer is full to its last frame",
             out[(BUF - 1) * MIX_CHANNELS], cell(0, SLEN - 1, 0));
    expect_int("  and the voice retired", active_voices(), 0);
    render(BUF);
    expect_f("  the next buffer is silence", out[0], 0.0f);
    expect_f("  all of it", out[BUF * MIX_CHANNELS - 1], 0.0f);

    puts("F. a voice ending mid-buffer zeroes the rest");
    fresh(1.0f, 1.0f);
    trigger(0);
    render(SLEN - 10);
    render(BUF);
    expect_f("last real frame", out[9 * MIX_CHANNELS], cell(0, SLEN - 1, 0));
    expect_f("  then silence, not the poison", out[10 * MIX_CHANNELS], 0.0f);
    expect_int("  retired", active_voices(), 0);

    puts("G. pool exhaustion steals the OLDEST voice");
    fresh(1.0f, 1.0f);
    /* One trigger per render, so the voices differ in how far they have
     * advanced and the oldest is identifiable by its position. */
    for (int i = 0; i < AUDIO_MAX_VOICES; i++) { trigger(0); render(1); }
    expect_int("pool full", active_voices(), AUDIO_MAX_VOICES);
    /* The oldest has advanced AUDIO_MAX_VOICES frames, the newest 1. */
    size_t oldest = 0;
    for (int i = 0; i < AUDIO_MAX_VOICES; i++)
        if (audio.voice[i].pos > oldest) oldest = audio.voice[i].pos;
    expect_int("  oldest is the furthest along",
               (long long)oldest, AUDIO_MAX_VOICES);

    trigger(0);
    render(1);
    expect_int("one more trigger: still exactly the pool size",
               active_voices(), AUDIO_MAX_VOICES);
    size_t newoldest = 0;
    for (int i = 0; i < AUDIO_MAX_VOICES; i++)
        if (audio.voice[i].pos > newoldest) newoldest = audio.voice[i].pos;
    expect_int("  and it is the OLDEST that was stolen, not a newer one",
               (long long)newoldest, AUDIO_MAX_VOICES);
    int fresh_voices = 0;
    for (int i = 0; i < AUDIO_MAX_VOICES; i++)
        if (audio.voice[i].pos == 1) fresh_voices++;
    expect_int("  the new voice started at zero and rendered one frame",
               fresh_voices, 1);

    puts("H. a full ring DROPS and counts, it does not block or corrupt");
    fresh(1.0f, 1.0f);
    for (int i = 0; i < TRIG_RING + 7; i++) trigger(0);
    expect_int("overruns counted", (long long)atomic_load(&trig.overruns), 7);
    render(1);
    /* TRIG_RING > AUDIO_MAX_VOICES, so the pool caps what survives; what
     * matters is that the ring accepted exactly TRIG_RING and nothing walked
     * off the end of it. */
    expect_int("  ring fully drained",
               (long long)(atomic_load(&trig.head) - atomic_load(&trig.tail)), 0);
    expect_int("  pool saturated, not overflowed",
               active_voices(), AUDIO_MAX_VOICES);

    puts("I. the ring wraps");
    fresh(1.0f, 1.0f);
    /* Many more triggers than the ring holds, but drained as we go so it
     * never fills: head and tail must chase each other around the buffer
     * without ever losing one. */
    for (int i = 0; i < TRIG_RING * 5; i++) {
        trigger(0);
        render(SLEN);            /* long enough to retire it immediately */
        if (active_voices() != 0) {
            printf("  FAIL  wrap iteration %d left a voice active\n", i);
            fails++;
            break;
        }
    }
    expect_int("no drops across 5 wraps",
               (long long)atomic_load(&trig.overruns), 0);
    expect_int("  head advanced by every trigger",
               (long long)atomic_load(&trig.head), TRIG_RING * 5);
    expect_int("  and tail caught up",
               (long long)atomic_load(&trig.tail), TRIG_RING * 5);

    puts("J. the sum is clamped, never NaN");
    fresh(4.0f, 4.0f);
    for (int i = 0; i < AUDIO_MAX_VOICES; i++) trigger(i & 1);
    render(BUF);
    expect_int("pool full of loud voices", active_voices(), AUDIO_MAX_VOICES);
    {
        int bad = 0;
        float peak = 0.0f;
        for (size_t k = 0; k < BUF * MIX_CHANNELS; k++) {
            if (isnan(out[k]) || out[k] > 1.0f || out[k] < -1.0f) bad++;
            if (fabsf(out[k]) > peak) peak = fabsf(out[k]);
        }
        expect_int("  every output sample inside [-1, 1]", bad, 0);
        expect_f("  and it did reach the ceiling", peak, 1.0f);
    }

    puts("K. audio_trigger is inert when there is nothing to play");
    fresh(1.0f, 1.0f);
    audio_available = 0;
    trigger(0);
    expect_int("audio off: nothing queued",
               (long long)atomic_load(&trig.head), 0);
    audio_available = 1;
    trigger(-1); trigger(AUDIO_NSAMPLES);
    expect_int("out-of-range sample id: nothing queued",
               (long long)atomic_load(&trig.head), 0);
    audio.sample[1].samples = NULL;
    trigger(1);
    expect_int("unmapped sample: nothing queued",
               (long long)atomic_load(&trig.head), 0);

    puts("L. sample_conform: channels");
    {
        static float mono[64];
        for (int i = 0; i < 64; i++) mono[i] = (float)i / 64.0f;
        audio_sample_t s = {0};
        expect_int("mono at the mix rate conforms",
                   sample_conform(mono, 64, 1, MIX_RATE, &s), 0);
        expect_int("  frame count unchanged", (long long)s.num_frames, 64);
        expect_f("  left channel", s.samples[10 * MIX_CHANNELS], mono[10]);
        expect_f("  right is a duplicate, not silence",
                 s.samples[10 * MIX_CHANNELS + 1], mono[10]);
        free(s.samples);

        static float stereo[64 * 2];
        for (int i = 0; i < 64 * 2; i++) stereo[i] = (float)i / 128.0f;
        memset(&s, 0, sizeof s);
        expect_int("stereo at the mix rate conforms",
                   sample_conform(stereo, 64, 2, MIX_RATE, &s), 0);
        expect_f("  passes through, left",  s.samples[20], stereo[20]);
        expect_f("  passes through, right", s.samples[21], stereo[21]);
        free(s.samples);
    }

    puts("M. sample_conform: rate");
    {
        /* A half-rate ramp must come back about twice as long, still a ramp,
         * with its endpoints where they started. */
        enum { N = 480 };
        static float half[N];
        for (int i = 0; i < N; i++) half[i] = (float)i / (float)N;
        audio_sample_t s = {0};
        expect_int("24k -> 48k conforms",
                   sample_conform(half, N, 1, MIX_RATE / 2, &s), 0);
        long long got = (long long)s.num_frames;
        if (got >= 2 * N - 8 && got <= 2 * N + 8)
            printf("  ok    about twice as many frames (%lld)\n", got);
        else { printf("  FAIL  frame count %lld, want ~%d\n", got, 2 * N); fails++; }
        /* Sinc interpolation rings at the edges, so check the middle, where
         * the ramp is unambiguous, with a tolerance the filter can meet. */
        float mid = s.samples[(s.num_frames / 2) * MIX_CHANNELS];
        if (fabsf(mid - 0.5f) < 0.01f)
            printf("  ok    midpoint still 0.5 (%.4f)\n", (double)mid);
        else { printf("  FAIL  midpoint %.4f, want ~0.5\n", (double)mid); fails++; }
        expect_f("  and it is still stereo-duplicated",
                 s.samples[(s.num_frames / 2) * MIX_CHANNELS + 1], mid);
        free(s.samples);

        memset(&s, 0, sizeof s);
        expect_int("a zero-frame sample is rejected, not mixed",
                   sample_conform(half, 0, 1, MIX_RATE, &s), -1);
    }

    /* --------------------------------------------------------------- *
     * Sub-buffer placement. The events drained in one cycle arrived
     * anywhere across the previous one, so dropping them all at frame 0
     * quantises every burst to the graph quantum. The drain maps the
     * window [cycle_ns - nframes, cycle_ns) onto [0, nframes) instead,
     * trading one fixed quantum of latency for no jitter at all.
     * --------------------------------------------------------------- */
    puts("N. sub-buffer placement");
    {
        /* A cycle far from zero, so nothing here passes by accident on a
         * small number. One buffer of BUF frames spans this many ns: */
        const uint64_t span   = (uint64_t)BUF * 1000000000ull / MIX_RATE;
        const uint64_t cycle  = 4000000000ull;

        /* Exactly one buffer old -> the very start. */
        fresh(1.0f, 1.0f);
        trigger_at(0, cycle - span);
        memset(out, 0x7f, sizeof out);
        audio_mix(out, BUF, cycle);
        expect_f("a full buffer old lands at frame 0", out[0], cell(0, 0, 0));

        /* Half a buffer old -> halfway in, with silence before it. */
        fresh(1.0f, 1.0f);
        trigger_at(0, cycle - span / 2);
        memset(out, 0x7f, sizeof out);
        audio_mix(out, BUF, cycle);
        expect_f("half a buffer old: silence at frame 0", out[0], 0.0f);
        expect_f("  silence right up to the offset",
                 out[(BUF / 2 - 1) * MIX_CHANNELS], 0.0f);
        expect_f("  and the sample starts at frame BUF/2",
                 out[(BUF / 2) * MIX_CHANNELS], cell(0, 0, 0));
        expect_f("  frame 1 of the sample follows it",
                 out[(BUF / 2 + 1) * MIX_CHANNELS], cell(0, 1, 0));

        /* THE POINT OF ALL THIS: two triggers a known distance apart come
         * out that same distance apart, inside one buffer. Frame 0 for
         * both is what this replaces. */
        fresh(1.0f, 1.0f);
        trigger_at(0, cycle - span * 3 / 4);
        trigger_at(1, cycle - span / 4);
        memset(out, 0x7f, sizeof out);
        audio_mix(out, BUF, cycle);
        expect_f("two triggers keep their spacing: first at BUF/4",
                 out[(BUF / 4) * MIX_CHANNELS], cell(0, 0, 0));
        expect_f("  second at 3*BUF/4, and they do not collide",
                 out[(BUF * 3 / 4) * MIX_CHANNELS],
                 cell(1, 0, 0) + cell(0, BUF / 2, 0));
        expect_int("  both are playing", active_voices(), 2);

        /* A voice placed near the end straddles the boundary: what did not
         * fit carries into the next buffer, from ITS frame 0. */
        fresh(1.0f, 1.0f);
        trigger_at(0, cycle - span / BUF);   /* one frame old -> last frame */
        memset(out, 0x7f, sizeof out);
        audio_mix(out, BUF, cycle);
        expect_f("a late trigger reaches the last frame",
                 out[(BUF - 1) * MIX_CHANNELS], cell(0, 0, 0));
        memset(out, 0x7f, sizeof out);
        audio_mix(out, BUF, cycle + span);
        expect_f("  and continues from frame 0 of the next buffer",
                 out[0], cell(0, 1, 0));
    }

    puts("O. placement degrades to frame 0, never to silence");
    {
        const uint64_t span  = (uint64_t)BUF * 1000000000ull / MIX_RATE;
        const uint64_t cycle = 4000000000ull;

        /* A stale trigger - a backlog, a stalled epoll loop - must play at
         * once rather than be pushed off the front of the buffer. */
        fresh(1.0f, 1.0f);
        trigger_at(0, cycle - span * 50);
        memset(out, 0x7f, sizeof out);
        audio_mix(out, BUF, cycle);
        expect_f("a stale trigger clamps to frame 0", out[0], cell(0, 0, 0));

        /* A timestamp from the future (a clock the daemon does not control,
         * or a report stamped a hair after the cycle snapshot) must stay
         * inside the buffer rather than run off the end of it. */
        fresh(1.0f, 1.0f);
        trigger_at(0, cycle + span);
        memset(out, 0x7f, sizeof out);
        audio_mix(out, BUF, cycle);
        expect_int("a future trigger still plays", active_voices(), 1);
        expect_f("  clamped to the last frame",
                 out[(BUF - 1) * MIX_CHANNELS], cell(0, 0, 0));

        /* No cycle time at all - pw_stream_get_time_n failed - is the old
         * behaviour, and must stay it. */
        fresh(1.0f, 1.0f);
        trigger_at(0, cycle - span / 2);
        memset(out, 0x7f, sizeof out);
        audio_mix(out, BUF, 0);
        expect_f("no cycle time: back to frame 0", out[0], cell(0, 0, 0));
    }

    printf("\n%s\n", fails ? "FAILURES ABOVE" : "all assertions passed");
    return fails ? 1 : 0;
}
