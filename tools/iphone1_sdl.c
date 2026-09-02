#include "harness_common.h"

#include "snapshot.h"

#include <SDL.h>

#include <errno.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

typedef struct {
    bool active, release_pending;
    uint64_t down_ticks;
    unsigned x, y;
    bool move_pending;
    unsigned move_x, move_y;
    bool button_active[S5L_BUTTON_COUNT];
    bool button_release_pending[S5L_BUTTON_COUNT];
    uint64_t button_down_ticks[S5L_BUTTON_COUNT];
    bool ringer_state;
} input_state_t;

typedef enum {
    SCRIPT_TAP,
    SCRIPT_KEY
} script_kind_t;

typedef struct {
    uint64_t at_ms;
    script_kind_t kind;
    unsigned x, y;
    SDL_Keycode key;
} script_event_t;

typedef struct {
    script_event_t *items;
    unsigned count, capacity, next;
} script_t;

static const char *status_name(arm_status_t s) {
    switch (s) {
    case ARM_OK: return "ARM_OK";
    case ARM_UNDEFINED: return "ARM_UNDEFINED";
    case ARM_HALT: return "ARM_HALT";
    case ARM_GUEST_UNDEFINED: return "ARM_GUEST_UNDEFINED";
    }
    return "other";
}

static void usage(void) {
    puts("iphone1_sdl --board m68|n82 --kernel PATH --devicetree PATH [options]");
    fputs(harness_common_usage(), stdout);
    puts("  --scale N          integer window scale, 1..6, default 2\n"
         "  --save FILE        snapshot path for F5/F9\n"
         "  --shot-dir DIR     PNG screenshot directory, default .\n"
         "  --steps N          absolute instruction stop count, default forever\n"
         "  --exit-after-ms N  quit after this host time, default never\n"
         "  --script FILE      timed headless input script");
}

static uint64_t host_now(void) {
    return SDL_GetPerformanceCounter();
}

static double host_seconds(uint64_t ticks) {
    return (double)ticks / (double)SDL_GetPerformanceFrequency();
}

static unsigned clamp_coord(int value, unsigned limit) {
    if (value < 0) return 0;
    if ((unsigned)value >= limit) return limit - 1u;
    return (unsigned)value;
}

static void guest_coords(SDL_Window *window, int x, int y,
                         unsigned *gx, unsigned *gy) {
    int width, height;
    double sx, sy;
    SDL_GetWindowSize(window, &width, &height);
    sx = width > 0 ? (double)width / 320.0 : 1.0;
    sy = height > 0 ? (double)height / 480.0 : 1.0;
    *gx = clamp_coord((int)floor((double)x / sx), 320);
    *gy = clamp_coord((int)floor((double)y / sy), 480);
}

static void clear_input_state(input_state_t *in) {
    memset(in, 0, sizeof *in);
}

static void release_touch(s5l8900_t *m, input_state_t *in) {
    if (in->active && s5l8900_set_touch(m, NULL, 0)) {
        in->active = false;
        in->release_pending = false;
    }
}

static void release_button(s5l8900_t *m, input_state_t *in, unsigned which) {
    if (in->button_active[which] &&
        s5l8900_set_button(m, which, false)) {
        in->button_active[which] = false;
        in->button_release_pending[which] = false;
    }
}

static void flush_pending_inputs(s5l8900_t *m, input_state_t *in) {
    uint64_t hold = m->tb_hz ? m->tb_hz / 10u : 600000u;
    if (in->release_pending &&
        m->timer.ticks - in->down_ticks >= hold)
        release_touch(m, in);
    for (unsigned i = 0; i < S5L_BUTTON_COUNT; i++)
        if (in->button_release_pending[i] &&
            m->timer.ticks - in->button_down_ticks[i] >= hold)
            release_button(m, in, i);
}

static void handle_touch_down(s5l8900_t *m, input_state_t *in,
                              unsigned x, unsigned y) {
    s5l_mt_contact_t c = {1u, MTZ2_PHASE_MAKE_TOUCH, (uint16_t)x,
                          (uint16_t)y, 255u, 8u, 8u};
    if (in->release_pending)
        release_touch(m, in);
    if (s5l8900_set_touch(m, &c, 1)) {
        in->active = true;
        in->release_pending = false;
        in->down_ticks = m->timer.ticks;
        in->x = x;
        in->y = y;
    }
}

static void handle_touch_motion(input_state_t *in, unsigned x, unsigned y) {
    if (in->active && (x != in->x || y != in->y)) {
        in->move_pending = true;
        in->move_x = x;
        in->move_y = y;
    }
}

