/*
 * doubletapd - Rapid-fire virtual keyboard daemon
 *
 * The daemon directly opens (and exclusively grabs) one or more evdev
 * keyboard devices - either an explicit config list or, by default,
 * every keyboard-shaped device that advertises both configured keys
 * (with inotify-driven hotplug either way) - applies a SOCD state
 * machine ("toggle"/"on" = last-input + reverting toggle, "snappy" =
 * last input wins, "analog" = toggle and off switched at runtime by how
 * far the keys are pressed, or "off" = plain remap with no cleaning, per
 * the config's socd field) to two configurable physical
 * keys (k1, k2 -> v1, v2), mirrors every other event verbatim into a
 * single uinput virtual keyboard, and plays a click sound through
 * PipeWire on every virtual key-press.
 *
 * In analog mode the keyboard's travel depth is read straight off its
 * vendor HID interface and the `deep` latch decides which machine is in
 * force: it arms when BOTH keys are on the backplate in one report (that
 * is a rock) and disarms when either comes back up past actuation (that
 * is tapping). See analog_deep_update.
 *
 * Copyright 2026 Zachary Kessler
 */

#define _GNU_SOURCE
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <getopt.h>
#include <limits.h>
#include <poll.h>
#include <signal.h>
#include <sched.h>
#include <stdarg.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>

#include <sys/epoll.h>
#include <sys/inotify.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/mman.h>

#include <linux/input.h>

#include <libevdev/libevdev.h>
#include <libevdev/libevdev-uinput.h>

#include <yaml.h>

#include <spa/param/audio/format-utils.h>
#include <pipewire/pipewire.h>

/* ------------------------------------------------------------------------- */
/* Defaults                                                                  */
/* ------------------------------------------------------------------------- */

#define DEF_K1     KEY_Z
#define DEF_K2     KEY_X
#define DEF_V1     KEY_F13
#define DEF_V2     KEY_F14
#define DEF_NAME   "doubletap virtual keyboard"

/* Upper bound on the physical-key pool. An analog report carries sixteen
 * slots (ANALOG_SLOTS), so a pool larger than that could not be read by
 * travel anyway, and nothing sane wants one. */
#define POOL_MAX   16

/* Installed data paths; CMake overrides these to match the install prefix. */
#ifndef DEF_WAV
#define DEF_WAV    "/usr/share/doubletap/click.wav"
#endif
#ifndef DEF_CONFIG
#define DEF_CONFIG "/usr/share/doubletap/config.yaml"
#endif

/* ------------------------------------------------------------------------- */
/* Logging                                                                   */
/* ------------------------------------------------------------------------- */

static void logf_(const char *level, const char *fmt, ...)
__attribute__((format(printf, 2, 3)));

/* Set by the offline tools (replay, regimetest) while they push samples
 * through the state machine. They run the same code the daemon does, so the
 * daemon's own progress chatter comes out of them too - and replay's
 * bottom_out sweep runs the whole trace nine times over, which turns one
 * latch log per rock into thousands of lines across a real recording.
 * Warnings and errors are never suppressed; only the running commentary. */
static int g_log_quiet;

static void logf_(const char *level, const char *fmt, ...) {
    va_list ap;
    if (g_log_quiet && strcmp(level, "info") == 0)
        return;
    fprintf(stderr, "[doubletapd] %s: ", level);
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fputc('\n', stderr);
    fflush(stderr);
}

#define LOG_INFO(...) logf_("info",  __VA_ARGS__)
#define LOG_WARN(...) logf_("warn",  __VA_ARGS__)
#define LOG_ERR(...)  logf_("error", __VA_ARGS__)

/* ------------------------------------------------------------------------- */
/* Signals                                                                   */
/* ------------------------------------------------------------------------- */

static volatile sig_atomic_t g_running = 1;

static void on_signal(int sig) {
    (void)sig;
    g_running = 0;
}

/* ------------------------------------------------------------------------- */
/* Audio (PipeWire)                                                          */
/* ------------------------------------------------------------------------- */

static int audio_available;

/* One playback voice: an independently loaded sample driven by its own
 * PipeWire stream. V1 keypresses trigger voice 0, V2 voice 1. The two
 * streams are fully independent so a V1 click and a V2 click overlap
 * rather than cutting each other off. */
#define AUDIO_NVOICES 2

/* process_event() return codes: which virtual key it emitted a press for
 * (and thus which hitsound voice to trigger). 0 = no press this event. */
#define VOICE_NONE 0
#define VOICE_V1   1
#define VOICE_V2   2

typedef struct {
    float                    *samples;
    size_t                    num_frames;
    int                       channels;
    int                       sample_rate;
    float                     gain;      /* linear multiplier, 1.0 = unity */

    struct pw_stream         *stream;

    atomic_int                pending;
    atomic_bool               playing;
    atomic_bool               reset;
    atomic_size_t             frame_pos;
} audio_voice_t;

static struct {
    struct pw_thread_loop    *loop;      /* one thread loop drives both voices */
    audio_voice_t             voice[AUDIO_NVOICES];
} audio;

typedef struct { char id[4]; uint32_t size; } wav_chunk;
typedef struct {
    uint16_t fmt;
    uint16_t ch;
    uint32_t rate;
    uint32_t br;
    uint16_t ba;
    uint16_t bps;
} wav_fmt;

static int wav_load(const char *path, audio_voice_t *v) {
    FILE *f = fopen(path, "rb");
    if (!f) {
        LOG_WARN("Cannot open WAV %s: %s", path, strerror(errno));
        return -1;
    }

    char riff[4]; uint32_t rsize; char wave[4];
    if (fread(riff, 4, 1, f) != 1 || fread(&rsize, 4, 1, f) != 1 ||
        fread(wave, 4, 1, f) != 1 ||
        memcmp(riff, "RIFF", 4) || memcmp(wave, "WAVE", 4)) {
        fclose(f);
        LOG_WARN("Not a RIFF/WAVE file: %s", path);
        return -1;
    }

    wav_fmt fmt = {0};
    uint32_t ds = 0;

    for (;;) {
        wav_chunk ch;
        if (fread(&ch, sizeof(ch), 1, f) != 1) break;
        if (!memcmp(ch.id, "fmt ", 4)) {
            size_t rs = ch.size < sizeof(fmt) ? ch.size : sizeof(fmt);
            if (fread(&fmt, rs, 1, f) != 1) { fclose(f); return -1; }
            if (ch.size > sizeof(fmt))
                fseek(f, (long)(ch.size - sizeof(fmt)), SEEK_CUR);
        } else if (!memcmp(ch.id, "data", 4)) {
            ds = ch.size; break;
        } else {
            fseek(f, (long)ch.size, SEEK_CUR);
        }
    }

    if (fmt.fmt != 1 || ds == 0 || fmt.bps == 0 || fmt.ch == 0) {
        fclose(f);
        LOG_WARN("Unsupported WAV format in %s", path);
        return -1;
    }

    v->channels    = fmt.ch;
    v->sample_rate = (int)fmt.rate;
    v->num_frames  = ds / (fmt.bps / 8u) / fmt.ch;
    v->samples     = calloc(v->num_frames * v->channels, sizeof(float));
    if (!v->samples) { fclose(f); return -1; }

    {
        uint8_t *raw = malloc(ds);
        if (!raw || fread(raw, ds, 1, f) != 1) {
            free(raw); fclose(f);
            free(v->samples); v->samples = NULL;
            return -1;
        }
        fclose(f);

        size_t total = v->num_frames * v->channels;
        switch (fmt.bps) {
            case 16:
            for (size_t i = 0; i < total; i++)
                v->samples[i] = ((int16_t *)raw)[i] / 32768.0f;
            break;
            case 24:
            for (size_t i = 0; i < total; i++) {
                int32_t s = (int32_t)(raw[i*3] | ((uint32_t)raw[i*3+1] << 8) |
                                     ((int32_t)((int8_t)raw[i*3+2]) << 16));
                v->samples[i] = s / 8388608.0f;
            }
            break;
            case 32:
            for (size_t i = 0; i < total; i++)
                v->samples[i] = ((int32_t *)raw)[i] / 2147483648.0f;
            break;
            default:
            free(raw);
            free(v->samples); v->samples = NULL;
            return -1;
        }
        free(raw);
    }

    LOG_INFO("Loaded %s: %zu frames, %d ch, %d Hz",
             path, v->num_frames, v->channels, v->sample_rate);
    return 0;
}

static void on_process(void *userdata) {
    audio_voice_t *v = userdata;
    struct pw_buffer *b;
    struct spa_buffer *buf;

    if ((b = pw_stream_dequeue_buffer(v->stream)) == NULL) {
        pw_log_warn("out of buffers: %m");
        return;
    }

    buf = b->buffer;
    float *dst = buf->datas[0].data;
    if (!dst) return;

    int stride = (int)(sizeof(float) * v->channels);
    int n_frames = buf->datas[0].maxsize / stride;
    if (b->requested)
        n_frames = SPA_MIN((int)b->requested, n_frames);
    size_t nf = (size_t)n_frames;

    if (!atomic_load_explicit(&v->playing, memory_order_acquire) &&
        atomic_load(&v->pending) > 0) {
        atomic_store_explicit(&v->playing, true, memory_order_relaxed);
        atomic_store(&v->frame_pos, 0);
        atomic_fetch_sub(&v->pending, 1);
    }

    if (atomic_load_explicit(&v->playing, memory_order_acquire)) {
        if (atomic_exchange(&v->reset, false))
            atomic_store(&v->frame_pos, 0);

        size_t pos = atomic_load(&v->frame_pos);
        size_t rem = v->num_frames - pos;
        size_t tc  = nf < rem ? nf : rem;

        if (tc > 0) {
            const float *src = v->samples + pos * v->channels;
            size_t nsamp = tc * (size_t)v->channels;
            float g = v->gain;
            if (g == 1.0f)
                memcpy(dst, src, nsamp * sizeof(float));
            else
                for (size_t i = 0; i < nsamp; i++)
                    dst[i] = src[i] * g;
        }
        if (nf > tc)
            memset(dst + tc * v->channels, 0, (nf - tc) * (size_t)stride);

        pos += tc;
        if (pos >= v->num_frames) {
            atomic_store(&v->playing, false);
            atomic_store(&v->frame_pos, 0);
            if (atomic_load(&v->pending) > 0) {
                atomic_store(&v->playing, true);
                atomic_store(&v->frame_pos, 0);
                atomic_fetch_sub(&v->pending, 1);
            }
        } else if (atomic_exchange(&v->reset, false)) {
            atomic_store(&v->frame_pos, 0);
        } else {
            atomic_store(&v->frame_pos, pos);
        }
    } else {
        memset(dst, 0, nf * (size_t)stride);
    }

    buf->datas[0].chunk->offset = 0;
    buf->datas[0].chunk->stride = stride;
    buf->datas[0].chunk->size   = nf * (size_t)stride;

    pw_stream_queue_buffer(v->stream, b);
}

static const struct pw_stream_events stream_events = {
    PW_VERSION_STREAM_EVENTS,
    .process = on_process,
};

static int audio_init(void) {
    pw_init(NULL, NULL);

    audio.loop = pw_thread_loop_new("doubletap-audio", NULL);
    if (!audio.loop) { pw_deinit(); return -1; }

    pw_thread_loop_lock(audio.loop);
    struct pw_loop *pl = pw_thread_loop_get_loop(audio.loop);

    static const char *const voice_name[AUDIO_NVOICES] = {
        "doubletap-v1", "doubletap-v2"
    };

    int connected = 0;
    for (int i = 0; i < AUDIO_NVOICES; i++) {
        audio_voice_t *v = &audio.voice[i];
        if (!v->samples) continue; /* voice with no sample: never plays */

        uint8_t podbuf[1024];
        struct spa_pod_builder b = SPA_POD_BUILDER_INIT(podbuf, sizeof(podbuf));
        const struct spa_pod *params[1];

        struct pw_properties *props = pw_properties_new(
            PW_KEY_MEDIA_TYPE,     "Audio",
            PW_KEY_MEDIA_CATEGORY, "Playback",
            PW_KEY_MEDIA_ROLE,     "Game",
            NULL);

        /* pass the voice as userdata so on_process knows which one it drives */
        v->stream = pw_stream_new_simple(pl, voice_name[i], props,
                                         &stream_events, v);
        if (!v->stream) continue;

        params[0] = spa_format_audio_raw_build(&b, SPA_PARAM_EnumFormat,
            &SPA_AUDIO_INFO_RAW_INIT(
                .format   = SPA_AUDIO_FORMAT_F32,
                .channels = v->channels,
                .rate     = v->sample_rate));

        pw_stream_connect(v->stream,
                          PW_DIRECTION_OUTPUT,
                          PW_ID_ANY,
                          PW_STREAM_FLAG_AUTOCONNECT |
                          PW_STREAM_FLAG_MAP_BUFFERS  |
                          PW_STREAM_FLAG_RT_PROCESS,
                          params, 1);
        connected++;
    }

    pw_thread_loop_unlock(audio.loop);

    if (connected == 0) {
        pw_thread_loop_destroy(audio.loop);
        audio.loop = NULL;
        pw_deinit();
        return -1;
    }

    if (pw_thread_loop_start(audio.loop) < 0) {
        pw_thread_loop_lock(audio.loop);
        for (int i = 0; i < AUDIO_NVOICES; i++) {
            if (audio.voice[i].stream) {
                pw_stream_destroy(audio.voice[i].stream);
                audio.voice[i].stream = NULL;
            }
        }
        pw_thread_loop_unlock(audio.loop);
        pw_thread_loop_destroy(audio.loop);
        audio.loop = NULL;
        pw_deinit();
        return -1;
    }

    return 0;
}

/* voice: 0 == V1, 1 == V2. Each stream restarts independently, so a V1
 * click and a V2 click overlap rather than cutting each other off. */
static void audio_trigger(int voice) {
    if (!audio_available) return;
    if (voice < 0 || voice >= AUDIO_NVOICES) return;
    audio_voice_t *v = &audio.voice[voice];
    if (!v->stream) return; /* no sample loaded for this voice */
    if (atomic_load(&v->playing))
        atomic_store(&v->reset, true);
    else
        atomic_fetch_add(&v->pending, 1);
}

static void audio_cleanup(void) {
    if (!audio_available) return;
    if (audio.loop) {
        pw_thread_loop_stop(audio.loop);
        pw_thread_loop_lock(audio.loop);
        for (int i = 0; i < AUDIO_NVOICES; i++) {
            if (audio.voice[i].stream) {
                pw_stream_destroy(audio.voice[i].stream);
                audio.voice[i].stream = NULL;
            }
        }
        pw_thread_loop_unlock(audio.loop);
        pw_thread_loop_destroy(audio.loop);
        audio.loop = NULL;
    }
    pw_deinit();
    for (int i = 0; i < AUDIO_NVOICES; i++) {
        free(audio.voice[i].samples);
        audio.voice[i].samples = NULL;
    }
    audio_available = 0;
}

/* ------------------------------------------------------------------------- */
/* Config                                                                    */
/* ------------------------------------------------------------------------- */

/* SOCD-cleaning behavior for k1/k2 (see process_event). */
enum {
    SOCD_TOGGLE = 0, /* last-input + reverting toggle (latching) */
    SOCD_SNAPPY = 1, /* last input wins; only the active key's release reverts */
    SOCD_OFF    = 2, /* no cleaning; k1/k2 -> v1/v2 remap only */
    SOCD_ANALOG = 3, /* toggle, but driven by travel depth (see analog_key_t) */
};

/* Analog mode tuning. Depths are millimetres of travel; see the banner in
 * the analog section for what each threshold gates. */
typedef struct {
    char  *device;        /* explicit /dev/hidrawN, NULL == auto-discover */
    float  travel_mm;     /* full key travel, for normalized -> mm */
    float  actuation_mm;  /* press threshold */
    float  release_mm;    /* DERIVED, not configured: actuation_mm scaled by
                           * ANALOG_RELEASE_RATIO. Hysteresis is not a taste
                           * - it only has to be far enough below actuation
                           * that a resting finger cannot straddle both, and
                           * far enough above the firmware's reporting floor
                           * that a key at rest still counts as released.
                           * Scaling beats a fixed offset because actuation
                           * ranges from 0.1mm to most of the travel, and a
                           * subtraction that suits one end goes negative at
                           * the other. */
    /* bottom_out_mm defines the backplate, and the backplate is the whole
     * gesture: both LATCH keys on it at once is what arms `deep` and hands
     * the pool to the toggle. It does triple duty, deliberately - one
     * number, so there is one place to tune how far down "planted" means:
     * the tapping bottom-out re-press, the latch threshold, and the only
     * threshold in force while riding. */
    int    rt_enabled;
    /* Rapid trigger is TAPPING-ONLY. While the latch is armed the backplate
     * is the switch and neither reversal distance runs at all - riding
     * bottoms out every stroke by definition, so every note in a burst
     * lands on the same hard physical line. (An earlier revision ran a
     * second `deep_press_mm`/`deep_release_mm` profile here; those config
     * keys were removed, and load_config warns if it still finds them.) */
    float  rt_press_mm;   /* downward reversal that re-presses */
    float  rt_release_mm; /* upward reversal that releases */
    float  bottom_out_mm; /* within this of full travel == bottomed out:
                           * presses regardless of rt_press_mm, and, for
                           * both latch keys at once, arms `deep` */
    /* HID usage overrides, parallel to keys.pool; -1 == derive from the
     * keymap. 'analog.hid_k1'/'hid_k2' are the legacy two-key spelling and
     * write entries 0 and 1. */
    int    hid[POOL_MAX];
    int    n_hid;         /* entries given via the 'hid' list, for checking */
} analog_config_t;

