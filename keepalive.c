/*
 * keepalive - invisible VRR keep-alive for KWin/Wayland
 *
 * Presents a 1x1 fully-transparent frame continuously so an "Always" VRR panel
 * stays pinned at its max refresh on a static desktop instead of free-running
 * down to its low floor (which causes flicker on every input event).
 *
 * Design (all handled client-side, no kwinrulesrc / no KWin scripting):
 *   - zwlr_layer_shell_v1 surface on the TOP layer, so a fullscreen window
 *     renders ABOVE us -> KWin occlusion-culls us -> we naturally stop
 *     presenting under a fullscreen app (the app holds refresh itself).
 *     Pass -l overlay to sit above fullscreen instead.
 *   - keyboard_interactivity = NONE      -> cannot take keyboard focus, ever
 *   - empty wl_surface input region      -> fully click-through, no pointer
 *   - wp_single_pixel_buffer (0,0,0,0)   -> 1x1 transparent buffer, no EGL/GL
 *   - wl_callback.frame pacing           -> vblank-paced, low CPU, and blocks
 *                                           (pauses) when the compositor stops
 *                                           presenting our surface
 *   - SIGINT/SIGTERM handled via a non-blocking self-pipe so the process can
 *     always be killed cleanly (focus can never be stolen).
 */
#define _GNU_SOURCE
#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include <unistd.h>
#include <errno.h>
#include <signal.h>
#include <poll.h>
#include <fcntl.h>
#include <time.h>
#include <wayland-client.h>
#include "protocols/wlr-layer-shell-v1-client.h"
#include "protocols/single-pixel-buffer-v1-client.h"

struct app {
    struct wl_display *display;
    struct wl_registry *registry;
    struct wl_compositor *compositor;
    struct wp_single_pixel_buffer_manager_v1 *spm;
    struct zwlr_layer_shell_v1 *layer_shell;
    struct wl_output *output;               /* first output seen (target) */
    struct wl_surface *surface;
    struct zwlr_layer_surface_v1 *layer_surface;
    struct wl_buffer *buffer;               /* 1x1 transparent single pixel */
    uint32_t layer;                         /* ZWLR_LAYER_SHELL_V1_LAYER_*  */
    bool started;                           /* first configure received     */
    bool cb_pending;                        /* frame callback in flight     */
    bool running;
    uint64_t frames;
    double fps_anchor;                      /* CLOCK_MONOTONIC at last report */
    uint64_t fps_frames;
};

static int g_self_pipe[2] = { -1, -1 };      /* signal -> main loop          */
static volatile sig_atomic_t g_sig;          /* last signal caught           */

static void commit_frame(struct app *a);
static const struct wl_callback_listener frame_listener;

/* ---- signal handling: byte into self-pipe, decoded safely in main loop ---- */
static void sighandler(int sig)
{
    g_sig = sig;
    if (g_self_pipe[1] >= 0) {
        char c = 'x';
        ssize_t ignored = write(g_self_pipe[1], &c, 1);
        (void)ignored;
    }
}

/* ---- registry globals ---- */
static void registry_handle_global(void *data, struct wl_registry *registry,
                                   uint32_t name, const char *iface,
                                   uint32_t version)
{
    struct app *a = data;
    (void)version;
    if (strcmp(iface, "wl_compositor") == 0) {
        a->compositor = wl_registry_bind(registry, name, &wl_compositor_interface, 4);
    } else if (strcmp(iface, "zwlr_layer_shell_v1") == 0) {
        a->layer_shell = wl_registry_bind(registry, name,
                                          &zwlr_layer_shell_v1_interface, 1);
    } else if (strcmp(iface, "wp_single_pixel_buffer_manager_v1") == 0) {
        a->spm = wl_registry_bind(registry, name,
                                  &wp_single_pixel_buffer_manager_v1_interface, 1);
    } else if (strcmp(iface, "wl_output") == 0 && !a->output) {
        a->output = wl_registry_bind(registry, name, &wl_output_interface, 1);
    }
}

static void registry_handle_global_remove(void *data, struct wl_registry *registry,
                                          uint32_t name)
{
    (void)data; (void)registry; (void)name;
}

