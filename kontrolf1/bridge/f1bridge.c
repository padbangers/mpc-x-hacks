/* f1bridge.so - a Native Instruments Traktor Kontrol F1 as a second set of MPC X pads, inside the MPC app.
 *
 * Preloaded into the MPC process (like shim_remap6.so) and inert in every other process.
 *   - F1 pads are injected into the app's control-surface stream as if the MPC's own pads were hit
 *     (99 nn 7f / 89 nn 00), so every pad mode works: playing, Step Sequencer, Pad Mute, ...
 *   - The pad colours the app sends to its surface (SysEx f0 47 7f 3a 65 00 04 <pad> <r> <g> <b> f7) are mirrored
 *     onto the F1's RGB pads.
 *   - Knobs and faders send CCs out of a hardware-type ALSA seq port "Kontrol F1" (map them with MIDI learn).
 *   - The encoder turns the MPC's data wheel (b0 64 01 / b0 64 7f).
 *
 * Injection: the app's "MIDI" thread poll()s an eventfd of its own plus the surface input fd (non-blocking), then
 * reads with snd_rawmidi_read(). poll() is wrapped to also wait on our own eventfd; when F1 bytes are pending the
 * surface fd is reported readable and the next snd_rawmidi_read() on the surface returns them.
 *
 * F1 protocol (hidraw, USB 17cc:1120), measured on the device 2026-09-30:
 *   in  0x01: 4 button bytes MSB first (pads 1-16 row by row from top left, then shift reverse size type browse
 *             encoder-push - -, stop1-4 sync quant capture -), encoder counter u8, 4 knobs + 4 faders u16le 0..4095
 *   out 0x80: 80 brightness bytes 0..127: right digit (dot g c b a f e d), left digit, browse size type reverse
 *             shift capture quant sync, 16 pads x (blue red green), 4 stops x 2 (stop 4 first)
 *
 * Log: /data/hacks/f1bridge.log
 * Build: zig cc -target arm-linux-gnueabihf.2.31 -shared -fPIC -O2 -o f1bridge.so f1bridge.c
 */
#define _GNU_SOURCE
#include <dirent.h>
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/eventfd.h>
#include <time.h>
#include <unistd.h>

/* ---- ALSA ABI subset (alsa/seq_event.h, alsa/seq.h) ---- */
typedef struct _snd_rawmidi snd_rawmidi_t;
typedef struct _snd_seq snd_seq_t;
typedef struct {
    unsigned char type, flags, tag, queue;
    unsigned int time[2];
    unsigned char source_client, source_port, dest_client, dest_port;
    union {
        struct { unsigned char channel, unused[3]; unsigned int param; int value; } control;
        unsigned char raw[12];
    } data;
} seq_event_t;
enum { SEQ_OPEN_OUTPUT = 1, SEQ_NONBLOCK = 1 };
enum { CAP_READ = 1 << 0, CAP_SUBS_READ = 1 << 5, TYPE_MIDI_GENERIC = 1 << 1, TYPE_HARDWARE = 1 << 16 };
enum { EVENT_CONTROLLER = 10, QUEUE_DIRECT = 253, ADDRESS_SUBSCRIBERS = 254, ADDRESS_UNKNOWN = 253 };

static int (*real_rawmidi_open)(snd_rawmidi_t **, snd_rawmidi_t **, const char *, int);
static ssize_t (*real_rawmidi_read)(snd_rawmidi_t *, void *, size_t);
static int (*real_rawmidi_poll_descriptors)(snd_rawmidi_t *, struct pollfd *, unsigned int);
static int (*real_poll)(struct pollfd *, nfds_t, int);
static ssize_t (*real_write)(int, const void *, size_t);
#define REAL(var, name) (var ? var : (var = dlsym(RTLD_NEXT, name)))

/* ---- settings ---- */
static const uint8_t kPadNotes[16] = { 37, 36, 42, 82, 40, 38, 46, 44, 48, 47, 45, 43, 49, 55, 51, 53 };  /* MPC pads 1-16 */
static int pad_velocity = 127;       /* F1 pads have no velocity */
static int cc_channel = 0;           /* MIDI channel 1 */
static int knob_cc = 20, fader_cc = 24, shift_knob_cc = 28, shift_fader_cc = 32;
static const char *port_name = "Kontrol F1";

