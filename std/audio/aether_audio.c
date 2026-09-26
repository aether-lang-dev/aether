/* std.audio playback substrate + mixer tier — see aether_audio.h.
 *
 * Backed by vendored miniaudio (std/audio/miniaudio.h — public domain /
 * MIT-0, David Reid). miniaudio's high-level engine (ma_engine / ma_sound)
 * provides device output + decode (wav/mp3/flac) + per-sound volume/seek in
 * one battle-tested layer, so this glue is bindings, not logic. The device
 * pulls samples on miniaudio's own realtime thread — the runtime-stays-C
 * hard line (docs/cross-references/audio.md §6): no Aether-emitted code runs
 * there; control (play/pause/volume/seek) flows in through ma_engine's own
 * atomics.
 *
 * Backend selection: open() first tries a real device; if none initialises
 * (headless CI, no sound card), it falls back to miniaudio's NULL backend so
 * every transport/seek/position behaviour stays deterministic and testable.
 * is_null_backend() reports which path won. All Aether-facing allocations go
 * through the caps allocator; miniaudio's own allocations use its default
 * (they are below the FFI line, like OpenSSL's). */
#include "aether_audio.h"
#include "../../runtime/aether_resource_caps.h"

#include <stdlib.h>
#include <string.h>
#include <math.h>

/* WASI has no audio device, and miniaudio's device layer reaches for
 * pthread_attr_setschedparam / sched_get_priority_max to raise its audio
 * thread's priority — none of which wasi-libc declares (#1655). MA_NO_DEVICE_IO
 * is not a way out: it removes ma_context/ma_backend, which this file's own
 * null-backend fallback uses.
 *
 * So the module is stubbed wholesale on wasi, reporting the SAME shape it
 * already reports on a box with no sound card: is_null_backend() true and a
 * last_error() explaining why. Callers that already handle the null backend
 * need no wasi-specific code. */
#if defined(__wasi__)

#include <stdint.h>   /* int64_t, used by the seek stub */

int aether_audio_open(void) { return 0; }
void aether_audio_close(void) { }
int aether_audio_is_null_backend(void) { return 1; }
const char* aether_audio_last_error(void) {
    return "audio is unavailable on wasm32-wasi (no device, no threads)";
}
void* aether_audio_load_wav(const char* d, int n) { (void)d; (void)n; return NULL; }
void* aether_audio_load_pcm(const char* d, int n, int ch, int sr, int bits) {
    (void)d; (void)n; (void)ch; (void)sr; (void)bits; return NULL;
}
void aether_audio_unload(void* s) { (void)s; }
int aether_audio_play(void* s) { (void)s; return 0; }
int aether_audio_pause(void* s) { (void)s; return 0; }
int aether_audio_stop(void* s) { (void)s; return 0; }
int aether_audio_is_playing(void* s) { (void)s; return 0; }
int aether_audio_set_volume(void* s, double v) { (void)s; (void)v; return 0; }
double aether_audio_get_volume(void* s) { (void)s; return 0.0; }
int aether_audio_seek_ns(void* s, int64_t ns) { (void)s; (void)ns; return 0; }
int aether_audio_channels(void* s) { (void)s; return 0; }
int aether_audio_sample_rate(void* s) { (void)s; return 0; }
int aether_audio_set_pan(void* s, double p) { (void)s; (void)p; return 0; }
double aether_audio_get_pan(void* s) { (void)s; return 0.0; }
int aether_audio_set_pitch(void* s, double r) { (void)s; (void)r; return 0; }
double aether_audio_get_pitch(void* s) { (void)s; return 1.0; }
int aether_audio_set_looping(void* s, int on) { (void)s; (void)on; return 0; }
int aether_audio_is_looping(void* s) { (void)s; return 0; }
int aether_audio_fade_ms(void* s, double a, double b, int64_t ms) {
    (void)s; (void)a; (void)b; (void)ms; return 0;
}
int64_t aether_audio_played_frames(void* s) { (void)s; return 0; }
void* aether_audio_group_new(void) { return NULL; }
void aether_audio_group_free(void* g) { (void)g; }
int aether_audio_group_set_volume(void* g, double v) { (void)g; (void)v; return 0; }
double aether_audio_group_get_volume(void* g) { (void)g; return 0.0; }
int aether_audio_set_group(void* s, void* g) { (void)s; (void)g; return 0; }
void* aether_audio_stream_open(int sr, int ch, int fmt, int cap) {
    (void)sr; (void)ch; (void)fmt; (void)cap; return NULL;
}
int aether_audio_stream_push(void* s, const char* d, int n) { (void)s; (void)d; (void)n; return -1; }
int aether_audio_stream_queued(void* s) { (void)s; return 0; }
int aether_audio_stream_capacity(void* s) { (void)s; return 0; }
int64_t aether_audio_stream_underruns(void* s) { (void)s; return 0; }
int aether_audio_open_headless(int sr, int ch) { (void)sr; (void)ch; return 0; }
int aether_audio_is_headless(void) { return 0; }
int aether_audio_render(int frames) { (void)frames; return 0; }
double aether_audio_rendered_peak(int ch) { (void)ch; return 0.0; }
double aether_audio_rendered_sample(int f, int ch) { (void)f; (void)ch; return 0.0; }

