/*
 * doubletapd - Rapid-fire virtual keyboard daemon
 *
 * The daemon directly opens (and exclusively grabs) one or more evdev
 * keyboard devices - either an explicit config list or, by default,
 * every keyboard-shaped device that advertises both configured keys
 * (with inotify-driven hotplug either way) - applies a SOCD state
 * machine ("toggle"/"on" = last-input + reverting toggle, "snappy" =
 * last input wins, or "off" = plain remap with no cleaning, per the
 * config's socd field) to two configurable physical
 * keys (k1, k2 -> v1, v2), mirrors every other event verbatim into a
 * single uinput virtual keyboard, and plays a click sound through
 * PipeWire on every virtual key-press.
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

static void logf_(const char *level, const char *fmt, ...) {
    va_list ap;
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
    float  release_mm;    /* full-release threshold; clears the deep latch */
    float  socd_depth_mm; /* depth at which a press latches as "deep" */
    int    rt_enabled;
    float  rt_press_mm;   /* downward reversal that re-presses */
    float  rt_release_mm; /* upward reversal that releases */
    float  bottom_out_mm; /* within this of full travel == bottomed out, which
                           * presses regardless of rt_press_mm; 0 disables */
    int    hid_k1;        /* HID usage override; -1 == derive from the keymap */
    int    hid_k2;
} analog_config_t;

typedef struct {
    char   **device_paths;
    size_t   n_devices;
    int      auto_discover; /* no 'devices' list: scan for matching keyboards */
    int      k1, k2, v1, v2;
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
    c->k1 = DEF_K1;
    c->k2 = DEF_K2;
    c->v1 = DEF_V1;
    c->v2 = DEF_V2;
    c->audio_enabled = 1;
    c->gain    = 1.0f;
    c->gain_v1 = -1.0f; /* sentinel: unset -> falls back to gain */
    c->gain_v2 = -1.0f;
    c->analog.travel_mm     = 4.0f;
    c->analog.actuation_mm  = 1.0f;
    c->analog.release_mm    = 0.4f;
    c->analog.socd_depth_mm = 1.5f;
    c->analog.rt_enabled    = 1;
    c->analog.rt_press_mm   = 0.3f;
    c->analog.rt_release_mm = 0.3f;
    c->analog.bottom_out_mm = 0.1f;
    c->analog.hid_k1        = -1;   /* sentinel: derive from the keymap */
    c->analog.hid_k2        = -1;
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
    if (a->release_mm <= 0.0f) {
        LOG_ERR("'analog.release_mm' must be greater than 0, otherwise a key "
                "can never register as fully released");
        bad = 1;
    }
    if (a->actuation_mm <= a->release_mm) {
        LOG_ERR("'analog.actuation_mm' (%.3f) must be greater than "
                "'analog.release_mm' (%.3f)",
                (double)a->actuation_mm, (double)a->release_mm);
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
    if (bad)
        return -1;

    /* Survivable, but almost certainly not what was meant. */
    if (a->bottom_out_mm >= a->travel_mm)
        LOG_WARN("'analog.rapid_trigger.bottom_out_mm' (%.3f) >= travel_mm "
                 "(%.3f): every sample counts as bottomed out",
                 (double)a->bottom_out_mm, (double)a->travel_mm);
    if (a->socd_depth_mm > a->travel_mm)
        LOG_WARN("'analog.socd_depth_mm' (%.3f) exceeds travel_mm (%.3f): "
                 "SOCD will never engage",
                 (double)a->socd_depth_mm, (double)a->travel_mm);
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
            { "k1", &c->k1 }, { "k2", &c->k2 },
            { "v1", &c->v1 }, { "v2", &c->v2 },
        };
        for (size_t i = 0; i < sizeof(kmap)/sizeof(kmap[0]); i++) {
            yaml_node_t *kn = map_get(&doc, keys, kmap[i].name);
            if (kn && parse_key_code(kn, kmap[i].out) != 0) {
                LOG_ERR("invalid key code for 'keys.%s'", kmap[i].name);
                goto out;
            }
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
            { "release_mm",    offsetof(analog_config_t, release_mm)    },
            { "socd_depth_mm", offsetof(analog_config_t, socd_depth_mm) },
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
            map_get(&doc, an, "gate_margin_mm"))
            LOG_WARN("'analog.gate', 'gate_depth_mm' and 'gate_margin_mm' "
                     "were removed and are ignored - SOCD engagement now "
                     "rests on socd_depth_mm alone");

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
        }
        static const struct { const char *key; size_t off; } hid[] = {
            { "hid_k1", offsetof(analog_config_t, hid_k1) },
            { "hid_k2", offsetof(analog_config_t, hid_k2) },
        };
        for (size_t i = 0; i < sizeof(hid) / sizeof(hid[0]); i++) {
            yaml_node_t *v = map_get(&doc, an, hid[i].key);
            if (!v) continue;
            const char *s = v->type == YAML_SCALAR_NODE
                            ? (const char *)v->data.scalar.value : "";
            if (!strcasecmp(s, "auto")) continue;
            char *end = NULL;
            errno = 0;
            long u = strtol(s, &end, 0);
            if (end == s || *end != '\0' || errno != 0 || u < 0 || u > 0xFFFF) {
                LOG_ERR("'analog.%s' must be \"auto\" or a HID usage id",
                        hid[i].key);
                goto out;
            }
            *(int *)((char *)&c->analog + hid[i].off) = (int)u;
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
    int k1;   /* k1 down? */
    int k2;   /* k2 down? */
    int act;  /* 1 == v1 active, 0 == v2 (or none) */
    int mode; /* mode of the last edge applied; release_stuck needs the
               * EFFECTIVE mode, which for the analog path is not cfg->socd */
} socd_state_t;