enum {
    BTN_PAD1 = 0, BTN_SHIFT = 16, BTN_REVERSE, BTN_SIZE, BTN_TYPE, BTN_BROWSE, BTN_ENCODER,
    BTN_STOP1 = 24, BTN_SYNC = 28, BTN_QUANT, BTN_CAPTURE,
};
enum { LED_DIGIT_R = 0, LED_DIGIT_L = 8, LED_BROWSE = 16, LED_PADS = 24, LED_STOPS = 72, LED_COUNT = 80 };
static const char kHidId[] = "HID_ID=0003:000017CC:00001120";

/* ---- state ---- */
static int active;
static FILE *logf;
static snd_rawmidi_t *surface_in;
static atomic_int surface_fd = -1;
static int wake_fd = -1;

static pthread_mutex_t inject_lock = PTHREAD_MUTEX_INITIALIZER;
static unsigned char inject_buf[512];
static int inject_len;
static atomic_int inject_pending;

static uint8_t mpc_pads[16][3];      /* r g b, 0..127, MPC pad index (0 = A01, bottom left) */
static atomic_uint mpc_pads_version;
static atomic_int step_mode;         /* the app's STEP SEQ button LED is on */

/* Surface button codes (ch0 notes) and LEDs (b0 <code> <0|3>), from the capture of 2026-09-30. */
enum { CODE_MAIN = 0x34, CODE_STEP_SEQ = 0x29, CODE_SHIFT = 0x31, CODE_MUTE = 0x4c, CODE_SOLO = 0x4d };

/* Track selection: the global mapping (Track Remap.xmm) turns CC 74 on channel 1, relative, into Change Track. */
enum { CC_TRACK_REL = 74 };

/* Track colours (r g b, 0..127) for tracks 1-4: red, cyan, orange, pink. */
static const uint8_t kTrackColour[4][3] = { { 127, 0, 0 }, { 0, 127, 127 }, { 127, 35, 0 }, { 127, 10, 50 } };
static const uint8_t kPlayhead[3] = { 0, 127, 0 };   /* green, whatever the track colour */
static const uint8_t kOtherTrackColour[3] = { 90, 90, 90 };
enum { MAX_TRACK = 63 };
static atomic_int selected_track;    /* 0-based; set by the encoder (the app's own track changes aren't seen) */
static atomic_int browse_held;       /* Browse + pad: pick the pad for the Step Sequencer */
static atomic_int app_track;         /* the track the app is on as far as we know (we switch it for mute/solo) */
static atomic_uchar track_muted[MAX_TRACK + 1], track_soloed[MAX_TRACK + 1];   /* from the MUTE/SOLO LEDs */

/* Trace (touch /tmp/f1trace): log surface buttons the app reads, button LED changes and pad colours. */
static int tracing(void) { return access("/tmp/f1trace", F_OK) == 0; }

static snd_seq_t *seq;
static int seq_port = -1;
static int (*seq_output_direct)(snd_seq_t *, seq_event_t *);

static void logmsg(const char *fmt, ...) {
    if (!logf) return;
    struct timespec t;
    clock_gettime(CLOCK_REALTIME, &t);
    struct tm tm;
    localtime_r(&t.tv_sec, &tm);
    fprintf(logf, "%02d:%02d:%02d ", tm.tm_hour, tm.tm_min, tm.tm_sec);
    va_list ap;
    va_start(ap, fmt);
    vfprintf(logf, fmt, ap);
    va_end(ap);
    fputc('\n', logf);
    fflush(logf);
}

static int64_t now_ms(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (int64_t)t.tv_sec * 1000 + t.tv_nsec / 1000000;
}

/* ---- surface injection ---- */

static void inject(const unsigned char *bytes, int n) {
    pthread_mutex_lock(&inject_lock);
    if (inject_len + n <= (int)sizeof inject_buf) {
        memcpy(inject_buf + inject_len, bytes, n);
        inject_len += n;
    }
    atomic_store(&inject_pending, 1);
    pthread_mutex_unlock(&inject_lock);
    uint64_t one = 1;
    if (wake_fd >= 0) REAL(real_write, "write")(wake_fd, &one, sizeof one);
}

