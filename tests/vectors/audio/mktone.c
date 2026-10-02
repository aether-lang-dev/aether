/* One-off fixture generator for #2364: a 440 Hz sine at amplitude 0.5,
 * 22050 Hz mono, 11025 frames (0.5 s), encoded as OGG Vorbis (quality 0.6).
 * The spec synthesises the same samples itself as its WAV twin.
 * Build: gcc mktone.c -o mktone -lvorbisenc -lvorbis -logg -lm */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <vorbis/vorbisenc.h>

#define RATE 22050
#define FRAMES 11025
#define FREQ 440.0
#define AMP 0.5

static void write_page(FILE* f, ogg_page* og) {
    fwrite(og->header, 1, og->header_len, f);
    fwrite(og->body, 1, og->body_len, f);
}

int main(int argc, char** argv) {
    if (argc != 2) { fprintf(stderr, "usage: mktone out.ogg\n"); return 2; }
    FILE* f = fopen(argv[1], "wb");
    if (!f) return 1;

    vorbis_info vi; vorbis_comment vc; vorbis_dsp_state vd; vorbis_block vb;
    ogg_stream_state os; ogg_page og; ogg_packet op;
    vorbis_info_init(&vi);
    if (vorbis_encode_init_vbr(&vi, 1, RATE, 0.6f) != 0) { fprintf(stderr, "init\n"); return 1; }
    vorbis_comment_init(&vc);
    vorbis_analysis_init(&vd, &vi);
    vorbis_block_init(&vd, &vb);
    ogg_stream_init(&os, 2364);

    ogg_packet h, hc, hcode;
    vorbis_analysis_headerout(&vd, &vc, &h, &hc, &hcode);
    ogg_stream_packetin(&os, &h);
    ogg_stream_packetin(&os, &hc);
    ogg_stream_packetin(&os, &hcode);
    while (ogg_stream_flush(&os, &og)) write_page(f, &og);

    float** buf = vorbis_analysis_buffer(&vd, FRAMES);
    for (int i = 0; i < FRAMES; i++)
        buf[0][i] = (float)(AMP * sin(2.0 * M_PI * FREQ * i / RATE));
    vorbis_analysis_wrote(&vd, FRAMES);
    vorbis_analysis_wrote(&vd, 0);

    int eos = 0;
    while (vorbis_analysis_blockout(&vd, &vb) == 1) {
        vorbis_analysis(&vb, NULL);
        vorbis_bitrate_addblock(&vb);
        while (vorbis_bitrate_flushpacket(&vd, &op)) {
            ogg_stream_packetin(&os, &op);
            while (!eos && ogg_stream_pageout(&os, &og)) {
                write_page(f, &og);
                if (ogg_page_eos(&og)) eos = 1;
            }
        }
    }
    while (ogg_stream_flush(&os, &og)) write_page(f, &og);

    ogg_stream_clear(&os); vorbis_block_clear(&vb); vorbis_dsp_clear(&vd);
    vorbis_comment_clear(&vc); vorbis_info_clear(&vi);
    fclose(f);
    return 0;
}