#else

#define MINIAUDIO_IMPLEMENTATION
/* Trim the build: we only need decoding + playback, no capture/encoding/
 * resource-manager niceties beyond what ma_engine needs by default. */
#define MA_NO_ENCODING
/* WASI has no audio device and no threads. miniaudio's device layer reaches
 * for pthread_attr_setschedparam / sched_get_priority_max to raise its audio
 * thread's priority, none of which wasi-libc declares (#1655). MA_NO_DEVICE_IO
 * drops the whole backend layer — the same warn-and-degrade posture the cross
 * path already takes for CoreAudio on a macos target, where zig's SDK stubs
 * omit the Apple-licensed framework headers. Decoding still builds; playback
 * reports unavailable at runtime, which is correct for a target with no
 * audio device to play to. */
#include "miniaudio.h"

/* ---- engine ------------------------------------------------------------ */

static ma_engine g_engine;
static ma_context g_null_ctx;
static int g_engine_ready = 0;
static int g_is_null = 0;
static int g_headless = 0;
static const char* g_audio_err = "";

/* Headless render block (aether_audio_render): the last mix pulled through
 * the graph, kept so a test can read peaks and samples back. */
static float*  g_render = NULL;
static size_t  g_render_bytes = 0;        /* allocation size, for the caps free */
static size_t  g_render_cap_frames = 0;   /* allocated capacity, frames */
static size_t  g_render_frames = 0;       /* frames in the last render */
#define AUDIO_RENDER_MAX_FRAMES (1u << 22) /* ~87 s at 48 kHz; bounds the block */

int aether_audio_open(void) {
    if (g_engine_ready) return 1;
    g_audio_err = "";

    /* First attempt: default engine, real device auto-selected. */
    if (ma_engine_init(NULL, &g_engine) == MA_SUCCESS) {
        g_is_null = 0;
        g_engine_ready = 1;
        return 1;
    }

    /* Fallback: force the null backend so headless environments still get a
     * fully-functional (silent) engine — decode, transport, seek, position
     * all work against the null device's steady clock. */
    ma_backend nullBackend = ma_backend_null;
    if (ma_context_init(&nullBackend, 1, NULL, &g_null_ctx) != MA_SUCCESS) {
        g_audio_err = "audio: could not initialise any backend";
        return 0;
    }
    ma_engine_config cfg = ma_engine_config_init();
    cfg.pContext = &g_null_ctx;
    if (ma_engine_init(&cfg, &g_engine) != MA_SUCCESS) {
        ma_context_uninit(&g_null_ctx);
        g_audio_err = "audio: could not initialise the null engine";
        return 0;
    }
    g_is_null = 1;
    g_engine_ready = 1;
    return 1;
}

/* Headless: an engine with no device and no mixer thread. Nothing advances
 * until render() pulls frames through the graph, so every number a test
 * reads back (position, played frames, peaks) is a function of the calls it
 * made, not of wall-clock time. This is what lets a headless test hold a mix
 * to numbers the way ae3d's renderers are held to pixels (#2205). */
int aether_audio_open_headless(int sample_rate, int channels) {
    if (g_engine_ready) { g_audio_err = "audio: engine already open"; return 0; }
    if (sample_rate <= 0) { g_audio_err = "audio: sample_rate must be > 0"; return 0; }
    if (channels <= 0)    { g_audio_err = "audio: channels must be > 0"; return 0; }
    g_audio_err = "";

    ma_engine_config cfg = ma_engine_config_init();
    cfg.noDevice   = MA_TRUE;
    cfg.channels   = (ma_uint32)channels;
    cfg.sampleRate = (ma_uint32)sample_rate;
    if (ma_engine_init(&cfg, &g_engine) != MA_SUCCESS) {
        g_audio_err = "audio: could not initialise the headless engine";
        return 0;
    }
    g_is_null = 1;
    g_headless = 1;
    g_engine_ready = 1;
    return 1;
}

void aether_audio_close(void) {
    if (!g_engine_ready) return;
    if (g_render) {
        aether_caps_free(g_render, g_render_bytes);
        g_render = NULL;
        g_render_bytes = 0;
        g_render_cap_frames = 0;
        g_render_frames = 0;
    }
    ma_engine_uninit(&g_engine);
    if (g_is_null && !g_headless) ma_context_uninit(&g_null_ctx);
    g_engine_ready = 0;
    g_is_null = 0;
    g_headless = 0;
}

int aether_audio_is_null_backend(void) {
    return g_is_null;
}

int aether_audio_is_headless(void) {
    return g_headless;
}

