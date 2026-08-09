/* contrib/avcodec — thin FFmpeg video-decode veneer for Aether.
 *
 * The narrowest surface that removes the intermediate file: open a media
 * source, pull decoded frames as packed RGBA8888, close. Deliberately video-
 * only and deliberately not a media framework — audio, seeking, filtering and
 * stream selection are all future work, and none of them are needed to feed a
 * vg LIVE_RASTER region.
 *
 *   avc_open_raw(url, want_w, want_h)   -> Decoder*  (NULL on failure)
 *   avc_width_raw(d) / avc_height_raw(d)-> int       (the SCALED output size)
 *   avc_frame_bytes_raw(d)              -> int       (w*h*4)
 *   avc_fps_num_raw(d) / avc_fps_den_raw(d) -> int   (source rate as a ratio)
 *   avc_try_next_frame(d)               -> int       (1 = frame ready, 0 = EOF/error)
 *   avc_get_frame_bytes()               -> const char*  (TLS slot from last try_)
 *   avc_get_frame_length()              -> int
 *   avc_release_frame()                 -> void      (free early; else next try_ frees)
 *   avc_copy_frame_into_raw(d, buf, cap)-> int       (bytes written; 0 on failure)
 *   avc_pts_ms_raw(d)                   -> int       (presentation time of the last frame)
 *   avc_error_raw(d)                    -> const char*  (always non-NULL)
 *   avc_close_raw(d)                    -> void
 *
 * The try_/get_/release_ trio mirrors contrib/sqlite's blob accessors, so
 * binary data crosses the boundary the same way it already does elsewhere.
 * avc_copy_frame_into_raw is the zero-allocation path: it writes straight
 * into a caller-owned buffer, which is what a per-frame video loop wants —
 * at 1080p a frame is 8 MB and allocating one per frame at 30fps is 250 MB/s
 * of churn.
 *
 * A C-only dependency, like contrib/sqlite: user programs link
 * -lavcodec -lavformat -lavutil -lswscale via aether.toml's link_flags.
 * Nothing is vendored here.
 */

#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/imgutils.h>
#include <libswscale/swscale.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    AVFormatContext* fmt;
    AVCodecContext*  dec;
    struct SwsContext* sws;
    AVFrame*   frame;      /* decoded, source pixel format */
    AVFrame*   rgba;       /* converted, packed RGBA8888   */
    AVPacket*  pkt;
    uint8_t*   rgba_buf;   /* backing store for `rgba`     */
    int        stream_idx;
    int        out_w, out_h;
    int        fps_num, fps_den;
    long       pts_ms;
    char       err[256];
} Decoder;

void avc_close_raw(void* h);   /* used by the open path's error unwind */

/* TLS frame slot — same shape as sqlite's blob slot. */
static _Thread_local char* g_frame_bytes = NULL;
static _Thread_local int   g_frame_len   = 0;

static void free_frame_tls(void) {
    if (g_frame_bytes) { free(g_frame_bytes); g_frame_bytes = NULL; }
    g_frame_len = 0;
}

static void set_err(Decoder* d, const char* msg) {
    if (!d) return;
    snprintf(d->err, sizeof(d->err), "%s", msg ? msg : "");
}

void* avc_open_raw(const char* url, int want_w, int want_h) {
    if (!url) return NULL;
    Decoder* d = (Decoder*)calloc(1, sizeof(Decoder));
    if (!d) return NULL;
    d->stream_idx = -1;

    if (avformat_open_input(&d->fmt, url, NULL, NULL) < 0) {
        set_err(d, "cannot open input");
        free(d);
        return NULL;
    }
    if (avformat_find_stream_info(d->fmt, NULL) < 0) {
        set_err(d, "no stream info");
        avformat_close_input(&d->fmt);
        free(d);
        return NULL;
    }

    const AVCodec* codec = NULL;
    d->stream_idx = av_find_best_stream(d->fmt, AVMEDIA_TYPE_VIDEO, -1, -1, &codec, 0);
    if (d->stream_idx < 0 || !codec) {
        set_err(d, "no video stream");
        avformat_close_input(&d->fmt);
        free(d);
        return NULL;
    }

    AVStream* st = d->fmt->streams[d->stream_idx];
    d->dec = avcodec_alloc_context3(codec);
    if (!d->dec || avcodec_parameters_to_context(d->dec, st->codecpar) < 0 ||
        avcodec_open2(d->dec, codec, NULL) < 0) {
        set_err(d, "cannot open decoder");
        if (d->dec) avcodec_free_context(&d->dec);
        avformat_close_input(&d->fmt);
        free(d);
        return NULL;
    }

    /* want_w/want_h <= 0 means "source size". Scaling here rather than in the
       caller keeps the RGBA conversion and the resize in one swscale pass. */
    d->out_w = want_w > 0 ? want_w : d->dec->width;
    d->out_h = want_h > 0 ? want_h : d->dec->height;

    AVRational fr = av_guess_frame_rate(d->fmt, st, NULL);
    d->fps_num = fr.num > 0 ? fr.num : 0;
    d->fps_den = fr.den > 0 ? fr.den : 1;

    d->sws = sws_getContext(d->dec->width, d->dec->height, d->dec->pix_fmt,
                            d->out_w, d->out_h, AV_PIX_FMT_RGBA,
                            SWS_BILINEAR, NULL, NULL, NULL);
    d->frame = av_frame_alloc();
    d->rgba  = av_frame_alloc();
    d->pkt   = av_packet_alloc();
    if (!d->sws || !d->frame || !d->rgba || !d->pkt) {
        set_err(d, "alloc failed");
        avc_close_raw(d);
        return NULL;
    }

    int nbytes = av_image_get_buffer_size(AV_PIX_FMT_RGBA, d->out_w, d->out_h, 1);
    d->rgba_buf = (uint8_t*)av_malloc((size_t)nbytes);
    if (!d->rgba_buf) {
        set_err(d, "alloc failed");
        avc_close_raw(d);
        return NULL;
    }
    av_image_fill_arrays(d->rgba->data, d->rgba->linesize, d->rgba_buf,
                         AV_PIX_FMT_RGBA, d->out_w, d->out_h, 1);
    return d;
}

