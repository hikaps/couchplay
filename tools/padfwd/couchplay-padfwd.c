// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 hikaps
//
// couchplay-padfwd: one virtual Xbox 360 pad per player.
//
// Creates a uinput gamepad and forwards the events of any number of grabbed
// physical controllers into it, so a sandboxed player only ever sees one
// stable pad that survives reconnects. Rumble is forwarded back.
//
// Usage: couchplay-padfwd --name "CouchPlay P1 pad"
//
// Stdout (one line each, flushed):
//   ready /dev/input/eventN      virtual pad exists and is accessible
//   error <what> ...             failure
//   lost <path>                  a source disconnected
//   chord                        BTN_START+BTN_SELECT held for 2 s
// Stdin commands: "attach <path>", "detach <path>"; EOF exits.

#define _GNU_SOURCE
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <linux/input.h>
#include <linux/uinput.h>
#include <poll.h>
#include <signal.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/signalfd.h>
#include <time.h>
#include <unistd.h>

#include "padmap.h"

#define MAX_SOURCES 16
#define MAX_EFFECTS 16
#define CMD_MAX 1024
#define NODE_WAIT_MS 3000
#define NODE_POLL_MS 50

struct source {
    int fd;
    char path[PATH_MAX];
    padmap_range abs[ABS_CNT];
    int has_ff;
    int ff_map[MAX_EFFECTS]; // virtual effect id -> source effect id, -1 = none
    int start_held;
    int select_held;
};

static struct source g_sources[MAX_SOURCES];
static int g_nsources;
static int g_uinput = -1;
static struct ff_effect g_effects[MAX_EFFECTS];
static int g_effect_valid[MAX_EFFECTS];
static padmap_chord g_chord;

static void say(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vprintf(fmt, ap);
    va_end(ap);
    fputc('\n', stdout);
    fflush(stdout);
}

static int64_t now_ms(void)
{
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) {
        return 0;
    }
    return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static void destroy_device(void)
{
    if (g_uinput >= 0) {
        ioctl(g_uinput, UI_DEV_DESTROY);
        close(g_uinput);
        g_uinput = -1;
    }
}

static int set_bit(int fd, unsigned long req, int value)
{
    return ioctl(fd, req, value);
}

static int setup_abs(int fd, int code, int min, int max, int fuzz, int flat)
{
    struct uinput_abs_setup as;
    memset(&as, 0, sizeof(as));
    as.code = (uint16_t)code;
    as.absinfo.minimum = min;
    as.absinfo.maximum = max;
    as.absinfo.fuzz = fuzz;
    as.absinfo.flat = flat;
    return ioctl(fd, UI_ABS_SETUP, &as);
}

static int create_device(const char *name)
{
    int fd = open("/dev/uinput", O_RDWR | O_NONBLOCK | O_CLOEXEC);
    if (fd < 0) {
        say("error uinput %s", strerror(errno));
        return -1;
    }

    static const int keys[] = {BTN_A,      BTN_B,    BTN_X,      BTN_Y,       BTN_TL,     BTN_TR,
                               BTN_SELECT, BTN_START, BTN_MODE,  BTN_THUMBL,  BTN_THUMBR};
    static const int sticks[] = {ABS_X, ABS_Y, ABS_RX, ABS_RY};
    static const int triggers[] = {ABS_Z, ABS_RZ};
    static const int hats[] = {ABS_HAT0X, ABS_HAT0Y};

    int ok = set_bit(fd, UI_SET_EVBIT, EV_KEY) == 0 && set_bit(fd, UI_SET_EVBIT, EV_ABS) == 0
        && set_bit(fd, UI_SET_EVBIT, EV_FF) == 0 && set_bit(fd, UI_SET_EVBIT, EV_SYN) == 0
        && set_bit(fd, UI_SET_FFBIT, FF_RUMBLE) == 0;
    for (size_t i = 0; ok && i < sizeof(keys) / sizeof(keys[0]); ++i) {
        ok = set_bit(fd, UI_SET_KEYBIT, keys[i]) == 0;
    }
    for (size_t i = 0; ok && i < sizeof(sticks) / sizeof(sticks[0]); ++i) {
        ok = set_bit(fd, UI_SET_ABSBIT, sticks[i]) == 0 && setup_abs(fd, sticks[i], -32767, 32767, 16, 128) == 0;
    }
    for (size_t i = 0; ok && i < sizeof(triggers) / sizeof(triggers[0]); ++i) {
        ok = set_bit(fd, UI_SET_ABSBIT, triggers[i]) == 0 && setup_abs(fd, triggers[i], 0, 255, 0, 0) == 0;
    }
    for (size_t i = 0; ok && i < sizeof(hats) / sizeof(hats[0]); ++i) {
        ok = set_bit(fd, UI_SET_ABSBIT, hats[i]) == 0 && setup_abs(fd, hats[i], -1, 1, 0, 0) == 0;
    }

    if (ok) {
        struct uinput_setup us;
        memset(&us, 0, sizeof(us));
        us.id.bustype = BUS_USB;
        us.id.vendor = 0x045e;
        us.id.product = 0x028e;
        us.id.version = 0x0110;
        us.ff_effects_max = MAX_EFFECTS;
        snprintf(us.name, sizeof(us.name), "%s", name);
        ok = ioctl(fd, UI_DEV_SETUP, &us) == 0 && ioctl(fd, UI_DEV_CREATE) == 0;
    }
    if (!ok) {
        say("error uinput-setup %s", strerror(errno));
        close(fd);
        return -1;
    }
    return fd;
}

