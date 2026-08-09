/* std.audio playback substrate — see aether_audio.h.
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

#define MINIAUDIO_IMPLEMENTATION
/* Trim the build: we only need decoding + playback, no capture/encoding/
 * resource-manager niceties beyond what ma_engine needs by default. */
#define MA_NO_ENCODING
#include "miniaudio.h"

/* ---- engine ------------------------------------------------------------ */

static ma_engine g_engine;
static ma_context g_null_ctx;
static int g_engine_ready = 0;
static int g_is_null = 0;
static const char* g_audio_err = "";

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

void aether_audio_close(void) {
    if (!g_engine_ready) return;
    ma_engine_uninit(&g_engine);
    if (g_is_null) ma_context_uninit(&g_null_ctx);
    g_engine_ready = 0;
}

int aether_audio_is_null_backend(void) {
    return g_is_null;
}

const char* aether_audio_last_error(void) {
    return g_audio_err;
}

/* ---- sound handle ------------------------------------------------------ */

typedef struct {
    ma_sound        sound;
    ma_decoder      decoder;
    ma_audio_buffer buffer;      /* raw-PCM sources use this instead */
    void*           data;        /* owned copy of the input, kept alive */
    size_t          data_len;
    int             have_sound;
    int             have_decoder;
    int             have_buffer;
} AudioSound;

/* Decode `length` bytes of `data` (wav / mp3 / flac — any format miniaudio
 * recognises) into a playable sound. Returns the handle or NULL, setting
 * g_audio_err on failure. The encoded bytes are copied and owned by the
 * handle so the caller's buffer need not outlive it. */
void* aether_audio_load_wav(const char* data, int length) {
    if (!g_engine_ready) { g_audio_err = "audio: engine not open"; return NULL; }
    if (!data || length <= 0) { g_audio_err = "audio: empty input"; return NULL; }

    AudioSound* s = (AudioSound*)aether_caps_malloc(sizeof(AudioSound));
    if (!s) { g_audio_err = "audio: out of memory"; return NULL; }
    memset(s, 0, sizeof(*s));

    /* Own a copy of the encoded data — ma_decoder_init_memory does not copy,
     * it reads from the pointer for the sound's whole lifetime. */
    s->data = aether_caps_malloc((size_t)length);
    if (!s->data) {
        aether_caps_free(s, sizeof(AudioSound));
        g_audio_err = "audio: out of memory"; return NULL;
    }
    memcpy(s->data, data, (size_t)length);
    s->data_len = (size_t)length;

    if (ma_decoder_init_memory(s->data, s->data_len, NULL, &s->decoder) != MA_SUCCESS) {
        aether_caps_free(s->data, s->data_len);
        aether_caps_free(s, sizeof(AudioSound));
        g_audio_err = "audio: unsupported or malformed audio data"; return NULL;
    }
    s->have_decoder = 1;

    if (ma_sound_init_from_data_source(&g_engine, &s->decoder, 0, NULL, &s->sound)
        != MA_SUCCESS) {
        ma_decoder_uninit(&s->decoder);
        aether_caps_free(s->data, s->data_len);
        aether_caps_free(s, sizeof(AudioSound));
        g_audio_err = "audio: could not create sound"; return NULL;
    }
    s->have_sound = 1;

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
    if (sample_rate <= 0) { g_audio_err = "audio: sample_rate must be > 0"; return NULL; }
    if (channels <= 0)    { g_audio_err = "audio: channels must be > 0"; return NULL; }

    ma_format fmt = (ma_format)format;
    if (fmt != ma_format_u8  && fmt != ma_format_s16 && fmt != ma_format_s24 &&
        fmt != ma_format_s32 && fmt != ma_format_f32) {
        g_audio_err = "audio: unsupported PCM format"; return NULL;
    }

    /* Bytes per frame = bytes per sample * channels. A partial trailing frame
     * means the caller mis-computed its buffer; refuse rather than play noise
     * off the end of the last frame. */
    ma_uint32 bps = ma_get_bytes_per_sample(fmt);
    size_t frame_bytes = (size_t)bps * (size_t)channels;
    if (frame_bytes == 0 || ((size_t)length % frame_bytes) != 0) {
        g_audio_err = "audio: length is not a whole number of frames";
        return NULL;
    }
    ma_uint64 frame_count = (ma_uint64)((size_t)length / frame_bytes);

    AudioSound* s = (AudioSound*)aether_caps_malloc(sizeof(AudioSound));
    if (!s) { g_audio_err = "audio: out of memory"; return NULL; }
    memset(s, 0, sizeof(*s));

    /* Own a copy: ma_audio_buffer with a config-supplied pointer reads from it
     * for the sound's whole lifetime, exactly as ma_decoder_init_memory does. */
    s->data = aether_caps_malloc((size_t)length);
    if (!s->data) {
        aether_caps_free(s, sizeof(AudioSound));
        g_audio_err = "audio: out of memory"; return NULL;
    }
    memcpy(s->data, data, (size_t)length);
    s->data_len = (size_t)length;

    ma_audio_buffer_config cfg = ma_audio_buffer_config_init(
        fmt, (ma_uint32)channels, frame_count, s->data, NULL);
    cfg.sampleRate = (ma_uint32)sample_rate;

    if (ma_audio_buffer_init(&cfg, &s->buffer) != MA_SUCCESS) {
        aether_caps_free(s->data, s->data_len);
        aether_caps_free(s, sizeof(AudioSound));
        g_audio_err = "audio: could not create PCM buffer"; return NULL;
    }
    s->have_buffer = 1;

    if (ma_sound_init_from_data_source(&g_engine, &s->buffer, 0, NULL, &s->sound)
        != MA_SUCCESS) {
        ma_audio_buffer_uninit(&s->buffer);
        aether_caps_free(s->data, s->data_len);
        aether_caps_free(s, sizeof(AudioSound));
        g_audio_err = "audio: could not create sound"; return NULL;
    }
    s->have_sound = 1;

    g_audio_err = "";
    return s;
}

void aether_audio_unload(void* sound) {
    if (!sound) return;
    AudioSound* s = (AudioSound*)sound;
    if (s->have_sound)   ma_sound_uninit(&s->sound);
    if (s->have_decoder) ma_decoder_uninit(&s->decoder);
    if (s->have_buffer)  ma_audio_buffer_uninit(&s->buffer);
    if (s->data) aether_caps_free(s->data, s->data_len);
    aether_caps_free(s, sizeof(AudioSound));
}

/* ---- transport --------------------------------------------------------- */

int aether_audio_play(void* sound) {
    if (!sound) return 0;
    AudioSound* s = (AudioSound*)sound;
    /* If it finished, rewind so play() restarts rather than no-ops. */
    if (ma_sound_at_end(&s->sound)) {
        ma_sound_seek_to_pcm_frame(&s->sound, 0);
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
    ma_sound_seek_to_pcm_frame(&s->sound, 0);
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
    if (ns < 0) ns = 0;
    int rate = sound_sample_rate(s);
    ma_uint64 len = 0;
    ma_sound_get_length_in_pcm_frames(&s->sound, &len);
    ma_uint64 frame = (ma_uint64)((double)ns * (double)rate / 1000000000.0);
    if (frame > len) frame = len;
    return ma_sound_seek_to_pcm_frame(&s->sound, frame) == MA_SUCCESS ? 1 : 0;
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
