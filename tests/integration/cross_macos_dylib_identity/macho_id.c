/* Print a 64-bit Mach-O dylib's identity as the dynamic loader reads it:
 *
 *   type=dylib
 *   id=<LC_ID_DYLIB name>
 *   minos=<LC_BUILD_VERSION minos, or LC_VERSION_MIN_MACOSX version>
 *
 * Read straight from the load commands so the test runs on a Linux host,
 * where there is no otool. Little-endian only (every macOS architecture ae
 * targets is). Exits 2 on a file that is not a 64-bit Mach-O. */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static uint32_t rd32(const unsigned char* p) {
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

static void print_version(const char* key, uint32_t v) {
    unsigned major = v >> 16, minor = (v >> 8) & 0xff, patch = v & 0xff;
    if (patch) printf("%s=%u.%u.%u\n", key, major, minor, patch);
    else       printf("%s=%u.%u\n", key, major, minor);
}

int main(int argc, char** argv) {
    if (argc != 2) { fprintf(stderr, "usage: macho_id FILE\n"); return 2; }
    FILE* f = fopen(argv[1], "rb");
    if (!f) { perror(argv[1]); return 2; }
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (n < 32) { fprintf(stderr, "%s: too short\n", argv[1]); return 2; }
    unsigned char* b = malloc((size_t)n);
    if (!b || fread(b, 1, (size_t)n, f) != (size_t)n) { fprintf(stderr, "read failed\n"); return 2; }
    fclose(f);

    if (rd32(b) != 0xfeedfacfu) { fprintf(stderr, "%s: not a 64-bit Mach-O\n", argv[1]); return 2; }
    uint32_t filetype = rd32(b + 12), ncmds = rd32(b + 16), sizeofcmds = rd32(b + 20);
    printf("type=%s\n", filetype == 6 ? "dylib" : filetype == 2 ? "execute" : "other");
    if (32 + (uint64_t)sizeofcmds > (uint64_t)n) { fprintf(stderr, "load commands truncated\n"); return 2; }

    size_t off = 32;
    for (uint32_t i = 0; i < ncmds; i++) {
        if (off + 8 > 32 + (size_t)sizeofcmds) break;
        uint32_t cmd = rd32(b + off), size = rd32(b + off + 4);
        if (size < 8 || off + size > 32 + (size_t)sizeofcmds) break;
        if (cmd == 0xd && size >= 24) {              /* LC_ID_DYLIB */
            uint32_t name = rd32(b + off + 8);
            if (name < size) printf("id=%.*s\n", (int)strnlen((const char*)b + off + name, size - name),
                                    (const char*)b + off + name);
        } else if (cmd == 0x32 && size >= 24) {      /* LC_BUILD_VERSION */
            print_version("minos", rd32(b + off + 12));
        } else if (cmd == 0x24 && size >= 16) {      /* LC_VERSION_MIN_MACOSX */
            print_version("minos", rd32(b + off + 8));
        }
        off += size;
    }
    free(b);
    return 0;
}