typedef struct {
    int               kind;    /* EP_INPUT */
    struct libevdev  *dev;
    int               fd;
    char             *path;
    dev_t             rdev; /* st_rdev, dedupes nodes reached via symlinks */
    int               grabbed; /* 0 = open but grab deferred (keys held) */
    int               analog;  /* k1/k2 come from the analog path; drop them */
    socd_state_t      socd;
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
    if (!libevdev_has_event_code(dev, EV_KEY, (unsigned)cfg->k1) ||
        !libevdev_has_event_code(dev, EV_KEY, (unsigned)cfg->k2))
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
 * only thing that decides whether an edge is emitted here. socd_depth_mm
 * is a deeper mark meaning "this press is being ridden, not tapped"; it
 * gates nothing in this function - it only sets the `deep` latch that
 * analog_socd_edge reads when deciding whether an overlap is a deliberate
 * rock or two taps that happened to touch.
 *
 * `deep` is a latch, deliberately not the same as "currently past
 * socd_depth_mm": once a press has proven itself deep it keeps gating the
 * OTHER key even as it bounces during normal riding, and lets go only on
 * a full release past release_mm. That is the same contract as
 * Wootility's "Continuous Rapid Trigger" - tracking stays engaged until
 * the key returns to the top, not merely until it recrosses a threshold.
 * `rt_extreme` follows the same continuity rule: a rapid-trigger release
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
 *      crosses socd_depth_mm and constitutes a reversal thus resolves
 *      unambiguously as the reversal; an engaged key never re-runs the
 *      fresh-actuation logic.
 *   3. Otherwise (never actuated this engagement) the fresh-actuation
 *      check runs instead.
 *   4. Finally, independent of which of 2/3 fired, the deep latch is
 *      evaluated once against the sample's resulting `live`/depth - so a
 *      sample that reaches socd_depth_mm on the same tick it went live
 *      latches immediately, while one that reaches it on the same tick it
 *      went non-live (a rapid-trigger release) correctly does not.
 *
 * Every branch that flips `live` is the branch that appends the matching
 * edge, so `live` cannot desynchronize from what was emitted - socd_apply
 * downstream assumes strict press/release alternation per key.
 */
typedef struct {
    int   live;        /* an edge has been emitted; key counts as pressed */
    int   deep;        /* latched: reached socd_depth_mm during this press */
    float rt_extreme;  /* local extreme for rapid trigger, mm */
} analog_key_t;

typedef struct {
    analog_key_t key[2];   /* [0] == k1, [1] == k2 */
} analog_state_t;

/* Feed one sample for one key. idx is 0 (k1) or 1 (k2); a key absent from
 * the hardware report is fed depth_mm == 0.0f. Writes up to 2 edges to
 * out[] (1 == press, 0 == release, in order) and returns the count. */
static int analog_key_feed(analog_state_t *st, int idx, float depth_mm,
                           const analog_config_t *cfg, int *out) {
    analog_key_t *self = &st->key[idx];
    int           n    = 0;

    /* 1. Full release: master reset. The only place that clears `deep`
     * or zeroes `rt_extreme`. */
    if (depth_mm < cfg->release_mm) {
        if (self->live)
            out[n++] = 0;
        self->live       = 0;
        self->deep       = 0;
        self->rt_extreme = 0.0f;
        return n;
    }

    if (cfg->rt_enabled && self->rt_extreme != 0.0f) {
        /* 2. Already engaged: rapid trigger owns this sample exclusively,
         * tracking the local extreme and firing on a large enough
         * reversal in either direction. */
        if (self->live) {
            if (depth_mm > self->rt_extreme) {
                self->rt_extreme = depth_mm;
            } else if (self->rt_extreme - depth_mm >= cfg->rt_release_mm) {
                out[n++]         = 0;
                self->live       = 0;
                self->rt_extreme = depth_mm;
            }
        } else {
            /* Bottoming out always presses, however small the down-travel
             * since the last reversal - the mirror of rule 1, where a full
             * release always releases however small the up-travel. Riding
             * the backplate as the neutral position depends on this: with a
             * large rt_press_mm, a wobble that ends against the bottom never
             * travels far enough to satisfy the reversal test on its own. */
            /* A re-press still has to clear actuation_mm: without that
             * floor, a reversal entirely within the top fraction of travel
             * synthesizes whole keystrokes where the sensor is least
             * linear. */
            int bottomed = cfg->bottom_out_mm > 0.0f &&
                           depth_mm >= cfg->travel_mm - cfg->bottom_out_mm;
            if (depth_mm < self->rt_extreme) {
                self->rt_extreme = depth_mm;
            } else if (depth_mm >= cfg->actuation_mm &&
                       (bottomed ||
                        depth_mm - self->rt_extreme >= cfg->rt_press_mm)) {
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

    /* 4. Deep latch, evaluated once against the outcome of 2/3 above. */
    if (self->live && depth_mm >= cfg->socd_depth_mm)
        self->deep = 1;

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
    uint16_t       usage[2];    /* HID usage id of k1, k2 */
    char           path[PATH_MAX];
    analog_state_t keys;
    socd_state_t   socd;
    int            regime;         /* 1 == this overlap is SOCD-managed */
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

    /* Resolve k1/k2 to HID usage ids: an explicit config override wins,
     * otherwise walk a keyboard's keymap for the mapping. */
    static uint16_t usage_of_key[KEY_MAX + 1];
    if (cfg->analog.hid_k1 < 0 || cfg->analog.hid_k2 < 0)
        hid_map_from_any_keyboard(input_dir, usage_of_key);

    ad->usage[0] = cfg->analog.hid_k1 >= 0 ? (uint16_t)cfg->analog.hid_k1
                                           : usage_of_key[cfg->k1];
    ad->usage[1] = cfg->analog.hid_k2 >= 0 ? (uint16_t)cfg->analog.hid_k2
                                           : usage_of_key[cfg->k2];
    if (!ad->usage[0] || !ad->usage[1]) {
        LOG_ERR("could not resolve k1/k2 to HID usage ids - run with -A to "
                "find them, then set analog.hid_k1 / analog.hid_k2");
        close(ad->fd);
        free(ad);
        return NULL;
    }

    g_analog_vid = ad->vid;
    g_analog_pid = ad->pid;

    LOG_INFO("Analog input on %s (%s, %04x:%04x): k1 -> hid 0x%02x, "
             "k2 -> hid 0x%02x", ad->path,
             ad->fmt == ANALOG_FMT_V2 ? "v2" : "v1", ad->vid, ad->pid,
             ad->usage[0], ad->usage[1]);
    LOG_INFO("Analog thresholds: actuation %.2fmm, release %.2fmm, "
             "socd_depth %.2fmm, rapid trigger %s",
             (double)cfg->analog.actuation_mm, (double)cfg->analog.release_mm,
             (double)cfg->analog.socd_depth_mm,
             cfg->analog.rt_enabled ? "on" : "off");
    return ad;
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
static int socd_apply(socd_state_t *in, int is_k1, int value, int mode,
                      const oid_config_t *cfg) {
    in->mode = mode;

    if (mode == SOCD_OFF) {
        /* k1/k2 still tracked so release_stuck can clean up a dying device.
         * Each key maps 1:1 to its own voice; both may sound if both are
         * pressed (they play on independent streams). */
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

/* evdev entry point: mirror everything that isn't k1/k2, and hand k1/k2 to
 * the SOCD core. In analog mode this device's k1/k2 are driven from travel
 * depth instead, so the digital copies are dropped here to avoid emitting
 * each press twice. */
static int process_event(input_dev_t *in, const struct input_event *ie,
                         const oid_config_t *cfg) {
    if (ie->type == EV_MSC && ie->code == MSC_SCAN)
        return VOICE_NONE;

    if (ie->type != EV_KEY ||
        (ie->code != (unsigned)cfg->k1 && ie->code != (unsigned)cfg->k2)) {
        libevdev_uinput_write_event(uidev, ie->type, ie->code, ie->value);
        return VOICE_NONE;
    }

    if (in->analog)
        return VOICE_NONE;

    return socd_apply(&in->socd, ie->code == (unsigned)cfg->k1, ie->value,
                      cfg->socd, cfg);
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

/* Route one synthesized edge into the SOCD core, deciding first whether
 * this overlap is SOCD-managed at all.
 *
 * That choice is made once, when the second key goes down, and held for the
 * rest of the overlap. Both keys deep means the player is riding the bottom
 * and wants the toggle; anything shallower is ordinary alternate tapping,
 * where the two keys must stay independent - running the toggle there turns
 * an incidental overlap into an extra keypress, which is exactly the misfire
 * this mode exists to remove.
 *
 * Latching at the second press rather than re-deciding per sample matters:
 * switching regimes mid-overlap would desynchronize `act` from the virtual
 * keys already emitted. On entry we seed `act` with the already-held key, so
 * the core's S_PRESS releases the right one. */
static int analog_socd_edge(analog_dev_t *ad, int is_k1, int value,
                            const oid_config_t *cfg) {
    if (value == 1) {
        int other      = is_k1 ? 1 : 0;
        int other_down = is_k1 ? ad->socd.k2 : ad->socd.k1;

        /* Only the ALREADY-HELD key's depth is meaningful here. The key
         * going down right now has barely travelled - its press edge fires
         * at actuation, or at a rapid-trigger reversal - so it cannot yet
         * be past socd_depth_mm, and demanding that it is makes engagement
         * depend on how far a rapid-trigger re-press happens to land. That
         * is stable only when socd_depth_mm sits above where re-presses
         * fire, and turns into a coin flip as it approaches them. The held
         * key being ridden deep is the actual signal that this is a wobble
         * and not alternate tapping. */
        if (other_down && !ad->regime && ad->keys.key[other].deep) {
            ad->regime   = 1;
            ad->socd.act = !is_k1;   /* the held key is the active one */
        }
    }

    int mode  = ad->regime ? cfg->socd : SOCD_OFF;
    int voice = socd_apply(&ad->socd, is_k1, value, mode, cfg);

    /* The regime ends only when a key comes all the way back up, which is
     * precisely what clears `deep`. It must NOT be tied to the emitted key
     * state: rapid trigger releases and re-presses constantly, so a moment
     * where both virtual keys happen to be up is just part of the rock, not
     * the end of the gesture. Dropping the regime there costs a beat to
     * plain mode before the toggle can re-engage, which is audible.
     *
     * It lapses once NEITHER key is deep any more - if it required both,
     * a wobble whose second key never crossed the threshold would drop the
     * regime on the very next sample. Checked after the edge, not before,
     * so the release that ends a wobble is still handled under the toggle
     * and reverts correctly.
     *
     * It must ALSO wait until nothing is still held. Changing the effective
     * mode while a virtual key is down strands it: the reverting toggle can
     * legitimately leave `act` on a key whose finger has already lifted, and
     * if the remaining release then routes through SOCD_OFF it writes only
     * its own code, so whatever the toggle pressed is never released. Both
     * physical keys being up is the one point where the two modes agree that
     * nothing is emitted, so it is the only safe place to switch. */
    if (!ad->keys.key[0].deep && !ad->keys.key[1].deep &&
        !ad->socd.k1 && !ad->socd.k2)
        ad->regime = 0;

    return voice;
}

/* Read every queued analog report and turn travel depth into SOCD edges.
 * A key absent from a report is at rest, so each frame is a complete
 * picture rather than a delta: resolve the current depth of k1/k2, feed
 * both to the front-end, and route whatever edges come back through the
 * same core the evdev path uses. Returns -1 if the device died. */
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

        float depth[2] = { 0.0f, 0.0f };
        for (int i = 0; i < ns; i++)
            for (int k = 0; k < 2; k++)
                if (s[i].code == ad->usage[k])
                    depth[k] = s[i].depth * cfg->analog.travel_mm;

        for (int k = 0; k < 2; k++) {
            int edges[2];
            int ne = analog_key_feed(&ad->keys, k, depth[k], &cfg->analog,
                                     edges);
            for (int e = 0; e < ne; e++) {
                int voice = analog_socd_edge(ad, k == 0, edges[e], cfg);
                if (voice != VOICE_NONE)
                    audio_trigger(voice - 1);
            }
        }
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
                    release_stuck(&ad->socd, cfg);
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
                release_stuck(&in->socd, cfg);
                epoll_ctl(epfd, EPOLL_CTL_DEL, in->fd, NULL);
                dev_list_remove(devs, in);
                input_close(in);
                free(in);
            }
        }
        if (rescan)
            reconcile_devices(devs, cfg, input_dir, epfd, 0);
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
        "toggle, snappy last-input-wins, or off, per the config's 'socd' field) to two\n"
        "configurable keys, and re-emits everything via a\n"
        "single uinput virtual keyboard. Plays a click on each virtual keypress.\n"
        "\n"
        "usage: %s [-h] [-A] [-c CONFIG] [-i DIR]\n"
        "\n"
        "options:\n"
        "    -h          show this help and exit\n"
        "    -A          analog monitor: print live key travel depth and exit\n"
        "                (for picking thresholds; grabs nothing)\n"
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

    for (int opt; (opt = getopt(argc, argv, "hAc:i:")) != -1; ) {
        switch (opt) {
            case 'h':
            print_usage(stdout, argv[0]);
            return EXIT_SUCCESS;
            case 'A':
            monitor = 1;
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

    if (monitor) {
        int rc = analog_monitor(cfg.analog.device, input_dir,
                                cfg.analog.travel_mm);
        config_free(&cfg);
        return rc;
    }

    const char *k1n = libevdev_event_code_get_name(EV_KEY, cfg.k1);
    const char *k2n = libevdev_event_code_get_name(EV_KEY, cfg.k2);
    const char *v1n = libevdev_event_code_get_name(EV_KEY, cfg.v1);
    const char *v2n = libevdev_event_code_get_name(EV_KEY, cfg.v2);
    char devdesc[32];
    if (cfg.auto_discover)
        snprintf(devdesc, sizeof(devdesc), "auto-discover");
    else
        snprintf(devdesc, sizeof(devdesc), "%zu device(s)", cfg.n_devices);
    LOG_INFO("Config: %s, keys k1=%s(%d) k2=%s(%d) "
             "v1=%s(%d) v2=%s(%d), socd=%s, audio=%s",
             devdesc,
             k1n ? k1n : "?", cfg.k1, k2n ? k2n : "?", cfg.k2,
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