int aether_audio_render(int frames) {
    if (!g_engine_ready || !g_headless) {
        g_audio_err = "audio: render needs a headless engine (open_headless)";
        return 0;
    }
    if (frames <= 0) return 0;
    if ((unsigned)frames > AUDIO_RENDER_MAX_FRAMES) {
        g_audio_err = "audio: render block too large";
        return 0;
    }
    ma_uint32 ch = ma_engine_get_channels(&g_engine);
    if ((size_t)frames > g_render_cap_frames) {
        if (g_render) aether_caps_free(g_render, g_render_bytes);
        g_render_bytes = (size_t)frames * ch * sizeof(float);
        g_render = (float*)aether_caps_malloc(g_render_bytes);
        if (!g_render) {
            g_render_bytes = 0;
            g_render_cap_frames = 0;
            g_audio_err = "audio: out of memory";
            return 0;
        }
        g_render_cap_frames = (size_t)frames;
    }
    ma_uint64 read = 0;
    ma_engine_read_pcm_frames(&g_engine, g_render, (ma_uint64)frames, &read);
    g_render_frames = (size_t)read;
    return (int)read;
}

double aether_audio_rendered_peak(int channel) {
    if (!g_render || !g_engine_ready) return 0.0;
    ma_uint32 ch = ma_engine_get_channels(&g_engine);
    if (channel < 0 || (ma_uint32)channel >= ch) return 0.0;
    float peak = 0.0f;
    for (size_t i = 0; i < g_render_frames; i++) {
        float v = g_render[i * ch + (size_t)channel];
        if (v < 0) v = -v;
        if (v > peak) peak = v;
    }
    return (double)peak;
}

double aether_audio_rendered_sample(int frame, int channel) {
    if (!g_render || !g_engine_ready) return 0.0;
    ma_uint32 ch = ma_engine_get_channels(&g_engine);
    if (frame < 0 || (size_t)frame >= g_render_frames) return 0.0;
    if (channel < 0 || (ma_uint32)channel >= ch) return 0.0;
    return (double)g_render[(size_t)frame * ch + (size_t)channel];
}

const char* aether_audio_last_error(void) {
    return g_audio_err;
}

/* ---- queue (stream) data source ---------------------------------------- */

/* A bounded ring of PCM the program pushes into while the sound plays. The
 * ring is miniaudio's lock-free single-producer/single-consumer ma_pcm_rb:
 * the program is the producer (stream_push, main thread), the mixer the
 * consumer (onRead, mixer thread or render()). Data crosses on a ring, not a
 * call — rule 3 of the realtime-callback contract in
 * docs/cross-references/audio.md §6. An empty ring plays silence and counts
 * the shortfall as underruns rather than ending the sound: a procedural
 * source that falls behind should recover, not stop. The sound pulls from
 * the ring a block (512 frames) ahead of the output, so `queued` and the
 * underrun count lead what has been heard by up to a block. */
typedef struct {
    ma_data_source_base base;
    ma_pcm_rb           rb;
    ma_format           format;
    ma_uint32           channels;
    ma_uint32           rate;
    ma_uint32           capacity;
    ma_atomic_uint64    frames_read;   /* real frames delivered to the mixer */
    ma_atomic_uint64    underruns;     /* silent frames substituted */
} StreamSource;

static ma_result stream_read(ma_data_source* ds, void* out, ma_uint64 n, ma_uint64* read) {
    StreamSource* s = (StreamSource*)ds;
    ma_uint32 bpf = ma_get_bytes_per_frame(s->format, s->channels);
    ma_uint8* dst = (ma_uint8*)out;
    ma_uint64 done = 0;
    while (done < n) {
        ma_uint64 left = n - done;
        ma_uint32 want = left > 0xFFFFFFFFu ? 0xFFFFFFFFu : (ma_uint32)left;
        void* src = NULL;
        if (ma_pcm_rb_acquire_read(&s->rb, &want, &src) != MA_SUCCESS || want == 0) break;
        if (dst) memcpy(dst + done * bpf, src, (size_t)want * bpf);
        ma_pcm_rb_commit_read(&s->rb, want);
        done += want;
    }
    if (done < n && dst) {
        ma_silence_pcm_frames(dst + done * bpf, n - done, s->format, s->channels);
    }
    ma_atomic_uint64_fetch_add(&s->frames_read, done);
    ma_atomic_uint64_fetch_add(&s->underruns, n - done);
    if (read) *read = n;
    return MA_SUCCESS;
}
static ma_result stream_seek(ma_data_source* ds, ma_uint64 frame) {
    (void)ds; (void)frame;
    return MA_NOT_IMPLEMENTED;   /* a queue has no past to seek into */
}
static ma_result stream_format(ma_data_source* ds, ma_format* f, ma_uint32* ch,
                               ma_uint32* rate, ma_channel* map, size_t cap) {
    StreamSource* s = (StreamSource*)ds;
    if (f) *f = s->format;
    if (ch) *ch = s->channels;
    if (rate) *rate = s->rate;
    if (map && cap > 0) ma_channel_map_init_standard(ma_standard_channel_map_default, map, cap, s->channels);
    return MA_SUCCESS;
}
static ma_result stream_cursor(ma_data_source* ds, ma_uint64* cur) {
    if (cur) *cur = ma_atomic_uint64_get(&((StreamSource*)ds)->frames_read);
    return MA_SUCCESS;
}
static ma_result stream_length(ma_data_source* ds, ma_uint64* len) {
    (void)ds;
    if (len) *len = 0;
    return MA_NOT_IMPLEMENTED;   /* open-ended: duration reports 0 */
}
static const ma_data_source_vtable g_stream_vtable = {
    stream_read, stream_seek, stream_format, stream_cursor, stream_length, NULL, 0
};