typedef struct {
    char   **device_paths;
    size_t   n_devices;
    int      auto_discover; /* no 'devices' list: scan for matching keyboards */
    /* The physical key pool. Every entry drives v1/v2 through the slot
     * layer; which of the two a key gets is decided per press, not here.
     * The legacy scalar keys.k1/k2 are simply a two-entry pool, so there is
     * one representation downstream and no special case for the old form. */
    int      pool[POOL_MAX];
    int      n_pool;
    /* Analog only: the two pool INDICES whose travel arms the deep latch.
     * Defaults to 0 and 1, which is what the legacy k1/k2 form resolves to. */
    int      latch[2];
    int      latch_set;     /* 'keys.latch' was given explicitly */
    int      v1, v2;
    int      socd;
    int      audio_enabled;
    char    *wav_path;      /* base sample; per-key fallback */
    char    *wav_v1;        /* optional V1 override (NULL -> use wav_path) */
    char    *wav_v2;        /* optional V2 override (NULL -> use wav_path) */
    float    gain;          /* base gain; per-key fallback (default 1.0) */
    float    gain_v1;       /* V1 gain override (<0 -> use gain) */
    float    gain_v2;       /* V2 gain override (<0 -> use gain) */
    char    *uinput_name;
    analog_config_t analog;
} oid_config_t;

static void config_init(oid_config_t *c) {
    memset(c, 0, sizeof(*c));
    c->pool[0]  = DEF_K1;
    c->pool[1]  = DEF_K2;
    c->n_pool   = 2;
    c->latch[0] = 0;
    c->latch[1] = 1;
    c->v1 = DEF_V1;
    c->v2 = DEF_V2;
    c->audio_enabled = 1;
    c->gain    = 1.0f;
    c->gain_v1 = -1.0f; /* sentinel: unset -> falls back to gain */
    c->gain_v2 = -1.0f;
    c->analog.travel_mm     = 4.0f;
    c->analog.actuation_mm  = 1.0f;
    c->analog.rt_enabled    = 1;
    c->analog.rt_press_mm   = 0.3f;
    c->analog.rt_release_mm = 0.3f;
    c->analog.bottom_out_mm = 0.20f;  /* 95% of the 4mm default travel */
    for (int i = 0; i < POOL_MAX; i++)
        c->analog.hid[i] = -1;      /* sentinel: derive from the keymap */
}

static void config_free(oid_config_t *c) {
    if (!c) return;
    for (size_t i = 0; i < c->n_devices; i++)
        free(c->device_paths[i]);
    free(c->device_paths);
    free(c->wav_path);
    free(c->wav_v1);
    free(c->wav_v2);
    free(c->uinput_name);
    free(c->analog.device);
    memset(c, 0, sizeof(*c));
}

/* "auto" (or an unset node) leaves the override alone; anything else must
 * parse as a HID usage id. */
static int parse_hid_usage(yaml_node_t *v, int *out) {
    const char *s = v && v->type == YAML_SCALAR_NODE
                    ? (const char *)v->data.scalar.value : "";
    if (!strcasecmp(s, "auto")) return 0;
    char *end = NULL;
    errno = 0;
    long u = strtol(s, &end, 0);
    if (end == s || *end != '\0' || errno != 0 || u < 0 || u > 0xFFFF)
        return -1;
    *out = (int)u;
    return 0;
}

/* Name a key code for a message: the symbolic name when libevdev knows one,
 * otherwise the bare number. Rotates through a few static buffers so more
 * than one can appear in a single call. */
static const char *key_name_or(int code) {
    static char buf[4][32];
    static int  turn;
    const char *nm = libevdev_event_code_get_name(EV_KEY, (unsigned)code);
    if (nm) return nm;
    turn = (turn + 1) % 4;
    snprintf(buf[turn], sizeof(buf[turn]), "%d", code);
    return buf[turn];
}

/* libyaml document-API helpers --------------------------------------------- */

static yaml_node_t* ynode(yaml_document_t *doc, int id) {
    return id ? yaml_document_get_node(doc, id) : NULL;
}

/* Look up a scalar key in a mapping node. Returns the value node or NULL. */
static yaml_node_t* map_get(yaml_document_t *doc, yaml_node_t *map, const char *key) {
    if (!map || map->type != YAML_MAPPING_NODE) return NULL;
    for (yaml_node_pair_t *p = map->data.mapping.pairs.start;
         p < map->data.mapping.pairs.top; p++) {
        yaml_node_t *k = ynode(doc, p->key);
        if (!k || k->type != YAML_SCALAR_NODE) continue;
        if (strcmp((const char *)k->data.scalar.value, key) == 0)
            return ynode(doc, p->value);
    }
    return NULL;
}

static int scalar_dup(yaml_node_t *n, char **out) {
    if (!n || n->type != YAML_SCALAR_NODE) return -1;
    char *s = strdup((const char *)n->data.scalar.value);
    if (!s) return -1;
    free(*out);
    *out = s;
    return 0;
}

static int parse_bool(yaml_node_t *n, int *out) {
    if (!n || n->type != YAML_SCALAR_NODE) return -1;
    const char *s = (const char *)n->data.scalar.value;
    if (!strcasecmp(s, "true")  || !strcasecmp(s, "yes") ||
        !strcasecmp(s, "on")    || !strcmp(s, "1"))    { *out = 1; return 0; }
    if (!strcasecmp(s, "false") || !strcasecmp(s, "no") ||
        !strcasecmp(s, "off")   || !strcmp(s, "0"))    { *out = 0; return 0; }
    return -1;
}

/* Parse a non-negative float (a linear gain). Rejects garbage and negatives. */
static int parse_gain(yaml_node_t *n, float *out) {
    if (!n || n->type != YAML_SCALAR_NODE) return -1;
    const char *s = (const char *)n->data.scalar.value;
    char *end = NULL;
    errno = 0;
    double d = strtod(s, &end);
    if (end == s || *end != '\0' || errno != 0 || d < 0.0)
        return -1;
    *out = (float)d;
    return 0;
}


/* Fill in the deep profile from the tapping profile wherever the config
 * left it unset, so everything downstream reads concrete numbers. */
/* Hysteresis as a fraction of actuation, and the shallowest derived
 * release worth trusting: a Wooting's firmware stops reporting a key a few
 * hundredths of a millimetre from rest, and a release threshold underneath
 * that would never fire. */
#define ANALOG_RELEASE_RATIO 0.8f
#define ANALOG_RELEASE_FLOOR 0.04f

static void analog_config_resolve(analog_config_t *a) {
    a->release_mm = a->actuation_mm * ANALOG_RELEASE_RATIO;
}

/* Thresholds that are individually well-formed can still combine into a
 * broken machine, and the failure modes are hard to attribute while playing:
 * a key that self-oscillates at the report rate, or one whose deep latch can
 * never clear. Reject those outright rather than let them reach the RT path.
 * Only enforced for socd: analog - an unused analog block stays harmless. */
static int analog_config_check(const analog_config_t *a) {
    int bad = 0;

    if (a->travel_mm <= 0.0f) {
        LOG_ERR("'analog.travel_mm' must be greater than 0");
        bad = 1;
    }
    if (a->actuation_mm <= 0.0f) {
        LOG_ERR("'analog.actuation_mm' must be greater than 0");
        bad = 1;
    }
    if (a->rt_enabled && a->rt_press_mm <= 0.0f) {
        LOG_ERR("'analog.rapid_trigger.press_mm' must be greater than 0");
        bad = 1;
    }
    if (a->rt_enabled && a->rt_release_mm <= 0.0f) {
        LOG_ERR("'analog.rapid_trigger.release_mm' must be greater than 0, "
                "otherwise a held key releases itself");
        bad = 1;
    }
    /* Nothing could ever re-press a key in the riding zone: travel is
     * switched off and there is no backplate rule to fall back on. */
    /* The backplate is load-bearing now: it defines the gesture, not just
     * a rapid-trigger corner case. */
    if (a->bottom_out_mm <= 0.0f) {
        LOG_ERR("'analog.rapid_trigger.bottom_out_mm' must be greater than "
                "0: it defines where the backplate starts, and both keys "
                "reaching it is what arms the deep latch");
        bad = 1;
    }
    /* Actuation has to sit above the backplate, and the deep latch makes
     * this load-bearing rather than merely tidy: actuation_mm is what
     * DISARMS the latch, so an actuation line at or below the backplate
     * would disarm it on the very sample it armed, and with actuation past
     * travel_mm the key could never actuate at all. Both are silent
     * bricks. */
    if (a->travel_mm > 0.0f && a->bottom_out_mm > 0.0f &&
        a->actuation_mm >= a->travel_mm - a->bottom_out_mm) {
        LOG_ERR("'analog.actuation_mm' (%.3f) must be shallower than the "
                "backplate at %.3fmm (travel_mm - bottom_out_mm), otherwise "
                "a key can reach the floor without ever actuating",
                (double)a->actuation_mm,
                (double)(a->travel_mm - a->bottom_out_mm));
        bad = 1;
    }
    if (a->bottom_out_mm >= a->travel_mm) {
        LOG_ERR("'analog.rapid_trigger.bottom_out_mm' (%.3f) must be less "
                "than travel_mm (%.3f): every sample would count as "
                "bottomed",
                (double)a->bottom_out_mm, (double)a->travel_mm);
        bad = 1;
    }
    if (a->hid[0] >= 0 && a->hid[0] == a->hid[1]) {
        LOG_ERR("'analog.hid_k1' and 'analog.hid_k2' are both 0x%02x: one "
                "physical key would drive both virtual keys",
                a->hid[0]);
        bad = 1;
    }
    if (bad)
        return -1;

    /* Survivable, but almost certainly not what was meant. */
    if (a->release_mm < ANALOG_RELEASE_FLOOR)
        LOG_WARN("'analog.actuation_mm' (%.3f) puts the derived release at "
                 "%.3fmm, under the ~%.2fmm the firmware stops reporting at: "
                 "a resting key may never count as released",
                 (double)a->actuation_mm, (double)a->release_mm,
                 (double)ANALOG_RELEASE_FLOOR);
    if (a->bottom_out_mm >= a->travel_mm)
        LOG_WARN("'analog.rapid_trigger.bottom_out_mm' (%.3f) >= travel_mm "
                 "(%.3f): every sample counts as bottomed out",
                 (double)a->bottom_out_mm, (double)a->travel_mm);
    /* A backplate under 90% of travel is reachable by a firm tap, and a tap
     * that reaches it is a tap that can arm the latch - which is exactly the
     * alt-tapping misfire the latch exists to avoid. */
    if (a->travel_mm > 0.0f && a->bottom_out_mm > 0.10f * a->travel_mm)
        LOG_WARN("'analog.rapid_trigger.bottom_out_mm' (%.3f) puts the "
                 "backplate at %.1f%% of travel: below ~90%% a firm tap can "
                 "reach it, and two overlapping taps could arm the deep "
                 "latch. Record a session with -T and check it with `replay`",
                 (double)a->bottom_out_mm,
                 (double)((a->travel_mm - a->bottom_out_mm)
                          / a->travel_mm * 100.0f));
    return 0;
}

/* Resolve a key code from a scalar - either a symbolic name ("KEY_Z") or
 * a decimal/hex integer. Returns 0 on success. */
static int parse_key_code(yaml_node_t *n, int *out) {
    if (!n || n->type != YAML_SCALAR_NODE) return -1;
    const char *s = (const char *)n->data.scalar.value;

    /* Try numeric first. */
    if (s[0] != '\0') {
        char *end = NULL;
        long v = strtol(s, &end, 0);
        if (end && *end == '\0' && end != s &&
            v >= 0 && v < KEY_MAX) {
            *out = (int)v;
            return 0;
        }
    }

    int code = libevdev_event_code_from_name(EV_KEY, s);
    if (code < 0) return -1;
    *out = code;
    return 0;
}

