/* contrib/xmp — a tracker-module player for std.audio, over libxmp.
 *
 * libxmp (https://github.com/libxmp/libxmp, MIT; see NOTICE) loads and
 * renders MOD, S3M, XM, IT, MED and the other formats it reads. This file is
 * the thin C side of contrib.xmp: one XmpPlayer per module, holding its own
 * libxmp context, render buffer and error, so two players never share state
 * and nothing here is a mutable global.
 *
 * Surface (module.ae wraps it; the raw names are not for callers):
 *   aether_xmp_new()                       -> XmpPlayer*   (NULL out of memory)
 *   aether_xmp_free(p)                     -> void         (NULL is a no-op)
 *   aether_xmp_load_memory(p, data, n)     -> int          (0, or -XMP_ERROR_*)
 *   aether_xmp_load_path(p, path)          -> int
 *   aether_xmp_strerror(code)              -> const char*  (static text)
 *   aether_xmp_title / format(p)           -> AetherString* (owned copies)
 *   aether_xmp_channels / patterns / orders / instruments / samples(p) -> int
 *   aether_xmp_duration_ms(p)              -> int
 *   aether_xmp_start(p, rate)              -> int          (0, or -XMP_ERROR_*)
 *   aether_xmp_render(p, frames)           -> int          (bytes in the buffer, or -code)
 *   aether_xmp_render_all(p)               -> int          (the same, to the end)
 *   aether_xmp_buffer(p)                   -> void*        (the bytes render left)
 *   aether_xmp_seek_ms(p, ms) / set_order(p, order) -> int
 *   aether_xmp_position_ms / order / row(p) -> int
 *   aether_xmp_set_looping(p, on) / is_looping / ended(p) -> int
 *
 * PCM is interleaved signed 16-bit stereo in the host's byte order (every
 * target Aether builds for is little-endian: std.audio's FORMAT_S16).
 *
 * Threading: a player is used by one thread at a time. render() does all the
 * work synchronously on the caller's thread; nothing here starts a thread or
 * needs pumping. Feeding std.audio is the caller pushing rendered blocks into
 * a stream source (module.ae's pump), whose ring the mixer thread drains.
 */

#include <limits.h>
#include <stdlib.h>
#include <string.h>

#include <xmp.h>

#include "../../std/string/aether_string.h"

#define XMP_BYTES_PER_FRAME 4          /* 16-bit stereo */
/* render(): one call's ceiling, std.audio's own render limit. */
#define XMP_MAX_BLOCK_FRAMES 4194304
/* render_all(): a module that never ends (its scan found no end and looping
 * is off, which libxmp should not allow, or a pathological one) stops here
 * rather than filling memory: 30 minutes at the highest rate fits an int. */
#define XMP_MAX_ALL_MS (30 * 60 * 1000)

typedef struct XmpPlayer {
    xmp_context ctx;
    int loaded;        /* a module is loaded */
    int playing;       /* start() succeeded */
    int rate;          /* sample rate start() was given */
    int looping;       /* 1: wrap at the end; 0: stop there */
    int ended;         /* not looping, and the end was reached */
    int loop_base;     /* libxmp's loop_count when end detection was armed */
    /* The part of libxmp's last frame not yet handed out. */
    const unsigned char* left;
    int left_bytes;
    /* The block render() fills; grown as needed, freed with the player. */
    unsigned char* buf;
    size_t buf_cap;
} XmpPlayer;

void* aether_xmp_new(void) {
    XmpPlayer* p = (XmpPlayer*)calloc(1, sizeof(XmpPlayer));
    if (!p) return NULL;
    p->ctx = xmp_create_context();
    if (!p->ctx) { free(p); return NULL; }
    return p;
}

void aether_xmp_free(void* vp) {
    XmpPlayer* p = (XmpPlayer*)vp;
    if (!p) return;
    if (p->playing) xmp_end_player(p->ctx);
    if (p->loaded) xmp_release_module(p->ctx);
    xmp_free_context(p->ctx);
    free(p->buf);
    free(p);
}

