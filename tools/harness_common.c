#include "harness_common.h"

#include "lzss.h"
#include "snapshot.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>

static harness_blobs_t *last_loaded_blobs;

static uint32_t le32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

harness_blob_t harness_read_blob(const char *path) {
    harness_blob_t b = {0};
    FILE *f;
    long n;

    f = fopen(path, "rb");
    if (!f) {
        fprintf(stderr, "cannot open %s: %s\n", path, strerror(errno));
        return b;
    }
    fseek(f, 0, SEEK_END);
    n = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (n <= 0 || !(b.p = malloc((size_t)n)) ||
        fread(b.p, 1, (size_t)n, f) != (size_t)n) {
        free(b.p);
        b.p = NULL;
    }
    fclose(f);
    b.n = b.p ? (size_t)n : 0;
    if (!b.p)
        return b;

    if (b.n >= 4 && !memcmp(b.p, "8900", 4)) {
        size_t off = 0x800, sz = b.n >= 16 ? le32(b.p + 12) : 0;
        uint8_t *q;
        if (off + sz > b.n) {
            free(b.p);
            return (harness_blob_t){0};
        }
        q = malloc(sz);
        if (!q) {
            free(b.p);
            return (harness_blob_t){0};
        }
        memcpy(q, b.p + off, sz);
        free(b.p);
        b = (harness_blob_t){q, sz};
        printf("detected 8900 wrapper, payload=%zu\n", b.n);
    }
    if (b.n >= 4 && !memcmp(b.p, "2gmI", 4)) {
        size_t off = 0x400, sz = b.n >= 0x1c ? le32(b.p + 0x18) : 0;
        uint8_t *q;
        if (off + sz > b.n) {
            free(b.p);
            return (harness_blob_t){0};
        }
        q = malloc(sz);
        if (!q) {
            free(b.p);
            return (harness_blob_t){0};
        }
        memcpy(q, b.p + off, sz);
        free(b.p);
        b = (harness_blob_t){q, sz};
        printf("detected IMG2 wrapper, payload=%zu\n", b.n);
    }
    {
        lzss_header_t h;
        if (lzss_parse_header(b.p, b.n, &h)) {
            uint8_t *q = malloc(h.uncompressed_size);
            if (!q || lzss_decompress(q, h.uncompressed_size,
                                      b.p + LZSS_HEADER_SIZE,
                                      h.compressed_size) !=
                      h.uncompressed_size) {
                free(q);
                free(b.p);
                return (harness_blob_t){0};
            }
            free(b.p);
            b = (harness_blob_t){q, h.uncompressed_size};
            printf("detected complzss, decompressed=%zu\n", b.n);
        } else {
            printf("using raw file, bytes=%zu\n", b.n);
        }
    }
    return b;
}

void harness_opts_init(harness_opts_t *o) {
    memset(o, 0, sizeof *o);
    o->framebuffer = true;
    o->cpi = 1;
}

bool harness_parse_common(harness_opts_t *o, int argc, char **argv, int *i) {
    const char *arg = argv[*i];
    if (!strcmp(arg, "--board") && *i + 1 < argc)
        o->board = argv[++*i];
    else if (!strcmp(arg, "--kernel") && *i + 1 < argc)
        o->kernel = argv[++*i];
    else if (!strcmp(arg, "--devicetree") && *i + 1 < argc)
        o->devicetree = argv[++*i];
    else if (!strcmp(arg, "--ramdisk") && *i + 1 < argc)
        o->ramdisk = argv[++*i];
    else if (!strcmp(arg, "--ram-mb") && *i + 1 < argc)
        o->ram_mb = (unsigned)strtoul(argv[++*i], NULL, 0);
    else if (!strcmp(arg, "--cpi") && *i + 1 < argc)
        o->cpi = (unsigned)strtoul(argv[++*i], NULL, 0);
    else if (!strcmp(arg, "--cmdline") && *i + 1 < argc)
        o->cmdline = argv[++*i];
    else if (!strcmp(arg, "--no-fb"))
        o->framebuffer = false;
    else if (!strcmp(arg, "--v-display") && *i + 1 < argc)
        o->v_display = (unsigned)strtoul(argv[++*i], NULL, 0);
    else if (!strcmp(arg, "--debug-enabled"))
        o->debug = true;
    else if (!strcmp(arg, "--serial") && *i + 1 < argc)
        o->serial = argv[++*i];
    else if (!strcmp(arg, "--load") && *i + 1 < argc)
        o->load = argv[++*i];
    else if (!strcmp(arg, "--unmatch") && *i + 1 < argc) {
        if (o->unmatch_n < sizeof o->unmatch / sizeof o->unmatch[0])
            o->unmatch[o->unmatch_n++] = argv[++*i];
        else
            ++*i;
    } else {
        return false;
    }
    return true;
}