static int load_config(const char *path, oid_config_t *c) {
    FILE *fp = fopen(path, "rb");
    if (!fp) {
        LOG_ERR("Cannot open config %s: %s", path, strerror(errno));
        return -1;
    }

    yaml_parser_t parser;
    yaml_document_t doc;
    memset(&doc, 0, sizeof(doc));
    int rc = -1;
    int doc_loaded = 0;

    if (!yaml_parser_initialize(&parser)) {
        LOG_ERR("yaml_parser_initialize failed");
        fclose(fp);
        return -1;
    }
    yaml_parser_set_input_file(&parser, fp);

    if (!yaml_parser_load(&parser, &doc)) {
        LOG_ERR("YAML parse error in %s: %s (line %zu, col %zu)",
                path, parser.problem ? parser.problem : "unknown",
                (size_t)parser.problem_mark.line + 1,
                (size_t)parser.problem_mark.column + 1);
        goto out;
    }
    doc_loaded = 1;

    yaml_node_t *root = yaml_document_get_root_node(&doc);
    if (!root) {
        LOG_ERR("config %s is empty", path);
        goto out;
    }
    if (root->type != YAML_MAPPING_NODE) {
        LOG_ERR("config root must be a mapping");
        goto out;
    }

    /* devices: optional sequence of scalar paths. Omitted (or the scalar
     * "auto") means auto-discovery: grab every keyboard-shaped device that
     * advertises both k1 and k2 (see auto_grab_ok). ------------------- */
    yaml_node_t *devs = map_get(&doc, root, "devices");
    if (!devs ||
        (devs->type == YAML_SCALAR_NODE &&
         !strcasecmp((const char *)devs->data.scalar.value, "auto"))) {
        c->auto_discover = 1;
    } else if (devs->type != YAML_SEQUENCE_NODE) {
        LOG_ERR("'devices' must be a sequence of paths, or \"auto\"");
        goto out;
    } else {
        size_t n = (size_t)(devs->data.sequence.items.top -
                            devs->data.sequence.items.start);
        if (n == 0) {
            LOG_ERR("'devices' list is empty");
            goto out;
        }
        c->device_paths = calloc(n, sizeof(char *));
        if (!c->device_paths) { LOG_ERR("oom"); goto out; }
        for (yaml_node_item_t *it = devs->data.sequence.items.start;
             it < devs->data.sequence.items.top; it++) {
            yaml_node_t *item = ynode(&doc, *it);
            if (scalar_dup(item, &c->device_paths[c->n_devices]) != 0) {
                LOG_ERR("'devices' entry must be a scalar string");
                goto out;
            }
            c->n_devices++;
        }
    }

    /* keys: optional mapping*/
    yaml_node_t *keys = map_get(&doc, root, "keys");
    if (keys) {
        if (keys->type != YAML_MAPPING_NODE) {
            LOG_ERR("'keys' must be a mapping");
            goto out;
        }
        struct { const char *name; int *out; } kmap[] = {
            { "v1", &c->v1 }, { "v2", &c->v2 },
        };
        for (size_t i = 0; i < sizeof(kmap)/sizeof(kmap[0]); i++) {
            yaml_node_t *kn = map_get(&doc, keys, kmap[i].name);
            if (kn && parse_key_code(kn, kmap[i].out) != 0) {
                LOG_ERR("invalid key code for 'keys.%s'", kmap[i].name);
                goto out;
            }
        }

        /* The pool. 'pool' is the general form; 'k1'/'k2' are the legacy
         * two-key spelling of the same thing. Accepting both at once would
         * mean silently picking a winner, so it is an error. */
        yaml_node_t *pool = map_get(&doc, keys, "pool");
        yaml_node_t *k1n  = map_get(&doc, keys, "k1");
        yaml_node_t *k2n  = map_get(&doc, keys, "k2");

        if (pool && (k1n || k2n)) {
            LOG_ERR("'keys.pool' and 'keys.k1'/'keys.k2' are two spellings "
                    "of the same setting - use one or the other");
            goto out;
        }

        if (pool) {
            if (pool->type != YAML_SEQUENCE_NODE) {
                LOG_ERR("'keys.pool' must be a sequence of key codes");
                goto out;
            }
            size_t n = (size_t)(pool->data.sequence.items.top -
                                pool->data.sequence.items.start);
            if (n < 2) {
                LOG_ERR("'keys.pool' needs at least two keys");
                goto out;
            }
            if (n > POOL_MAX) {
                LOG_ERR("'keys.pool' has %zu keys; the maximum is %d",
                        n, POOL_MAX);
                goto out;
            }
            c->n_pool = 0;
            for (yaml_node_item_t *it = pool->data.sequence.items.start;
                 it < pool->data.sequence.items.top; it++) {
                int code = 0;
                if (parse_key_code(ynode(&doc, *it), &code) != 0) {
                    LOG_ERR("invalid key code in 'keys.pool' (entry %d)",
                            c->n_pool + 1);
                    goto out;
                }
                for (int j = 0; j < c->n_pool; j++) {
                    if (c->pool[j] == code) {
                        LOG_ERR("'keys.pool' lists %s twice",
                                key_name_or(code));
                        goto out;
                    }
                }
                c->pool[c->n_pool++] = code;
            }
        } else {
            if (k1n && parse_key_code(k1n, &c->pool[0]) != 0) {
                LOG_ERR("invalid key code for 'keys.k1'");
                goto out;
            }
            if (k2n && parse_key_code(k2n, &c->pool[1]) != 0) {
                LOG_ERR("invalid key code for 'keys.k2'");
                goto out;
            }
            c->n_pool = 2;
            if (c->pool[0] == c->pool[1]) {
                LOG_ERR("'keys.k1' and 'keys.k2' are the same key");
                goto out;
            }
        }

        /* latch: which two pool keys arm the deep latch. Analog only - it
         * names a pair of travel-read keys, and nothing else in the daemon
         * reads travel. Defaults to the first two pool entries, which is
         * exactly what the legacy k1/k2 form means. */
        yaml_node_t *latch = map_get(&doc, keys, "latch");
        if (latch) {
            if (latch->type != YAML_SEQUENCE_NODE ||
                latch->data.sequence.items.top -
                latch->data.sequence.items.start != 2) {
                LOG_ERR("'keys.latch' must be a sequence of exactly two keys");
                goto out;
            }
            int i = 0;
            for (yaml_node_item_t *it = latch->data.sequence.items.start;
                 it < latch->data.sequence.items.top; it++, i++) {
                int code = 0;
                if (parse_key_code(ynode(&doc, *it), &code) != 0) {
                    LOG_ERR("invalid key code in 'keys.latch' (entry %d)",
                            i + 1);
                    goto out;
                }
                c->latch[i] = -1;
                for (int j = 0; j < c->n_pool; j++)
                    if (c->pool[j] == code) { c->latch[i] = j; break; }
                if (c->latch[i] < 0) {
                    LOG_ERR("'keys.latch' names %s, which is not in the pool",
                            key_name_or(code));
                    goto out;
                }
            }
            if (c->latch[0] == c->latch[1]) {
                LOG_ERR("'keys.latch' names the same key twice");
                goto out;
            }
            c->latch_set = 1;
        }
    }

    /* socd: optional scalar */
    if (map_get(&doc, root, "mode")) {
        LOG_ERR("'mode' has been renamed to 'socd'");
        goto out;
    }
    yaml_node_t *socd = map_get(&doc, root, "socd");
    if (socd) {
        const char *s = socd->type == YAML_SCALAR_NODE
                        ? (const char *)socd->data.scalar.value : "";
        if (!strcasecmp(s, "toggle") || !strcasecmp(s, "on"))
            c->socd = SOCD_TOGGLE;
        else if (!strcasecmp(s, "snappy") || !strcasecmp(s, "snappy-tappy"))
            c->socd = SOCD_SNAPPY;
        else if (!strcasecmp(s, "off"))
            c->socd = SOCD_OFF;
        else if (!strcasecmp(s, "analog"))
            c->socd = SOCD_ANALOG;
        else {
            LOG_ERR("'socd' must be \"toggle\"/\"on\", \"snappy\", "
                    "\"analog\", or \"off\"");
            goto out;
        }
    }

    /* analog: optional mapping */
    yaml_node_t *an = map_get(&doc, root, "analog");
    if (an) {
        if (an->type != YAML_MAPPING_NODE) {
            LOG_ERR("'analog' must be a mapping");
            goto out;
        }
        yaml_node_t *dv = map_get(&doc, an, "device");
        if (dv) {
            const char *s = dv->type == YAML_SCALAR_NODE
                            ? (const char *)dv->data.scalar.value : "";
            if (strcasecmp(s, "auto") != 0 &&
                scalar_dup(dv, &c->analog.device) != 0) {
                LOG_ERR("'analog.device' must be a scalar string");
                goto out;
            }
        }
        static const struct { const char *key; size_t off; } mm[] = {
            { "travel_mm",     offsetof(analog_config_t, travel_mm)     },
            { "actuation_mm",  offsetof(analog_config_t, actuation_mm)  },
        };
        for (size_t i = 0; i < sizeof(mm) / sizeof(mm[0]); i++) {
            yaml_node_t *v = map_get(&doc, an, mm[i].key);
            if (v && parse_gain(v, (float *)((char *)&c->analog + mm[i].off))) {
                LOG_ERR("'analog.%s' must be a non-negative number", mm[i].key);
                goto out;
            }
        }
        /* Removed along with the gate. Warn rather than fail: a stale key
         * in a config file should not stop the daemon starting. */
        if (map_get(&doc, an, "gate") || map_get(&doc, an, "gate_depth_mm") ||
            map_get(&doc, an, "gate_margin_mm") ||
            map_get(&doc, an, "socd_depth_mm") ||
            map_get(&doc, an, "engage") ||
            map_get(&doc, an, "release_mm"))
            LOG_WARN("'analog.gate', 'gate_depth_mm', 'gate_margin_mm', "
                     "'socd_depth_mm', 'engage' and 'release_mm' were "
                     "removed and are ignored - both keys reaching the "
                     "backplate is what arms the deep latch, and release "
                     "follows actuation");

        yaml_node_t *rt = map_get(&doc, an, "rapid_trigger");
        if (rt) {
            if (rt->type != YAML_MAPPING_NODE) {
                LOG_ERR("'analog.rapid_trigger' must be a mapping");
                goto out;
            }
            yaml_node_t *en = map_get(&doc, rt, "enabled");
            if (en && parse_bool(en, &c->analog.rt_enabled) != 0) {
                LOG_ERR("'analog.rapid_trigger.enabled' must be a boolean");
                goto out;
            }
            yaml_node_t *pm = map_get(&doc, rt, "press_mm");
            if (pm && parse_gain(pm, &c->analog.rt_press_mm) != 0) {
                LOG_ERR("'analog.rapid_trigger.press_mm' must be a "
                        "non-negative number");
                goto out;
            }
            yaml_node_t *bo = map_get(&doc, rt, "bottom_out_mm");
            if (bo && parse_gain(bo, &c->analog.bottom_out_mm) != 0) {
                LOG_ERR("'analog.rapid_trigger.bottom_out_mm' must be a "
                        "non-negative number");
                goto out;
            }
            yaml_node_t *rm = map_get(&doc, rt, "release_mm");
            if (rm && parse_gain(rm, &c->analog.rt_release_mm) != 0) {
                LOG_ERR("'analog.rapid_trigger.release_mm' must be a "
                        "non-negative number");
                goto out;
            }
            if (map_get(&doc, rt, "deep_press_mm") ||
                map_get(&doc, rt, "deep_release_mm"))
                LOG_WARN("'analog.rapid_trigger.deep_press_mm' and "
                         "'deep_release_mm' were removed and are ignored - "
                         "while riding, the backplate itself is the switch "
                         "and no rapid trigger runs");
        }
        /* HID usage overrides. 'hid' is a list parallel to keys.pool;
         * 'hid_k1'/'hid_k2' are the legacy two-key spelling of entries 0
         * and 1. Both accept "auto" (or omission) to derive from the
         * keymap. */
        static const struct { const char *key; int idx; } hidk[] = {
            { "hid_k1", 0 }, { "hid_k2", 1 },
        };
        yaml_node_t *hidl = map_get(&doc, an, "hid");
        if (hidl && (map_get(&doc, an, "hid_k1") ||
                     map_get(&doc, an, "hid_k2"))) {
            LOG_ERR("'analog.hid' and 'analog.hid_k1'/'hid_k2' are two "
                    "spellings of the same setting - use one or the other");
            goto out;
        }
        if (hidl) {
            if (hidl->type != YAML_SEQUENCE_NODE) {
                LOG_ERR("'analog.hid' must be a sequence of HID usage ids");
                goto out;
            }
            int i = 0;
            for (yaml_node_item_t *it = hidl->data.sequence.items.start;
                 it < hidl->data.sequence.items.top; it++, i++) {
                if (i >= POOL_MAX) {
                    LOG_ERR("'analog.hid' has more than %d entries", POOL_MAX);
                    goto out;
                }
                if (parse_hid_usage(ynode(&doc, *it), &c->analog.hid[i]) != 0) {
                    LOG_ERR("'analog.hid' entry %d must be \"auto\" or a HID "
                            "usage id", i + 1);
                    goto out;
                }
            }
            c->analog.n_hid = i;
        } else {
            for (size_t i = 0; i < sizeof(hidk) / sizeof(hidk[0]); i++) {
                yaml_node_t *v = map_get(&doc, an, hidk[i].key);
                if (!v) continue;
                if (parse_hid_usage(v, &c->analog.hid[hidk[i].idx]) != 0) {
                    LOG_ERR("'analog.%s' must be \"auto\" or a HID usage id",
                            hidk[i].key);
                    goto out;
                }
            }
        }
    }

    /* audio: optional mapping */
    yaml_node_t *aud = map_get(&doc, root, "audio");
    if (aud) {
        if (aud->type != YAML_MAPPING_NODE) {
            LOG_ERR("'audio' must be a mapping");
            goto out;
        }
        yaml_node_t *en = map_get(&doc, aud, "enabled");
        if (en && parse_bool(en, &c->audio_enabled) != 0) {
            LOG_ERR("'audio.enabled' must be a boolean");
            goto out;
        }
        yaml_node_t *wav = map_get(&doc, aud, "wav");
        if (wav && scalar_dup(wav, &c->wav_path) != 0) {
            LOG_ERR("'audio.wav' must be a scalar string");
            goto out;
        }
        yaml_node_t *wav1 = map_get(&doc, aud, "wav_v1");
        if (wav1 && scalar_dup(wav1, &c->wav_v1) != 0) {
            LOG_ERR("'audio.wav_v1' must be a scalar string");
            goto out;
        }
        yaml_node_t *wav2 = map_get(&doc, aud, "wav_v2");
        if (wav2 && scalar_dup(wav2, &c->wav_v2) != 0) {
            LOG_ERR("'audio.wav_v2' must be a scalar string");
            goto out;
        }
        yaml_node_t *g = map_get(&doc, aud, "gain");
        if (g && parse_gain(g, &c->gain) != 0) {
            LOG_ERR("'audio.gain' must be a non-negative number");
            goto out;
        }
        yaml_node_t *g1 = map_get(&doc, aud, "gain_v1");
        if (g1 && parse_gain(g1, &c->gain_v1) != 0) {
            LOG_ERR("'audio.gain_v1' must be a non-negative number");
            goto out;
        }
        yaml_node_t *g2 = map_get(&doc, aud, "gain_v2");
        if (g2 && parse_gain(g2, &c->gain_v2) != 0) {
            LOG_ERR("'audio.gain_v2' must be a non-negative number");
            goto out;
        }
    }
    /* Base sample defaults only when no per-key override covers a voice.
     * A voice with no wav (base or override) simply stays silent. */
    if (c->audio_enabled && !c->wav_path && (!c->wav_v1 || !c->wav_v2)) {
        c->wav_path = strdup(DEF_WAV);
        if (!c->wav_path) { LOG_ERR("oom"); goto out; }
    }

    /* uinput: optional mapping */
    yaml_node_t *ui = map_get(&doc, root, "uinput");
    if (ui) {
        if (ui->type != YAML_MAPPING_NODE) {
            LOG_ERR("'uinput' must be a mapping");
            goto out;
        }
        yaml_node_t *nm = map_get(&doc, ui, "name");
        if (nm && scalar_dup(nm, &c->uinput_name) != 0) {
            LOG_ERR("'uinput.name' must be a scalar string");
            goto out;
        }
    }
    if (!c->uinput_name) {
        c->uinput_name = strdup(DEF_NAME);
        if (!c->uinput_name) { LOG_ERR("oom"); goto out; }
    }

    /* A 'hid' list that does not line up with the pool is a mistake worth
     * naming: a short one silently leaves later keys to the keymap walk, and
     * a long one silently drops entries. */
    if (c->analog.n_hid && c->analog.n_hid != c->n_pool) {
        LOG_ERR("'analog.hid' has %d entries but the pool has %d - it must "
                "have one per pool key, in pool order",
                c->analog.n_hid, c->n_pool);
        goto out;
    }

    /* 'keys.latch' only means something in analog mode: it names the pair
     * whose TRAVEL arms the deep latch, and no other mode reads travel.
     * Silently ignoring it would leave a config that looks like it selects
     * a behaviour it does not. */
    if (c->latch_set && c->socd != SOCD_ANALOG) {
        LOG_ERR("'keys.latch' applies only to 'socd: analog'");
        goto out;
    }

    analog_config_resolve(&c->analog);
    if (c->socd == SOCD_ANALOG && analog_config_check(&c->analog) != 0)
        goto out;

    rc = 0;

out:
    if (doc_loaded) yaml_document_delete(&doc);
    yaml_parser_delete(&parser);
    fclose(fp);
    if (rc != 0) config_free(c);
    return rc;
}

/* ------------------------------------------------------------------------- */
/* Input devices                                                             */
/* ------------------------------------------------------------------------- */

/* epoll user-data tag. The inotify fd is still identified by a NULL ptr;
 * everything else leads with one of these so run_loop can tell an evdev
 * device from the analog hidraw node. */
enum { EP_INPUT = 1, EP_ANALOG = 2 };

/* SOCD state for one input source. Per-source, not global: several
 * keyboards are tracked independently even though they share one virtual
 * output device, and the analog path carries its own instance too. */
typedef struct {
    int k1;   /* slot 0 occupied? */
    int k2;   /* slot 1 occupied? */
    int act;  /* 1 == v1 active, 0 == v2 (or none) */
    int mode; /* mode of the last edge applied; release_stuck needs the
               * EFFECTIVE mode, which for the analog path is not cfg->socd */
} socd_state_t;

/* The physical-key -> slot layer, sitting between the input paths and the
 * SOCD core.
 *
 * The core has always had two SLOTS, not two keys: socd_apply's first
 * argument picks a slot, and the slot picks the virtual code. With one key
 * per slot that distinction never had to be drawn. A pool of N keys draws
 * it - which slot a key drives is decided per press here, and everything
 * downstream (the four-case transition, `act`, release_stuck, the deep
 * latch) is untouched.
 *
 * Slot occupancy is a COUNT, because more than one pool key can share a
 * slot: a third key pressed while both slots are held has nowhere else to
 * go. An edge reaches socd_apply only when a count crosses 0<->1, so the
 * newcomer's press registers as a real transition and its release is silent
 * while a slot-mate is still down - no phantom note.
 *
 * `slot` is seeded by index rather than left unassigned, which is what
 * keeps a two-key pool identical to the old k1/k2 build: whichever key is
 * pressed FIRST after startup must still get its own virtual, not whichever
 * slot happens to alternate next. */
typedef struct {
    int slot;  /* which slot this key drives; seeded by index */
    int held;  /* this key is currently down */
} pool_key_t;

typedef struct {
    socd_state_t socd;             /* the 2-slot core, unchanged */
    pool_key_t   key[POOL_MAX];    /* parallel to cfg->pool */
    int          count[2];         /* pool keys currently held per slot */
    int          last_slot;        /* slot of the most recent press */
} pool_state_t;

static void pool_init(pool_state_t *p, int n) {
    memset(p, 0, sizeof(*p));
    for (int i = 0; i < n && i < POOL_MAX; i++)
        p->key[i].slot = i & 1;
    /* So the first alternating press lands on slot 0. */
    p->last_slot = 1;
}

/* Which pool index is this key code, or -1. */
static int pool_index(const oid_config_t *cfg, unsigned code) {
    for (int i = 0; i < cfg->n_pool; i++)
        if ((unsigned)cfg->pool[i] == code) return i;
    return -1;
}

/* Pick the slot for a press of pool index `i`.
 *
 * Sticky: a key reuses the slot it last drove whenever that slot is free,
 * so a key tapped on its own always produces the same virtual and a two-key
 * pool behaves exactly as k1/k2 always did. When its own slot is taken the
 * free one wins, which is what makes any sequence of distinct keys
 * alternate. When BOTH are taken there is no free slot, so the newcomer
 * alternates off the last press and shares.
 *
 * `alternating` drops the sticky rule for the both-free case, so repeated
 * taps of one key alternate too. It is scoped to `socd: off` with a pool
 * larger than two - the "F13/F14 riding atop the real keys" shape - and
 * keyed on the CONFIGURED mode, not the effective one: the analog path runs
 * effective OFF whenever the latch is not armed, and alternating solo taps
 * there would contradict what analog mode is for. */
static int pool_pick_slot(pool_state_t *p, int i, int alternating) {
    if (p->count[0] == 0 && p->count[1] == 0)
        return alternating ? !p->last_slot : p->key[i].slot;
    if (p->count[0] == 0) return 0;
    if (p->count[1] == 0) return 1;
    return !p->last_slot;
}

typedef struct {
    int               kind;    /* EP_INPUT */
    struct libevdev  *dev;
    int               fd;
    char             *path;
    dev_t             rdev; /* st_rdev, dedupes nodes reached via symlinks */
    int               grabbed; /* 0 = open but grab deferred (keys held) */
    int               analog;  /* pool keys come from the analog path; drop */
    pool_state_t      pool;
} input_dev_t;

/* Grabbed devices as stable heap pointers: epoll user data points at the
 * entries, so the list may grow and shrink but entries never move. */
typedef struct {
    input_dev_t **v;
    size_t        n, cap;
} dev_list_t;