static const struct wl_registry_listener registry_listener = {
    .global = registry_handle_global,
    .global_remove = registry_handle_global_remove,
};

/* ---- frame callback: compositor present done -> commit next frame ----
 * If the compositor stops presenting our surface (e.g. occluded by a
 * fullscreen app), no frame callback arrives and commit_frame() is not called
 * again: we pause for free. */
static void frame_done_cb(void *data, struct wl_callback *cb, uint32_t time)
{
    (void)time;
    struct app *a = data;
    wl_callback_destroy(cb);
    a->cb_pending = false;
    if (a->running)
        commit_frame(a);
}

static const struct wl_callback_listener frame_listener = {
    .done = frame_done_cb,
};

/* attach the 1x1 transparent buffer + install next frame callback + commit */
static void commit_frame(struct app *a)
{
    if (!a->started || a->cb_pending)
        return;
    a->cb_pending = true;
    struct wl_callback *cb = wl_surface_frame(a->surface);
    wl_callback_add_listener(cb, &frame_listener, a);
    wl_surface_attach(a->surface, a->buffer, 0, 0);
    wl_surface_damage_buffer(a->surface, 0, 0, 1, 1);
    wl_surface_commit(a->surface);
    wl_display_flush(a->display);
    a->frames++;
}

/* ---- layer surface ---- */
static void ls_configure(void *data, struct zwlr_layer_surface_v1 *ls,
                         uint32_t serial, uint32_t w, uint32_t h)
{
    struct app *a = data;
    zwlr_layer_surface_v1_ack_configure(ls, serial);
    (void)w; (void)h;
    if (!a->started) {
        a->started = true;
        commit_frame(a); /* begin continuous present loop */
    }
}

static void ls_closed(void *data, struct zwlr_layer_surface_v1 *ls)
{
    (void)ls;
    struct app *a = data;
    a->running = false;
}

static const struct zwlr_layer_surface_v1_listener ls_listener = {
    .configure = ls_configure,
    .closed = ls_closed,
};

static void create_layer_surface(struct app *a)
{
    a->surface = wl_compositor_create_surface(a->compositor);
    a->layer_surface =
        zwlr_layer_shell_v1_get_layer_surface(a->layer_shell, a->surface,
                                              a->output, a->layer, "keepalive");
    zwlr_layer_surface_v1_add_listener(a->layer_surface, &ls_listener, a);
    zwlr_layer_surface_v1_set_size(a->layer_surface, 1, 1);
    zwlr_layer_surface_v1_set_anchor(a->layer_surface,
        ZWLR_LAYER_SURFACE_V1_ANCHOR_BOTTOM | ZWLR_LAYER_SURFACE_V1_ANCHOR_RIGHT);
    zwlr_layer_surface_v1_set_exclusive_zone(a->layer_surface, 0);
    zwlr_layer_surface_v1_set_keyboard_interactivity(a->layer_surface,
        ZWLR_LAYER_SURFACE_V1_KEYBOARD_INTERACTIVITY_NONE);

    /* fully click-through: empty input region */
    struct wl_region *region = wl_compositor_create_region(a->compositor);
    wl_surface_set_input_region(a->surface, region);
    wl_region_destroy(region); /* compositor copies the region */

    /* 1x1 fully-transparent buffer (premultiplied RGBA, all 0) */
    a->buffer = wp_single_pixel_buffer_manager_v1_create_u32_rgba_buffer(
        a->spm, 0, 0, 0, 0);

    /* initial commit WITHOUT a buffer (required before attaching one) */
    wl_surface_commit(a->surface);
    wl_display_flush(a->display);
}

/* ---- main loop ---- */
static void report_fps(struct app *a)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    double now = ts.tv_sec + ts.tv_nsec / 1e9;
    double elapsed = now - a->fps_anchor;
    if (elapsed >= 5.0) {
        fprintf(stderr, "keepalive: %.0f fps (over %.0f s)\n",
                (a->frames - a->fps_frames) / elapsed, elapsed);
        a->fps_anchor = now;
        a->fps_frames = a->frames;
    }
}