const char *harness_common_usage(void) {
    return "  --board m68|n82   target board\n"
           "  --kernel PATH      ARM kernel Mach-O\n"
           "  --devicetree PATH  device tree blob\n"
           "  --ramdisk PATH     optional root filesystem\n"
           "  --ram-mb N         guest RAM size\n"
           "  --cpi N            cycles per retired instruction, 1..16, default 1\n"
           "  --cmdline TEXT     kernel command line\n"
           "  --no-fb            disable framebuffer setup\n"
           "  --v-display N      display selector\n"
           "  --debug-enabled    enable OS debug flag\n"
           "  --serial PATH      serial tap file, or - for stdout\n"
           "  --load PATH        load a snapshot after boot\n"
           "  --unmatch PATH     disable a device-tree node match\n";
}

bool harness_load_blobs(const harness_opts_t *o, harness_blobs_t *b) {
    memset(b, 0, sizeof *b);
    if (!o->kernel || !o->devicetree)
        return false;
    b->kernel = harness_read_blob(o->kernel);
    b->devicetree = harness_read_blob(o->devicetree);
    if (o->ramdisk)
        b->ramdisk = harness_read_blob(o->ramdisk);
    if (!b->kernel.p || !b->devicetree.p ||
        (o->ramdisk && !b->ramdisk.p)) {
        harness_free_blobs(b);
        return false;
    }
    last_loaded_blobs = b;
    return true;
}

bool harness_init_machine(const harness_opts_t *o, s5l8900_t *m,
                          const s5l_board_t **board_out) {
    const s5l_board_t *b = s5l_board_find(o->board);
    char why[256];

    if (!b || !o->devicetree)
        return false;
    if (!last_loaded_blobs) {
        snprintf(why, sizeof why, "device tree was not loaded");
        fprintf(stderr, "device tree rejected: %s\n", why);
        return false;
    }
    if (!s5l_board_check_devicetree(b, last_loaded_blobs->devicetree.p,
                                    last_loaded_blobs->devicetree.n,
                                    why, sizeof why)) {
        fprintf(stderr, "device tree rejected: %s\n", why);
        return false;
    }
    if (!s5l_board_init(b, m, o->ram_mb ? o->ram_mb << 20 : 0)) {
        fprintf(stderr, "machine init failed (RAM may overlap a device window)\n");
        return false;
    }
    *board_out = b;
    return true;
}

bool harness_boot(const harness_opts_t *o, const s5l_board_t *b,
                  s5l8900_t *m, const harness_blobs_t *blobs) {
    s5l_os1_request_t q;
    s5l_os1_result_t r;
    q = (s5l_os1_request_t){
        blobs->kernel.p, blobs->kernel.n, blobs->devicetree.p,
        blobs->devicetree.n, blobs->ramdisk.p, blobs->ramdisk.n,
        o->cmdline, o->framebuffer, o->v_display, b->lcd_width,
        b->lcd_height, o->debug, o->unmatch, o->unmatch_n
    };
    if (!s5l_os1_boot(m, &q, &r)) {
        fprintf(stderr, "boot failed: %s\n", r.detail);
        return false;
    }
    printf("layout virt_base=0x%08x phys_base=0x%08x ram_size=0x%08x\n",
           r.virt_base, r.phys_base, r.ram_size);
    printf("kernel_begin_pa=0x%08x kernel_end_pa=0x%08x entry_pa=0x%08x entry_va=0x%08x\n",
           r.kernel_begin_pa, r.kernel_end_pa, r.entry_pa, r.entry_va);
    printf("devicetree_pa=0x%08x devicetree_va=0x%08x devicetree_size=0x%08x\n",
           r.devicetree_pa, r.devicetree_va, r.devicetree_size);
    printf("boot_args_pa=0x%08x ramdisk_pa=0x%08x ramdisk_va=0x%08x ramdisk_size=0x%08x\n",
           r.boot_args_pa, r.ramdisk_pa, r.ramdisk_va, r.ramdisk_size);
    printf("framebuffer_pa=0x%08x vram_bytes=0x%08x top_of_kernel_data_pa=0x%08x cmdline=\"%s\"\n",
           r.framebuffer_pa, r.vram_bytes, r.top_of_kernel_data_pa, r.cmdline);
    if (o->load) {
        snapshot_status_t ss = snapshot_load(m, o->load);
        if (ss != SNAP_OK) {
            fprintf(stderr, "snapshot load failed: %s\n",
                    snapshot_strerror(ss));
            return false;
        }
        fprintf(stderr, "loaded snapshot %s at step %llu\n", o->load,
                (unsigned long long)m->cpu.cycles);
    }
    if (!s5l8900_set_cycles_per_insn(m, o->cpi)) {
        fprintf(stderr, "invalid --cpi %u (expected 1..16)\n", o->cpi);
        return false;
    }
    return true;
}