/* ---- pan node ---------------------------------------------------------- */

/* Equal-power panning as a node between the sound and its group. miniaudio's
 * own ma_panner is a linear crossfade in both of its modes; the constant-
 * power law the ask names is the Web Audio StereoPannerNode one, which this
 * reproduces exactly so an engine can compute its expected numbers from the
 * spec:
 *   mono source:  x = (pan+1)/2;  L = m*cos(x*pi/2),  R = m*sin(x*pi/2)
 *   stereo, pan<=0: x = pan+1;    L = l + r*cos(x*pi/2), R = r*sin(x*pi/2)
 *   stereo, pan>0:  x = pan;      L = l*cos(x*pi/2),     R = r + l*sin(x*pi/2)
 * A stereo source at 0 passes through at unity, so the pre-#2205 volume of
 * existing stereo playback is unchanged. `pan` is read atomically: the
 * setter runs on the program's thread, this on the mixer's. Non-stereo
 * engine layouts pass through untouched. */
typedef struct {
    ma_node_base    base;
    ma_atomic_float pan;
    int             mono;     /* the source is 1-channel (upmixed L == R) */
} PanNode;

static void pan_process(ma_node* node, const float** in, ma_uint32* in_count,
                        float** out, ma_uint32* out_count) {
    PanNode* p = (PanNode*)node;
    ma_uint32 n = *out_count;
    ma_uint32 ch = ma_node_get_output_channels(node, 0);
    const float* src = in[0];
    float* dst = out[0];
    if (ch != 2) {
        ma_copy_pcm_frames(dst, src, n, ma_format_f32, ch);
        *in_count = n;
        return;
    }
    float pan = ma_atomic_float_get(&p->pan);
    if (p->mono) {
        double x = (pan + 1.0) * 0.5;
        float gl = (float)cos(x * MA_PI_D / 2.0), gr = (float)sin(x * MA_PI_D / 2.0);
        for (ma_uint32 i = 0; i < n; i++) {
            float m = src[i * 2];
            dst[i * 2] = m * gl;
            dst[i * 2 + 1] = m * gr;
        }
    } else if (pan <= 0.0f) {
        double x = pan + 1.0;
        float gl = (float)cos(x * MA_PI_D / 2.0), gr = (float)sin(x * MA_PI_D / 2.0);
        for (ma_uint32 i = 0; i < n; i++) {
            float l = src[i * 2], r = src[i * 2 + 1];
            dst[i * 2] = l + r * gl;
            dst[i * 2 + 1] = r * gr;
        }
    } else {
        double x = pan;
        float gl = (float)cos(x * MA_PI_D / 2.0), gr = (float)sin(x * MA_PI_D / 2.0);
        for (ma_uint32 i = 0; i < n; i++) {
            float l = src[i * 2], r = src[i * 2 + 1];
            dst[i * 2] = l * gl;
            dst[i * 2 + 1] = r + l * gr;
        }
    }
    *in_count = n;
}
static const ma_node_vtable g_pan_vtable = { pan_process, NULL, 1, 1, 0 };

/* ---- sound handle ------------------------------------------------------ */

typedef struct {
    ma_sound        sound;
    ma_decoder      decoder;
    ma_audio_buffer buffer;      /* raw-PCM sources use this instead */
    StreamSource    stream;      /* queue sources use this instead */
    PanNode         pan;
    void*           data;        /* owned copy of the input, kept alive */
    size_t          data_len;
    int             have_sound;
    int             have_decoder;
    int             have_buffer;
    int             have_stream;
    int             have_pan;
    /* Play count (aether_audio_played_frames). The sound node's local time
     * advances by exactly the frames it produces and is reset to the target
     * by a seek, so "frames played" is that clock summed across the seeks
     * this file issues: `played_base` holds the frames produced before the
     * last seek, `played_mark` the time that seek set. A seek is deferred to
     * the mixer's next pass; while one is pending (the sound's seekTarget is
     * set) the clock still shows the pre-seek reading, which is ignored. */
    ma_uint64       played_base;
    ma_uint64       played_mark;
} AudioSound;

typedef struct {
    ma_sound_group group;
} AudioGroup;

static ma_uint32 pcm_frame_bytes(int sample_rate, int channels, int format);

/* Release everything a handle holds, in dependency order: the sound and pan
 * node detach from the graph first, then the data sources they read, then
 * the bytes those decode. Used by unload and by every failed-load path. */
static void sound_free(AudioSound* s) {
    if (s->have_sound)    ma_sound_uninit(&s->sound);
    if (s->have_pan)      ma_node_uninit(&s->pan, NULL);
    if (s->have_decoder)  ma_decoder_uninit(&s->decoder);
    if (s->have_buffer)   ma_audio_buffer_uninit(&s->buffer);
    if (s->have_stream) {
        ma_pcm_rb_uninit(&s->stream.rb);
        ma_data_source_uninit(&s->stream);
    }
    if (s->data) aether_caps_free(s->data, s->data_len);
    aether_caps_free(s, sizeof(AudioSound));
}