static void handle_touch_up(s5l8900_t *m, input_state_t *in) {
    uint64_t hold = m->tb_hz ? m->tb_hz / 10u : 600000u;
    if (!in->active)
        return;
    if (m->timer.ticks - in->down_ticks < hold)
        in->release_pending = true;
    else
        release_touch(m, in);
}

static void flush_touch_motion(s5l8900_t *m, input_state_t *in) {
    s5l_mt_contact_t c;
    if (!in->move_pending || !in->active)
        return;
    c = (s5l_mt_contact_t){1u, MTZ2_PHASE_TOUCHING,
                           (uint16_t)in->move_x, (uint16_t)in->move_y,
                           255u, 8u, 8u};
    if (s5l8900_set_touch(m, &c, 1)) {
        in->x = in->move_x;
        in->y = in->move_y;
    }
    in->move_pending = false;
}

static void handle_button_down(s5l8900_t *m, input_state_t *in,
                               unsigned which) {
    if (which == S5L_BUTTON_RINGERAB) {
        in->ringer_state = !in->ringer_state;
        (void)s5l8900_set_button(m, which, in->ringer_state);
        return;
    }
    if (in->button_release_pending[which])
        release_button(m, in, which);
    if (s5l8900_set_button(m, which, true)) {
        in->button_active[which] = true;
        in->button_release_pending[which] = false;
        in->button_down_ticks[which] = m->timer.ticks;
    }
}

static void handle_button_up(s5l8900_t *m, input_state_t *in,
                             unsigned which) {
    uint64_t hold = m->tb_hz ? m->tb_hz / 10u : 600000u;
    if (which == S5L_BUTTON_RINGERAB || !in->button_active[which])
        return;
    if (m->timer.ticks - in->button_down_ticks[which] < hold)
        in->button_release_pending[which] = true;
    else
        release_button(m, in, which);
}

static bool key_button(SDL_Keycode key, unsigned *which) {
    switch (key) {
    case SDLK_ESCAPE:
    case SDLK_h: *which = S5L_BUTTON_MENU; return true;
    case SDLK_p: *which = S5L_BUTTON_HOLD; return true;
    case SDLK_UP: *which = S5L_BUTTON_VOLUP; return true;
    case SDLK_DOWN: *which = S5L_BUTTON_VOLDOWN; return true;
    case SDLK_r: *which = S5L_BUTTON_RINGERAB; return true;
    default: return false;
    }
}

static bool parse_key(const char *name, SDL_Keycode *key) {
    if (!strcmp(name, "escape")) *key = SDLK_ESCAPE;
    else if (!strcmp(name, "h")) *key = SDLK_h;
    else if (!strcmp(name, "p")) *key = SDLK_p;
    else if (!strcmp(name, "up")) *key = SDLK_UP;
    else if (!strcmp(name, "down")) *key = SDLK_DOWN;
    else if (!strcmp(name, "r")) *key = SDLK_r;
    else if (!strcmp(name, "f5")) *key = SDLK_F5;
    else if (!strcmp(name, "f9")) *key = SDLK_F9;
    else if (!strcmp(name, "f12")) *key = SDLK_F12;
    else if (!strcmp(name, "space")) *key = SDLK_SPACE;
    else if (!strcmp(name, "q")) *key = SDLK_q;
    else return false;
    return true;
}

static bool script_add(script_t *s, const script_event_t *event) {
    if (s->count == s->capacity) {
        unsigned cap = s->capacity ? s->capacity * 2u : 16u;
        script_event_t *p = realloc(s->items, cap * sizeof *p);
        if (!p) return false;
        s->items = p;
        s->capacity = cap;
    }
    s->items[s->count++] = *event;
    return true;
}

static bool load_script(const char *path, script_t *s) {
    char line[256];
    FILE *f = fopen(path, "r");
    if (!f) {
        fprintf(stderr, "cannot open %s: %s\n", path, strerror(errno));
        return false;
    }
    while (fgets(line, sizeof line, f)) {
        char command[16], name[16];
        unsigned long long at;
        script_event_t e;
        if (line[0] == '#' || line[0] == '\n')
            continue;
        if (sscanf(line, "at %llu %15s", &at, command) != 2) {
            fclose(f);
            return false;
        }
        e.at_ms = (uint64_t)at;
        if (!strcmp(command, "tap") &&
            sscanf(line, "at %llu tap %u %u", &at, &e.x, &e.y) == 3) {
            e.kind = SCRIPT_TAP;
        } else if (!strcmp(command, "key") &&
                   sscanf(line, "at %llu key %15s", &at, name) == 2 &&
                   parse_key(name, &e.key)) {
            e.kind = SCRIPT_KEY;
        } else {
            fclose(f);
            return false;
        }
        if (!script_add(s, &e)) {
            fclose(f);
            return false;
        }
    }
    fclose(f);
    return true;
}