const char* aether_xmp_strerror(int code) {
    switch (code < 0 ? -code : code) {
    case 0:                  return "";
    case XMP_END:            return "xmp: the module has ended";
    case XMP_ERROR_INTERNAL: return "xmp: internal error";
    case XMP_ERROR_FORMAT:   return "xmp: not a module format libxmp recognises";
    case XMP_ERROR_LOAD:     return "xmp: the module is damaged or truncated";
    case XMP_ERROR_DEPACK:   return "xmp: the packed module could not be unpacked";
    case XMP_ERROR_SYSTEM:   return "xmp: system error (cannot read the file, or out of memory)";
    case XMP_ERROR_INVALID:  return "xmp: invalid argument";
    case XMP_ERROR_STATE:    return "xmp: the player is not in a state for that (load, then start)";
    default:                 return "xmp: error";
    }
}

static void xmp_unload(XmpPlayer* p) {
    if (p->playing) { xmp_end_player(p->ctx); p->playing = 0; }
    if (p->loaded) { xmp_release_module(p->ctx); p->loaded = 0; }
    p->left = NULL; p->left_bytes = 0; p->ended = 0;
}

/* `data` is declared `@aether string` (docs/c-interop.md, #3297): the
 * counted string itself, so its own length bounds the read and a module's
 * bytes are never mistaken for a string header. */
int aether_xmp_load_memory(void* vp, const void* data, int length) {
    XmpPlayer* p = (XmpPlayer*)vp;
    if (!p) return -XMP_ERROR_INVALID;
    const char* bytes = data ? aether_string_data(data) : NULL;
    size_t have = data ? aether_string_length(data) : 0;
    if (length > 0 && (size_t)length > have) length = (int)have;
    if (!bytes || length <= 0) return -XMP_ERROR_FORMAT;   /* nothing to recognise */
    xmp_unload(p);
    int rc = xmp_load_module_from_memory(p->ctx, bytes, (long)length);
    if (rc < 0) return rc;
    p->loaded = 1;
    return 0;
}

/* `path` is a plain `string`: its characters. */
int aether_xmp_load_path(void* vp, const char* path) {
    XmpPlayer* p = (XmpPlayer*)vp;
    if (!p) return -XMP_ERROR_INVALID;
    if (!path || !path[0]) return -XMP_ERROR_INVALID;
    xmp_unload(p);
    int rc = xmp_load_module(p->ctx, path);
    if (rc < 0) return rc;
    p->loaded = 1;
    return 0;
}

static struct xmp_module* xmp_mod(XmpPlayer* p, struct xmp_module_info* mi) {
    if (!p || !p->loaded) return NULL;
    xmp_get_module_info(p->ctx, mi);
    return mi->mod;
}

void* aether_xmp_title(void* vp) {
    struct xmp_module_info mi;
    struct xmp_module* m = xmp_mod((XmpPlayer*)vp, &mi);
    return string_new(m ? m->name : "");
}

void* aether_xmp_format(void* vp) {
    struct xmp_module_info mi;
    struct xmp_module* m = xmp_mod((XmpPlayer*)vp, &mi);
    return string_new(m ? m->type : "");
}

int aether_xmp_channels(void* vp) {
    struct xmp_module_info mi;
    struct xmp_module* m = xmp_mod((XmpPlayer*)vp, &mi);
    return m ? m->chn : 0;
}

int aether_xmp_patterns(void* vp) {
    struct xmp_module_info mi;
    struct xmp_module* m = xmp_mod((XmpPlayer*)vp, &mi);
    return m ? m->pat : 0;
}

int aether_xmp_orders(void* vp) {
    struct xmp_module_info mi;
    struct xmp_module* m = xmp_mod((XmpPlayer*)vp, &mi);
    return m ? m->len : 0;
}

int aether_xmp_instruments(void* vp) {
    struct xmp_module_info mi;
    struct xmp_module* m = xmp_mod((XmpPlayer*)vp, &mi);
    return m ? m->ins : 0;
}

int aether_xmp_samples(void* vp) {
    struct xmp_module_info mi;
    struct xmp_module* m = xmp_mod((XmpPlayer*)vp, &mi);
    return m ? m->smp : 0;
}