static AudioSound* sound_alloc(void) {
    AudioSound* s = (AudioSound*)aether_caps_malloc(sizeof(AudioSound));
    if (!s) { g_audio_err = "audio: out of memory"; return NULL; }
    memset(s, 0, sizeof(*s));
    return s;
}

/* Copy the caller's bytes into the handle: ma_decoder_init_memory and
 * ma_audio_buffer both read from the pointer for the sound's whole lifetime. */
static int sound_own_data(AudioSound* s, const char* data, int length) {
    s->data = aether_caps_malloc((size_t)length);
    if (!s->data) { g_audio_err = "audio: out of memory"; return 0; }
    memcpy(s->data, data, (size_t)length);
    s->data_len = (size_t)length;
    return 1;
}

/* Build the playable graph over a data source: sound -> pan node -> output.
 * Spatialization is off — the engine owns the model (#2205), pan/pitch are
 * set by number — so the volume the program sets is the volume it gets. */
static int sound_attach(AudioSound* s, ma_data_source* ds) {
    if (ma_sound_init_from_data_source(&g_engine, ds, MA_SOUND_FLAG_NO_SPATIALIZATION,
                                       NULL, &s->sound) != MA_SUCCESS) {
        g_audio_err = "audio: could not create sound";
        return 0;
    }
    s->have_sound = 1;

    ma_uint32 src_ch = 0;
    ma_data_source_get_data_format(ds, NULL, &src_ch, NULL, NULL, 0);
    s->pan.mono = (src_ch == 1);
    ma_atomic_float_set(&s->pan.pan, 0.0f);

    ma_uint32 ch = ma_engine_get_channels(&g_engine);
    ma_node_config nc = ma_node_config_init();
    nc.vtable = &g_pan_vtable;
    nc.pInputChannels = &ch;
    nc.pOutputChannels = &ch;
    if (ma_node_init(ma_engine_get_node_graph(&g_engine), &nc, NULL, &s->pan) != MA_SUCCESS) {
        g_audio_err = "audio: could not create pan node";
        return 0;
    }
    s->have_pan = 1;

    if (ma_node_attach_output_bus(&s->sound, 0, &s->pan, 0) != MA_SUCCESS ||
        ma_node_attach_output_bus(&s->pan, 0, ma_engine_get_endpoint(&g_engine), 0) != MA_SUCCESS) {
        g_audio_err = "audio: could not attach sound to the graph";
        return 0;
    }
    return 1;
}

/* Decode `length` bytes of `data` (wav / mp3 / flac — any format miniaudio
 * recognises) into a playable sound. Returns the handle or NULL, setting
 * g_audio_err on failure. The encoded bytes are copied and owned by the
 * handle so the caller's buffer need not outlive it. */
void* aether_audio_load_wav(const char* data, int length) {
    if (!g_engine_ready) { g_audio_err = "audio: engine not open"; return NULL; }
    if (!data || length <= 0) { g_audio_err = "audio: empty input"; return NULL; }

    AudioSound* s = sound_alloc();
    if (!s) return NULL;
    if (!sound_own_data(s, data, length)) { sound_free(s); return NULL; }

    /* The decoder reads the encoded bytes incrementally as the mixer pulls,
     * so a long track is never decoded whole: this IS the streaming path for
     * music and long ambiences (#2205). */
    if (ma_decoder_init_memory(s->data, s->data_len, NULL, &s->decoder) != MA_SUCCESS) {
        sound_free(s);
        g_audio_err = "audio: unsupported or malformed audio data"; return NULL;
    }
    s->have_decoder = 1;

    if (!sound_attach(s, &s->decoder)) { sound_free(s); return NULL; }

    g_audio_err = "";
    return s;
}

/* Play PCM samples the CALLER already decoded (asks/pcm-please.md).
 *
 * Every other entry point takes an ENCODED container that miniaudio demuxes
 * itself (load_wav is really ma_decoder_init_memory, so it accepts mp3/flac
 * too). That leaves no way in for samples a *different* decoder produced —
 * which is exactly the case when contrib/avcodec has already demuxed an MP4
 * and holds the audio packets. Without this, an app has to pre-extract a
 * sidecar WAV: ~20 MB for a 21 MB clip, a manual step before playback, and no
 * option at all for a live source with no file to extract from.
 *
 * `format` is a MA_FORMAT_* value; the wrapper exposes the useful ones as
 * constants so callers do not hardcode miniaudio's numbering.
 *
 * Uses ma_audio_buffer rather than ma_decoder, fed to the SAME
 * ma_sound_init_from_data_source the encoded path uses — so play/pause/
 * position_ms/duration_ms/seek_ms/volume all work unchanged. They read
 * s->sound, never the decoder, which is why this drops in cleanly.
 * position_ms in particular keeps working as the A/V-sync master clock. */