int avc_width_raw(void* h)        { Decoder* d = (Decoder*)h; return d ? d->out_w : 0; }
int avc_height_raw(void* h)       { Decoder* d = (Decoder*)h; return d ? d->out_h : 0; }
int avc_frame_bytes_raw(void* h)  { Decoder* d = (Decoder*)h; return d ? d->out_w * d->out_h * 4 : 0; }
int avc_fps_num_raw(void* h)      { Decoder* d = (Decoder*)h; return d ? d->fps_num : 0; }
int avc_fps_den_raw(void* h)      { Decoder* d = (Decoder*)h; return d ? d->fps_den : 1; }
int avc_pts_ms_raw(void* h)       { Decoder* d = (Decoder*)h; return d ? (int)d->pts_ms : 0; }

const char* avc_error_raw(void* h) {
    Decoder* d = (Decoder*)h;
    return (d && d->err[0]) ? d->err : "";
}

/* Decode until one frame is converted into d->rgba. Returns 1 on success,
   0 at EOF or on error. Packets that yield no frame (B-frame reordering,
   parameter sets) are consumed and the loop continues, so a caller sees one
   call == one frame rather than having to understand FFmpeg's buffering. */
static int decode_one(Decoder* d) {
    if (!d || !d->fmt || !d->dec) return 0;
    for (;;) {
        int rc = avcodec_receive_frame(d->dec, d->frame);
        if (rc == 0) {
            sws_scale(d->sws, (const uint8_t* const*)d->frame->data,
                      d->frame->linesize, 0, d->dec->height,
                      d->rgba->data, d->rgba->linesize);
            AVStream* st = d->fmt->streams[d->stream_idx];
            int64_t pts = d->frame->best_effort_timestamp;
            if (pts == AV_NOPTS_VALUE) pts = 0;
            d->pts_ms = (long)(pts * av_q2d(st->time_base) * 1000.0);
            return 1;
        }
        if (rc != AVERROR(EAGAIN) && rc != AVERROR_EOF) {
            set_err(d, "decode error");
            return 0;
        }
        if (rc == AVERROR_EOF) return 0;

        int got = 0;
        while (av_read_frame(d->fmt, d->pkt) >= 0) {
            if (d->pkt->stream_index == d->stream_idx) {
                int sc = avcodec_send_packet(d->dec, d->pkt);
                av_packet_unref(d->pkt);
                if (sc < 0) { set_err(d, "send packet failed"); return 0; }
                got = 1;
                break;
            }
            av_packet_unref(d->pkt);
        }
        if (!got) {
            /* Input exhausted: flush the decoder's held frames, then EOF. */
            avcodec_send_packet(d->dec, NULL);
            if (avcodec_receive_frame(d->dec, d->frame) == 0) {
                sws_scale(d->sws, (const uint8_t* const*)d->frame->data,
                          d->frame->linesize, 0, d->dec->height,
                          d->rgba->data, d->rgba->linesize);
                return 1;
            }
            return 0;
        }
    }
}

/* Decode the next frame into the TLS slot (allocating a copy). */
int avc_try_next_frame(void* h) {
    free_frame_tls();
    Decoder* d = (Decoder*)h;
    if (!decode_one(d)) return 0;
    int n = d->out_w * d->out_h * 4;
    g_frame_bytes = (char*)malloc((size_t)n);
    if (!g_frame_bytes) { g_frame_len = 0; return 0; }
    memcpy(g_frame_bytes, d->rgba_buf, (size_t)n);
    g_frame_len = n;
    return 1;
}

const char* avc_get_frame_bytes(void) { return g_frame_bytes ? g_frame_bytes : ""; }
int         avc_get_frame_length(void) { return g_frame_len; }
void        avc_release_frame(void)    { free_frame_tls(); }

/* Zero-allocation path: decode straight into a caller-owned buffer.
   Returns bytes written, or 0 on EOF/failure/insufficient capacity. */
int avc_copy_frame_into_raw(void* h, void* buf, int cap) {
    Decoder* d = (Decoder*)h;
    if (!d || !buf) return 0;
    int n = d->out_w * d->out_h * 4;
    if (cap < n) return 0;
    if (!decode_one(d)) return 0;
    memcpy(buf, d->rgba_buf, (size_t)n);
    return n;
}

void avc_close_raw(void* h) {
    Decoder* d = (Decoder*)h;
    if (!d) return;
    if (d->sws)      sws_freeContext(d->sws);
    if (d->frame)    av_frame_free(&d->frame);
    if (d->rgba)     av_frame_free(&d->rgba);
    if (d->pkt)      av_packet_free(&d->pkt);
    if (d->rgba_buf) av_free(d->rgba_buf);
    if (d->dec)      avcodec_free_context(&d->dec);
    if (d->fmt)      avformat_close_input(&d->fmt);
    free(d);
}
