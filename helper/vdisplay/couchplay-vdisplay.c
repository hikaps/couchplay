// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 hikaps
//
// couchplay-vdisplay: hold one KWin virtual output open.
//
// KWin creates a virtual output through zkde_screencast_unstable_v1.stream_virtual_output
// and removes it again when the stream object is destroyed. This tiny client keeps the
// stream alive for as long as it runs, so the output exists for exactly one session.
//
// It must run unsandboxed as the desktop user (KWin refuses the protocol to sandboxed
// clients). Exit codes: 0 closed, 1 compositor failure, 2 protocol unavailable, 64 usage.
//
// Usage: couchplay-vdisplay --name <name> --width <w> --height <h>
// Stdout: "READY <pipewire-node>" once the output exists.

#define _GNU_SOURCE
#include <errno.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/prctl.h>
#include <sys/signalfd.h>
#include <unistd.h>

#include <wayland-client.h>

#include "zkde-screencast-unstable-v1-client-protocol.h"

#define MAX_PROTOCOL_VERSION 5

struct state {
    struct zkde_screencast_unstable_v1 *screencast;
    uint32_t screencast_name;
    uint32_t screencast_version;
    int done;
    int exit_code;
};

static void registry_global(void *data, struct wl_registry *registry, uint32_t name, const char *interface, uint32_t version)
{
    struct state *st = data;
    if (strcmp(interface, zkde_screencast_unstable_v1_interface.name) == 0) {
        st->screencast_name = name;
        st->screencast_version = version < MAX_PROTOCOL_VERSION ? version : MAX_PROTOCOL_VERSION;
        st->screencast = wl_registry_bind(registry, name, &zkde_screencast_unstable_v1_interface, st->screencast_version);
    }
}

static void registry_global_remove(void *data, struct wl_registry *registry, uint32_t name)
{
    (void)data;
    (void)registry;
    (void)name;
}

static const struct wl_registry_listener registry_listener = {
    .global = registry_global,
    .global_remove = registry_global_remove,
};

static void stream_closed(void *data, struct zkde_screencast_stream_unstable_v1 *stream)
{
    (void)stream;
    struct state *st = data;
    st->done = 1;
}

static void stream_created(void *data, struct zkde_screencast_stream_unstable_v1 *stream, uint32_t node)
{
    (void)data;
    (void)stream;
    printf("READY %u\n", node);
    fflush(stdout);
}

static void stream_failed(void *data, struct zkde_screencast_stream_unstable_v1 *stream, const char *error)
{
    (void)stream;
    struct state *st = data;
    fprintf(stderr, "FAILED %s\n", error ? error : "unknown error");
    st->exit_code = 1;
    st->done = 1;
}

static void stream_serial(void *data, struct zkde_screencast_stream_unstable_v1 *stream, uint32_t hi, uint32_t lo)
{
    (void)data;
    (void)stream;
    (void)hi;
    (void)lo;
}

static const struct zkde_screencast_stream_unstable_v1_listener stream_listener = {
    .closed = stream_closed,
    .created = stream_created,
    .failed = stream_failed,
    .serial = stream_serial,
};

static int parse_positive(const char *text, int *out)
{
    char *end = NULL;
    errno = 0;
    long value = strtol(text, &end, 10);
    if (errno != 0 || end == text || *end != '\0' || value < 1 || value > 16384) {
        return -1;
    }
    *out = (int)value;
    return 0;
}

static int usage(void)
{
    fprintf(stderr, "usage: couchplay-vdisplay --name <name> --width <w> --height <h>\n");
    return 64;
}

int main(int argc, char **argv)
{
    const char *name = NULL;
    int width = 0;
    int height = 0;
    for (int i = 1; i < argc; ++i) {
        if (strcmp(argv[i], "--name") == 0 && i + 1 < argc) {
            name = argv[++i];
        } else if (strcmp(argv[i], "--width") == 0 && i + 1 < argc) {
            if (parse_positive(argv[++i], &width) != 0) {
                return usage();
            }
        } else if (strcmp(argv[i], "--height") == 0 && i + 1 < argc) {
            if (parse_positive(argv[++i], &height) != 0) {
                return usage();
            }
        } else {
            return usage();
        }
    }
    if (!name || !*name || width == 0 || height == 0) {
        return usage();
    }

    // Die with the parent (the GUI or flatpak-spawn) so a crashed session cannot leak the output.
    prctl(PR_SET_PDEATHSIG, SIGTERM);
    if (getppid() == 1) {
        return 0;
    }

    sigset_t mask;
    sigemptyset(&mask);
    sigaddset(&mask, SIGTERM);
    sigaddset(&mask, SIGINT);
    sigprocmask(SIG_BLOCK, &mask, NULL);
    int sigfd = signalfd(-1, &mask, SFD_CLOEXEC);
    if (sigfd < 0) {
        perror("signalfd");
        return 1;
    }

    struct wl_display *display = wl_display_connect(NULL);
    if (!display) {
        fprintf(stderr, "FAILED cannot connect to the Wayland display\n");
        return 1;
    }

    struct state st = {0};
    struct wl_registry *registry = wl_display_get_registry(display);
    wl_registry_add_listener(registry, &registry_listener, &st);
    wl_display_roundtrip(display);

    if (!st.screencast || st.screencast_version < 2) {
        fprintf(stderr, "FAILED kwin screencast protocol unavailable\n");
        return 2;
    }

    struct zkde_screencast_stream_unstable_v1 *stream = zkde_screencast_unstable_v1_stream_virtual_output(
        st.screencast, name, width, height, wl_fixed_from_int(1), ZKDE_SCREENCAST_UNSTABLE_V1_POINTER_EMBEDDED);
    zkde_screencast_stream_unstable_v1_add_listener(stream, &stream_listener, &st);

    while (!st.done) {
        while (wl_display_prepare_read(display) != 0) {
            wl_display_dispatch_pending(display);
        }
        if (wl_display_flush(display) < 0 && errno != EAGAIN) {
            wl_display_cancel_read(display);
            st.exit_code = 1;
            break;
        }

        struct pollfd fds[2] = {
            {.fd = wl_display_get_fd(display), .events = POLLIN},
            {.fd = sigfd, .events = POLLIN},
        };
        if (poll(fds, 2, -1) < 0 && errno != EINTR) {
            wl_display_cancel_read(display);
            st.exit_code = 1;
            break;
        }

        if (fds[0].revents & (POLLIN | POLLERR | POLLHUP)) {
            if (wl_display_read_events(display) < 0) {
                st.exit_code = 1;
                break;
            }
            wl_display_dispatch_pending(display);
        } else {
            wl_display_cancel_read(display);
        }

        if (fds[1].revents & POLLIN) {
            break;
        }
    }

    // Closing the stream removes the virtual output.
    zkde_screencast_stream_unstable_v1_close(stream);
    wl_display_flush(display);
    wl_display_roundtrip(display);
    wl_display_disconnect(display);
    return st.exit_code;
}