void* aether_audio_load_pcm(const char* data, int length,
                            int sample_rate, int channels, int format) {
    if (!g_engine_ready) { g_audio_err = "audio: engine not open"; return NULL; }
    if (!data || length <= 0) { g_audio_err = "audio: empty input"; return NULL; }

    ma_uint32 frame_bytes = pcm_frame_bytes(sample_rate, channels, format);
    if (frame_bytes == 0) return NULL;
    ma_format fmt = (ma_format)format;

    /* A partial trailing frame means the caller mis-computed its buffer;
     * refuse rather than play noise off the end of the last frame. */
    if (((size_t)length % frame_bytes) != 0) {
        g_audio_err = "audio: length is not a whole number of frames";
        return NULL;
    }
    ma_uint64 frame_count = (ma_uint64)((size_t)length / frame_bytes);

    AudioSound* s = sound_alloc();
    if (!s) return NULL;
    if (!sound_own_data(s, data, length)) { sound_free(s); return NULL; }

    ma_audio_buffer_config cfg = ma_audio_buffer_config_init(
        fmt, (ma_uint32)channels, frame_count, s->data, NULL);
    cfg.sampleRate = (ma_uint32)sample_rate;

    if (ma_audio_buffer_init(&cfg, &s->buffer) != MA_SUCCESS) {
        sound_free(s);
        g_audio_err = "audio: could not create PCM buffer"; return NULL;
    }
    s->have_buffer = 1;

    if (!sound_attach(s, &s->buffer)) { sound_free(s); return NULL; }

    g_audio_err = "";
    return s;
}

/* Validate a caller-supplied PCM geometry; shared by load_pcm and stream_open.
 * Returns bytes per frame, or 0 with g_audio_err set. */
static ma_uint32 pcm_frame_bytes(int sample_rate, int channels, int format) {
    if (sample_rate <= 0) { g_audio_err = "audio: sample_rate must be > 0"; return 0; }
    if (channels <= 0)    { g_audio_err = "audio: channels must be > 0"; return 0; }
    ma_format fmt = (ma_format)format;
    if (fmt != ma_format_u8  && fmt != ma_format_s16 && fmt != ma_format_s24 &&
        fmt != ma_format_s32 && fmt != ma_format_f32) {
        g_audio_err = "audio: unsupported PCM format"; return 0;
    }
    return ma_get_bytes_per_frame(fmt, (ma_uint32)channels);
}

/* ---- queue source ------------------------------------------------------ */

void* aether_audio_stream_open(int sample_rate, int channels, int format,
                               int capacity_frames) {
    if (!g_engine_ready) { g_audio_err = "audio: engine not open"; return NULL; }
    if (!pcm_frame_bytes(sample_rate, channels, format)) return NULL;
    if (capacity_frames <= 0) { g_audio_err = "audio: capacity must be > 0"; return NULL; }

    AudioSound* s = sound_alloc();
    if (!s) return NULL;

    StreamSource* st = &s->stream;
    st->format   = (ma_format)format;
    st->channels = (ma_uint32)channels;
    st->rate     = (ma_uint32)sample_rate;
    st->capacity = (ma_uint32)capacity_frames;
    ma_atomic_uint64_set(&st->frames_read, 0);
    ma_atomic_uint64_set(&st->underruns, 0);

    ma_data_source_config dc = ma_data_source_config_init();
    dc.vtable = &g_stream_vtable;
    if (ma_data_source_init(&dc, st) != MA_SUCCESS) {
        sound_free(s);
        g_audio_err = "audio: could not create stream"; return NULL;
    }
    if (ma_pcm_rb_init(st->format, st->channels, st->capacity, NULL, NULL, &st->rb) != MA_SUCCESS) {
        ma_data_source_uninit(st);
        sound_free(s);
        g_audio_err = "audio: could not allocate the stream ring"; return NULL;
    }
    ma_pcm_rb_set_sample_rate(&st->rb, st->rate);
    s->have_stream = 1;

    if (!sound_attach(s, st)) { sound_free(s); return NULL; }

    g_audio_err = "";
    return s;
}

int aether_audio_stream_push(void* sound, const char* data, int length) {
    if (!sound) { g_audio_err = "audio: null source"; return -1; }
    AudioSound* s = (AudioSound*)sound;
    if (!s->have_stream) { g_audio_err = "audio: not a stream source"; return -1; }
    if (!data || length < 0) { g_audio_err = "audio: empty input"; return -1; }
    StreamSource* st = &s->stream;
    ma_uint32 bpf = ma_get_bytes_per_frame(st->format, st->channels);
    if ((ma_uint32)length % bpf != 0) {
        g_audio_err = "audio: length is not a whole number of frames"; return -1;
    }
    ma_uint32 total = (ma_uint32)length / bpf;
    ma_uint32 done = 0;
    while (done < total) {
        ma_uint32 want = total - done;
        void* dst = NULL;
        if (ma_pcm_rb_acquire_write(&st->rb, &want, &dst) != MA_SUCCESS || want == 0) break;
        memcpy(dst, data + (size_t)done * bpf, (size_t)want * bpf);
        ma_pcm_rb_commit_write(&st->rb, want);
        done += want;
    }
    g_audio_err = "";
    return (int)done;
}