static int take_injected(void *buffer, size_t size) {
    pthread_mutex_lock(&inject_lock);
    int n = inject_len < (int)size ? inject_len : (int)size;
    /* never split a 3-byte message */
    n -= n % 3;
    memcpy(buffer, inject_buf, n);
    memmove(inject_buf, inject_buf + n, inject_len - n);
    inject_len -= n;
    atomic_store(&inject_pending, inject_len > 0);
    pthread_mutex_unlock(&inject_lock);
    return n;
}

ssize_t snd_rawmidi_read(snd_rawmidi_t *rmidi, void *buffer, size_t size) {
    if (active && rmidi == surface_in && atomic_load(&inject_pending)) {
        int n = take_injected(buffer, size);
        if (n > 0) return n;
    }
    ssize_t n = REAL(real_rawmidi_read, "snd_rawmidi_read")(rmidi, buffer, size);
    if (active && rmidi == surface_in && n >= 3 && tracing()) {
        const unsigned char *b = buffer;
        for (ssize_t i = 0; i + 2 < n; i++) {
            if (b[i] == 0x90 && b[i + 2]) logmsg("trace: surface button 0x%02x", b[i + 1]);
            if (b[i] == 0x99 && b[i + 2]) logmsg("trace: surface pad note %d", b[i + 1]);
        }
    }
    return n;
}

int snd_rawmidi_open(snd_rawmidi_t **in, snd_rawmidi_t **out, const char *name, int mode) {
    int r = REAL(real_rawmidi_open, "snd_rawmidi_open")(in, out, name, mode);
    if (active && r == 0 && name && in && *in && strstr(name, "2,0,1")) {
        surface_in = *in;
        struct pollfd p;
        if (REAL(real_rawmidi_poll_descriptors, "snd_rawmidi_poll_descriptors")(*in, &p, 1) == 1) {
            atomic_store(&surface_fd, p.fd);
        }
        logmsg("surface input %s fd=%d", name, atomic_load(&surface_fd));
    }
    return r;
}

/* The app's MIDI thread waits here for the surface; we add our wake fd. */
int poll(struct pollfd *fds, nfds_t nfds, int timeout) {
    int sfd = atomic_load(&surface_fd);
    if (!active || sfd < 0 || wake_fd < 0 || nfds > 15) return REAL(real_poll, "poll")(fds, nfds, timeout);
    int at = -1;
    for (nfds_t i = 0; i < nfds; i++) {
        if (fds[i].fd == sfd && (fds[i].events & POLLIN)) at = (int)i;
    }
    if (at < 0) return REAL(real_poll, "poll")(fds, nfds, timeout);

    struct pollfd all[16];
    memcpy(all, fds, nfds * sizeof *fds);
    all[nfds].fd = wake_fd;
    all[nfds].events = POLLIN;
    all[nfds].revents = 0;
    int r = REAL(real_poll, "poll")(all, nfds + 1, atomic_load(&inject_pending) ? 0 : timeout);
    if (r < 0) return r;
    if (all[nfds].revents & POLLIN) {
        uint64_t v;
        (void)read(wake_fd, &v, sizeof v);
    }
    if (atomic_load(&inject_pending)) all[at].revents |= POLLIN;
    int count = 0;
    for (nfds_t i = 0; i < nfds; i++) {
        fds[i].revents = all[i].revents;
        if (fds[i].revents) count++;
    }
    return count;
}

/* ---- pad colours out of the app ---- */

static void scan_pad_colours(const unsigned char *b, size_t n) {
    static const unsigned char head[7] = { 0xf0, 0x47, 0x7f, 0x3a, 0x65, 0x00, 0x04 };
    int changed = 0;
    for (size_t i = 0; i + 12 <= n; i++) {
        if (b[i] != 0xf0 || memcmp(b + i, head, 7) || b[i + 11] != 0xf7) continue;
        int pad = b[i + 7];
        if (pad < 16) {
            mpc_pads[pad][0] = b[i + 8];
            mpc_pads[pad][1] = b[i + 9];
            mpc_pads[pad][2] = b[i + 10];
            changed = 1;
        }
        i += 11;
    }
    if (changed) {
        atomic_fetch_add(&mpc_pads_version, 1);
        if (tracing()) {
            char line[16 * 8 + 1];
            int o = 0;
            for (int p = 0; p < 16; p++) o += snprintf(line + o, sizeof line - o, "%02x%02x%02x ", mpc_pads[p][0], mpc_pads[p][1], mpc_pads[p][2]);
            logmsg("trace: pads %s", line);
        }
    }
}