/* The length of the module's first (main) sequence, as libxmp's scan
 * measured it when the module loaded. */
int aether_xmp_duration_ms(void* vp) {
    struct xmp_module_info mi;
    struct xmp_module* m = xmp_mod((XmpPlayer*)vp, &mi);
    if (!m || mi.num_sequences < 1 || !mi.seq_data) return 0;
    return mi.seq_data[0].duration;
}

/* Arm end detection from where the player stands now: the end is the next
 * time libxmp's loop counter moves. */
static void xmp_arm_end(XmpPlayer* p) {
    struct xmp_frame_info fi;
    xmp_get_frame_info(p->ctx, &fi);
    p->loop_base = fi.loop_count;
    p->ended = 0;
}

int aether_xmp_start(void* vp, int rate) {
    XmpPlayer* p = (XmpPlayer*)vp;
    if (!p) return -XMP_ERROR_INVALID;
    if (!p->loaded) return -XMP_ERROR_STATE;
    if (rate < XMP_MIN_SRATE || rate > XMP_MAX_SRATE) return -XMP_ERROR_INVALID;
    if (p->playing) { xmp_end_player(p->ctx); p->playing = 0; }
    int rc = xmp_start_player(p->ctx, rate, 0);   /* 16-bit signed stereo */
    if (rc < 0) return rc;
    p->playing = 1;
    p->rate = rate;
    p->left = NULL; p->left_bytes = 0;
    xmp_arm_end(p);
    return 0;
}

static int xmp_reserve(XmpPlayer* p, size_t bytes) {
    if (bytes <= p->buf_cap) return 1;
    size_t cap = p->buf_cap ? p->buf_cap : 16384;
    while (cap < bytes) cap *= 2;
    unsigned char* nb = (unsigned char*)realloc(p->buf, cap);
    if (!nb) return 0;
    p->buf = nb;
    p->buf_cap = cap;
    return 1;
}

/* Append up to `want` bytes of PCM to p->buf at `at`, playing libxmp frames
 * as needed. Returns the bytes written; fewer than asked only at the end of
 * a module that is not looping (p->ended is then set). */
static int xmp_fill(XmpPlayer* p, size_t at, int want) {
    int done = 0;
    while (done < want) {
        if (p->left_bytes == 0) {
            if (p->ended) break;
            if (xmp_play_frame(p->ctx) < 0) { p->ended = 1; break; }
            struct xmp_frame_info fi;
            xmp_get_frame_info(p->ctx, &fi);
            /* A frame played after the loop counter moved is the module
             * starting over: with looping off, the end came first. */
            if (!p->looping && fi.loop_count > p->loop_base) { p->ended = 1; break; }
            p->left = (const unsigned char*)fi.buffer;
            p->left_bytes = fi.buffer_size;
            if (p->left_bytes <= 0) { p->left_bytes = 0; continue; }
        }
        int n = want - done;
        if (n > p->left_bytes) n = p->left_bytes;
        memcpy(p->buf + at + (size_t)done, p->left, (size_t)n);
        p->left += n;
        p->left_bytes -= n;
        done += n;
    }
    return done;
}

int aether_xmp_render(void* vp, int frames) {
    XmpPlayer* p = (XmpPlayer*)vp;
    if (!p) return -XMP_ERROR_INVALID;
    if (!p->playing) return -XMP_ERROR_STATE;
    if (frames < 0 || frames > XMP_MAX_BLOCK_FRAMES) return -XMP_ERROR_INVALID;
    int want = frames * XMP_BYTES_PER_FRAME;
    if (!xmp_reserve(p, (size_t)want + 1)) return -XMP_ERROR_SYSTEM;
    return xmp_fill(p, 0, want);
}