int aether_audio_stream_queued(void* sound) {
    if (!sound) return 0;
    AudioSound* s = (AudioSound*)sound;
    if (!s->have_stream) return 0;
    return (int)ma_pcm_rb_available_read(&s->stream.rb);
}

int aether_audio_stream_capacity(void* sound) {
    if (!sound) return 0;
    AudioSound* s = (AudioSound*)sound;
    if (!s->have_stream) return 0;
    return (int)s->stream.capacity;
}

int64_t aether_audio_stream_underruns(void* sound) {
    if (!sound) return 0;
    AudioSound* s = (AudioSound*)sound;
    if (!s->have_stream) return 0;
    return (int64_t)ma_atomic_uint64_get(&s->stream.underruns);
}

void aether_audio_unload(void* sound) {
    if (!sound) return;
    sound_free((AudioSound*)sound);
}

/* Record a seek to `target` for the play count: bank the frames produced
 * since the last seek, and remember the reading a not-yet-applied seek will
 * still show. Every seek this file issues goes through here. */
static int sound_seek_pending(AudioSound* s) {
    return ma_atomic_load_64(&s->sound.seekTarget) != MA_SEEK_TARGET_NONE;
}

static void sound_seek(AudioSound* s, ma_uint64 target) {
    if (!sound_seek_pending(s)) {
        ma_uint64 t = ma_sound_get_time_in_pcm_frames(&s->sound);
        if (t >= s->played_mark) s->played_base += t - s->played_mark;
    }
    /* else: the previous seek has not been applied yet; its frames are banked. */
    s->played_mark = target;
    ma_sound_seek_to_pcm_frame(&s->sound, target);
}

int64_t aether_audio_played_frames(void* sound) {
    if (!sound) return 0;
    AudioSound* s = (AudioSound*)sound;
    if (sound_seek_pending(s)) return (int64_t)s->played_base;
    ma_uint64 t = ma_sound_get_time_in_pcm_frames(&s->sound);
    if (t < s->played_mark) return (int64_t)s->played_base;
    return (int64_t)(s->played_base + (t - s->played_mark));
}

/* ---- transport --------------------------------------------------------- */

int aether_audio_play(void* sound) {
    if (!sound) return 0;
    AudioSound* s = (AudioSound*)sound;
    /* If it finished, rewind so play() restarts rather than no-ops. */
    if (ma_sound_at_end(&s->sound)) {
        sound_seek(s, 0);
    }
    return ma_sound_start(&s->sound) == MA_SUCCESS ? 1 : 0;
}

int aether_audio_pause(void* sound) {
    if (!sound) return 0;
    AudioSound* s = (AudioSound*)sound;
    /* ma_sound_stop halts playback but keeps the cursor — this is "pause". */
    return ma_sound_stop(&s->sound) == MA_SUCCESS ? 1 : 0;
}

int aether_audio_stop(void* sound) {
    if (!sound) return 0;
    AudioSound* s = (AudioSound*)sound;
    ma_sound_stop(&s->sound);
    if (!s->have_stream) sound_seek(s, 0);   /* a queue has nothing to rewind to */
    return 1;
}

int aether_audio_is_playing(void* sound) {
    if (!sound) return 0;
    return ma_sound_is_playing(&((AudioSound*)sound)->sound) ? 1 : 0;
}

/* ---- volume ------------------------------------------------------------ */

int aether_audio_set_volume(void* sound, double v) {
    if (!sound) return 0;
    if (v < 0.0) v = 0.0;
    if (v > 1.0) v = 1.0;
    ma_sound_set_volume(&((AudioSound*)sound)->sound, (float)v);
    return 1;
}

double aether_audio_get_volume(void* sound) {
    if (!sound) return 0.0;
    return (double)ma_sound_get_volume(&((AudioSound*)sound)->sound);
}

/* ---- position / duration ---------------------------------------------- */

static int sound_sample_rate(AudioSound* s) {
    ma_uint32 rate = 0;
    ma_sound_get_data_format(&s->sound, NULL, NULL, &rate, NULL, 0);
    return (int)rate;
}

static int64_t frames_to_ns(ma_uint64 frames, int rate) {
    if (rate <= 0) return 0;
    return (int64_t)((double)frames * 1000000000.0 / (double)rate);
}

int64_t aether_audio_position_ns(void* sound) {
    if (!sound) return 0;
    AudioSound* s = (AudioSound*)sound;
    ma_uint64 cur = 0;
    ma_sound_get_cursor_in_pcm_frames(&s->sound, &cur);
    return frames_to_ns(cur, sound_sample_rate(s));
}

int64_t aether_audio_duration_ns(void* sound) {
    if (!sound) return 0;
    AudioSound* s = (AudioSound*)sound;
    ma_uint64 len = 0;
    ma_sound_get_length_in_pcm_frames(&s->sound, &len);
    return frames_to_ns(len, sound_sample_rate(s));
}