/* Button LEDs go out as b0 <code> <value>; the STEP SEQ LED tells us the app is in the Step Sequencer. */
static unsigned char led_state[128];

static void scan_button_leds(const unsigned char *b, size_t n) {
    for (size_t i = 0; i + 3 <= n; i += 3) {
        if (b[i] != 0xb0) return;
        if (b[i + 1] == CODE_MUTE || b[i + 1] == CODE_SOLO) {
            /* The MUTE and SOLO LEDs show the current track's state. */
            int t = atomic_load(&app_track);
            atomic_store(b[i + 1] == CODE_MUTE ? &track_muted[t] : &track_soloed[t], b[i + 2] != 0);
            atomic_fetch_add(&mpc_pads_version, 1);
        }
        if (b[i + 1] < 128 && led_state[b[i + 1]] != b[i + 2]) {
            led_state[b[i + 1]] = b[i + 2];
            if (tracing() && b[i + 1] != 0x35 && b[i + 1] != 0x43) logmsg("trace: led 0x%02x = %d", b[i + 1], b[i + 2]);
        }
        if (b[i + 1] == CODE_STEP_SEQ) {
            int on = b[i + 2] != 0;
            if (atomic_exchange(&step_mode, on) != on) {
                atomic_fetch_add(&mpc_pads_version, 1);
                logmsg("step sequencer %s", on ? "on" : "off");
            }
        }
    }
}

/* Only writes to the MPC's own rawmidi device are looked at (fd classes cached: 1 = surface, 2 = other). */
static unsigned char fd_class[1024];

static int is_surface_fd(int fd) {
    if (fd < 0 || fd >= 1024) return 0;
    if (!fd_class[fd]) {
        char p[32], t[64];
        snprintf(p, sizeof p, "/proc/self/fd/%d", fd);
        ssize_t n = readlink(p, t, sizeof t - 1);
        fd_class[fd] = n > 0 && (t[n] = 0, !strncmp(t, "/dev/snd/midiC2D0", 17)) ? 1 : 2;
    }
    return fd_class[fd] == 1;
}

ssize_t write(int fd, const void *buf, size_t n) {
    const unsigned char *b = buf;
    if (active && n >= 3 && (b[0] == 0xf0 || b[0] == 0xb0) && is_surface_fd(fd)) {
        if (b[0] == 0xf0) scan_pad_colours(b, n);
        else scan_button_leds(b, n);
    }
    return REAL(real_write, "write")(fd, buf, n);
}

int close(int fd) {
    static int (*real_close)(int);
    if (fd >= 0 && fd < 1024) fd_class[fd] = 0;
    return REAL(real_close, "close")(fd);
}

/* ---- CCs out of our own port ---- */

static void open_port(void) {
    int (*open)(snd_seq_t **, const char *, int, int) = dlsym(RTLD_DEFAULT, "snd_seq_open");
    int (*set_name)(snd_seq_t *, const char *) = dlsym(RTLD_DEFAULT, "snd_seq_set_client_name");
    int (*create_port)(snd_seq_t *, const char *, unsigned, unsigned) = dlsym(RTLD_DEFAULT, "snd_seq_create_simple_port");
    seq_output_direct = dlsym(RTLD_DEFAULT, "snd_seq_event_output_direct");
    if (!open || !set_name || !create_port || !seq_output_direct) {
        logmsg("ALSA seq functions missing");
        return;
    }
    if (open(&seq, "default", SEQ_OPEN_OUTPUT, SEQ_NONBLOCK) < 0) {
        seq = NULL;
        logmsg("snd_seq_open failed");
        return;
    }
    set_name(seq, port_name);
    seq_port = create_port(seq, port_name, CAP_READ | CAP_SUBS_READ, TYPE_MIDI_GENERIC | TYPE_HARDWARE);
    logmsg("seq port '%s' = %d", port_name, seq_port);
}

