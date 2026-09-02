#ifndef IPHONE1_HARNESS_COMMON_H
#define IPHONE1_HARNESS_COMMON_H

#include "board.h"
#include "bringup_os1.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

typedef struct {
    uint8_t *p;
    size_t n;
} harness_blob_t;

typedef struct {
    const char *board, *kernel, *devicetree, *ramdisk, *cmdline, *serial, *load;
    unsigned ram_mb, v_display, cpi;
    bool framebuffer, debug;
    const char *unmatch[16];
    unsigned unmatch_n;
} harness_opts_t;

typedef struct {
    harness_blob_t kernel, devicetree, ramdisk;
} harness_blobs_t;

typedef struct {
    FILE *serial;
    const s5l8900_t *m;
    bool bol;
    bool echo_stdout;
} harness_serial_t;

void harness_opts_init(harness_opts_t *o);
bool harness_parse_common(harness_opts_t *o, int argc, char **argv, int *i);
const char *harness_common_usage(void);

harness_blob_t harness_read_blob(const char *path);
bool harness_load_blobs(const harness_opts_t *o, harness_blobs_t *b);
bool harness_init_machine(const harness_opts_t *o, s5l8900_t *m,
                          const s5l_board_t **board_out);
bool harness_boot(const harness_opts_t *o, const s5l_board_t *b,
                  s5l8900_t *m, const harness_blobs_t *blobs);
void harness_free_blobs(harness_blobs_t *b);

bool harness_frame_rgb(const s5l8900_t *m, uint8_t *rgb,
                       unsigned *w, unsigned *h);
bool harness_save_ppm(const char *path, const s5l8900_t *m);

void harness_serial_tx(void *ctx, uint8_t c);
bool harness_serial_open(harness_serial_t *t, const char *path,
                         const s5l8900_t *m, bool echo_stdout);
void harness_serial_close(harness_serial_t *t);

bool harness_write_png(const char *path, const uint8_t *rgb,
                       unsigned w, unsigned h);

#endif