static int run(struct app *a)
{
    int dispfd = wl_display_get_fd(a->display);
    while (a->running) {
        wl_display_flush(a->display);

        struct pollfd fds[2];
        int n = 0;
        fds[n].fd = dispfd;    fds[n].events = POLLIN; n++;
        fds[n].fd = g_self_pipe[0]; fds[n].events = POLLIN; n++;

        int r = poll(fds, n, -1);
        if (r < 0) {
            if (errno == EINTR) continue;
            fprintf(stderr, "keepalive: poll failed: %s\n", strerror(errno));
            break;
        }

        /* self-pipe -> signal caught */
        if (fds[1].revents & POLLIN) {
            char b[64];
            while (read(g_self_pipe[0], b, sizeof b) > 0) { }
            fprintf(stderr, "keepalive: signal %d, exiting\n", (int)g_sig);
            break;
        }

        /* wayland events (drives frame callbacks -> pacing / pause) */
        if (fds[0].revents & POLLIN) {
            while (wl_display_prepare_read(a->display) != 0)
                wl_display_dispatch_pending(a->display);
            wl_display_flush(a->display);
            wl_display_read_events(a->display);
            wl_display_dispatch_pending(a->display);
        }

        report_fps(a);
    }

    return 0;
}

static void usage(const char *prog)
{
    fprintf(stderr,
        "usage: %s [-l top|overlay]\n"
        "\n"
        "  -l layer   layer-shell layer (default: top)\n"
        "               top    -> below fullscreen windows (they cover us; we\n"
        "                         auto-pause under fullscreen) [default]\n"
        "               overlay-> above fullscreen windows (never paused)\n"
        "\n"
        "Invisible VRR keep-alive: presents a 1x1 transparent frame every\n"
        "vblank so an 'Always' VRR panel stays at max refresh on a static\n"
        "desktop. Runs until SIGINT/SIGTERM.\n",
        prog);
}

int main(int argc, char **argv)
{
    struct app a;
    memset(&a, 0, sizeof a);
    a.layer = ZWLR_LAYER_SHELL_V1_LAYER_TOP;
    a.running = true;

    int opt;
    while ((opt = getopt(argc, argv, "l:h")) != -1) {
        switch (opt) {
        case 'l':
            if (strcmp(optarg, "top") == 0)      a.layer = ZWLR_LAYER_SHELL_V1_LAYER_TOP;
            else if (strcmp(optarg, "overlay") == 0) a.layer = ZWLR_LAYER_SHELL_V1_LAYER_OVERLAY;
            else { fprintf(stderr, "bad layer '%s'\n", optarg); usage(argv[0]); return 1; }
            break;
        case 'h': usage(argv[0]); return 0;
        default: usage(argv[0]); return 1;
        }
    }

    if (pipe2(g_self_pipe, O_CLOEXEC | O_NONBLOCK) != 0) { perror("pipe2"); return 1; }
    signal(SIGINT, sighandler);
    signal(SIGTERM, sighandler);

    a.display = wl_display_connect(NULL);
    if (!a.display) {
        fprintf(stderr, "keepalive: cannot connect to Wayland compositor "
                        "(WAYLAND_DISPLAY set?)\n");
        return 1;
    }
    a.registry = wl_display_get_registry(a.display);
    wl_registry_add_listener(a.registry, &registry_listener, &a);
    wl_display_roundtrip(a.display); /* populate globals */

    if (!a.compositor || !a.layer_shell || !a.spm) {
        fprintf(stderr, "keepalive: required globals missing "
                        "(compositor=%d layer_shell=%d single_pixel=%d)\n",
                a.compositor ? 1 : 0, a.layer_shell ? 1 : 0, a.spm ? 1 : 0);
        return 1;
    }

    create_layer_surface(&a);
    fprintf(stderr, "keepalive: running on layer %s (1x1), Ctrl-C / SIGTERM to stop\n",
            a.layer == ZWLR_LAYER_SHELL_V1_LAYER_TOP ? "top" : "overlay");

    struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
    a.fps_anchor = ts.tv_sec + ts.tv_nsec / 1e9;

    int rc = run(&a);

    /* cleanup */
    if (a.buffer) wl_buffer_destroy(a.buffer);
    if (a.layer_surface) zwlr_layer_surface_v1_destroy(a.layer_surface);
    if (a.surface) wl_surface_destroy(a.surface);
    wl_display_disconnect(a.display);
    return rc;
}
