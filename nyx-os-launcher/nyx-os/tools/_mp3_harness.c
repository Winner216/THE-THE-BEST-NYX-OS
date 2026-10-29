// tools/_mp3_harness.c — host-side decoder test for cpu/mp3dec.c
// Decodes an MP3 with the kernel decoder and writes interleaved s16le
// PCM to stdout (or a file arg), so a reference decoder (gst) output
// can be compared numerically.
#include <mp3dec.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int main(int argc, char** argv) {
    if (argc < 2) { fprintf(stderr, "usage: %s in.mp3 [out.raw] [maxframes]\n", argv[0]); return 2; }
    FILE* f = fopen(argv[1], "rb");
    if (!f) { fprintf(stderr, "cannot open %s\n", argv[1]); return 2; }
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    unsigned char* data = malloc(sz);
    if (fread(data, 1, sz, f) != (size_t)sz) { fprintf(stderr, "short read\n"); return 2; }
    fclose(f);

    FILE* out = stdout;
    if (argc > 2) { out = fopen(argv[2], "wb"); if (!out) { fprintf(stderr, "cannot write %s\n", argv[2]); return 2; } }

    int maxf = (argc > 3) ? atoi(argv[3]) : 1000000;

    Mp3Dec d;
    memset(&d, 0, sizeof(d));
    int f0 = mp3_init(&d, data, (int)sz);
    if (f0 < 0) { fprintf(stderr, "mp3_init failed (no MPEG1 Layer III frame)\n"); return 1; }
    fprintf(stderr, "first frame @%d, ch=%d sr=%d br=%d\n", f0, d.channels, d.samplerate, d.bitrate);

    int frames = 0;
    while (frames < maxf) {
        int n = mp3_decode_frame(&d);
        if (n <= 0) break;
        if (d.channels == 1) {
            for (int i = 0; i < n; i++) {
                int16_t s = d.pcm[0][i];
                fwrite(&s, 2, 1, out);
                fwrite(&s, 2, 1, out);
            }
        } else {
            fwrite(d.pcm[0], 2, n, out);
            fwrite(d.pcm[1], 2, n, out);
        }
        frames++;
    }
    fprintf(stderr, "decoded %d frames, %d samples/ch\n", frames, frames * MP3_FRAME_SAMPLES);
    if (out != stdout) fclose(out);
    free(data);
    return 0;
}