int aether_audio_seek_ns(void* sound, int64_t ns) {
    if (!sound) return 0;
    AudioSound* s = (AudioSound*)sound;
    /* ma_sound defers seeks to the next read, so the stream vtable's refusal
     * would never reach the caller; refuse here instead. */
    if (s->have_stream) { g_audio_err = "audio: a stream cannot seek"; return 0; }
    if (ns < 0) ns = 0;
    int rate = sound_sample_rate(s);
    ma_uint64 len = 0;
    ma_sound_get_length_in_pcm_frames(&s->sound, &len);
    ma_uint64 frame = (ma_uint64)((double)ns * (double)rate / 1000000000.0);
    if (frame > len) frame = len;
    sound_seek(s, frame);
    return 1;
}

int aether_audio_channels(void* sound) {
    if (!sound) return 0;
    AudioSound* s = (AudioSound*)sound;
    ma_uint32 ch = 0;
    ma_sound_get_data_format(&s->sound, NULL, &ch, NULL, NULL, 0);
    return (int)ch;
}

int aether_audio_sample_rate(void* sound) {
    if (!sound) return 0;
    return sound_sample_rate((AudioSound*)sound);
}

/* ---- pan / pitch / looping / fade ------------------------------------- */

int aether_audio_set_pan(void* sound, double pan) {
    if (!sound) return 0;
    if (pan < -1.0) pan = -1.0;
    if (pan >  1.0) pan =  1.0;
    ma_atomic_float_set(&((AudioSound*)sound)->pan.pan, (float)pan);
    return 1;
}

double aether_audio_get_pan(void* sound) {
    if (!sound) return 0.0;
    return (double)ma_atomic_float_get(&((AudioSound*)sound)->pan.pan);
}

int aether_audio_set_pitch(void* sound, double ratio) {
    if (!sound) return 0;
    if (!(ratio > 0.0)) { g_audio_err = "audio: pitch ratio must be > 0"; return 0; }
    ma_sound_set_pitch(&((AudioSound*)sound)->sound, (float)ratio);
    return 1;
}

double aether_audio_get_pitch(void* sound) {
    if (!sound) return 1.0;
    return (double)ma_sound_get_pitch(&((AudioSound*)sound)->sound);
}

int aether_audio_set_looping(void* sound, int on) {
    if (!sound) return 0;
    AudioSound* s = (AudioSound*)sound;
    if (s->have_stream) { g_audio_err = "audio: a stream cannot loop"; return 0; }
    ma_sound_set_looping(&s->sound, on ? MA_TRUE : MA_FALSE);
    return 1;
}

int aether_audio_is_looping(void* sound) {
    if (!sound) return 0;
    return ma_sound_is_looping(&((AudioSound*)sound)->sound) ? 1 : 0;
}

int aether_audio_fade_ms(void* sound, double from, double to, int64_t ms) {
    if (!sound) return 0;
    if (ms < 0) { g_audio_err = "audio: fade length must be >= 0"; return 0; }
    if (from > 1.0) from = 1.0;
    if (from < 0.0 && from != -1.0) from = 0.0;
    if (to < 0.0) to = 0.0;
    if (to > 1.0) to = 1.0;
    ma_sound_set_fade_in_milliseconds(&((AudioSound*)sound)->sound,
                                      (float)from, (float)to, (ma_uint64)ms);
    return 1;
}

/* ---- groups ------------------------------------------------------------ */

void* aether_audio_group_new(void) {
    if (!g_engine_ready) { g_audio_err = "audio: engine not open"; return NULL; }
    AudioGroup* g = (AudioGroup*)aether_caps_malloc(sizeof(AudioGroup));
    if (!g) { g_audio_err = "audio: out of memory"; return NULL; }
    memset(g, 0, sizeof(*g));
    if (ma_sound_group_init(&g_engine, MA_SOUND_FLAG_NO_SPATIALIZATION, NULL, &g->group)
        != MA_SUCCESS) {
        aether_caps_free(g, sizeof(AudioGroup));
        g_audio_err = "audio: could not create group"; return NULL;
    }
    g_audio_err = "";
    return g;
}

void aether_audio_group_free(void* group) {
    if (!group) return;
    AudioGroup* g = (AudioGroup*)group;
    ma_sound_group_uninit(&g->group);
    aether_caps_free(g, sizeof(AudioGroup));
}

int aether_audio_group_set_volume(void* group, double v) {
    if (!group) return 0;
    if (v < 0.0) v = 0.0;
    if (v > 1.0) v = 1.0;
    ma_sound_group_set_volume(&((AudioGroup*)group)->group, (float)v);
    return 1;
}

double aether_audio_group_get_volume(void* group) {
    if (!group) return 0.0;
    return (double)ma_sound_group_get_volume(&((AudioGroup*)group)->group);
}

int aether_audio_set_group(void* sound, void* group) {
    if (!sound) return 0;
    AudioSound* s = (AudioSound*)sound;
    ma_node* target = group ? (ma_node*)&((AudioGroup*)group)->group
                            : ma_engine_get_endpoint(&g_engine);
    return ma_node_attach_output_bus(&s->pan, 0, target, 0) == MA_SUCCESS ? 1 : 0;
}

#endif /* __wasi__ */