void harness_free_blobs(harness_blobs_t *b) {
    if (last_loaded_blobs == b)
        last_loaded_blobs = NULL;
    free(b->kernel.p);
    free(b->devicetree.p);
    free(b->ramdisk.p);
    memset(b, 0, sizeof *b);
}

bool harness_frame_rgb(const s5l8900_t *m, uint8_t *rgb,
                       unsigned *w, unsigned *h) {
    unsigned k;
    if (!m || !rgb || !w || !h)
        return false;
    k = s5l_clcd_active_window(&m->clcd);
    if (k == CLCD_WIN_NONE)
        return false;
    if (!s5l_clcd_scanout(&m->clcd, k, m->ram, m->ram_base, m->ram_size,
                          rgb, 320u * 480u * 3u, w, h) ||
        *w > 320u || *h > 480u)
        return false;
    return true;
}

bool harness_save_ppm(const char *path, const s5l8900_t *m) {
    uint8_t *rgb = malloc(320u * 480u * 3u);
    unsigned w = 0, h = 0;
    FILE *f;
    bool ok;

    if (!rgb || !harness_frame_rgb(m, rgb, &w, &h)) {
        free(rgb);
        return false;
    }
    f = fopen(path, "wb");
    if (!f) {
        free(rgb);
        return false;
    }
    fprintf(f, "P6\n%u %u\n255\n", w, h);
    ok = fwrite(rgb, 1, (size_t)w * h * 3u, f) == (size_t)w * h * 3u;
    if (fclose(f) != 0)
        ok = false;
    free(rgb);
    return ok;
}

void harness_serial_tx(void *ctx, uint8_t c) {
    harness_serial_t *t = ctx;
    if (t->echo_stdout) {
        putchar((int)c);
        fflush(stdout);
    }
    if (t->serial) {
        if (t->bol)
            fprintf(t->serial, "%llu ", (unsigned long long)t->m->cpu.cycles);
        fputc(c, t->serial);
        fflush(t->serial);
        t->bol = c == '\n';
    }
}

bool harness_serial_open(harness_serial_t *t, const char *path,
                         const s5l8900_t *m, bool echo_stdout) {
    memset(t, 0, sizeof *t);
    t->m = m;
    t->bol = true;
    t->echo_stdout = echo_stdout;
    if (!path)
        return true;
    if (!strcmp(path, "-")) {
        t->echo_stdout = true;
        return true;
    }
    t->serial = fopen(path, "wb");
    if (!t->serial) {
        fprintf(stderr, "cannot open %s: %s\n", path, strerror(errno));
        return false;
    }
    return true;
}

void harness_serial_close(harness_serial_t *t) {
    if (t && t->serial)
        fclose(t->serial);
    if (t)
        t->serial = NULL;
}

static uint32_t crc_table[256];
static bool crc_ready;

static void crc_init(void) {
    if (crc_ready)
        return;
    for (unsigned i = 0; i < 256; i++) {
        uint32_t c = i;
        for (unsigned j = 0; j < 8; j++)
            c = c & 1u ? UINT32_C(0xedb88320) ^ (c >> 1) : c >> 1;
        crc_table[i] = c;
    }
    crc_ready = true;
}