// Finds /dev/input/eventN for the virtual device. Returns 0 on success.
static int resolve_node(int fd, char *node, size_t len)
{
    char sysname[64];
    if (ioctl(fd, UI_GET_SYSNAME(sizeof(sysname)), sysname) < 0) {
        return -1;
    }
    char dir[PATH_MAX];
    snprintf(dir, sizeof(dir), "/sys/devices/virtual/input/%s", sysname);
    DIR *d = opendir(dir);
    if (!d) {
        return -1;
    }
    int found = -1;
    struct dirent *e;
    while ((e = readdir(d)) != NULL) {
        if (strncmp(e->d_name, "event", 5) == 0) {
            snprintf(node, len, "/dev/input/%s", e->d_name);
            found = 0;
            break;
        }
    }
    closedir(d);
    return found;
}

static int wait_for_node(int fd, char *node, size_t len)
{
    node[0] = '\0';
    for (int waited = 0; waited <= NODE_WAIT_MS; waited += NODE_POLL_MS) {
        if (node[0] == '\0') {
            resolve_node(fd, node, len);
        }
        if (node[0] != '\0' && access(node, R_OK | W_OK) == 0) {
            return 0;
        }
        struct timespec ts = {0, NODE_POLL_MS * 1000000L};
        nanosleep(&ts, NULL);
    }
    return -1;
}

static void send_events(const padmap_event *ev, int n)
{
    if (n <= 0) {
        return;
    }
    struct input_event out[128];
    if (n > 128) {
        n = 128;
    }
    memset(out, 0, sizeof(out[0]) * (size_t)n);
    for (int i = 0; i < n; ++i) {
        out[i].type = ev[i].type;
        out[i].code = ev[i].code;
        out[i].value = ev[i].value;
    }
    ssize_t w = write(g_uinput, out, sizeof(out[0]) * (size_t)n);
    (void)w; // EAGAIN on a full queue: dropping input is acceptable
}

static void update_chord(void)
{
    int start = 0, select = 0;
    for (int i = 0; i < g_nsources; ++i) {
        start |= g_sources[i].start_held;
        select |= g_sources[i].select_held;
    }
    int64_t t = now_ms();
    padmap_chord_update(&g_chord, BTN_START, start, t);
    padmap_chord_update(&g_chord, BTN_SELECT, select, t);
}

static void upload_to_source(struct source *s, int vid)
{
    if (!s->has_ff || !g_effect_valid[vid]) {
        return;
    }
    struct ff_effect eff = g_effects[vid];
    eff.id = (int16_t)s->ff_map[vid];
    if (ioctl(s->fd, EVIOCSFF, &eff) == 0) {
        s->ff_map[vid] = eff.id;
    }
}