typedef struct {
    s5l8900_t *m;
    input_state_t *input;
    SDL_Window *window;
    const char *save_path;
    const char *load_path;
    const char *shot_dir;
    bool *paused;
    bool *quit;
    bool *reanchor;
} action_context_t;

static void action_snapshot_save(action_context_t *a) {
    if (!a->save_path) {
        fprintf(stderr, "F5: no --save path\n");
        return;
    }
    snapshot_status_t s = snapshot_save(a->m, a->save_path);
    if (s == SNAP_OK)
        fprintf(stderr, "F5: saved snapshot %s\n", a->save_path);
    else
        fprintf(stderr, "F5: snapshot save failed: %s\n",
                snapshot_strerror(s));
}

static void action_snapshot_load(action_context_t *a) {
    const char *path = a->save_path;
    snapshot_status_t s;
    if (!path || access(path, F_OK) != 0)
        path = a->load_path;
    if (!path) {
        fprintf(stderr, "F9: no snapshot path\n");
        return;
    }
    s = snapshot_load(a->m, path);
    if (s != SNAP_OK) {
        fprintf(stderr, "F9: snapshot load failed: %s\n", snapshot_strerror(s));
        return;
    }
    clear_input_state(a->input);
    *a->reanchor = true;
    fprintf(stderr, "F9: loaded snapshot %s at step %llu\n", path,
            (unsigned long long)a->m->cpu.cycles);
}

static void action_screenshot(action_context_t *a) {
    uint8_t rgb[320u * 480u * 3u];
    unsigned w = 320, h = 480;
    char path[512];
    bool frame = harness_frame_rgb(a->m, rgb, &w, &h);
    if (!frame) {
        memset(rgb, 0, sizeof rgb);
        w = 320;
        h = 480;
    }
    snprintf(path, sizeof path, "%s/iphone1_%llu.png", a->shot_dir,
             (unsigned long long)a->m->cpu.cycles);
    if (harness_write_png(path, rgb, w, h))
        fprintf(stderr, "F12: saved screenshot %s\n", path);
    else
        fprintf(stderr, "F12: screenshot failed: %s\n", path);
}

static void handle_key(action_context_t *a, SDL_Keycode key, bool down) {
    unsigned which;
    if (!down) {
        if (key_button(key, &which))
            handle_button_up(a->m, a->input, which);
        return;
    }
    if (key == SDLK_q) {
        *a->quit = true;
        return;
    }
    if (key == SDLK_SPACE) {
        *a->paused = !*a->paused;
        *a->reanchor = true;
        fprintf(stderr, "Space: %s\n", *a->paused ? "paused" : "resumed");
        return;
    }
    if (key == SDLK_F5) {
        action_snapshot_save(a);
        return;
    }
    if (key == SDLK_F9) {
        action_snapshot_load(a);
        return;
    }
    if (key == SDLK_F12) {
        action_screenshot(a);
        return;
    }
    if (key_button(key, &which))
        handle_button_down(a->m, a->input, which);
}

static void pump_events(action_context_t *a) {
    SDL_Event event;
    while (SDL_PollEvent(&event)) {
        switch (event.type) {
        case SDL_QUIT:
            *a->quit = true;
            break;
        case SDL_MOUSEBUTTONDOWN:
            if (event.button.button == SDL_BUTTON_LEFT) {
                unsigned x, y;
                guest_coords(a->window, event.button.x, event.button.y, &x, &y);
                handle_touch_down(a->m, a->input, x, y);
            }
            break;
        case SDL_MOUSEMOTION:
            if (event.motion.state & SDL_BUTTON_LMASK) {
                unsigned x, y;
                guest_coords(a->window, event.motion.x, event.motion.y, &x, &y);
                handle_touch_motion(a->input, x, y);
            }
            break;
        case SDL_MOUSEBUTTONUP:
            if (event.button.button == SDL_BUTTON_LEFT)
                handle_touch_up(a->m, a->input);
            break;
        case SDL_KEYDOWN:
            if (!event.key.repeat)
                handle_key(a, event.key.keysym.sym, true);
            break;
        case SDL_KEYUP:
            handle_key(a, event.key.keysym.sym, false);
            break;
        default:
            break;
        }
    }
    flush_touch_motion(a->m, a->input);
}