static void send_cc(int cc, int value) {
    if (!seq || seq_port < 0) return;
    seq_event_t ev;
    memset(&ev, 0, sizeof ev);
    ev.type = EVENT_CONTROLLER;
    ev.queue = QUEUE_DIRECT;
    ev.source_port = (unsigned char)seq_port;
    ev.dest_client = ADDRESS_SUBSCRIBERS;
    ev.dest_port = ADDRESS_UNKNOWN;
    ev.data.control.channel = (unsigned char)cc_channel;
    ev.data.control.param = (unsigned)cc;
    ev.data.control.value = value;
    seq_output_direct(seq, &ev);
}

/* ---- the F1 ---- */

static int open_f1(void) {
    DIR *dir = opendir("/sys/class/hidraw");
    if (!dir) return -1;
    int fd = -1;
    struct dirent *e;
    while ((e = readdir(dir))) {
        if (strncmp(e->d_name, "hidraw", 6)) continue;
        char path[300], uevent[512];
        snprintf(path, sizeof path, "/sys/class/hidraw/%s/device/uevent", e->d_name);
        int u = open(path, O_RDONLY);
        if (u < 0) continue;
        int n = (int)read(u, uevent, sizeof uevent - 1);
        close(u);
        if (n <= 0) continue;
        uevent[n] = 0;
        if (!strstr(uevent, kHidId)) continue;
        snprintf(path, sizeof path, "/dev/%s", e->d_name);
        fd = open(path, O_RDWR | O_NONBLOCK);
        if (fd >= 0) {
            logmsg("Kontrol F1 on %s", path);
            break;
        }
    }
    closedir(dir);
    return fd;
}

/* F1 pads are numbered row by row from the top left; the MPC's pad 1 (A01) is bottom left. */
static int f1_pad_to_mpc(int pad) { return (3 - pad / 4) * 4 + pad % 4; }

static void draw_digit(uint8_t *leds, int offset, char c) {
    /* segments a b c d e f g as bits 6..0; byte order on the F1: dot g c b a f e d */
    static const uint8_t digits[10] = { 0x7e, 0x30, 0x6d, 0x79, 0x33, 0x5b, 0x5f, 0x70, 0x7f, 0x7b };
    static const uint8_t bit[8] = { 0, 0x01, 0x10, 0x20, 0x40, 0x02, 0x04, 0x08 };
    uint8_t g = c >= '0' && c <= '9' ? digits[c - '0'] : c == 'F' ? 0x47 : c == '-' ? 0x01 : 0;
    for (int i = 1; i < 8; i++) leds[offset + i] = (g & bit[i]) ? 127 : 0;
}

/* In the Step Sequencer the F1 shows steps 1-16 row by row from the top left (the MPC starts bottom left). */
static int f1_pad_to_step(int pad) { return pad; }

static int build_leds(uint8_t *leds, int shift) {
    memset(leds, 0, LED_COUNT);
    int track = atomic_load(&selected_track);
    int picking = atomic_load(&browse_held);
    int steps = atomic_load(&step_mode) && !picking;
    const uint8_t *colour = track < 4 ? kTrackColour[track] : kOtherTrackColour;
    int shown = track + 1;
    draw_digit(leds, LED_DIGIT_L, shown >= 10 ? (char)('0' + shown / 10 % 10) : ' ');
    draw_digit(leds, LED_DIGIT_R, (char)('0' + shown % 10));
    leds[LED_DIGIT_R] = shift ? 127 : 0;   /* the dot shows Shift (second CC layer) */
    uint8_t dim[3];
    for (int i = 0; i < 3; i++) dim[i] = colour[i] / 4;
    for (int pad = 0; pad < 16; pad++) {
        const uint8_t *rgb;
        if (picking) {
            rgb = dim;   /* Browse held: pick a pad */
        } else if (steps) {
            /* The app draws a step with a note yellow (7f 7f 00) and the playhead green (00 7f 00). */
            const uint8_t *app = mpc_pads[f1_pad_to_step(pad)];
            static const uint8_t off[3];
            if (!app[0] && !app[1] && !app[2]) rgb = off;
            else if (app[1] && !app[0]) rgb = kPlayhead;
            else rgb = colour;
        } else {
            rgb = mpc_pads[f1_pad_to_mpc(pad)];
        }
        uint8_t *p = leds + LED_PADS + 3 * pad;
        p[0] = rgb[2];
        p[1] = rgb[0];
        p[2] = rgb[1];
    }
    leds[LED_BROWSE] = picking ? 127 : 0;
    /* STOP 1-4 lit = track muted (as the MPC's MUTE button); with Shift held: soloed */
    for (int t = 0; t < 4; t++) {
        uint8_t *stop = leds + LED_STOPS + 2 * (3 - t);   /* stop 4 first */
        int on = shift ? atomic_load(&track_soloed[t]) : atomic_load(&track_muted[t]);
        stop[0] = stop[1] = on ? 127 : 0;
    }
    return LED_COUNT;
}