static int dev_list_add(dev_list_t *l, input_dev_t *in) {
    if (l->n == l->cap) {
        size_t cap = l->cap ? l->cap * 2 : 8;
        input_dev_t **v = realloc(l->v, cap * sizeof(*v));
        if (!v) return -1;
        l->v   = v;
        l->cap = cap;
    }
    l->v[l->n++] = in;
    return 0;
}

static void dev_list_remove(dev_list_t *l, input_dev_t *in) {
    for (size_t i = 0; i < l->n; i++) {
        if (l->v[i] == in) {
            l->v[i] = l->v[--l->n];
            return;
        }
    }
}

static int dev_list_has_rdev(const dev_list_t *l, dev_t rdev) {
    for (size_t i = 0; i < l->n; i++)
        if (l->v[i]->rdev == rdev) return 1;
    return 0;
}

/* Vendor/product of the keyboard whose k1/k2 the analog path owns, or 0
 * when nothing does. Devices matching it get in->analog set, which makes
 * process_event drop their digital k1/k2 so each press is emitted once. */
static uint16_t g_analog_vid, g_analog_pid;

/* Never grab a doubletap output device: grabbing our own uinput node (or
 * another instance's) feeds every emitted event straight back in as input -
 * an instant feedback loop. Applies in both explicit and auto mode. */
static int is_doubletap_output(struct libevdev *dev, const oid_config_t *cfg) {
    const char *uniq = libevdev_get_uniq(dev);
    const char *name = libevdev_get_name(dev);
    if (uniq && strcmp(uniq, "doubletap") == 0) return 1;
    if (name && strcmp(name, cfg->uinput_name) == 0) return 1;
    return 0;
}

/* Auto-discovery filter: keyboard-shaped devices that advertise both
 * configured physical keys. Devices with pointer/absolute axes are
 * rejected because the virtual device is key-only - grabbing a mouse
 * whose HID descriptor also claims keyboard keys (common on gaming mice)
 * would swallow its motion. Virtual devices (ours, keyd's, ...) are
 * rejected to stay loop-free among remappers. */
static int auto_grab_ok(struct libevdev *dev, const oid_config_t *cfg) {
    if (libevdev_get_id_bustype(dev) == BUS_VIRTUAL) return 0;
    /* Every pool key, not just some: the slot layer alternates across the
     * whole pool, and a board carrying only part of it would alternate
     * against keys it cannot see. */
    for (int i = 0; i < cfg->n_pool; i++)
        if (!libevdev_has_event_code(dev, EV_KEY, (unsigned)cfg->pool[i]))
            return 0;
    if (libevdev_has_event_type(dev, EV_REL) ||
        libevdev_has_event_type(dev, EV_ABS))
        return 0;
    return 1;
}

/* Any key physically down right now? libevdev syncs key state at init and
 * keeps it current as events are read, so this needs no extra ioctl. */
static int any_key_down(const struct libevdev *dev) {
    for (unsigned int code = 1; code <= KEY_MAX; code++)
        if (libevdev_get_event_value(dev, EV_KEY, code))
            return 1;
    return 0;
}

/* Open, vet, and grab one event node. Returns NULL (silently, for the
 * expected cases) when the device shouldn't or can't be grabbed. */
static input_dev_t *input_try_open(const char *path, const oid_config_t *cfg,
                                   int auto_mode, int quiet) {
    int fd = open(path, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
    if (fd < 0) {
        /* EACCES right after a hotplug is expected - udev hasn't applied
         * the input-group permissions yet; the IN_ATTRIB watch retriggers
         * reconcile_devices once it does. */
        if (!quiet)
            LOG_ERR("open(%s): %s", path, strerror(errno));
        else if (errno != EACCES && errno != ENOENT)
            LOG_WARN("open(%s): %s", path, strerror(errno));
        return NULL;
    }

    struct stat st;
    if (fstat(fd, &st) < 0) {
        close(fd);
        return NULL;
    }

    struct libevdev *dev = NULL;
    int rc = libevdev_new_from_fd(fd, &dev);
    if (rc < 0) {
        if (!quiet)
            LOG_ERR("libevdev_new_from_fd(%s): %s", path, strerror(-rc));
        close(fd);
        return NULL;
    }

    if (is_doubletap_output(dev, cfg) ||
        (auto_mode && !auto_grab_ok(dev, cfg))) {
        libevdev_free(dev);
        close(fd);
        return NULL;
    }

    /* Grabbing while a key is physically down would swallow its release:
     * the press already reached the display server through the raw device,
     * but the release would arrive only here and be re-emitted on the
     * virtual keyboard, leaving the raw key logically stuck (typing the
     * Enter that starts the daemon is enough to hit this). Defer instead:
     * keep the fd open ungrabbed, discard its events (the system still
     * gets them directly), and let drain_device finish the grab once
     * every key is up. */
    int defer = any_key_down(dev);
    if (!defer) {
        rc = libevdev_grab(dev, LIBEVDEV_GRAB);
        if (rc < 0) {
            LOG_WARN("libevdev_grab(%s \"%s\"): %s", path,
                     libevdev_get_name(dev) ? libevdev_get_name(dev) : "?",
                     strerror(-rc));
            libevdev_free(dev);
            close(fd);
            return NULL;
        }
    }

    input_dev_t *in = calloc(1, sizeof(*in));
    if (!in) {
        LOG_ERR("oom");
        if (!defer) libevdev_grab(dev, LIBEVDEV_UNGRAB);
        libevdev_free(dev);
        close(fd);
        return NULL;
    }
    in->kind    = EP_INPUT;
    /* Seeds each key's slot by index; zero-init would put every pool key in
     * slot 0 and the first press of k2 would come out as v1. */
    pool_init(&in->pool, cfg->n_pool);
    in->analog  = g_analog_vid &&
                  libevdev_get_id_vendor(dev)  == (int)g_analog_vid &&
                  libevdev_get_id_product(dev) == (int)g_analog_pid;
    in->fd      = fd;
    in->dev     = dev;
    in->path    = strdup(path);
    in->rdev    = st.st_rdev;
    in->grabbed = !defer;
    if (defer)
        LOG_INFO("Opened %s (\"%s\") - keys held, deferring grab until "
                 "all released", path,
                 libevdev_get_name(dev) ? libevdev_get_name(dev) : "?");
    else
        LOG_INFO("Opened and grabbed %s (\"%s\")",
                 path, libevdev_get_name(dev) ? libevdev_get_name(dev) : "?");
    return in;
}

static void input_close(input_dev_t *in) {
    if (!in) return;
    if (in->dev) {
        libevdev_grab(in->dev, LIBEVDEV_UNGRAB);
        libevdev_free(in->dev);
        in->dev = NULL;
    }
    if (in->fd >= 0) {
        close(in->fd);
        in->fd = -1;
    }
    free(in->path);
    in->path = NULL;
}

static int is_event_node(const struct dirent *d) {
    return strncmp(d->d_name, "event", 5) == 0;
}

static void try_grab(dev_list_t *devs, const char *path,
                     const oid_config_t *cfg, int auto_mode, int loud,
                     int epfd) {
    struct stat st;
    if (stat(path, &st) != 0) {
        if (loud && !auto_mode)
            LOG_WARN("stat(%s): %s", path, strerror(errno));
        return;
    }
    if (!S_ISCHR(st.st_mode))
        return;
    if (dev_list_has_rdev(devs, st.st_rdev))
        return; /* already grabbed (possibly via another path/symlink) */

    input_dev_t *in = input_try_open(path, cfg, auto_mode, !loud);
    if (!in)
        return;

    struct epoll_event ev = { .events = EPOLLIN, .data = { .ptr = in } };
    if (dev_list_add(devs, in) != 0 ||
        epoll_ctl(epfd, EPOLL_CTL_ADD, in->fd, &ev) < 0) {
        LOG_ERR("failed to register %s: %s", path, strerror(errno));
        dev_list_remove(devs, in);
        input_close(in);
        free(in);
    }
}

/* (Re)open whatever should be grabbed but currently isn't: every configured
 * path in explicit mode, every matching event node under input_dir in auto
 * mode. Runs at startup (loud) and again on every inotify event under
 * input_dir (quiet - the same non-matching nodes get revisited each time). */
static void reconcile_devices(dev_list_t *devs, const oid_config_t *cfg,
                              const char *input_dir, int epfd, int loud) {
    if (!cfg->auto_discover) {
        for (size_t i = 0; i < cfg->n_devices; i++)
            try_grab(devs, cfg->device_paths[i], cfg, 0, loud, epfd);
        return;
    }

    struct dirent **ents = NULL;
    int n = scandir(input_dir, &ents, is_event_node, alphasort);
    if (n < 0) {
        LOG_WARN("scandir(%s): %s", input_dir, strerror(errno));
        return;
    }
    for (int i = 0; i < n; i++) {
        char path[PATH_MAX];
        snprintf(path, sizeof(path), "%s/%s", input_dir, ents[i]->d_name);
        try_grab(devs, path, cfg, 1, loud, epfd);
        free(ents[i]);
    }
    free(ents);
}

/* ------------------------------------------------------------------------- */
/* Analog input (hidraw)                                                     */
/* ------------------------------------------------------------------------- */

/*
 * Analog keyboards report per-key travel depth on a vendor-defined HID
 * interface, separate from the keyboard interface evdev binds to. The
 * kernel creates no input node for a vendor usage page, so that interface
 * exists purely as a /dev/hidrawN character device: a pollable fd that
 * drops straight into the epoll set in run_loop(), and one that EVIOCGRAB
 * on the keyboard node has no bearing on. We can grab the keyboard and
 * read its analog stream at the same time.
 *
 * Two report formats are in the wild:
 *
 *   v2 (usage page 0xFF53) - 64 bytes, sixteen 4-byte slots, 10-bit depth
 *       [matrix_pos][key][packed][value]
 *   v1 (usage page 0xFF54) - 48 bytes, sixteen 3-byte slots, 8-bit depth
 *       [code_hi][code_lo][value]
 *
 * Both carry only keys that are off their rest position. A key that has
 * just been released appears once with a zero value and then vanishes, so
 * "absent from the report" means "at rest" - see analog_feed().
 */

#define WOOTING_VID         0x31E3
#define WOOTING_VID_LEGACY  0x03EB

#define ANALOG_SLOTS        16
#define ANALOG_V1_REPORT    (ANALOG_SLOTS * 3)  /* 48 */
#define ANALOG_V2_REPORT    (ANALOG_SLOTS * 4)  /* 64 */
#define ANALOG_DESC_MAX     4096

enum { ANALOG_FMT_NONE = 0, ANALOG_FMT_V1, ANALOG_FMT_V2 };

/* One decoded key from an analog report. */
typedef struct {
    uint16_t code;   /* HID usage id; namespace 0 == ordinary keyboard key */
    float    depth;  /* 0.0 at rest .. 1.0 bottomed out */
} analog_sample_t;

/* HID_ID=0003:000031E3:00001320 -> bus, vendor, product. */
static int analog_read_ids(const char *sysdir, uint16_t *vid, uint16_t *pid) {
    char path[PATH_MAX], line[256];
    int  found = -1;

    snprintf(path, sizeof(path), "%s/device/uevent", sysdir);
    FILE *f = fopen(path, "re");
    if (!f) return -1;
    while (fgets(line, sizeof(line), f)) {
        unsigned bus, v, p;
        if (sscanf(line, "HID_ID=%x:%x:%x", &bus, &v, &p) == 3) {
            *vid = (uint16_t)v;
            *pid = (uint16_t)p;
            found = 0;
            break;
        }
    }
    fclose(f);
    return found;
}

/* The analog interface is identified by its vendor usage page, which uevent
 * doesn't expose - so read the report descriptor and look for a Usage Page
 * item (0x06, two-byte payload) carrying 0xFF53 or 0xFF54. This is a byte
 * scan rather than a real HID item walk; paired with the vendor check in
 * analog_scan() that's specific enough, and it keeps this to a few lines. */
static int analog_probe_fmt(const char *sysdir) {
    char          path[PATH_MAX];
    unsigned char buf[ANALOG_DESC_MAX];

    snprintf(path, sizeof(path), "%s/device/report_descriptor", sysdir);
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) return ANALOG_FMT_NONE;
    ssize_t n = read(fd, buf, sizeof(buf));
    close(fd);

    for (ssize_t i = 0; i + 2 < n; i++) {
        if (buf[i] != 0x06) continue;
        if (buf[i + 1] == 0x53 && buf[i + 2] == 0xFF) return ANALOG_FMT_V2;
        if (buf[i + 1] == 0x54 && buf[i + 2] == 0xFF) return ANALOG_FMT_V1;
    }
    return ANALOG_FMT_NONE;
}

/* Decode one raw report into samples; returns the number of live slots.
 * Slots that are entirely empty are skipped, but a key reporting zero is
 * kept - that's the one-shot release notification. */
static int analog_decode(int fmt, const unsigned char *buf, size_t len,
                         analog_sample_t *out, size_t max) {
    size_t n = 0;

    if (fmt == ANALOG_FMT_V2) {
        for (size_t i = 0; i + 4 <= len && n < max; i += 4) {
            unsigned key    = buf[i + 1];
            unsigned packed = buf[i + 2];
            unsigned value  = buf[i + 3];
            unsigned ns     = (packed >> 2) & 0x0F;
            unsigned lo     = (packed >> 6) & 0x03;
            unsigned v10    = (value << 2) | lo;   /* 10-bit, 0..1023 */
            if (!key && !v10) continue;            /* empty slot */
            out[n].code  = (uint16_t)((ns << 8) | key);
            out[n].depth = v10 >= 1023 ? 1.0f : (float)v10 / 1023.0f;
            n++;
        }
    } else if (fmt == ANALOG_FMT_V1) {
        for (size_t i = 0; i + 3 <= len && n < max; i += 3) {
            uint16_t code = (uint16_t)((buf[i] << 8) | buf[i + 1]);
            unsigned v    = buf[i + 2];
            if (!code) continue;                   /* empty slot */
            out[n].code  = code;
            out[n].depth = v >= 255 ? 1.0f : (float)v / 255.0f;
            n++;
        }
    }
    return (int)n;
}

/*
 * Analog front-end for one physical key: turns a stream of travel-depth
 * samples (mm) into the same press/release edges the digital evdev path
 * feeds the SOCD core.
 *
 * Two thresholds instead of one is the whole feature, but they answer
 * different questions. actuation_mm means "this is a keypress" and is the
 * only thing that decides whether an edge is emitted here. The backplate
 * (travel_mm - bottom_out_mm) means "this key is against the floor", and
 * this function uses it for one thing only: the bottom-out re-press rule.
 * Whether the PAIR of keys is on the floor is analog_deep_update's
 * business, and it reads the two depths directly rather than any latch
 * kept here - which is why this struct holds no commitment flag.
 *
 * `rt_extreme` is continuous across rapid-trigger edges: a rapid-trigger release
 * does not zero it, so a micro-reversal re-press measures its travel from
 * the right anchor rather than a stale actuation point. Since every path
 * that sets `live` also anchors `rt_extreme` to a real sample depth
 * (always > 0), "engaged but not yet fully released" reads straight off
 * `rt_extreme != 0.0f` with no extra flag - and rule 1 is the only place
 * that zeroes it.
 *
 * Ordering within one sample, which is what makes a contradictory
 * press+release pair for one key impossible:
 *
 *   1. Full release (depth < release_mm) is a master reset. It overrides
 *      everything else unconditionally - the key left the switch - emits
 *      the release if one is owed, and returns.
 *   2. Otherwise, if the key is still engaged (rt_extreme != 0) and rapid
 *      trigger is on, ONLY the reversal check runs. A sample that both
 *      reaches the backplate and constitutes a reversal thus resolves
 *      unambiguously as the reversal; an engaged key never re-runs the
 *      fresh-actuation logic.
 *   3. Otherwise (never actuated this engagement) the fresh-actuation
 *      check runs instead.
 *
 * Every branch that flips `live` is the branch that appends the matching
 * edge, so `live` cannot desynchronize from what was emitted - socd_apply
 * downstream assumes strict press/release alternation per key.
 */
typedef struct {
    int   live;        /* an edge has been emitted; key counts as pressed */
    float rt_extreme;  /* local extreme for rapid trigger, mm */
} analog_key_t;

typedef struct {
    analog_key_t key[POOL_MAX];   /* parallel to cfg->pool */
} analog_state_t;

/* Feed one sample for one key. idx is 0 (k1) or 1 (k2); a key absent from
 * the hardware report is fed depth_mm == 0.0f. Writes at most ONE edge to
 * out[] (1 == press, 0 == release) and returns the count: rule 1 returns
 * early and rules 2/3 are mutually exclusive, so no path emits two. The
 * array is kept at two so callers need not change if that ever does. */