static void run_script(action_context_t *a, script_t *s, uint64_t elapsed_ms) {
    while (s->next < s->count && s->items[s->next].at_ms <= elapsed_ms) {
        script_event_t *e = &s->items[s->next++];
        if (e->kind == SCRIPT_TAP) {
            handle_touch_down(a->m, a->input, e->x, e->y);
            handle_touch_up(a->m, a->input);
        } else {
            handle_key(a, e->key, true);
            handle_key(a, e->key, false);
        }
    }
}

static void present(SDL_Renderer *renderer, SDL_Texture *texture,
                    const s5l8900_t *m) {
    static uint8_t rgb[320u * 480u * 3u];
    unsigned w = 320, h = 480;
    SDL_Rect rect;
    memset(rgb, 0, sizeof rgb);
    SDL_UpdateTexture(texture, NULL, rgb, 320 * 3);
    if (harness_frame_rgb(m, rgb, &w, &h)) {
        rect = (SDL_Rect){0, 0, (int)w, (int)h};
        SDL_UpdateTexture(texture, &rect, rgb, (int)w * 3);
    } else {
        w = h = 0;
    }
    SDL_RenderClear(renderer);
    SDL_RenderCopy(renderer, texture, NULL, NULL);
    SDL_RenderPresent(renderer);
}