// Neutral state for every control of the virtual pad, so that a vanished source
// does not leave buttons or sticks held.
static void release_all(void)
{
    static const uint16_t keys[] = {BTN_A,  BTN_B,      BTN_X,      BTN_Y,      BTN_TL,    BTN_TR,
                                    BTN_SELECT, BTN_START, BTN_MODE, BTN_THUMBL, BTN_THUMBR};
    static const uint16_t axes[] = {ABS_X, ABS_Y, ABS_RX, ABS_RY, ABS_Z, ABS_RZ, ABS_HAT0X, ABS_HAT0Y};
    padmap_event ev[sizeof(keys) / sizeof(keys[0]) + sizeof(axes) / sizeof(axes[0]) + 1];
    int n = 0;
    for (size_t i = 0; i < sizeof(keys) / sizeof(keys[0]); ++i) {
        ev[n++] = (padmap_event){EV_KEY, keys[i], 0};
    }
    for (size_t i = 0; i < sizeof(axes) / sizeof(axes[0]); ++i) {
        ev[n++] = (padmap_event){EV_ABS, axes[i], 0};
    }
    ev[n++] = (padmap_event){EV_SYN, SYN_REPORT, 0};
    send_events(ev, n);
}

static void close_source(int idx)
{
    struct source *s = &g_sources[idx];
    ioctl(s->fd, EVIOCGRAB, 0);
    close(s->fd);
    g_sources[idx] = g_sources[g_nsources - 1];
    g_nsources--;
    if (g_nsources == 0 && g_uinput >= 0) {
        release_all();
    }
    update_chord();
}

static int find_source(const char *path)
{
    for (int i = 0; i < g_nsources; ++i) {
        if (strcmp(g_sources[i].path, path) == 0) {
            return i;
        }
    }
    return -1;
}