static int analog_key_feed(analog_state_t *st, int idx, float depth_mm,
                           const analog_config_t *cfg, int riding, int *out) {
    analog_key_t *self = &st->key[idx];
    int           n    = 0;

    /* 0. Riding: the backplate IS the switch, and rapid trigger does not run
     * at all.
     *
     * This is the whole simplification. While the latch is armed the player
     * is bottoming out every stroke by definition - that is what riding is -
     * so the only threshold that means anything is the floor. Synthesizing
     * edges from reversal distances here was solving a problem the gesture
     * does not have, and it made the note land at a different depth
     * depending on how far the finger happened to travel.
     *
     * No hysteresis is needed and none is wanted: the backplate is a
     * physical stop, so a finger is either against it (reading full travel)
     * or off it. You cannot hover on a hard surface, which is the same
     * property that makes the latch itself aimable.
     *
     * `rt_extreme` is kept at the current depth purely so the handoff OUT of
     * riding has a sane anchor - see analog_deep_set, which clears `live` and
     * leaves this parked, putting the key in the "engaged but not live" state
     * the tapping profile understands. */
    if (riding) {
        int on = depth_mm >= cfg->travel_mm - cfg->bottom_out_mm;
        self->rt_extreme = depth_mm;
        if (on != self->live) {
            self->live = on;
            out[n++]   = on;
        }
        return n;
    }

    /* 1. Full release: master reset. The only place that zeroes
     * `rt_extreme`. */
    if (depth_mm < cfg->release_mm) {
        if (self->live)
            out[n++] = 0;
        self->live       = 0;
        self->rt_extreme = 0.0f;
        return n;
    }

    /* Only the tapping profile reaches here: rule 0 returned for every
     * riding sample, so there is one set of distances and nothing selects
     * between them. An earlier design ran a second "deep" profile here and
     * picked between the two by the latch; the backplate rule above replaces
     * it outright. */
    float press   = cfg->rt_press_mm;
    float release = cfg->rt_release_mm;

    /* Against the backplate: the re-press rule below. The same line is
     * what analog_deep_update tests for BOTH keys at once, but it reads
     * the depths itself - nothing about the pair is decided here. */
    int bottomed = depth_mm >= cfg->travel_mm - cfg->bottom_out_mm;

    if (cfg->rt_enabled && self->rt_extreme != 0.0f) {
        /* 2. Already engaged: rapid trigger owns this sample exclusively,
         * tracking the local extreme and firing on a large enough
         * reversal in either direction. A zero distance disables that
         * zone's edge outright - the key then holds until rule 1. */
        if (self->live) {
            if (depth_mm > self->rt_extreme) {
                self->rt_extreme = depth_mm;
            } else if (release > 0.0f &&
                       self->rt_extreme - depth_mm >= release) {
                out[n++]         = 0;
                self->live       = 0;
                self->rt_extreme = depth_mm;
            }
        } else {
            /* Bottoming out always presses, however small the down-travel
             * since the last reversal - the mirror of rule 1, where a full
             * release always releases however small the up-travel. Riding
             * the backplate as the neutral position depends on this: with a
             * large press distance, a stroke that ends against the bottom
             * never travels far enough to satisfy the reversal test on its
             * own. (While RIDING this no longer applies at all - rule 0
             * returns before any of this.) */
            /* A re-press still has to clear actuation_mm: without that
             * floor, a reversal entirely within the top fraction of travel
             * synthesizes whole keystrokes where the sensor is least
             * linear. */
            if (depth_mm < self->rt_extreme) {
                self->rt_extreme = depth_mm;
            } else if (depth_mm >= cfg->actuation_mm &&
                       (bottomed ||
                        (press > 0.0f &&
                         depth_mm - self->rt_extreme >= press))) {
                out[n++]         = 1;
                self->live       = 1;
                self->rt_extreme = depth_mm;
            }
        }
    } else if (!self->live && depth_mm >= cfg->actuation_mm) {
        /* 3. Fresh actuation. */
        self->live       = 1;
        self->rt_extreme = depth_mm;
        out[n++]         = 1;
    }

    return n;
}


/* Find the analog interface of an analog-capable keyboard. Returns the fd
 * (and fills path/fmt), or -1 when there's nothing to open. An explicit
 * path skips the vendor check but still has to look like an analog node. */