static uint32_t crc32(const uint8_t *p, size_t n) {
    uint32_t c = UINT32_C(0xffffffff);
    crc_init();
    while (n--)
        c = crc_table[(c ^ *p++) & 0xffu] ^ (c >> 8);
    return c ^ UINT32_C(0xffffffff);
}

static uint32_t adler32(const uint8_t *p, size_t n) {
    uint32_t a = 1, b = 0;
    while (n--) {
        a = (a + *p++) % 65521u;
        b = (b + a) % 65521u;
    }
    return (b << 16) | a;
}

static void put_be32(uint8_t *p, uint32_t x) {
    p[0] = (uint8_t)(x >> 24);
    p[1] = (uint8_t)(x >> 16);
    p[2] = (uint8_t)(x >> 8);
    p[3] = (uint8_t)x;
}

static bool png_chunk(FILE *f, const char type[4],
                      const uint8_t *data, size_t n) {
    uint8_t len[4], crcbuf[4];
    uint32_t c;
    if (n > UINT32_MAX)
        return false;
    put_be32(len, (uint32_t)n);
    if (fwrite(len, 1, 4, f) != 4 || fwrite(type, 1, 4, f) != 4 ||
        (n && fwrite(data, 1, n, f) != n))
        return false;
    c = crc32((const uint8_t *)type, 4);
    if (n) {
        uint8_t *tmp = malloc(n + 4);
        if (!tmp)
            return false;
        memcpy(tmp, type, 4);
        memcpy(tmp + 4, data, n);
        c = crc32(tmp, n + 4);
        free(tmp);
    }
    put_be32(crcbuf, c);
    return fwrite(crcbuf, 1, 4, f) == 4;
}

bool harness_write_png(const char *path, const uint8_t *rgb,
                       unsigned w, unsigned h) {
    static const uint8_t signature[8] =
        {137, 80, 78, 71, 13, 10, 26, 10};
    size_t raw_n, blocks, z_n, pos = 0, raw_pos = 0;
    uint8_t *raw, *z, ihdr[13];
    FILE *f;
    bool ok = false;

    if (!path || !rgb || !w || !h ||
        (size_t)w > (SIZE_MAX - h) / 3u)
        return false;
    raw_n = (size_t)h * ((size_t)w * 3u + 1u);
    blocks = (raw_n + 65534u) / 65535u;
    if (blocks > (SIZE_MAX - 6u - 4u) / 65540u)
        return false;
    z_n = 2u + raw_n + blocks * 5u + 4u;
    raw = malloc(raw_n);
    z = malloc(z_n);
    if (!raw || !z) {
        free(raw);
        free(z);
        return false;
    }
    for (unsigned y = 0; y < h; y++) {
        raw[raw_pos++] = 0;
        memcpy(raw + raw_pos, rgb + (size_t)y * w * 3u, (size_t)w * 3u);
        raw_pos += (size_t)w * 3u;
    }
    z[pos++] = 0x78;
    z[pos++] = 0x01;
    while (raw_n) {
        size_t n = raw_n > 65535u ? 65535u : raw_n;
        bool final = n == raw_n;
        z[pos++] = final ? 1u : 0u;
        z[pos++] = (uint8_t)n;
        z[pos++] = (uint8_t)(n >> 8);
        z[pos++] = (uint8_t)~n;
        z[pos++] = (uint8_t)(~n >> 8);
        memcpy(z + pos, raw + raw_pos - raw_n, n);
        pos += n;
        raw_n -= n;
    }
    put_be32(z + pos, adler32(raw, raw_pos));
    pos += 4;
    f = fopen(path, "wb");
    if (f && fwrite(signature, 1, sizeof signature, f) == sizeof signature) {
        memset(ihdr, 0, sizeof ihdr);
        put_be32(ihdr, w);
        put_be32(ihdr + 4, h);
        ihdr[8] = 8;
        ihdr[9] = 2;
        ok = png_chunk(f, "IHDR", ihdr, sizeof ihdr) &&
             png_chunk(f, "IDAT", z, pos) &&
             png_chunk(f, "IEND", NULL, 0);
        if (fclose(f) != 0)
            ok = false;
    } else if (f) {
        fclose(f);
    }
    free(raw);
    free(z);
    return ok;
}