static int write_leds(int fd, const uint8_t *leds) {
    uint8_t report[1 + LED_COUNT];
    report[0] = 0x80;
    memcpy(report + 1, leds, LED_COUNT);
    return REAL(real_write, "write")(fd, report, sizeof report) == (ssize_t)sizeof report;
}

struct input {
    int valid;
    uint32_t buttons;
    uint8_t encoder;
    int analog[8];
};

static void inject_button(int code) {
    unsigned char msg[6] = { 0x90, (unsigned char)code, 0x7f, 0x90, (unsigned char)code, 0x00 };
    inject(msg, 6);
}

static void redraw(void) { atomic_fetch_add(&mpc_pads_version, 1); }

/* Absolute track selection with the relative Change Track mapping: all the way down (-64), then up to the track.
 * Sent in full on every change, so a track changed on the MPC itself is corrected by the next encoder step. */
static void send_track(int track) {
    send_cc(CC_TRACK_REL, 0x40);
    if (track > 0) send_cc(CC_TRACK_REL, track);
    atomic_store(&app_track, track);
}

static void select_track(int track) {
    if (track < 0) track = 0;
    if (track > MAX_TRACK) track = MAX_TRACK;
    send_track(track);
    atomic_store(&selected_track, track);
    redraw();
    logmsg("track %d", track + 1);
}

/* STOP n: MUTE (or SOLO) on track n. The MPC's buttons act on the current track, so: switch to track n, press the
 * button, switch back. Track changes come over the seq port and buttons over the surface, two different paths in
 * the app, hence the pauses. */
static void toggle_on_track(int track, int code) {
    int back = atomic_load(&selected_track);
    if (back != track) {
        send_track(track);
        usleep(150000);
    }
    inject_button(code);
    usleep(150000);
    if (back != track) send_track(back);
    logmsg("%s track %d", code == CODE_MUTE ? "mute" : "solo", track + 1);
}

/* Browse + pad: pick that pad for the Step Sequencer. In the Step Sequencer Shift + pad selects the pad but also
 * toggles the step at that pad's position (seen 2026-09-30), so it's sent twice: selected, step unchanged.
 * Step Seq first if the app isn't in it yet. */
static void pick_pad(int mpc_pad) {
    if (!atomic_load(&step_mode)) {
        inject_button(CODE_STEP_SEQ);
        usleep(120000);
    }
    unsigned char note = kPadNotes[mpc_pad];
    unsigned char seq[12] = { 0x90, CODE_SHIFT, 0x7f, 0x99, note, 0x7f, 0x89, note, 0x00, 0x90, CODE_SHIFT, 0x00 };
    inject(seq, 12);
    inject(seq, 12);
    logmsg("pad %d picked (shift + pad, twice)", mpc_pad + 1);
}