static int analog_open(const char *forced, char *path_out, size_t path_sz,
                       int *fmt_out, uint16_t *vid_out, uint16_t *pid_out) {
    if (forced) {
        const char *base = strrchr(forced, '/');
        char sysdir[PATH_MAX];
        snprintf(sysdir, sizeof(sysdir), "/sys/class/hidraw/%s",
                 base ? base + 1 : forced);
        int fmt = analog_probe_fmt(sysdir);
        if (fmt == ANALOG_FMT_NONE) {
            LOG_ERR("%s is not an analog interface "
                    "(no 0xFF53/0xFF54 usage page)", forced);
            return -1;
        }
        int fd = open(forced, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
        if (fd < 0) {
            LOG_ERR("open(%s): %s", forced, strerror(errno));
            return -1;
        }
        analog_read_ids(sysdir, vid_out, pid_out);
        snprintf(path_out, path_sz, "%s", forced);
        *fmt_out = fmt;
        return fd;
    }

    struct dirent **ents;
    int n = scandir("/sys/class/hidraw", &ents, NULL, alphasort);
    if (n < 0) return -1;

    int fd = -1;
    for (int i = 0; i < n; i++) {
        if (fd < 0 && strncmp(ents[i]->d_name, "hidraw", 6) == 0) {
            char     sysdir[PATH_MAX], dev[PATH_MAX];
            uint16_t vid = 0, pid = 0;

            snprintf(sysdir, sizeof(sysdir), "/sys/class/hidraw/%s",
                     ents[i]->d_name);
            if (analog_read_ids(sysdir, &vid, &pid) == 0 &&
                (vid == WOOTING_VID || vid == WOOTING_VID_LEGACY)) {
                int fmt = analog_probe_fmt(sysdir);
                if (fmt != ANALOG_FMT_NONE) {
                    snprintf(dev, sizeof(dev), "/dev/%s", ents[i]->d_name);
                    int f = open(dev, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
                    if (f < 0) {
                        LOG_WARN("open(%s): %s - analog unavailable "
                                 "(udev rules? see README)",
                                 dev, strerror(errno));
                    } else {
                        fd = f;
                        *fmt_out = fmt;
                        *vid_out = vid;
                        *pid_out = pid;
                        snprintf(path_out, path_sz, "%s", dev);
                    }
                }
            }
        }
        free(ents[i]);
    }
    free(ents);
    return fd;
}

/* hid-input stores a key's HID usage as its evdev scancode (usage page 0x07
 * for ordinary keyboard keys), so the keyboard itself carries the mapping
 * between config KEY_* codes and analog report usage ids. Walking its keymap
 * beats hardcoding a table: it stays correct across layouts and firmware. */
#define HID_PAGE_KEYBOARD 0x07

static int hid_map_build(int fd, uint16_t *usage_of_key) {
    int found = 0;
    for (unsigned idx = 0; idx < KEY_MAX * 2; idx++) {
        struct input_keymap_entry e;
        uint32_t sc;

        memset(&e, 0, sizeof(e));
        e.flags = INPUT_KEYMAP_BY_INDEX;
        e.index = idx;
        if (ioctl(fd, EVIOCGKEYCODE_V2, &e) < 0)
            break;                    /* EINVAL == walked off the end */
        if (e.len != sizeof(sc) || e.keycode > KEY_MAX)
            continue;
        memcpy(&sc, e.scancode, sizeof(sc));
        if ((sc >> 16) != HID_PAGE_KEYBOARD)
            continue;
        usage_of_key[e.keycode] = (uint16_t)(sc & 0xFFFF);
        found++;
    }
    return found;
}

/* Walk every keyboard-shaped evdev node until one yields a usable keymap.
 * Used both to resolve k1/k2 and to label codes in monitor mode. */
static int hid_map_from_any_keyboard(const char *input_dir,
                                     uint16_t *usage_of_key) {
    struct dirent **ents;
    int n = scandir(input_dir, &ents, is_event_node, alphasort);
    if (n < 0) return 0;

    int found = 0;
    for (int i = 0; i < n; i++) {
        if (!found) {
            char path[PATH_MAX];
            snprintf(path, sizeof(path), "%s/%s", input_dir, ents[i]->d_name);
            int fd = open(path, O_RDONLY | O_CLOEXEC);
            if (fd >= 0) {
                found = hid_map_build(fd, usage_of_key);
                close(fd);
            }
        }
        free(ents[i]);
    }
    free(ents);
    return found;
}

/* An open analog interface plus the SOCD state it drives. One per daemon:
 * the analog path owns k1/k2 outright, so a second analog board would only
 * fight the first over the same two virtual keys. */
typedef struct {
    int            kind;        /* EP_ANALOG */
    int            fd;
    int            fmt;
    uint16_t       vid, pid;
    uint16_t       usage[POOL_MAX];  /* HID usage id per pool index */
    int            n;                /* == cfg->n_pool */
    int            latch[2];         /* pool indices of the latch pair */
    char           path[PATH_MAX];
    analog_state_t keys;
    pool_state_t   pool;        /* slot layer + the SOCD core */
    int            deep;        /* 1 == both LATCH keys planted on the
                                 * backplate; they run the toggle instead of
                                 * the plain remap, and every other pool key
                                 * is silenced. See analog_deep_update. */
    int            last_press;  /* POOL INDEX of the most recent press edge.
                                 * Picks which virtual survives when the
                                 * latch arms with both already down - last
                                 * input wins. An index rather than a slot
                                 * because arming can move a key between
                                 * slots; analog_deep_set resolves it to a
                                 * slot after that. */
} analog_dev_t;

static analog_dev_t *analog_dev_open(const oid_config_t *cfg,
                                     const char *input_dir) {
    analog_dev_t *ad = calloc(1, sizeof(*ad));
    if (!ad) {
        LOG_ERR("oom");
        return NULL;
    }
    ad->kind = EP_ANALOG;
    ad->fd   = analog_open(cfg->analog.device, ad->path, sizeof(ad->path),
                           &ad->fmt, &ad->vid, &ad->pid);
    if (ad->fd < 0) {
        free(ad);
        return NULL;
    }

    ad->n        = cfg->n_pool;
    ad->latch[0] = cfg->latch[0];
    ad->latch[1] = cfg->latch[1];
    pool_init(&ad->pool, ad->n);

    /* Resolve EVERY pool key to a HID usage id: an explicit config override
     * wins, otherwise walk a keyboard's keymap for the mapping. All of them,
     * not just the latch pair - in analog mode the whole pool is read by
     * travel, so that the daemon's own rapid trigger governs every key
     * rather than half of them running the firmware's actuation instead.
     * Two different feels inside one burst is the same jitter the deep latch
     * exists to avoid. */
    static uint16_t usage_of_key[KEY_MAX + 1];
    int need_map = 0;
    for (int i = 0; i < ad->n; i++)
        if (cfg->analog.hid[i] < 0) need_map = 1;
    if (need_map)
        hid_map_from_any_keyboard(input_dir, usage_of_key);

    for (int i = 0; i < ad->n; i++) {
        ad->usage[i] = cfg->analog.hid[i] >= 0
                       ? (uint16_t)cfg->analog.hid[i]
                       : usage_of_key[cfg->pool[i]];
        if (!ad->usage[i]) {
            LOG_ERR("could not resolve %s to a HID usage id - run with -A to "
                    "find it, then set it in analog.hid",
                    key_name_or(cfg->pool[i]));
            close(ad->fd);
            free(ad);
            return NULL;
        }
        for (int j = 0; j < i; j++) {
            if (ad->usage[j] == ad->usage[i]) {
                LOG_ERR("%s and %s both resolve to HID usage 0x%02x - one of "
                        "them needs an explicit analog.hid entry",
                        key_name_or(cfg->pool[j]), key_name_or(cfg->pool[i]),
                        ad->usage[i]);
                close(ad->fd);
                free(ad);
                return NULL;
            }
        }
    }

    g_analog_vid = ad->vid;
    g_analog_pid = ad->pid;

    char keydesc[POOL_MAX * 28];
    size_t ko = 0;
    for (int i = 0; i < ad->n && ko < sizeof(keydesc); i++)
        ko += (size_t)snprintf(keydesc + ko, sizeof(keydesc) - ko,
                               "%s%s->0x%02x%s", i ? ", " : "",
                               key_name_or(cfg->pool[i]), ad->usage[i],
                               (i == ad->latch[0] || i == ad->latch[1])
                               ? "*" : "");
    LOG_INFO("Analog input on %s (%s, %04x:%04x): %s  (* = deep latch pair)",
             ad->path, ad->fmt == ANALOG_FMT_V2 ? "v2" : "v1",
             ad->vid, ad->pid, keydesc);
    LOG_INFO("Analog thresholds: actuation %.2fmm, release %.2fmm "
             "(derived), backplate at %.2fmm, rapid trigger %s",
             (double)cfg->analog.actuation_mm, (double)cfg->analog.release_mm,
             (double)(cfg->analog.travel_mm - cfg->analog.bottom_out_mm),
             cfg->analog.rt_enabled ? "on" : "off");
    LOG_INFO("SOCD engages while both keys are planted on the backplate "
             "(deep latch); while engaged the backplate is the switch and "
             "rapid trigger does not run; disengages when BOTH keys leave "
             "the backplate - one finger lifting is a stroke, not an exit");
    if (cfg->analog.rt_enabled) {
        LOG_INFO("Rapid trigger (tapping only): press %.2fmm release %.2fmm"
                 " - while riding, the backplate is the switch",
                 (double)cfg->analog.rt_press_mm,
                 (double)cfg->analog.rt_release_mm);
    }
    return ad;
}

/* Bring an analog_dev_t up as a bare two-key pool, with no hidraw node
 * behind it. tools/replay.c and tools/regimetest.c drive the analog
 * front-end directly and need exactly this much of analog_dev_open.
 *
 * It is not a memset: pool_init seeds each key's slot by index, and a
 * zeroed pool_state_t would put both keys in slot 0, so the second key's
 * press would come out as v1. */
__attribute__((unused))
static void analog_dev_stub(analog_dev_t *ad) {
    memset(ad, 0, sizeof(*ad));
    ad->kind     = EP_ANALOG;
    ad->fd       = -1;
    ad->n        = 2;
    ad->latch[0] = 0;
    ad->latch[1] = 1;
    pool_init(&ad->pool, ad->n);
}

static void analog_dev_close(analog_dev_t *ad) {
    if (!ad) return;
    if (ad->fd >= 0) close(ad->fd);
    free(ad);
}

/* -A: dump live travel depth so thresholds can be picked against the real
 * board, and so a key's usage id can be discovered when the keymap walk
 * comes up empty. Deliberately touches nothing else - no grab, no uinput,
 * no audio - so it is safe to run alongside a live daemon. */
/* -T: dump the travel of k1/k2 to stdout, one line per hardware report,
 * so a play session can be replayed through the state machine offline.
 *
 * Passive, exactly like -A: this opens the hidraw node and resolves the
 * two HID usages, and does nothing else. No EVIOCGRAB, no uinput device,
 * nothing in the input path - whoever is recording plays on their own
 * setup, with their own keyboard behaving normally. That is what makes it
 * reasonable to ask someone else to run it.
 *
 * Both keys are emitted on every line even when only one moved, because a
 * released key vanishes from the report rather than reporting a zero, so
 * the absent one really is at rest. Timestamps are monotonic microseconds
 * from the first report. */
static int analog_trace(const oid_config_t *cfg, const char *input_dir) {
    analog_dev_t *ad = analog_dev_open(cfg, input_dir);
    if (!ad)
        return EXIT_FAILURE;

    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = on_signal;
    sigaction(SIGINT,  &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);

    printf("# doubletapd trace v1\n");
    printf("# travel_mm=%.4f\n", (double)cfg->analog.travel_mm);
    printf("us,k1_mm,k2_mm\n");
    fflush(stdout);

    LOG_INFO("Tracing k1/k2 travel to stdout. Play normally; Ctrl-C to stop.");

    struct timespec t0;
    int             have_t0 = 0;
    unsigned long   lines   = 0;

    while (g_running) {
        struct pollfd pfd = { .fd = ad->fd, .events = POLLIN };
        int pr = poll(&pfd, 1, 200);
        if (pr < 0) {
            if (errno == EINTR) continue;
            break;
        }
        if (pr == 0) continue;

        unsigned char buf[ANALOG_V2_REPORT];
        ssize_t       n = read(ad->fd, buf, sizeof(buf));
        if (n < 0) {
            if (errno == EAGAIN || errno == EINTR) continue;
            LOG_ERR("read(%s): %s", ad->path, strerror(errno));
            break;
        }

        struct timespec now;
        clock_gettime(CLOCK_MONOTONIC, &now);
        if (!have_t0) { t0 = now; have_t0 = 1; }
        long long us = (long long)(now.tv_sec - t0.tv_sec) * 1000000
                     + (now.tv_nsec - t0.tv_nsec) / 1000;

        analog_sample_t s[ANALOG_SLOTS];
        int             ns = analog_decode(ad->fmt, buf, (size_t)n, s,
                                           ANALOG_SLOTS);
        /* The LATCH PAIR only, whatever the pool's size. The trace format is
         * `us,k1_mm,k2_mm` and every recorded trace under tests/ is in it; widening
         * this would invalidate the corpus to record keys that cannot arm
         * the latch anyway. */
        float depth[2] = { 0.0f, 0.0f };
        for (int i = 0; i < ns; i++)
            for (int k = 0; k < 2; k++)
                if (s[i].code == ad->usage[ad->latch[k]])
                    depth[k] = s[i].depth * cfg->analog.travel_mm;

        printf("%lld,%.4f,%.4f\n", us, (double)depth[0], (double)depth[1]);
        lines++;
    }

    fflush(stdout);
    LOG_INFO("Traced %lu reports.", lines);
    analog_dev_close(ad);
    return EXIT_SUCCESS;
}

static int analog_monitor(const char *forced, const char *input_dir,
                          float travel_mm) {
    char     path[PATH_MAX];
    int      fmt = ANALOG_FMT_NONE;
    uint16_t vid = 0, pid = 0;

    int fd = analog_open(forced, path, sizeof(path), &fmt, &vid, &pid);
    if (fd < 0) {
        LOG_ERR("no analog interface found%s",
                forced ? "" : " (is an analog keyboard connected?)");
        return EXIT_FAILURE;
    }

    static uint16_t usage_of_key[KEY_MAX + 1];
    char            name_of_usage[256][32];

    memset(name_of_usage, 0, sizeof(name_of_usage));
    hid_map_from_any_keyboard(input_dir, usage_of_key);
    for (int code = 1; code <= KEY_MAX; code++) {
        uint16_t u = usage_of_key[code];
        if (!u || u >= 256 || name_of_usage[u][0])
            continue;
        const char *nm = libevdev_event_code_get_name(EV_KEY, code);
        if (nm)
            snprintf(name_of_usage[u], sizeof(name_of_usage[u]), "%s", nm);
    }

    LOG_INFO("Analog monitor on %s (%s, %04x:%04x), travel %.1fmm",
             path, fmt == ANALOG_FMT_V2 ? "v2" : "v1", vid, pid, travel_mm);
    LOG_INFO("Press keys to see live depth; peak is reported on release. "
             "Ctrl-C to exit.");

    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = on_signal;
    sigaction(SIGINT,  &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);

    float peak[256], cur[256];
    memset(peak, 0, sizeof(peak));
    memset(cur,  0, sizeof(cur));

    while (g_running) {
        struct pollfd pfd = { .fd = fd, .events = POLLIN };
        int pr = poll(&pfd, 1, 200);
        if (pr < 0) {
            if (errno == EINTR) continue;
            break;
        }
        if (pr == 0) continue;

        unsigned char buf[ANALOG_V2_REPORT];
        ssize_t       n = read(fd, buf, sizeof(buf));
        if (n < 0) {
            if (errno == EAGAIN || errno == EINTR) continue;
            LOG_ERR("read(%s): %s", path, strerror(errno));
            break;
        }

        analog_sample_t s[ANALOG_SLOTS];
        int             ns = analog_decode(fmt, buf, (size_t)n, s, ANALOG_SLOTS);

        /* A released key drops out of the report entirely on at least some
         * firmware, rather than reporting a final zero - so rest is inferred
         * from absence, by diffing this frame against the last. */
        float now[256];
        memset(now, 0, sizeof(now));
        for (int i = 0; i < ns; i++)
            if (s[i].code < 256)
                now[s[i].code] = s[i].depth;

        for (int c = 0; c < 256; c++) {
            if (now[c] > peak[c])
                peak[c] = now[c];
            if (now[c] > 0.0f || cur[c] <= 0.0f)
                continue;
            /* was down last frame, absent now: report and clear the peak */
            const char *nm = name_of_usage[c][0] ? name_of_usage[c] : "?";
            printf("\33[2K\r  %-14s hid 0x%02x   peak %5.2f mm  (%.3f)\n",
                   nm, c, peak[c] * travel_mm, peak[c]);
            peak[c] = 0.0f;
        }
        memcpy(cur, now, sizeof(cur));

        /* Live line: everything currently off its rest position. */
        char line[512];
        int  off = 0, shown = 0;
        for (int i = 0; i < ns && shown < 4; i++) {
            if (s[i].depth <= 0.0f || s[i].code >= 256) continue;
            const char *nm = name_of_usage[s[i].code][0]
                           ? name_of_usage[s[i].code] : "?";
            off += snprintf(line + off, sizeof(line) - (size_t)off,
                            "  %s 0x%02x %5.2fmm", nm, s[i].code,
                            s[i].depth * travel_mm);
            shown++;
            if (off >= (int)sizeof(line)) break;
        }
        printf("\33[2K\r%s", shown ? line : "  (all keys at rest)");
        fflush(stdout);
    }

    printf("\33[2K\r");
    fflush(stdout);
    close(fd);
    return EXIT_SUCCESS;
}

/* ------------------------------------------------------------------------- */
/* Virtual uinput device                                                     */
/* ------------------------------------------------------------------------- */

static struct libevdev         *vdev  = NULL;
static struct libevdev_uinput  *uidev = NULL;

/* Virtual keyboard with a fixed keyboard-wide key set plus configured
 * v1/v2. uinput capabilities are immutable after creation and hotplugged
 * keyboards may carry codes a startup-time union wouldn't have, so every
 * KEY_* code is enabled up front; the BTN_* pointer/gamepad ranges are
 * skipped so desktops keep classifying the device as a keyboard.
 * EV_SYN always enabled. Not cloning EV_REL,EV_ABS,etc */
static int build_virtual(const oid_config_t *cfg) {
    vdev = libevdev_new();
    if (!vdev) {
        LOG_ERR("libevdev_new failed");
        return -1;
    }
    libevdev_set_name(vdev, cfg->uinput_name);
    libevdev_set_uniq(vdev, "doubletap");
    libevdev_set_id_bustype(vdev, BUS_VIRTUAL);
    libevdev_set_id_vendor (vdev, 0x0001);
    libevdev_set_id_product(vdev, 0x0001);
    libevdev_set_id_version(vdev, 0x0001);

    libevdev_enable_event_type(vdev, EV_SYN);
    libevdev_enable_event_code(vdev, EV_SYN, SYN_REPORT,    NULL);
    libevdev_enable_event_code(vdev, EV_SYN, SYN_MT_REPORT, NULL);

    libevdev_enable_event_type(vdev, EV_KEY);
    for (unsigned int code = 1; code < KEY_MAX; code++) {
        if (code >= BTN_MISC && code < KEY_OK)
            continue; /* mouse/joystick/gamepad/digitizer buttons */
        if (code >= BTN_DPAD_UP && code <= BTN_DPAD_RIGHT)
            continue;
        if (code >= BTN_TRIGGER_HAPPY)
            continue;
        libevdev_enable_event_code(vdev, EV_KEY, code, NULL);
    }
    libevdev_enable_event_code(vdev, EV_KEY, cfg->v1, NULL);
    libevdev_enable_event_code(vdev, EV_KEY, cfg->v2, NULL);

    int rc = libevdev_uinput_create_from_device(
        vdev, LIBEVDEV_UINPUT_OPEN_MANAGED, &uidev);
    if (rc < 0) {
        LOG_ERR("libevdev_uinput_create_from_device: %s", strerror(-rc));
        libevdev_free(vdev);
        vdev = NULL;
        return -1;
    }

    const char *node = libevdev_uinput_get_devnode(uidev);
    LOG_INFO("Created virtual keyboard \"%s\" at %s",
             cfg->uinput_name, node ? node : "(unknown)");
    return 0;
}

static void destroy_virtual(void) {
    if (uidev) { libevdev_uinput_destroy(uidev); uidev = NULL; }
    if (vdev)  { libevdev_free(vdev);            vdev  = NULL; }
}

/* ------------------------------------------------------------------------- */
/* Radio button state machine                                                */
/* ------------------------------------------------------------------------- */

/*
 * Returns 1 if a virtual key-down was emitted (so the caller can trigger
 * audio), 0 otherwise.
 *
 *   state = k1 + k2 + ie.value   (k1, k2 post-update)
 *
 *     0 = NONE     last key released
 *     1 = RELEASE  two keys held -> one released
 *     2 = SINGLE   first key pressed (zero -> one)
 *     3 = PRESS    one key held -> second pressed (one -> two)
 *
 * RELEASE depends on cfg->socd: SOCD_TOGGLE reverts to the other key no
 * matter which of the two was released (latching), SOCD_SNAPPY ("last
 * input wins") only falls through to the still-held key when the active
 * key is the one released. SOCD_OFF skips the state machine entirely:
 * k1/k2 are remapped to v1/v2 one-to-one, nothing else.
 */
enum { S_NONE = 0, S_RELEASE = 1, S_SINGLE = 2, S_PRESS = 3 };

/* The SOCD core. Takes a synthesized (is_k1, value) edge rather than a raw
 * evdev event so that both the evdev path and the analog path can drive it:
 * process_event() below feeds it real key events, analog_drive() feeds it
 * edges derived from travel depth. The transition logic is identical either
 * way, which is the point. */
static int socd_apply(socd_state_t *in, int slot, int value, int mode,
                      const oid_config_t *cfg) {
    int is_k1 = (slot == 0);
    in->mode = mode;

    if (mode == SOCD_OFF) {
        /* Slot occupancy still tracked so release_stuck can clean up a
         * dying device. Each slot maps 1:1 to its own voice; both may sound
         * if both are pressed (they play on independent streams).
         * Autorepeat is deliberately forwarded here - see the value == 2
         * bail below, which applies only to the state machine. */
        if (value != 2) {
            if (is_k1) in->k1 = value;
            else       in->k2 = value;
        }
        libevdev_uinput_write_event(uidev, EV_KEY,
                                    is_k1 ? cfg->v1 : cfg->v2, value);
        libevdev_uinput_write_event(uidev, EV_SYN, SYN_REPORT, 0);
        if (value != 1)
            return VOICE_NONE;
        return is_k1 ? VOICE_V1 : VOICE_V2;
    }

    if (value == 2) /* autorepeat messes up state*/
        return VOICE_NONE;

    if (is_k1) in->k1 = value;
    else       in->k2 = value;

    int state = in->k1 + in->k2 + value;
    int triggered = VOICE_NONE;

    switch (state) {
        case S_SINGLE: {
        int code = is_k1 ? cfg->v1 : cfg->v2;
        in->act = is_k1;
        libevdev_uinput_write_event(uidev, EV_KEY, code, 1);
        libevdev_uinput_write_event(uidev, EV_SYN, SYN_REPORT, 0);
        triggered = is_k1 ? VOICE_V1 : VOICE_V2;
        break;
        }
        case S_RELEASE:
        /* Snappy mode: releasing the non-active (already suppressed) key
         * changes nothing; only the active key's release falls through to
         * the still-held one. Toggle mode reverts on either release. */
        if (mode == SOCD_SNAPPY && is_k1 != in->act)
            break;
        /* fallthrough */
        case S_PRESS: {
        int up_code   = in->act ? cfg->v1 : cfg->v2;
        int down_code = in->act ? cfg->v2 : cfg->v1;
        libevdev_uinput_write_event(uidev, EV_KEY, up_code, 0);
        libevdev_uinput_write_event(uidev, EV_SYN, SYN_REPORT, 0);
        libevdev_uinput_write_event(uidev, EV_KEY, down_code, 1);
        libevdev_uinput_write_event(uidev, EV_SYN, SYN_REPORT, 0);
        /* down_code is the newly-pressed key: v2 when act was 1, else v1 */
        triggered = in->act ? VOICE_V2 : VOICE_V1;
        in->act = !in->act;
        break;
        }
        case S_NONE: {
        int up_code = in->act ? cfg->v1 : cfg->v2;
        libevdev_uinput_write_event(uidev, EV_KEY, up_code, 0);
        libevdev_uinput_write_event(uidev, EV_SYN, SYN_REPORT, 0);
        in->act = 0;
        break;
        }
        default:
        /* shouldn't make it here w/ sane inputs. */
        break;
    }

    return triggered;
}

/* Feed one physical key edge for pool index `i` through the slot layer.
 *
 * Only 0<->1 occupancy crossings reach the core, which is what lets a slot
 * hold more than one key without inventing edges: the second key to arrive
 * in a slot is already represented there, and its release is not a release
 * of the slot.
 *
 * Autorepeat (value == 2) is passed straight through on the key's recorded
 * slot without touching the counts. That is not a no-op: SOCD_OFF forwards
 * autorepeat to v1/v2 verbatim, and always has. Filtering it here would
 * quietly change `off` mode. The state-machine modes drop it inside
 * socd_apply, exactly as before. */
static int pool_feed(pool_state_t *p, int i, int value, int mode,
                     const oid_config_t *cfg) {
    if (value == 2) {
        if (!p->key[i].held) return VOICE_NONE;
        return socd_apply(&p->socd, p->key[i].slot, 2, mode, cfg);
    }

    if (value) {
        if (p->key[i].held) return VOICE_NONE;   /* duplicate press */
        int alternating = (cfg->socd == SOCD_OFF && cfg->n_pool > 2);
        int s = pool_pick_slot(p, i, alternating);
        p->key[i].held = 1;
        p->key[i].slot = s;
        p->last_slot   = s;
        p->count[s]++;
        /* A press ALWAYS reaches the core, even when it joins a slot that
         * was already occupied - which happens only when both slots were
         * taken, since pool_pick_slot prefers a free one. That is the third
         * simultaneous key, and it must still be worth a note: socd_apply
         * reads slot occupancy as already-1 and this as a second key going
         * down, which is S_PRESS, so the toggle alternates exactly as it
         * would for any other press. Gating this on the 0<->1 crossing the
         * way releases are gated would silently swallow it. */
        return socd_apply(&p->socd, s, 1, mode, cfg);
    }

    /* A release for a key we never saw press - a SYNC drop, or a key held
     * across the grab - has no slot to act on. Drop it rather than guess:
     * guessing writes an up for a virtual nobody pressed. */
    if (!p->key[i].held) return VOICE_NONE;
    p->key[i].held = 0;
    int s = p->key[i].slot;
    /* Releases ARE gated on the crossing, unlike presses. A slot with a key
     * still in it has not been released, and writing an up for it would
     * kill a hold the player is still making. */
    if (--p->count[s] != 0)
        return VOICE_NONE;
    return socd_apply(&p->socd, s, 0, mode, cfg);
}

/* evdev entry point: mirror everything that isn't a pool key, and hand pool
 * keys to the slot layer. In analog mode this device's pool keys are driven
 * from travel depth instead, so the digital copies are dropped here to
 * avoid emitting each press twice. */
static int process_event(input_dev_t *in, const struct input_event *ie,
                         const oid_config_t *cfg) {
    if (ie->type == EV_MSC && ie->code == MSC_SCAN)
        return VOICE_NONE;

    int i = ie->type == EV_KEY ? pool_index(cfg, ie->code) : -1;
    if (i < 0) {
        libevdev_uinput_write_event(uidev, ie->type, ie->code, ie->value);
        return VOICE_NONE;
    }

    if (in->analog)
        return VOICE_NONE;

    return pool_feed(&in->pool, i, ie->value, cfg->socd, cfg);
}

/* ------------------------------------------------------------------------- */
/* loop                                                                      */
/* ------------------------------------------------------------------------- */

/* drain everything libevdev has buffered, looping over SYNC drops
 * as needed. return 0 on normal EAGAIN, -1 on fatal error. */
static int drain_device(input_dev_t *in, const oid_config_t *cfg) {
    unsigned int flag = LIBEVDEV_READ_FLAG_NORMAL;
    for (;;) {
        struct input_event ie;
        int rc = libevdev_next_event(in->dev, flag, &ie);
        if (rc == -EAGAIN)
            break;
        if (rc == LIBEVDEV_READ_STATUS_SYNC) {
            flag = LIBEVDEV_READ_FLAG_SYNC;
            continue;
        }
        if (rc != LIBEVDEV_READ_STATUS_SUCCESS) {
            LOG_WARN("libevdev_next_event(%s): %s",
                     in->path, strerror(-rc));
            return -1;
        }
        flag = LIBEVDEV_READ_FLAG_NORMAL;
        /* Grab still deferred: the system receives these events directly
         * through the ungrabbed device, so mirroring or filtering them
         * would double them up - just let libevdev track the key state. */
        if (in->grabbed) {
            int voice = process_event(in, &ie, cfg);
            if (voice != VOICE_NONE)
                audio_trigger(voice - 1); /* VOICE_V1/V2 -> voice index 0/1 */
        }
    }

    if (!in->grabbed && !any_key_down(in->dev)) {
        int rc = libevdev_grab(in->dev, LIBEVDEV_GRAB);
        if (rc < 0) {
            LOG_WARN("libevdev_grab(%s): %s", in->path, strerror(-rc));
            return -1; /* dropped; inotify-driven reconcile retries later */
        }
        in->grabbed = 1;
        LOG_INFO("All keys released - grabbed %s", in->path);
    }
    return 0;
}

/* Which virtual keys `mode` would be holding down, given which physical
 * keys `held` says are down. SOCD_OFF mirrors one-to-one; the toggle holds
 * exactly one - the `act` one - whenever anything at all is held. Used to
 * diff the two modes against each other when the `deep` latch changes
 * under held fingers. */
static void socd_virtual_state(const int *held, int act, int mode,
                               int *down) {
    down[0] = down[1] = 0;
    if (mode == SOCD_OFF) {
        down[0] = held[0];
        down[1] = held[1];
    } else if (held[0] || held[1]) {
        down[act ? 0 : 1] = 1;
    }
}

/* Arm or disarm the `deep` latch, reconciling the virtual keys as we go.
 *
 * The two modes disagree about what should be held: OFF wants a virtual key
 * per held physical key, the toggle wants exactly one. Flipping the flag
 * without settling that difference is what strands a key - the toggle can
 * legitimately leave `act` on a finger that has already lifted, and a later
 * release routed through OFF writes only its own code, so whatever the
 * toggle pressed is never released. So compute both pictures and emit the
 * difference.
 *
 * That matters MORE than it did when the latch cleared at release_mm:
 * disarming at actuation_mm means the mode can flip with a finger still
 * resting well down on a key, so a mode change under held fingers is
 * routine rather than a corner.
 *
 * Arming with both keys already down - which is always, since arming means
 * both are on the floor and the plain remap has pressed both - means one of
 * them has to let go. The most recently pressed key keeps it: last input
 * wins, the same principle the core runs on. Which one keeps it does NOT
 * decide `act`; see analog_socd_edge. */
static void analog_deep_set(analog_dev_t *ad, int on,
                            const oid_config_t *cfg) {
    /* Where the virtual keys stand comes from socd, which reflects the
     * edges already APPLIED. Where they should end up comes from the
     * front-end's `live`, which is this sample's truth - socd still lags it
     * by whatever edges have not been routed yet. Targeting socd instead
     * would press a key for a finger that left on this very sample, then
     * release it again when the edge lands: a phantom note on the way out
     * of a rock. */
    int cur[2] = { ad->pool.socd.k1, ad->pool.socd.k2 };
    int from[2], to[2];

    socd_virtual_state(cur, ad->pool.socd.act,
                       ad->deep ? cfg->socd : SOCD_OFF, from);

    if (on) {
        /* EVICT every non-latch pool key from the slot bookkeeping before
         * computing anything. Parking their front-ends is not enough, and
         * both failures are silent.
         *
         * It would BLIND THE TOGGLE. A non-latch key sharing a latch key's
         * slot leaves that slot's count at 2. When the latch key then lifts
         * off the backplate the count goes 2->1, there is no 0<->1
         * crossing, and socd_apply never sees the release - the ride emits
         * nothing at all.
         *
         * It would also STRAND THE KEY: `held` would stay set for a key
         * nothing is feeding, so its first press after disarm is swallowed
         * as a duplicate.
         *
         * `slot` is deliberately left alone, so stickiness survives the
         * armed window. With a two-key pool there are no non-latch keys and
         * this whole loop is a no-op. */
        for (int k = 0; k < ad->n; k++) {
            if (k == ad->latch[0] || k == ad->latch[1]) continue;
            if (ad->pool.key[k].held) {
                ad->pool.key[k].held = 0;
                ad->pool.count[ad->pool.key[k].slot]--;
            }
            ad->keys.key[k].live = 0;
        }

        /* CANONICALISE the latch pair onto one slot each. They can arrive
         * sharing one: press a non-latch key second and it takes the free
         * slot, so the second latch key finds both held and shares off
         * `!last_slot`. Evicting the interloper then leaves both latch keys
         * in one slot and the other empty - and from there every backplate
         * crossing for the rest of the rock is a 2<->1 move inside that one
         * slot. No 0<->1 crossing means socd_apply is never called, and the
         * ride is dead until the latch disarms.
         *
         * Safe to do here because eviction has already run, so the only
         * held keys are these two and the picture is fully determined.
         * `from[]` was captured before any of this, so whatever moves comes
         * out in the from->to difference below. A two-key pool can never
         * reach this - two keys never share a slot - so the corpus is
         * untouched. regimetest case R. */
        int a = ad->latch[0], b = ad->latch[1];
        if (ad->pool.key[a].slot == ad->pool.key[b].slot) {
            int was = ad->pool.key[b].slot;
            ad->pool.key[b].slot = !was;
            if (ad->pool.key[b].held) {
                ad->pool.count[was]--;
                ad->pool.count[!was]++;
            }
        }

        ad->pool.socd.k1 = ad->pool.count[0] > 0;
        ad->pool.socd.k2 = ad->pool.count[1] > 0;
    }

    /* Where the virtual keys should END UP. Slot occupancy after the
     * transition, built from the front-end's `live` - this sample's truth -
     * rather than from socd, which lags it by whatever edges have not been
     * routed yet. Only the latch pair survives arming; nothing survives
     * disarming. */
    int next[2] = { 0, 0 };
    if (on) {
        for (int i = 0; i < 2; i++)
            if (ad->keys.key[ad->latch[i]].live)
                next[ad->pool.key[ad->latch[i]].slot] = 1;
    }

    if (on) {
        /* Resolved to a slot HERE, not when the press was recorded: the
         * canonicalisation above may have just moved the key to the other
         * slot, and `keep` has to name where it actually ended up. When the
         * last press was an evicted non-latch key its slot still names one
         * of the two, which is arbitrary but deterministic - the "last
         * input wins" rule has nothing to say about a key that is no longer
         * in the picture. */
        int keep = ad->pool.key[ad->last_press].slot;
        if (!next[keep]) keep = !keep;      /* it lifted; the other has it */
        ad->pool.socd.act = (keep == 0);
    } else {
        /* PARK both front ends on the way out. Disarming means neither key
         * is on the floor, so nothing is held - but a key can still be well
         * down (it need only have left the backplate), and the tapping
         * profile is about to judge it against actuation_mm instead.
         *
         * Clearing `live` alone would let rule 3 read that depth as a FRESH
         * actuation and press a finger that is on its way up - a phantom
         * note landing exactly where a slider ends. Leaving `live` set is no
         * better: its eventual release emits an up with no matching down,
         * and socd_apply assumes strict alternation per key.
         *
         * `live = 0` with `rt_extreme` still holding the current depth is
         * neither: it is rule 2's "engaged but not live" state, where the
         * key tracks its extreme on the way up, emits nothing, and re-presses
         * only on a real stroke - a bottom-out, or press_mm of down-travel.
         * analog_key_feed's riding branch leaves rt_extreme parked for
         * exactly this handoff.
         *
         * EVERY pool key is parked, not just the latch pair. The non-latch
         * keys were evicted and left unfed for the whole armed window, so
         * their `rt_extreme` is stale by however long the rock lasted;
         * analog_drain re-anchors it to the current sample as it resumes
         * feeding them. Parked is the state that emits nothing while
         * rising and re-presses only on a real stroke, which is exactly
         * what a key that was silenced mid-flight needs. */
        for (int k = 0; k < ad->n; k++)
            ad->keys.key[k].live = 0;
        next[0] = next[1] = 0;
    }
    ad->deep = on;
    socd_virtual_state(next, ad->pool.socd.act, on ? cfg->socd : SOCD_OFF, to);

    /* Emit the difference. BOTH directions are release-only now, which is
     * what the new disarm rule buys:
     *
     * ARMING goes from OFF's two keys down to the toggle's one. Planting is
     * not a note, and both keys already sounded under OFF when they bottomed.
     *
     * DISARMING goes from the toggle's one to none. The old rule could fire
     * with the other finger still planted deep, so it had to PRESS to avoid
     * stranding that hold - at the cost of a second note for one lift. Under
     * "both off the floor" there is by definition nobody still riding, so
     * there is no survivor to strand and nothing to press. The parked front
     * ends above pick the keys back up on their next real stroke.
     *
     * The audio_trigger call that used to sit here went with it: nothing
     * here presses, so nothing here clicks. */
    for (int k = 0; k < 2; k++) {
        if (from[k] == to[k]) continue;
        libevdev_uinput_write_event(uidev, EV_KEY,
                                    k == 0 ? cfg->v1 : cfg->v2, to[k]);
        libevdev_uinput_write_event(uidev, EV_SYN, SYN_REPORT, 0);
    }
}

/* Resolve the `deep` latch for this sample, before the sample's edges are
 * applied.
 *
 * This is the whole discriminator between the two play patterns, and both
 * halves are pure depth tests on the CURRENT frame - no per-key latch, no
 * comparison of one key's depth against the other's. That last point is not
 * an accident: every abandoned version of this feature gated on relative
 * depth, trying to decide whether a shallow press "deserved" to steal from a
 * deep one, and that is not a problem that exists. Do not reintroduce it.
 *
 * Deciding before the edges is what stops the lift that ends a rock from
 * being reverted by a toggle on its way out: the release then lands as a
 * plain release, over virtual keys the reconciliation has already put where
 * SOCD_OFF expects them. */
static void analog_deep_update(analog_dev_t *ad, const oid_config_t *cfg,
                               const float *depth) {
    const analog_config_t *a = &cfg->analog;

    float floor_mm = a->travel_mm - a->bottom_out_mm;

    /* The latch pair, and only ever a pair: whatever the pool's size, this
     * stays the same two-key backplate test it has always been. Alt-tapping
     * cannot satisfy it, no depth is compared against another, and nothing
     * about the wider pool enters here. */
    float d0 = depth[ad->latch[0]], d1 = depth[ad->latch[1]];

    if (ad->deep) {
        /* BOTH fingers off the floor ends it. One key leaving is a stroke,
         * not an exit - which is the whole point, and why the earlier rule
         * (either key back past actuation_mm) was wrong. In a burst a finger
         * routinely lifts clear of the switch while the other stays planted;
         * that tore the latch down mid-gesture, and the next note then fired
         * off the tapping profile at actuation instead of at the backplate.
         * Measured on tests/toggle_5burst_3phys_5virt.csv: k2 sat at 3.5000
         * throughout while k1 lifted to 0.0000, and note 4 landed 11.9ms
         * early at 0.25mm instead of at 3.44mm. Two thresholds inside one
         * burst is jitter, and "how completely you lift" is exactly the kind
         * of amplitude-driven rule this design already rejects elsewhere. */
        if (d0 < floor_mm && d1 < floor_mm) {
            LOG_INFO("Deep latch cleared: k1 %.2fmm, k2 %.2fmm - both off "
                     "the backplate (%.2fmm), back to the plain remap",
                     (double)d0, (double)d1, (double)floor_mm);
            analog_deep_set(ad, 0, cfg);
        }
        return;
    }

    /* Both against the backplate in ONE sample.
     *
     * Alt-tapping cannot satisfy this. Its defining shape is one finger
     * leaving as the other arrives, so the two are never both on the floor
     * together, however fast it is played and however much the presses
     * overlap in the middle of travel. Rocking, by contrast, BEGINS by
     * planting both - you cannot start one any other way. That asymmetry is
     * the entire signal, and unlike a depth in the middle of travel the
     * backplate is a hard physical stop: you can hit it without
     * proprioception, and "plant both keys to start rocking" is an
     * instruction a player can actually follow.
     *
     * Hysteresis is enormous by construction - on the defaults this arms at
     * 3.80mm and disarms at 1.00mm - so there is no chatter and no need for
     * a debounce counter. "One consecutive sample" IS the instantaneous
     * test; if a longer confirmation is ever wanted it wraps this test and
     * nothing else moves. */
    if (d0 >= floor_mm && d1 >= floor_mm) {
        /* Logged because arming is otherwise INVISIBLE: it is release-only
         * by construction, it never clicks, and the first dip out of it
         * reverts through two writes the kernel drops. With no trace of it
         * anywhere, a latch that never arms and a latch that arms and works
         * look identical from outside - which is exactly the question you
         * are asking when the daemon seems stuck on the plain remap.
         * Transitions are once per rock, not per report. */
        LOG_INFO("Deep latch ARMED: k1 %.2fmm, k2 %.2fmm both at or past "
                 "the backplate (%.2fmm) - SOCD toggle engaged",
                 (double)d0, (double)d1, (double)floor_mm);
        analog_deep_set(ad, 1, cfg);
    }
}

/* Route one synthesized edge into the SOCD core under whatever mode the
 * `deep` latch selects. The latch itself is resolved once per sample in
 * analog_drain, before any of that sample's edges are applied. */
static int analog_socd_edge(analog_dev_t *ad, int idx, int value,
                            const oid_config_t *cfg) {
    /* cfg->socd is SOCD_ANALOG here; socd_apply special-cases only OFF and
     * SNAPPY, so ANALOG runs the toggle path. Deliberate, not accidental. */
    int mode = ad->deep ? cfg->socd : SOCD_OFF;

    /* `act` needs no lazy binding any more, and that is a consequence of the
     * backplate rule rather than a separate fix.
     *
     * The old design had to defer it: arming released one virtual, and if the
     * key that then dipped was the OTHER one, `act` named a key whose virtual
     * was already up - so the handover wrote up(already-up) + down(already-
     * down) and the kernel ate both. Deferring the binding to the first dip
     * answered "which finger is leaving", but it made `act` lie about which
     * virtual was down, which is what cost the note.
     *
     * Now arming leaves the picture in the toggle's canonical form - exactly
     * one virtual down, and `act` names it truthfully - and every later edge
     * is a plain backplate crossing. up(act) is always a real release and
     * down(!act) always a real press, whichever finger moves. So the choice
     * at arming is genuinely arbitrary, `act_bound` is gone, and so is the
     * cycle that used to repair the no-op. */
    int held[2] = { ad->pool.socd.k1, ad->pool.socd.k2 };
    int before[2];
    socd_virtual_state(held, ad->pool.socd.act, mode, before);

    int voice = pool_feed(&ad->pool, idx, value, mode, cfg);

    /* Recorded AFTER the feed, because the slot layer is what decides which
     * slot this key drives - reading it before would name the slot the key
     * held last time. It is a slot rather than a key index because
     * analog_deep_set compares it against slot occupancy. */
    if (value == 1)
        ad->last_press = idx;

    int after[2];
    held[0] = ad->pool.socd.k1;
    held[1] = ad->pool.socd.k2;
    socd_virtual_state(held, ad->pool.socd.act, mode, after);

    /* Claim a click only when the virtual picture actually moved. Under the
     * backplate rule every toggle transition does, so this no longer has a
     * repair to make - it stays as the guard that keeps the click and the
     * emitted output in step, and regimetest asserts it never fires while
     * riding. */
    if (voice != VOICE_NONE &&
        !(before[voice - 1] == 0 && after[voice - 1] == 1))
        voice = VOICE_NONE;

    return voice;
}

/* Which voice a report claimed. Indirected through a macro purely so
 * tools/regimetest.c can observe it: defining it over audio_trigger itself
 * would rewrite that function's own definition, which the tools compile too.
 * Nothing else should redefine this. */
#ifndef ANALOG_CLICK
#define ANALOG_CLICK(voice) audio_trigger((voice) - 1)
#endif

/* Turn ONE report's worth of depths into SOCD edges. Split out of
 * analog_drain so that tools/replay.c and tools/regimetest.c drive this
 * exact function instead of reimplementing the ordering - the skip-while-
 * armed, the latch resolution, the re-anchor at disarm and the edge routing
 * all have to happen in this order, and a copy in the tools would be free
 * to drift from the daemon while still passing its own assertions. */
static void analog_report(analog_dev_t *ad, const oid_config_t *cfg,
                          const float *depth) {
    /* Feed every key before judging any: engagement and lapse read the
     * deep/live state of the latch pair, which is only complete once both
     * have been fed. The latch is then resolved BEFORE this sample's edges
     * are applied, so every edge runs under the mode actually in force for
     * it. That ordering is what stops the lift ending a rock from being
     * reverted by a toggle on its way out - the release lands as a plain
     * release, and the reconciliation in analog_deep_set has already put
     * the virtual keys where OFF expects them. */
    int edges[POOL_MAX][2], ne[POOL_MAX] = { 0 };
    int was_deep = ad->deep;
    for (int k = 0; k < ad->n; k++) {
        int is_latch = (k == ad->latch[0] || k == ad->latch[1]);
        /* While armed, non-latch keys are not fed AT ALL. Feeding them
         * and dropping the edges would leave the front-end `live`, and
         * the parked state they are meant to sit in exists precisely to
         * re-press on the next real stroke - which is what must not
         * happen while the rock owns both virtuals. They resume below,
         * re-anchored, once the latch clears. */
        if (was_deep && !is_latch)
            continue;
        /* Riding is a property of the latch pair. Nothing else is on the
         * backplate by definition of the latch, and nothing else is
         * being fed while it is armed. */
        ne[k] = analog_key_feed(&ad->keys, k, depth[k], &cfg->analog,
                                was_deep && is_latch, edges[k]);
    }

    analog_deep_update(ad, cfg, depth);

    /* Re-anchor the keys that were silenced, now that they are fed
     * again. Their rt_extreme is as stale as the rock was long, and
     * rule 2 would read the gap between it and the current depth as a
     * reversal. Anchoring on the present sample with `live` already
     * clear is analog_deep_set's parked state: silent while rising,
     * re-pressing only on a real stroke. */
    if (was_deep && !ad->deep)
        for (int k = 0; k < ad->n; k++)
            if (k != ad->latch[0] && k != ad->latch[1])
                ad->keys.key[k].rt_extreme = depth[k];

    for (int k = 0; k < ad->n; k++)
        for (int e = 0; e < ne[k]; e++) {
            int voice = analog_socd_edge(ad, k, edges[k][e], cfg);
            if (voice != VOICE_NONE)
                ANALOG_CLICK(voice);
        }
}

/* Read every queued analog report and turn travel depth into SOCD edges.
 * A key absent from a report is at rest, so each frame is a complete
 * picture rather than a delta: resolve the current depth of every pool
 * key, then hand the frame to analog_report. Returns -1 if the device
 * died. */
static int analog_drain(analog_dev_t *ad, const oid_config_t *cfg) {
    for (;;) {
        unsigned char buf[ANALOG_V2_REPORT];
        ssize_t       n = read(ad->fd, buf, sizeof(buf));
        if (n < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) break;
            if (errno == EINTR) continue;
            LOG_WARN("read(%s): %s", ad->path, strerror(errno));
            return -1;
        }
        if (n == 0)
            return -1;

        analog_sample_t s[ANALOG_SLOTS];
        int             ns = analog_decode(ad->fmt, buf, (size_t)n, s,
                                           ANALOG_SLOTS);

        float depth[POOL_MAX] = { 0.0f };
        for (int i = 0; i < ns; i++)
            for (int k = 0; k < ad->n; k++)
                if (s[i].code == ad->usage[k])
                    depth[k] = s[i].depth * cfg->analog.travel_mm;

        analog_report(ad, cfg, depth);
    }
    return 0;
}

/* A device that dies while it holds k1/k2 would leave its active virtual
 * key stuck down forever; release it before dropping the device. */
static void release_stuck(socd_state_t *in, const oid_config_t *cfg) {
    if (!(in->k1 || in->k2))
        return;
    if (in->mode == SOCD_OFF) {
        /* no active-key tracking in off mode; both may be down */
        if (in->k1) {
            libevdev_uinput_write_event(uidev, EV_KEY, cfg->v1, 0);
            libevdev_uinput_write_event(uidev, EV_SYN, SYN_REPORT, 0);
        }
        if (in->k2) {
            libevdev_uinput_write_event(uidev, EV_KEY, cfg->v2, 0);
            libevdev_uinput_write_event(uidev, EV_SYN, SYN_REPORT, 0);
        }
        return;
    }
    libevdev_uinput_write_event(uidev, EV_KEY,
                                in->act ? cfg->v1 : cfg->v2, 0);
    libevdev_uinput_write_event(uidev, EV_SYN, SYN_REPORT, 0);
}

/* Bring the analog interface back after it has gone away.
 *
 * Losing it is survivable - the daemon drops to the evdev path rather than
 * dying - but it used to be PERMANENT: the evdev node returns through the
 * inotify reconcile while nothing ever retried the hidraw one, so a replug,
 * a USB glitch or a suspend left the daemon quietly running the digital
 * toggle under an analog config until someone restarted it. Silent, and
 * indistinguishable from the state machine misbehaving.
 *
 * Nothing watches /sys/class/hidraw, so the evdev node coming back is the
 * signal that the board is back; the analog node is retried alongside it.
 * A failed retry is quiet (analog_open only complains about permissions,
 * not absence), so this can run on every reconcile pass.
 *
 * The re-marking at the end is the part that is easy to miss. Devices that
 * survived the outage had in->analog cleared when the board went away, and
 * reconcile_devices only sets it on devices it OPENS - so without this the
 * analog keyboard's digital k1/k2 would flow through on top of the analog
 * path and every press would be emitted twice. */
static void analog_reconcile(analog_dev_t **ad, analog_dev_t **adp,
                             const oid_config_t *cfg, dev_list_t *devs,
                             const char *input_dir, int epfd) {
    if (cfg->socd != SOCD_ANALOG || *ad || devs->n == 0)
        return;

    analog_dev_t *n = analog_dev_open(cfg, input_dir);
    if (!n)
        return;

    struct epoll_event ev = { .events = EPOLLIN, .data = { .ptr = n } };
    if (epoll_ctl(epfd, EPOLL_CTL_ADD, n->fd, &ev) < 0) {
        LOG_WARN("epoll_ctl ADD analog: %s - staying on the digital path",
                 strerror(errno));
        analog_dev_close(n);
        return;
    }
    *ad = *adp = n;

    for (size_t d = 0; d < devs->n; d++)
        devs->v[d]->analog =
            libevdev_get_id_vendor(devs->v[d]->dev)  == (int)g_analog_vid &&
            libevdev_get_id_product(devs->v[d]->dev) == (int)g_analog_pid;

    LOG_INFO("Analog interface recovered - back on the analog path");
}

static int run_loop(dev_list_t *devs, const oid_config_t *cfg,
                    const char *input_dir, analog_dev_t **adp) {
    analog_dev_t *ad = *adp;   /* cleared through adp if it disappears */
    int epfd = epoll_create1(EPOLL_CLOEXEC);
    if (epfd < 0) {
        LOG_ERR("epoll_create1: %s", strerror(errno));
        return -1;
    }

    /* Hotplug: any create/attrib change under input_dir (or its by-id /
     * by-path symlink dirs) triggers a reconcile pass. IN_ATTRIB matters:
     * nodes are typically root-only at IN_CREATE time and only become
     * readable once udev applies the input-group permissions. */
    int ifd = inotify_init1(IN_NONBLOCK | IN_CLOEXEC);
    if (ifd < 0) {
        LOG_WARN("inotify_init1: %s - hotplug disabled", strerror(errno));
    } else {
        static const char *subs[] = { "", "/by-id", "/by-path" };
        for (size_t i = 0; i < sizeof(subs) / sizeof(subs[0]); i++) {
            char p[PATH_MAX];
            snprintf(p, sizeof(p), "%s%s", input_dir, subs[i]);
            inotify_add_watch(ifd, p, IN_CREATE | IN_ATTRIB | IN_MOVED_TO);
        }
        struct epoll_event ev = { .events = EPOLLIN, .data = { .ptr = NULL } };
        if (epoll_ctl(epfd, EPOLL_CTL_ADD, ifd, &ev) < 0) {
            LOG_WARN("epoll_ctl ADD inotify: %s - hotplug disabled",
                     strerror(errno));
            close(ifd);
            ifd = -1;
        }
    }

    if (ad) {
        struct epoll_event ev = { .events = EPOLLIN, .data = { .ptr = ad } };
        if (epoll_ctl(epfd, EPOLL_CTL_ADD, ad->fd, &ev) < 0) {
            LOG_ERR("epoll_ctl ADD analog: %s", strerror(errno));
            close(epfd);
            return -1;
        }
    }

    reconcile_devices(devs, cfg, input_dir, epfd, 1);
    if (devs->n == 0) {
        if (ifd < 0) {
            LOG_ERR("Failed to open any input device - aborting");
            close(epfd);
            return -1;
        }
        LOG_WARN("No %s present; waiting for hotplug",
                 cfg->auto_discover ? "matching keyboard"
                                    : "configured device");
    }

    struct epoll_event events[16];

    while (g_running) {
        int nfd = epoll_wait(epfd, events, 16, -1);
        if (nfd < 0) {
            if (errno == EINTR) continue;
            LOG_ERR("epoll_wait: %s", strerror(errno));
            break;
        }
        int rescan = 0;
        for (int i = 0; i < nfd; i++) {
            if (events[i].data.ptr == NULL) { /* inotify fd */
                char buf[4096];
                while (read(ifd, buf, sizeof(buf)) > 0)
                    ;
                rescan = 1;
                continue;
            }

            if (*(int *)events[i].data.ptr == EP_ANALOG) {
                /* The analog board going away is not fatal: release
                 * whatever it held and carry on with the evdev path. */
                if ((events[i].events & (EPOLLERR | EPOLLHUP)) ||
                    analog_drain(ad, cfg) < 0) {
                    LOG_WARN("analog device %s gone; falling back to the "
                             "digital path", ad->path);
                    release_stuck(&ad->pool.socd, cfg);
                    epoll_ctl(epfd, EPOLL_CTL_DEL, ad->fd, NULL);
                    g_analog_vid = g_analog_pid = 0;
                    for (size_t d = 0; d < devs->n; d++)
                        devs->v[d]->analog = 0;
                    analog_dev_close(ad);
                    ad = *adp = NULL;
                }
                continue;
            }

            input_dev_t *in = events[i].data.ptr;
            int dead = 0;

            if (events[i].events & (EPOLLERR | EPOLLHUP)) {
                LOG_WARN("device %s gone (EPOLLERR/HUP), removing",
                         in->path);
                dead = 1;
            } else if (drain_device(in, cfg) < 0) {
                LOG_WARN("dropping %s", in->path);
                dead = 1;
            }

            if (dead) {
                release_stuck(&in->pool.socd, cfg);
                epoll_ctl(epfd, EPOLL_CTL_DEL, in->fd, NULL);
                dev_list_remove(devs, in);
                input_close(in);
                free(in);
            }
        }
        if (rescan) {
            reconcile_devices(devs, cfg, input_dir, epfd, 0);
            analog_reconcile(&ad, adp, cfg, devs, input_dir, epfd);
        }
        if (devs->n == 0 && ifd < 0) {
            LOG_ERR("No devices left and hotplug unavailable - exiting");
            break;
        }
    }

    if (ifd >= 0) close(ifd);
    close(epfd);
    return 0;
}

/* ------------------------------------------------------------------------- */
/* main                                                                      */
/* ------------------------------------------------------------------------- */

static void print_usage(FILE *s, const char *prog) {
    fprintf(s,
        "doubletapd - SOCD-cleaning input daemon\n"
        "\n"
        "Grabs evdev keyboard devices, applies a SOCD radio-button filter (reverting\n"
        "toggle, snappy last-input-wins, analog, or off, per the config's 'socd'\n"
        "field) to two configurable keys, and re-emits everything via a\n"
        "single uinput virtual keyboard. Plays a click on each virtual keypress.\n"
        "\n"
        "'analog' needs an analog keyboard: it runs 'off' while you alt-tap and\n"
        "switches to the toggle while you rock, deciding between them on how far\n"
        "the keys are actually pressed, and adds a software rapid trigger.\n"
        "\n"
        "usage: %s [-h] [-A|-T] [-c CONFIG] [-i DIR]\n"
        "\n"
        "options:\n"
        "    -h          show this help and exit\n"
        "    -A          analog monitor: print live key travel depth and exit\n"
        "                (for picking thresholds; grabs nothing)\n"
        "    -T          trace k1/k2 travel as CSV on stdout until Ctrl-C\n"
        "                (grabs nothing; replay it with the `replay` tool)\n"
        "    -c CONFIG   path to YAML config\n"
        "    -i DIR      directory to scan/watch for event devices\n"
        "                (default /dev/input; mainly for testing)\n"
        "\n"
        "Without -c, the config is looked up at\n"
        "$XDG_CONFIG_HOME/doubletap/config.yaml (~/.config if unset),\n"
        "falling back to %s.\n",
        prog, DEF_CONFIG);
}

/* Resolve the config path when -c wasn't given: prefer the per-user XDG
 * config, fall back to the installed default. */
static const char *default_config_path(void) {
    static char path[4096];
    const char *xdg = getenv("XDG_CONFIG_HOME");
    int n;

    if (xdg && *xdg) {
        n = snprintf(path, sizeof(path), "%s/doubletap/config.yaml", xdg);
    } else {
        const char *home = getenv("HOME");
        if (!home || !*home)
            return DEF_CONFIG;
        n = snprintf(path, sizeof(path), "%s/.config/doubletap/config.yaml",
                     home);
    }
    if (n < 0 || (size_t)n >= sizeof(path))
        return DEF_CONFIG;
    if (access(path, R_OK) == 0)
        return path;
    return DEF_CONFIG;
}

int
main(int argc, char **argv) {
    const char *config_path = NULL;
    const char *input_dir   = "/dev/input";
    int         monitor     = 0;
    int         trace       = 0;

    for (int opt; (opt = getopt(argc, argv, "hATc:i:")) != -1; ) {
        switch (opt) {
            case 'h':
            print_usage(stdout, argv[0]);
            return EXIT_SUCCESS;
            case 'A':
            monitor = 1;
            break;
            case 'T':
            trace = 1;
            break;
            case 'c':
            config_path = optarg;
            break;
            case 'i':
            input_dir = optarg;
            break;
            default:
            print_usage(stderr, argv[0]);
            return EXIT_FAILURE;
        }
    }

    if (!config_path)
        config_path = default_config_path();
    LOG_INFO("Using config %s", config_path);

    oid_config_t cfg;
    config_init(&cfg);
    if (load_config(config_path, &cfg) != 0)
        return EXIT_FAILURE;

    if (trace) {
        int rc = analog_trace(&cfg, input_dir);
        config_free(&cfg);
        return rc;
    }

    if (monitor) {
        int rc = analog_monitor(cfg.analog.device, input_dir,
                                cfg.analog.travel_mm);
        config_free(&cfg);
        return rc;
    }

    const char *v1n = libevdev_event_code_get_name(EV_KEY, cfg.v1);
    const char *v2n = libevdev_event_code_get_name(EV_KEY, cfg.v2);
    char devdesc[32];
    if (cfg.auto_discover)
        snprintf(devdesc, sizeof(devdesc), "auto-discover");
    else
        snprintf(devdesc, sizeof(devdesc), "%zu device(s)", cfg.n_devices);
    char pooldesc[POOL_MAX * 24];
    size_t po = 0;
    for (int i = 0; i < cfg.n_pool && po < sizeof(pooldesc); i++)
        po += (size_t)snprintf(pooldesc + po, sizeof(pooldesc) - po, "%s%s%s",
                               i ? " " : "", key_name_or(cfg.pool[i]),
                               (cfg.socd == SOCD_ANALOG &&
                                (i == cfg.latch[0] || i == cfg.latch[1]))
                               ? "*" : "");
    LOG_INFO("Config: %s, pool [%s]%s -> "
             "v1=%s(%d) v2=%s(%d), socd=%s, audio=%s",
             devdesc, pooldesc,
             cfg.socd == SOCD_ANALOG ? " (* = deep latch pair)" : "",
             v1n ? v1n : "?", cfg.v1, v2n ? v2n : "?", cfg.v2,
             cfg.socd == SOCD_SNAPPY ? "snappy"
                 : cfg.socd == SOCD_OFF    ? "off"
                 : cfg.socd == SOCD_ANALOG ? "analog" : "toggle",
             cfg.audio_enabled ? "enabled" : "disabled");

	/* use rt scheduler if we can */
    struct sched_param sp = { .sched_priority = 90 };
    if (sched_setscheduler(0, SCHED_FIFO, &sp) < 0) {
        LOG_WARN("Failed to set SCHED_FIFO: %s. Falling back to standard scheduler.", strerror(errno));
    } else {
        LOG_INFO("Successfully acquired SCHED_FIFO real-time priority.");
    }

    /* best-effort audio init: resolve per-voice sample + gain (per-key
     * override falls back to the base wav/gain), load each present voice,
     * then bring up the streams. Audio stays available if at least one
     * voice loads. */
    if (cfg.audio_enabled) {
        const char *wav_paths[AUDIO_NVOICES] = {
            cfg.wav_v1 ? cfg.wav_v1 : cfg.wav_path,
            cfg.wav_v2 ? cfg.wav_v2 : cfg.wav_path,
        };
        float gains[AUDIO_NVOICES] = {
            cfg.gain_v1 >= 0.0f ? cfg.gain_v1 : cfg.gain,
            cfg.gain_v2 >= 0.0f ? cfg.gain_v2 : cfg.gain,
        };

        int loaded = 0;
        for (int i = 0; i < AUDIO_NVOICES; i++) {
            if (!wav_paths[i]) continue; /* voice intentionally silent */
            if (wav_load(wav_paths[i], &audio.voice[i]) == 0) {
                audio.voice[i].gain = gains[i];
                loaded++;
            } else {
                LOG_WARN("V%d hitsound load failed (%s); that key stays silent",
                         i + 1, wav_paths[i]);
            }
        }

        if (loaded > 0 && audio_init() == 0) {
            mlockall(MCL_CURRENT | MCL_FUTURE);
            audio_available = 1;
        } else {
            LOG_WARN("Audio disabled (WAV load or PipeWire init failed)");
            for (int i = 0; i < AUDIO_NVOICES; i++) {
                free(audio.voice[i].samples);
                audio.voice[i].samples = NULL;
            }
            audio_available = 0;
        }
    }

    /* Single virtual keyboard. Created before any grabs: device discovery
     * runs inside run_loop and skips it via is_doubletap_output. */
    if (build_virtual(&cfg) != 0) {
        audio_cleanup();
        config_free(&cfg);
        return EXIT_FAILURE;
    }

    /* handlers for graceful shutdown. */
    struct sigaction sa = { .sa_handler = on_signal };
    sigemptyset(&sa.sa_mask);
    sigaction(SIGINT,  &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);
    signal(SIGPIPE, SIG_IGN);

    /* Open the analog interface before the first reconcile pass, so newly
     * grabbed keyboards already know their k1/k2 are analog-driven. */
    analog_dev_t *ad = NULL;
    if (cfg.socd == SOCD_ANALOG) {
        ad = analog_dev_open(&cfg, input_dir);
        if (!ad)
            LOG_WARN("Analog input unavailable - falling back to the "
                     "digital reverting toggle");
    }

    /* run_loop opens/grabs devices itself (initial reconcile + hotplug). */
    dev_list_t devs = { 0 };
    LOG_INFO("Running.");
    int rc = run_loop(&devs, &cfg, input_dir, &ad);

    analog_dev_close(ad);
    LOG_INFO("Shutting down");

    destroy_virtual();
    for (size_t i = 0; i < devs.n; i++) {
        input_close(devs.v[i]);
        free(devs.v[i]);
    }
    free(devs.v);
    audio_cleanup();
    config_free(&cfg);
    return rc == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