int aether_xmp_render_all(void* vp) {
    XmpPlayer* p = (XmpPlayer*)vp;
    if (!p) return -XMP_ERROR_INVALID;
    if (!p->playing) return -XMP_ERROR_STATE;
    int was_looping = p->looping;
    if (was_looping) { p->looping = 0; xmp_arm_end(p); }
    long long cap = (long long)XMP_MAX_ALL_MS * p->rate / 1000 * XMP_BYTES_PER_FRAME;
    if (cap > INT_MAX - 1) cap = INT_MAX - 1;
    size_t total = 0;
    int rc = 0;
    const int chunk = 65536 * XMP_BYTES_PER_FRAME;
    while (!p->ended) {
        if ((long long)total + chunk > cap) { rc = -XMP_ERROR_INVALID; break; }
        if (!xmp_reserve(p, total + (size_t)chunk + 1)) { rc = -XMP_ERROR_SYSTEM; break; }
        total += (size_t)xmp_fill(p, total, chunk);
    }
    if (was_looping) { p->looping = 1; xmp_arm_end(p); }
    return rc < 0 ? rc : (int)total;
}

void* aether_xmp_buffer(void* vp) {
    XmpPlayer* p = (XmpPlayer*)vp;
    return p ? p->buf : NULL;
}

int aether_xmp_seek_ms(void* vp, int ms) {
    XmpPlayer* p = (XmpPlayer*)vp;
    if (!p) return -XMP_ERROR_INVALID;
    if (!p->playing) return -XMP_ERROR_STATE;
    if (ms < 0) ms = 0;
    int rc = xmp_seek_time_frame(p->ctx, ms);
    if (rc < 0) return rc;
    p->left = NULL; p->left_bytes = 0;
    xmp_arm_end(p);
    return 0;
}

int aether_xmp_set_order(void* vp, int order) {
    XmpPlayer* p = (XmpPlayer*)vp;
    if (!p) return -XMP_ERROR_INVALID;
    if (!p->playing) return -XMP_ERROR_STATE;
    if (order < 0 || order >= aether_xmp_orders(p)) return -XMP_ERROR_INVALID;
    /* The new position, or -1 for order 0, which libxmp marks as a restart
     * (the same value as -XMP_END, so it cannot be read as an error). */
    int rc = xmp_set_position(p->ctx, order);
    if (rc < -1) return rc;
    p->left = NULL; p->left_bytes = 0;
    xmp_arm_end(p);
    return 0;
}

/* Where the next rendered frame is, in the module's time: libxmp's clock
 * stands after the frame it last played, less what of that frame is still
 * waiting to be handed out. */
int aether_xmp_position_ms(void* vp) {
    XmpPlayer* p = (XmpPlayer*)vp;
    if (!p || !p->playing) return 0;
    struct xmp_frame_info fi;
    xmp_get_frame_info(p->ctx, &fi);
    long long t = fi.time;
    if (p->rate > 0) t -= (long long)p->left_bytes / XMP_BYTES_PER_FRAME * 1000 / p->rate;
    return t < 0 ? 0 : (int)t;
}

int aether_xmp_order(void* vp) {
    XmpPlayer* p = (XmpPlayer*)vp;
    if (!p || !p->playing) return 0;
    struct xmp_frame_info fi;
    xmp_get_frame_info(p->ctx, &fi);
    return fi.pos;
}

int aether_xmp_row(void* vp) {
    XmpPlayer* p = (XmpPlayer*)vp;
    if (!p || !p->playing) return 0;
    struct xmp_frame_info fi;
    xmp_get_frame_info(p->ctx, &fi);
    return fi.row;
}

int aether_xmp_set_looping(void* vp, int on) {
    XmpPlayer* p = (XmpPlayer*)vp;
    if (!p) return -XMP_ERROR_INVALID;
    p->looping = on ? 1 : 0;
    /* Looping on after the end carries on from the top; looping off re-arms
     * the end from here, but leaves a module that already ended ended. */
    if (p->playing && (on || !p->ended)) xmp_arm_end(p);
    return 0;
}

int aether_xmp_is_looping(void* vp) {
    XmpPlayer* p = (XmpPlayer*)vp;
    return p ? p->looping : 0;
}

int aether_xmp_ended(void* vp) {
    XmpPlayer* p = (XmpPlayer*)vp;
    return (p && p->ended) ? 1 : 0;
}
