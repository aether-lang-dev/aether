/* std.audio — playback substrate (v1) + mixer tier (#2205).
 *
 * The device/decode layer is C by necessity — the runtime-stays-C hard
 * line (docs/cross-references/audio.md §6): a real audio backend pulls
 * samples on a realtime thread that must not allocate or block, so no
 * Aether-emitted code can run there. This header is the FFI surface the
 * `std.audio` module binds; the implementation carries a self-contained
 * WAV decoder and a NULL (silent, deterministic) backend that is the
 * default and the CI/test path. A real device backend (ALSA/CoreAudio/
 * WASAPI, or vendored miniaudio) slots in behind the same surface without
 * changing the Aether API. */
#ifndef AETHER_AUDIO_H
#define AETHER_AUDIO_H

#include <stdint.h>

/* Engine lifecycle. open() picks a backend (null unless a real one is
 * compiled in and available); returns 1 on success, 0 on failure. */
int  aether_audio_open(void);
void aether_audio_close(void);
/* 1 if the active backend is the null (silent) backend — lets tests and
 * headless callers assert deterministic behaviour. */
int  aether_audio_is_null_backend(void);

/* Decode `length` bytes of `data` (a WAV byte string) into a new sound
 * handle. Returns the handle (opaque pointer) or NULL on failure; the
 * failure reason is available via aether_audio_last_error(). The caller
 * owns the handle until aether_audio_unload(). */
void* aether_audio_load_wav(const char* data, int length);
const char* aether_audio_last_error(void);
void  aether_audio_unload(void* sound);

/* Transport. play() starts/resumes; pause() halts keeping position;
 * stop() halts and rewinds to 0. Each returns 1 on success, 0 on a null
 * handle. */
int aether_audio_play(void* sound);
int aether_audio_pause(void* sound);
int aether_audio_stop(void* sound);
int aether_audio_is_playing(void* sound);

/* Volume in [0.0, 1.0] (clamped). */
int    aether_audio_set_volume(void* sound, double v);
double aether_audio_get_volume(void* sound);

/* Position / duration in FRAMES and in NANOSECONDS (for Aether's
 * Duration). seek() clamps to [0, total_frames]. */
int64_t aether_audio_position_ns(void* sound);
int64_t aether_audio_duration_ns(void* sound);
int     aether_audio_seek_ns(void* sound, int64_t ns);

int aether_audio_channels(void* sound);
int aether_audio_sample_rate(void* sound);

/* ---- mixer tier (#2205): what a game engine's mixer needs per source ---- */

/* Pan in [-1.0, 1.0] (clamped), equal-power (Web Audio StereoPannerNode
 * law): a mono source at 0 sits centred at -3 dB in each ear, at -1 wholly
 * in the left; a stereo source passes through unchanged at 0 and folds the
 * far channel into the near one as it moves. Applied in a C node between
 * the sound and its group, on the mixer thread. Only the first two output
 * channels are panned. */
int    aether_audio_set_pan(void* sound, double pan);
double aether_audio_get_pan(void* sound);

/* Playback-rate ratio (> 0): 2.0 plays an octave up at double speed, 0.5 an
 * octave down at half. A ratio <= 0 is refused. */
int    aether_audio_set_pitch(void* sound, double ratio);
double aether_audio_get_pitch(void* sound);

/* Loop back to the start on reaching the end (engines, rain, wind). */
int aether_audio_set_looping(void* sound, int on);
int aether_audio_is_looping(void* sound);

/* Fade the volume from `from` to `to` over `ms`, starting now. A `from` of
 * -1.0 means "from wherever the volume is now". */
int aether_audio_fade_ms(void* sound, double from, double to, int64_t ms);

/* Frames of the SOURCE the mixer has consumed since load, across loops —
 * the "frames it would have played" a headless test holds a mix to. */
int64_t aether_audio_played_frames(void* sound);

/* Groups / buses (music, effects, ambience, voice): a volume applied to
 * every source assigned to the group. A group must outlive the sources
 * assigned to it: reassign or unload them before freeing it. */
void*  aether_audio_group_new(void);
void   aether_audio_group_free(void* group);
int    aether_audio_group_set_volume(void* group, double v);
double aether_audio_group_get_volume(void* group);
/* Route `sound` through `group`; NULL routes it straight to the output. */
int    aether_audio_set_group(void* sound, void* group);

/* Queue source: a bounded ring of PCM the program pushes into as the sound
 * plays (music from a decoder, a procedural wind). `capacity_frames` bounds
 * the ring. Reads past what has been pushed play silence and are counted as
 * underruns rather than ending the sound. Not seekable; duration is 0. */
void*   aether_audio_stream_open(int sample_rate, int channels, int format,
                                 int capacity_frames);
/* Push `length` bytes of interleaved samples (whole frames only). Returns
 * the number of FRAMES accepted (fewer than offered when the ring is
 * full), or -1 and sets last_error on a bad argument. */
int     aether_audio_stream_push(void* sound, const char* data, int length);
int     aether_audio_stream_queued(void* sound);
int     aether_audio_stream_capacity(void* sound);
int64_t aether_audio_stream_underruns(void* sound);

/* Headless engine: no device, no mixer thread, nothing advances until
 * render() pulls frames through the mixer. Deterministic — the CI path for
 * holding a mix to numbers. is_null_backend() is also true here. */
int aether_audio_open_headless(int sample_rate, int channels);
int aether_audio_is_headless(void);
/* Mix `frames` frames of output (headless only). Returns frames rendered.
 * The rendered block is kept for rendered_peak() / rendered_sample(). */
int    aether_audio_render(int frames);
double aether_audio_rendered_peak(int channel);
double aether_audio_rendered_sample(int frame, int channel);

#endif /* AETHER_AUDIO_H */