static void handle_report(const uint8_t *r, struct input *prev, int *shift) {
    uint32_t buttons = (uint32_t)r[1] << 24 | r[2] << 16 | r[3] << 8 | r[4];
    uint32_t changed = buttons ^ prev->buttons;
    for (int b = 0; b < 32; b++) {
        uint32_t mask = 0x80000000u >> b;
        if (!(changed & mask)) continue;
        int down = (buttons & mask) != 0;
        if (b < 16) {
            static int sounding[16] = { -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1 };
            if (!down) {   /* release what was pressed, whatever the mode is now */
                if (sounding[b] >= 0) inject((const unsigned char[]){ 0x89, kPadNotes[sounding[b]], 0 }, 3);
                sounding[b] = -1;
            } else if (atomic_load(&browse_held)) {
                pick_pad(f1_pad_to_mpc(b));
            } else {
                int mpc_pad = atomic_load(&step_mode) ? f1_pad_to_step(b) : f1_pad_to_mpc(b);
                inject((const unsigned char[]){ 0x99, kPadNotes[mpc_pad], (unsigned char)pad_velocity }, 3);
                sounding[b] = mpc_pad;
            }
        } else if (b >= BTN_STOP1 && b < BTN_STOP1 + 4) {
            if (down) toggle_on_track(b - BTN_STOP1, *shift ? CODE_SOLO : CODE_MUTE);
        } else if (b == BTN_BROWSE) {
            atomic_store(&browse_held, down);
            redraw();
        } else if (b == BTN_SHIFT) {
            *shift = down;
        } else if (down) {
            logmsg("button %d (not mapped yet)", b);
        }
    }
    if (prev->valid) {
        int8_t delta = (int8_t)(r[5] - prev->encoder);
        if (delta && !*shift) {
            select_track(atomic_load(&selected_track) + delta);
        } else {   /* Shift + encoder: the MPC's data wheel */
            for (; delta > 0; delta--) inject((const unsigned char[]){ 0xb0, 0x64, 0x01 }, 3);
            for (; delta < 0; delta++) inject((const unsigned char[]){ 0xb0, 0x64, 0x7f }, 3);
        }
    }
    for (int i = 0; i < 8; i++) {
        int v = r[6 + 2 * i] | (r[7 + 2 * i] & 0x0f) << 8;
        int v7 = v * 127 / 4095;
        if (prev->valid && v7 != prev->analog[i]) {
            int base = i < 4 ? (*shift ? shift_knob_cc : knob_cc) : (*shift ? shift_fader_cc : fader_cc);
            send_cc(base + (i & 3), v7);
        }
        prev->analog[i] = v7;
    }
    prev->buttons = buttons;
    prev->encoder = r[5];
    prev->valid = 1;
}

static void *f1_thread(void *arg) {
    (void)arg;
    sleep(5);   /* let the app open its devices and ports first */
    open_port();
    int fd = -1, shift = 0;
    int64_t next_scan = 0, last_led = 0;
    unsigned drawn_version = ~0u;
    int drawn_shift = -1;
    struct input in;
    memset(&in, 0, sizeof in);
    for (;;) {
        if (fd < 0) {
            if (now_ms() >= next_scan) {
                fd = open_f1();
                next_scan = now_ms() + 1000;
                memset(&in, 0, sizeof in);
                drawn_version = ~0u;
            }
            if (fd < 0) {
                usleep(100000);
                continue;
            }
        }
        struct pollfd p = { fd, POLLIN, 0 };
        int r = REAL(real_poll, "poll")(&p, 1, 10);
        if (r > 0 && (p.revents & (POLLERR | POLLHUP | POLLNVAL))) r = -1;
        if (r > 0) {
            uint8_t report[64];
            ssize_t n;
            while ((n = read(fd, report, sizeof report)) > 0) {
                if (report[0] == 0x01 && n >= 22) handle_report(report, &in, &shift);
            }
            if (n < 0 && errno != EAGAIN) r = -1;
        }
        unsigned version = atomic_load(&mpc_pads_version);
        if (r >= 0 && (version != drawn_version || shift != drawn_shift) && now_ms() - last_led >= 10) {
            uint8_t leds[LED_COUNT];
            build_leds(leds, shift);
            if (!write_leds(fd, leds)) r = -1;
            drawn_version = version;
            drawn_shift = shift;
            last_led = now_ms();
        }
        if (r < 0) {
            logmsg("Kontrol F1 gone");
            close(fd);
            fd = -1;
        }
    }
    return NULL;
}

__attribute__((constructor)) static void init(void) {
    /* Loaded via /etc/ld.so.preload into every process: only the MPC app does anything. */
    char comm[32] = { 0 };
    int f = open("/proc/self/comm", O_RDONLY);
    if (f >= 0) {
        if (read(f, comm, sizeof comm - 1) > 0) comm[strcspn(comm, "\n")] = 0;
        close(f);
    }
    if (strcmp(comm, "MPC")) return;
    logf = fopen("/data/hacks/f1bridge.log", "a");
    wake_fd = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
    active = 1;
    logmsg("=== f1bridge pid %d ===", getpid());
    pthread_t t;
    if (pthread_create(&t, NULL, f1_thread, NULL) == 0) pthread_detach(t);
}