static void attach_source(const char *path)
{
    if (find_source(path) >= 0) {
        return;
    }
    if (g_nsources >= MAX_SOURCES) {
        say("error attach %s too-many-sources", path);
        return;
    }
    int writable = 1; // rumble is written back to the source, which needs a writable fd
    int fd = open(path, O_RDWR | O_NONBLOCK | O_CLOEXEC);
    if (fd < 0 && (errno == EACCES || errno == EPERM)) {
        writable = 0;
        fd = open(path, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
    }
    if (fd < 0) {
        say("error attach %s %s", path, strerror(errno));
        return;
    }
    if (ioctl(fd, EVIOCGRAB, 1) < 0) {
        int err = errno;
        close(fd);
        say("error attach %s %s", path, strerror(err));
        return;
    }

    struct source *s = &g_sources[g_nsources];
    memset(s, 0, sizeof(*s));
    s->fd = fd;
    snprintf(s->path, sizeof(s->path), "%s", path);
    for (int i = 0; i < MAX_EFFECTS; ++i) {
        s->ff_map[i] = -1;
    }
    for (int code = 0; code < ABS_CNT; ++code) {
        padmap_range dummy;
        uint16_t target = (uint16_t)code;
        if (code == ABS_GAS) {
            target = ABS_RZ;
        } else if (code == ABS_BRAKE) {
            target = ABS_Z;
        }
        if (!padmap_target_range(target, &dummy)) {
            continue;
        }
        struct input_absinfo ai;
        if (ioctl(fd, EVIOCGABS(code), &ai) == 0) {
            s->abs[code].min = ai.minimum;
            s->abs[code].max = ai.maximum;
        }
    }
    unsigned long evbits[(EV_MAX + 8 * sizeof(unsigned long)) / (8 * sizeof(unsigned long))];
    memset(evbits, 0, sizeof(evbits));
    if (ioctl(fd, EVIOCGBIT(0, sizeof(evbits)), evbits) >= 0) {
        s->has_ff = writable && ((evbits[EV_FF / (8 * sizeof(unsigned long))] >> (EV_FF % (8 * sizeof(unsigned long)))) & 1);
    }
    g_nsources++;
    for (int i = 0; i < MAX_EFFECTS; ++i) {
        upload_to_source(s, i);
    }
}

static void detach_source(const char *path)
{
    int idx = find_source(path);
    if (idx >= 0) {
        close_source(idx);
    }
}

// Returns 0 normally, -1 if the source is gone and was closed.
static int pump_source(int idx)
{
    struct input_event buf[64];
    struct source *s = &g_sources[idx];
    for (;;) {
        ssize_t r = read(s->fd, buf, sizeof(buf));
        if (r < 0) {
            if (errno == EINTR) {
                continue;
            }
            if (errno == EAGAIN) {
                return 0;
            }
            say("lost %s", s->path);
            close_source(idx);
            return -1;
        }
        if (r == 0) {
            say("lost %s", s->path);
            close_source(idx);
            return -1;
        }
        size_t n = (size_t)r / sizeof(buf[0]);
        padmap_event out[128];
        int nout = 0;
        int chord_dirty = 0;
        for (size_t i = 0; i < n; ++i) {
            if (buf[i].type == EV_KEY) {
                if (buf[i].code == BTN_START) {
                    s->start_held = buf[i].value != 0;
                    chord_dirty = 1;
                } else if (buf[i].code == BTN_SELECT) {
                    s->select_held = buf[i].value != 0;
                    chord_dirty = 1;
                }
            }
            padmap_event tr[2];
            int k = padmap_translate(buf[i].type, buf[i].code, buf[i].value, s->abs, tr);
            for (int j = 0; j < k; ++j) {
                if (nout == 126) {
                    send_events(out, nout);
                    nout = 0;
                }
                out[nout++] = tr[j];
            }
        }
        send_events(out, nout);
        if (chord_dirty) {
            update_chord();
        }
    }
}

static void handle_ff_upload(int request_id)
{
    struct uinput_ff_upload up;
    memset(&up, 0, sizeof(up));
    up.request_id = (uint32_t)request_id;
    if (ioctl(g_uinput, UI_BEGIN_FF_UPLOAD, &up) < 0) {
        return;
    }
    int vid = up.effect.id;
    if (vid >= 0 && vid < MAX_EFFECTS) {
        g_effects[vid] = up.effect;
        g_effect_valid[vid] = 1;
        for (int i = 0; i < g_nsources; ++i) {
            upload_to_source(&g_sources[i], vid);
        }
    }
    up.retval = 0;
    ioctl(g_uinput, UI_END_FF_UPLOAD, &up);
}

static void handle_ff_erase(int request_id)
{
    struct uinput_ff_erase er;
    memset(&er, 0, sizeof(er));
    er.request_id = (uint32_t)request_id;
    if (ioctl(g_uinput, UI_BEGIN_FF_ERASE, &er) < 0) {
        return;
    }
    int vid = (int)er.effect_id;
    if (vid >= 0 && vid < MAX_EFFECTS) {
        g_effect_valid[vid] = 0;
        for (int i = 0; i < g_nsources; ++i) {
            struct source *s = &g_sources[i];
            if (s->ff_map[vid] >= 0) {
                ioctl(s->fd, EVIOCRMFF, s->ff_map[vid]);
                s->ff_map[vid] = -1;
            }
        }
    }
    er.retval = 0;
    ioctl(g_uinput, UI_END_FF_ERASE, &er);
}

static void pump_uinput(void)
{
    struct input_event buf[32];
    for (;;) {
        ssize_t r = read(g_uinput, buf, sizeof(buf));
        if (r <= 0) {
            return; // EAGAIN / error: nothing more to do
        }
        size_t n = (size_t)r / sizeof(buf[0]);
        for (size_t i = 0; i < n; ++i) {
            const struct input_event *e = &buf[i];
            if (e->type == EV_UINPUT) {
                if (e->code == UI_FF_UPLOAD) {
                    handle_ff_upload(e->value);
                } else if (e->code == UI_FF_ERASE) {
                    handle_ff_erase(e->value);
                }
            } else if (e->type == EV_FF) {
                for (int k = 0; k < g_nsources; ++k) {
                    struct source *s = &g_sources[k];
                    if (!s->has_ff) {
                        continue;
                    }
                    struct input_event out;
                    memset(&out, 0, sizeof(out));
                    out.type = EV_FF;
                    out.value = e->value;
                    if (e->code == FF_GAIN) {
                        out.code = FF_GAIN;
                    } else if (e->code < MAX_EFFECTS && s->ff_map[e->code] >= 0) {
                        out.code = (uint16_t)s->ff_map[e->code];
                    } else {
                        continue;
                    }
                    ssize_t w = write(s->fd, &out, sizeof(out));
                    (void)w;
                }
            }
        }
    }
}

static void handle_command(char *line)
{
    char *sp = strchr(line, ' ');
    if (!sp) {
        return;
    }
    *sp = '\0';
    const char *arg = sp + 1;
    while (*arg == ' ') {
        arg++;
    }
    if (*arg == '\0') {
        return;
    }
    if (strcmp(line, "attach") == 0) {
        attach_source(arg);
    } else if (strcmp(line, "detach") == 0) {
        detach_source(arg);
    }
}

// Reads stdin into the line buffer. Returns -1 on EOF.
static int pump_stdin(char *buf, size_t *len, int *overflow)
{
    char tmp[256];
    ssize_t r = read(STDIN_FILENO, tmp, sizeof(tmp));
    if (r < 0) {
        return (errno == EINTR || errno == EAGAIN) ? 0 : -1;
    }
    if (r == 0) {
        return -1;
    }
    for (ssize_t i = 0; i < r; ++i) {
        char c = tmp[i];
        if (c == '\n') {
            if (!*overflow) {
                buf[*len] = '\0';
                handle_command(buf);
            }
            *len = 0;
            *overflow = 0;
        } else if (c == '\r') {
            continue;
        } else if (*len + 1 < CMD_MAX) {
            buf[(*len)++] = c;
        } else {
            *overflow = 1;
        }
    }
    return 0;
}

int main(int argc, char **argv)
{
    const char *name = NULL;
    for (int i = 1; i < argc; ++i) {
        if (strcmp(argv[i], "--name") == 0 && i + 1 < argc) {
            name = argv[++i];
        } else {
            fprintf(stderr, "usage: couchplay-padfwd --name <name>\n");
            return 64;
        }
    }
    if (!name || !*name) {
        fprintf(stderr, "usage: couchplay-padfwd --name <name>\n");
        return 64;
    }

    sigset_t mask;
    sigemptyset(&mask);
    sigaddset(&mask, SIGTERM);
    sigaddset(&mask, SIGINT);
    sigaddset(&mask, SIGHUP);
    sigaddset(&mask, SIGPIPE);
    if (sigprocmask(SIG_BLOCK, &mask, NULL) != 0) {
        say("error sigprocmask %s", strerror(errno));
        return 1;
    }
    int sfd = signalfd(-1, &mask, SFD_NONBLOCK | SFD_CLOEXEC);
    if (sfd < 0) {
        say("error signalfd %s", strerror(errno));
        return 1;
    }

    g_uinput = create_device(name);
    if (g_uinput < 0) {
        return 1;
    }
    char node[PATH_MAX];
    if (wait_for_node(g_uinput, node, sizeof(node)) != 0) {
        say("error node-not-accessible %s", node[0] ? node : "unknown");
        destroy_device();
        return 1;
    }
    for (int i = 0; i < MAX_SOURCES; ++i) {
        g_sources[i].fd = -1;
    }
    padmap_chord_init(&g_chord);
    say("ready %s", node);

    char cmd[CMD_MAX];
    size_t cmd_len = 0;
    int cmd_overflow = 0;
    int stdin_open = 1;
    int rc = 0;

    while (stdin_open) {
        struct pollfd fds[3 + MAX_SOURCES];
        nfds_t n = 0;
        fds[n++] = (struct pollfd){.fd = STDIN_FILENO, .events = POLLIN};
        fds[n++] = (struct pollfd){.fd = sfd, .events = POLLIN};
        fds[n++] = (struct pollfd){.fd = g_uinput, .events = POLLIN};
        for (int i = 0; i < g_nsources; ++i) {
            fds[n++] = (struct pollfd){.fd = g_sources[i].fd, .events = POLLIN};
        }
        int timeout = padmap_chord_next_ms(&g_chord, now_ms());

        int pr = poll(fds, n, timeout);
        if (pr < 0 && errno != EINTR) {
            say("error poll %s", strerror(errno));
            rc = 1;
            break;
        }

        if (padmap_chord_poll(&g_chord, now_ms())) {
            say("chord");
        }
        if (pr <= 0) {
            continue;
        }

        if (fds[1].revents & POLLIN) {
            break; // SIGTERM/SIGINT/SIGHUP: clean exit
        }
        if (fds[2].revents & POLLIN) {
            pump_uinput();
        }
        // Sources: iterate backwards since close_source() swaps the last entry in.
        // The pollfd order matches g_sources order at poll time.
        for (int i = (int)n - 4; i >= 0; --i) {
            short re = fds[3 + i].revents;
            if (!re || i >= g_nsources || g_sources[i].fd != fds[3 + i].fd) {
                continue;
            }
            if (re & POLLIN) {
                pump_source(i);
            } else if (re & (POLLHUP | POLLERR | POLLNVAL)) {
                say("lost %s", g_sources[i].path);
                close_source(i);
            }
        }
        if (fds[0].revents & (POLLIN | POLLHUP | POLLERR | POLLNVAL)) {
            if (pump_stdin(cmd, &cmd_len, &cmd_overflow) < 0) {
                stdin_open = 0;
            }
        }
    }

    while (g_nsources > 0) {
        close_source(g_nsources - 1);
    }
    destroy_device();
    close(sfd);
    return rc;
}