int main(int argc, char **argv) {
    harness_opts_t o;
    harness_blobs_t blobs;
    const s5l_board_t *board;
    const char *save_path = NULL, *shot_dir = ".";
    const char *script_path = NULL;
    unsigned scale = 2;
    uint64_t steps = 0, exit_after_ms = 0;
    script_t script = {0};
    s5l8900_t m;
    input_state_t input;
    SDL_Window *window = NULL;
    SDL_Renderer *renderer = NULL;
    SDL_Texture *texture = NULL;
    harness_serial_t serial;
    bool paused = false, quit = false, reanchor = false;
    uint64_t start, anchor_host, last_present, last_title;
    uint64_t anchor_guest, title_guest;
    uint64_t title_instr = 0;
    uint64_t total;
    arm_status_t status = ARM_OK;
    int result = 0;

    harness_opts_init(&o);
    for (int i = 1; i < argc; i++) {
        if (harness_parse_common(&o, argc, argv, &i))
            continue;
        if (!strcmp(argv[i], "--scale") && i + 1 < argc)
            scale = (unsigned)strtoul(argv[++i], NULL, 0);
        else if (!strcmp(argv[i], "--save") && i + 1 < argc)
            save_path = argv[++i];
        else if (!strcmp(argv[i], "--shot-dir") && i + 1 < argc)
            shot_dir = argv[++i];
        else if (!strcmp(argv[i], "--steps") && i + 1 < argc)
            steps = strtoull(argv[++i], NULL, 0);
        else if (!strcmp(argv[i], "--exit-after-ms") && i + 1 < argc)
            exit_after_ms = strtoull(argv[++i], NULL, 0);
        else if (!strcmp(argv[i], "--script") && i + 1 < argc)
            script_path = argv[++i];
        else {
            usage();
            return 2;
        }
    }
    if (!o.board || !o.kernel || !o.devicetree || scale < 1 || scale > 6) {
        usage();
        return 2;
    }
    if (script_path && !load_script(script_path, &script)) {
        fprintf(stderr, "invalid script %s\n", script_path);
        free(script.items);
        return 2;
    }
    if (!harness_load_blobs(&o, &blobs))
        goto boot_failure;
    if (!harness_init_machine(&o, &m, &board)) {
        harness_free_blobs(&blobs);
        goto boot_failure;
    }
    if (!harness_boot(&o, board, &m, &blobs)) {
        s5l8900_free(&m);
        harness_free_blobs(&blobs);
        goto boot_failure;
    }
    if (!harness_serial_open(&serial, o.serial, &m, false)) {
        s5l8900_free(&m);
        harness_free_blobs(&blobs);
        goto boot_failure;
    }
    (void)s5l8900_set_uart0_host(&m, harness_serial_tx, &serial);
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_TIMER) != 0) {
        fprintf(stderr, "SDL initialization failed: %s\n", SDL_GetError());
        result = 1;
        goto done;
    }
    SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, "0");
    window = SDL_CreateWindow("iphone1", SDL_WINDOWPOS_UNDEFINED,
                              SDL_WINDOWPOS_UNDEFINED, 320 * (int)scale,
                              480 * (int)scale, SDL_WINDOW_RESIZABLE);
    if (!window) {
        fprintf(stderr, "SDL window creation failed: %s\n", SDL_GetError());
        result = 1;
        goto done;
    }
    renderer = SDL_CreateRenderer(window, -1, SDL_RENDERER_ACCELERATED);
    if (!renderer)
        renderer = SDL_CreateRenderer(window, -1, 0);
    if (!renderer) {
        fprintf(stderr, "SDL renderer creation failed: %s\n", SDL_GetError());
        result = 1;
        goto done;
    }
    SDL_RenderSetLogicalSize(renderer, 320, 480);
    texture = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_RGB24,
                                SDL_TEXTUREACCESS_STREAMING, 320, 480);
    if (!texture) {
        fprintf(stderr, "SDL texture creation failed: %s\n", SDL_GetError());
        result = 1;
        goto done;
    }
    clear_input_state(&input);
    total = m.cpu.cycles;
    start = anchor_host = last_present = last_title = host_now();
    anchor_guest = title_guest = m.timer.ticks;
    for (;;) {
        uint64_t now = host_now();
        uint64_t elapsed_ms = (uint64_t)(host_seconds(now - start) * 1000.0);
        action_context_t actions = {&m, &input, window, save_path, o.load,
                                    shot_dir, &paused, &quit, &reanchor};
        if (exit_after_ms && elapsed_ms >= exit_after_ms)
            quit = true;
        run_script(&actions, &script, elapsed_ms);
        pump_events(&actions);
        flush_pending_inputs(&m, &input);
        if (quit)
            break;
        if (reanchor) {
            anchor_host = now;
            anchor_guest = m.timer.ticks;
            reanchor = false;
        }
        if (paused) {
            SDL_Delay(10);
            present(renderer, texture, &m);
            last_present = host_now();
        } else {
            double guest_ahead = (double)(m.timer.ticks - anchor_guest) /
                                 (double)m.tb_hz -
                                 host_seconds(now - anchor_host);
            if (guest_ahead > 0.002) {
                SDL_Delay(1);
                now = host_now();
                if (host_seconds(now - last_present) >= 0.016) {
                    present(renderer, texture, &m);
                    last_present = now;
                }
            } else {
                uint64_t slice_end = host_now() +
                                     SDL_GetPerformanceFrequency() / 250u;
                while (host_now() < slice_end) {
                    unsigned n;
                    flush_pending_inputs(&m, &input);
                    n = s5l8900_run(&m, 100000, &status);
                    flush_pending_inputs(&m, &input);
                    total += n;
                    title_instr += n;
                    if (!n || status != ARM_OK) {
                        fprintf(stderr, "run stopped reason=%s instructions=%llu\n",
                                n ? status_name(status) : "no progress",
                                (unsigned long long)total);
                        quit = true;
                        break;
                    }
                    if (steps && total >= steps) {
                        fprintf(stderr, "run stopped reason=steps exhausted instructions=%llu\n",
                                (unsigned long long)total);
                        quit = true;
                        break;
                    }
                }
                now = host_now();
                if (host_seconds(now - last_present) >= 0.016) {
                    present(renderer, texture, &m);
                    last_present = now;
                }
            }
        }
        now = host_now();
        if (host_seconds(now - last_title) >= 0.5) {
            double dt = host_seconds(now - last_title);
            double speed = (double)title_instr / dt / 1000000.0;
            double ratio = ((double)(m.timer.ticks - title_guest) /
                            (double)m.tb_hz) / dt;
            char title[256];
            snprintf(title, sizeof title,
                     "iphone1 — %.1f M instr/s — guest/real %.2fx — cycle %llu%s",
                     speed, ratio, (unsigned long long)m.cpu.cycles,
                     paused ? " — PAUSED" : "");
            SDL_SetWindowTitle(window, title);
            if (script_path)
                fprintf(stderr, "%s\n", title);
            last_title = now;
            title_guest = m.timer.ticks;
            title_instr = 0;
        }
        if (m.timer.ticks > anchor_guest &&
            (double)(m.timer.ticks - anchor_guest) / (double)m.tb_hz -
            host_seconds(now - anchor_host) < -0.250) {
            anchor_host = now;
            anchor_guest = m.timer.ticks;
        }
    }

done:
    if (texture) SDL_DestroyTexture(texture);
    if (renderer) SDL_DestroyRenderer(renderer);
    if (window) SDL_DestroyWindow(window);
    SDL_Quit();
    harness_serial_close(&serial);
    s5l8900_free(&m);
    harness_free_blobs(&blobs);
    free(script.items);
    return result;

boot_failure:
    free(script.items);
    return 1;
}
