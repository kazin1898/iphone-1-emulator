#include "harness_common.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int g_pass, g_fail;
#define CHECK(cond, ...) do { \
    if (cond) g_pass++; \
    else { g_fail++; printf("  FAIL %s:%d: ", __func__, __LINE__); \
           printf(__VA_ARGS__); printf("\n"); } \
} while (0)

static void test_common_options(void) {
    char *argv[] = {
        (char *)"test", (char *)"--board", (char *)"m68",
        (char *)"--kernel", (char *)"k", (char *)"--devicetree", (char *)"d",
        (char *)"--ramdisk", (char *)"r", (char *)"--ram-mb", (char *)"256",
        (char *)"--cpi", (char *)"3", (char *)"--cmdline", (char *)"x",
        (char *)"--no-fb", (char *)"--v-display", (char *)"2",
        (char *)"--debug-enabled", (char *)"--serial", (char *)"s",
        (char *)"--load", (char *)"l", (char *)"--unmatch", (char *)"u",
        (char *)"--unknown"
    };
    harness_opts_t o;
    int i;

    harness_opts_init(&o);
    for (i = 1; i < (int)(sizeof argv / sizeof argv[0]); i++) {
        if (harness_parse_common(&o, (int)(sizeof argv / sizeof argv[0]),
                                 argv, &i))
            continue;
        CHECK(!strcmp(argv[i], "--unknown"), "unknown option was consumed");
    }
    CHECK(!strcmp(o.board, "m68") && !strcmp(o.kernel, "k") &&
              !strcmp(o.devicetree, "d") && !strcmp(o.ramdisk, "r"),
          "path options");
    CHECK(o.ram_mb == 256 && o.cpi == 3 && !strcmp(o.cmdline, "x") &&
              o.v_display == 2 && !o.framebuffer && o.debug,
          "numeric and boolean options");
    CHECK(!strcmp(o.serial, "s") && !strcmp(o.load, "l") &&
              o.unmatch_n == 1 && !strcmp(o.unmatch[0], "u"),
          "tap, snapshot, and unmatch options");
}

static void test_serial_dash(void) {
    harness_serial_t t;
    s5l8900_t m;
    CHECK(s5l8900_init(&m, 0, 1u << 20), "machine init");
    CHECK(harness_serial_open(&t, "-", &m, false), "open dash");
    CHECK(t.serial == NULL && t.echo_stdout, "dash semantics");
    harness_serial_close(&t);
    s5l8900_free(&m);
}

static uint32_t crc_table[256];
static uint32_t crc32(const uint8_t *p, size_t n) {
    uint32_t c = UINT32_C(0xffffffff);
    static int initialized;
    if (!initialized) {
        for (unsigned i = 0; i < 256; i++) {
            uint32_t v = i;
            for (unsigned j = 0; j < 8; j++)
                v = v & 1u ? UINT32_C(0xedb88320) ^ (v >> 1) : v >> 1;
            crc_table[i] = v;
        }
        initialized = 1;
    }
    while (n--) c = crc_table[(c ^ *p++) & 0xffu] ^ (c >> 8);
    return c ^ UINT32_C(0xffffffff);
}

static uint32_t be32(const uint8_t *p) {
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8) | p[3];
}

static void test_png(void) {
    static const uint8_t pixels[] = {
        1, 2, 3, 4, 5, 6, 7, 8, 9,
        10, 11, 12, 13, 14, 15, 16, 17, 18
    };
    uint8_t *file = NULL, *inflated = NULL, *idat = NULL;
    size_t file_n = 0, idat_n = 0, inflated_n = 0, pos = 0;
    const char *path = "/tmp/test_harness.png";
    FILE *f = fopen(path, "rb");

    if (f) fclose(f);
    CHECK(harness_write_png(path, pixels, 3, 2), "write PNG");
    f = fopen(path, "rb");
    CHECK(f != NULL, "open PNG");
    if (!f) return;
    fseek(f, 0, SEEK_END);
    file_n = (size_t)ftell(f);
    fseek(f, 0, SEEK_SET);
    file = malloc(file_n);
    CHECK(file && fread(file, 1, file_n, f) == file_n, "read PNG");
    fclose(f);
    if (!file || file_n < 33) goto done;
    CHECK(!memcmp(file, "\x89PNG\r\n\x1a\n", 8), "PNG signature");
    pos = 8;
    while (pos + 12 <= file_n) {
        uint32_t n = be32(file + pos);
        const uint8_t *type = file + pos + 4;
        const uint8_t *data = type + 4;
        size_t end = pos + 12u + n;
        CHECK(end <= file_n, "chunk bounds");
        if (end > file_n) goto done;
        CHECK(crc32(type, (size_t)n + 4u) == be32(data + n),
              "chunk CRC");
        if (!memcmp(type, "IHDR", 4)) {
            CHECK(n == 13 && be32(data) == 3 && be32(data + 4) == 2 &&
                      data[8] == 8 && data[9] == 2,
                  "IHDR fields");
        } else if (!memcmp(type, "IDAT", 4)) {
            uint8_t *q = realloc(idat, idat_n + n);
            CHECK(q != NULL, "IDAT allocation");
            if (!q) goto done;
            idat = q;
            memcpy(idat + idat_n, data, n);
            idat_n += n;
        }
        pos = end;
        if (!memcmp(type, "IEND", 4)) break;
    }
    CHECK(idat_n >= 6 && idat[0] == 0x78 && idat[1] == 0x01,
          "stored zlib header");
    if (idat_n >= 6) {
        size_t p = 2;
        while (p + 5 <= idat_n) {
            uint8_t hdr = idat[p++];
            uint16_t n = (uint16_t)idat[p] | ((uint16_t)idat[p + 1] << 8);
            uint16_t nn = (uint16_t)idat[p + 2] |
                          ((uint16_t)idat[p + 3] << 8);
            p += 4;
            CHECK((hdr & 6u) == 0 && (uint16_t)~n == nn &&
                      p + n <= idat_n, "stored block framing");
            if (p + n > idat_n) goto done;
            inflated = realloc(inflated, inflated_n + n);
            CHECK(inflated != NULL, "inflate allocation");
            if (!inflated) goto done;
            memcpy(inflated + inflated_n, idat + p, n);
            inflated_n += n;
            p += n;
            if (hdr & 1u) {
                CHECK(p + 4 == idat_n, "zlib trailer");
                break;
            }
        }
        CHECK(inflated_n == 20, "inflated size=%zu", inflated_n);
        if (inflated_n == 20) {
            static const uint8_t expected[] = {
                0, 1, 2, 3, 4, 5, 6, 7, 8, 9,
                0, 10, 11, 12, 13, 14, 15, 16, 17, 18
            };
            CHECK(!memcmp(inflated, expected, sizeof expected),
                  "pixel bytes");
        }
    }
done:
    free(file);
    free(idat);
    free(inflated);
    remove(path);
}

int main(void) {
    test_common_options();
    test_serial_dash();
    test_png();
    printf("harness: %d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
