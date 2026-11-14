/* mcubridge.so - a Mackie Control (MCU Pro) on the MPC X, inside the MPC app.
 *
 * Preloaded into the MPC process (like f1bridge.so) and inert in every other process.
 * Stage 0 (this file): take the MCU away from the app, log what it sends, say hello ("MPC X" on the LCD, fader 1
 * up and down).
 *
 * The app reaches every MIDI device but its own surface through the ALSA sequencer: one input and one output port
 * per device on its client ("MPC 3"), connected to the device's port (seen 2026-11-03: MCU Pro = client 24 port 0,
 * 24:0 -> 130:2 and 130:12 -> 24:0). So the MCU is taken over by unsubscribing those two connections (checked every
 * second: the app reconnects after a hotplug) and connecting a seq client of our own instead.
 *
 * Config /data/hacks/mackie.conf (optional, one line):
 *   usb                  MCU Pro on USB (default): seq client "MCU Pro...", port 0
 *   din A|B              MCU on the MPC X's DIN MIDI In/Out A or B ("MPC X Controller", port 2/3)
 *
 * Log: /data/hacks/mcubridge.log
 * Build: zig cc -target arm-linux-gnueabihf.2.31 -shared -fPIC -O2 -o mcubridge.so mcubridge.c -lm
 */
#define _GNU_SOURCE
#include <dlfcn.h>
#include <fcntl.h>
#include <math.h>
#include <poll.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

/* ---- ALSA seq ABI subset (alsa/seq.h, alsa/seq_midi_event.h), resolved from the app's libasound ---- */
typedef struct _snd_seq snd_seq_t;
typedef struct _snd_seq_client_info snd_seq_client_info_t;
typedef struct _snd_seq_port_info snd_seq_port_info_t;
typedef struct _snd_seq_port_subscribe snd_seq_port_subscribe_t;
typedef struct _snd_midi_event snd_midi_event_t;
typedef struct { unsigned char client, port; } seq_addr_t;
typedef struct {
    unsigned char type, flags, tag, queue;
    unsigned int time[2];
    seq_addr_t source, dest;
    unsigned char data[12];
} seq_event_t;
enum { SEQ_OPEN_DUPLEX = 3, SEQ_NONBLOCK = 1 };
enum { CAP_READ = 1 << 0, CAP_WRITE = 1 << 1, CAP_SUBS_READ = 1 << 5, CAP_SUBS_WRITE = 1 << 6 };
enum { TYPE_MIDI_GENERIC = 1 << 1, TYPE_APPLICATION = 1 << 20 };
enum { QUEUE_DIRECT = 253, ADDRESS_SUBSCRIBERS = 254, ADDRESS_UNKNOWN = 253 };

static struct {
    int (*open)(snd_seq_t **, const char *, int, int);
    int (*set_client_name)(snd_seq_t *, const char *);
    int (*client_id)(snd_seq_t *);
    int (*create_simple_port)(snd_seq_t *, const char *, unsigned, unsigned);
    int (*connect_from)(snd_seq_t *, int, int, int);
    int (*connect_to)(snd_seq_t *, int, int, int);
    int (*event_input)(snd_seq_t *, seq_event_t **);
    int (*event_output_direct)(snd_seq_t *, seq_event_t *);
    int (*poll_descriptors)(snd_seq_t *, struct pollfd *, unsigned, short);
    int (*client_info_malloc)(snd_seq_client_info_t **);
    void (*client_info_set_client)(snd_seq_client_info_t *, int);
    int (*client_info_get_client)(const snd_seq_client_info_t *);
    const char *(*client_info_get_name)(snd_seq_client_info_t *);
    int (*client_info_get_pid)(const snd_seq_client_info_t *);
    int (*query_next_client)(snd_seq_t *, snd_seq_client_info_t *);
    int (*port_info_malloc)(snd_seq_port_info_t **);
    void (*port_info_set_client)(snd_seq_port_info_t *, int);
    void (*port_info_set_port)(snd_seq_port_info_t *, int);
    int (*port_info_get_port)(const snd_seq_port_info_t *);
    int (*query_next_port)(snd_seq_t *, snd_seq_port_info_t *);
    int (*port_subscribe_malloc)(snd_seq_port_subscribe_t **);
    void (*port_subscribe_set_sender)(snd_seq_port_subscribe_t *, const seq_addr_t *);
    void (*port_subscribe_set_dest)(snd_seq_port_subscribe_t *, const seq_addr_t *);
    int (*unsubscribe_port)(snd_seq_t *, snd_seq_port_subscribe_t *);
    int (*midi_event_new)(size_t, snd_midi_event_t **);
    long (*midi_event_encode)(snd_midi_event_t *, const unsigned char *, long, seq_event_t *);
    long (*midi_event_decode)(snd_midi_event_t *, unsigned char *, long, const seq_event_t *);
    void (*midi_event_reset_encode)(snd_midi_event_t *);
    void (*midi_event_no_status)(snd_midi_event_t *, int);
} A;

#define SYM(field, name) if (!(*(void **)&A.field = dlsym(RTLD_DEFAULT, name))) { logmsg("missing %s", name); return 0; }

/* ---- settings ---- */
static char mcu_client_name[64] = "MCU Pro";   /* prefix of the MCU's seq client name */
static int mcu_port_no = 0;

/* ---- state ---- */
static FILE *logfile;
static snd_seq_t *seq;
static int my_client = -1, my_port = -1;
static int mcu_client = -1;                    /* -1 = not found (yet) */
static snd_midi_event_t *enc, *dec;
static pthread_mutex_t out_lock = PTHREAD_MUTEX_INITIALIZER;
/* The app's own seq handles: only a connection's own end may cut it (the app's ports don't allow third parties). */
static snd_seq_t *app_seqs[8];
static int app_seq_count;
static pthread_mutex_t app_seq_lock = PTHREAD_MUTEX_INITIALIZER;
static int (*real_seq_open)(snd_seq_t **, const char *, int, int);
static int (*real_seq_close)(snd_seq_t *);

static void logmsg(const char *fmt, ...) {
    if (!logfile) return;
    struct timespec t;
    clock_gettime(CLOCK_REALTIME, &t);
    struct tm tm;
    localtime_r(&t.tv_sec, &tm);
    fprintf(logfile, "%02d:%02d:%02d.%03ld ", tm.tm_hour, tm.tm_min, tm.tm_sec, t.tv_nsec / 1000000);
    va_list ap;
    va_start(ap, fmt);
    vfprintf(logfile, fmt, ap);
    va_end(ap);
    fputc('\n', logfile);
    fflush(logfile);
}

static void loghex(const char *what, const unsigned char *b, size_t n) {
    char line[3 * 256 + 4];
    int o = 0;
    for (size_t i = 0; i < n && o + 4 < (int)sizeof line; i++) o += snprintf(line + o, sizeof line - o, "%02x ", b[i]);
    if (o + 4 >= (int)sizeof line) snprintf(line + o - 1, sizeof line - o + 1, "..");
    logmsg("%s %s", what, line);
}

static int64_t now_ms(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (int64_t)t.tv_sec * 1000 + t.tv_nsec / 1000000;
}

static void read_config(void) {
    FILE *f = fopen("/data/hacks/mackie.conf", "r");
    if (!f) return;
    char line[128], ab;
    while (fgets(line, sizeof line, f)) {
        if (sscanf(line, "din %c", &ab) == 1 && (ab == 'A' || ab == 'B' || ab == 'a' || ab == 'b')) {
            snprintf(mcu_client_name, sizeof mcu_client_name, "MPC X Controller");
            mcu_port_no = 2 + (ab == 'B' || ab == 'b');
        } else if (!strncmp(line, "usb", 3)) {
            snprintf(mcu_client_name, sizeof mcu_client_name, "MCU Pro");
            mcu_port_no = 0;
        }
    }
    fclose(f);
}

static int load_alsa(void) {
    SYM(open, "snd_seq_open") SYM(set_client_name, "snd_seq_set_client_name") SYM(client_id, "snd_seq_client_id")
    SYM(create_simple_port, "snd_seq_create_simple_port")
    SYM(connect_from, "snd_seq_connect_from") SYM(connect_to, "snd_seq_connect_to")
    SYM(event_input, "snd_seq_event_input") SYM(event_output_direct, "snd_seq_event_output_direct")
    SYM(poll_descriptors, "snd_seq_poll_descriptors")
    SYM(client_info_malloc, "snd_seq_client_info_malloc") SYM(client_info_set_client, "snd_seq_client_info_set_client")
    SYM(client_info_get_client, "snd_seq_client_info_get_client") SYM(client_info_get_name, "snd_seq_client_info_get_name")
    SYM(client_info_get_pid, "snd_seq_client_info_get_pid") SYM(query_next_client, "snd_seq_query_next_client")
    SYM(port_info_malloc, "snd_seq_port_info_malloc") SYM(port_info_set_client, "snd_seq_port_info_set_client")
    SYM(port_info_set_port, "snd_seq_port_info_set_port") SYM(port_info_get_port, "snd_seq_port_info_get_port")
    SYM(query_next_port, "snd_seq_query_next_port")
    SYM(port_subscribe_malloc, "snd_seq_port_subscribe_malloc")
    SYM(port_subscribe_set_sender, "snd_seq_port_subscribe_set_sender")
    SYM(port_subscribe_set_dest, "snd_seq_port_subscribe_set_dest") SYM(unsubscribe_port, "snd_seq_unsubscribe_port")
    SYM(midi_event_new, "snd_midi_event_new") SYM(midi_event_encode, "snd_midi_event_encode")
    SYM(midi_event_decode, "snd_midi_event_decode") SYM(midi_event_reset_encode, "snd_midi_event_reset_encode")
    SYM(midi_event_no_status, "snd_midi_event_no_status")
    return 1;
}

static int open_seq(void) {
    if (A.open(&seq, "default", SEQ_OPEN_DUPLEX, SEQ_NONBLOCK) < 0) { seq = NULL; logmsg("snd_seq_open failed"); return 0; }
    A.set_client_name(seq, "Mackie Bridge");
    my_client = A.client_id(seq);
    /* not a hardware port: the app must not pick it up as a MIDI device */
    my_port = A.create_simple_port(seq, "Mackie Bridge", CAP_READ | CAP_WRITE, TYPE_MIDI_GENERIC | TYPE_APPLICATION);
    A.midi_event_new(1024, &enc);
    A.midi_event_new(1024, &dec);
    A.midi_event_no_status(dec, 1);
    logmsg("seq client %d port %d", my_client, my_port);
    return my_port >= 0;
}

/* ---- the MPK mini IV we pretend to be ----
 * MPC 3.10 drives an MPK mini IV as a control surface (knobs = track volume 1-8 / 9-16). The app knows its ports by
 * name (seen in the app's memory): MIDI Port, DAW Port, Software Control Port, Plugin Port. Stage 1a: a seq client
 * with those ports, hardware-type like a USB device, and a log of whatever the app sends to it. */
static snd_seq_t *mpk;
static int mpk_client = -1;
static void mcu_send(const unsigned char *b, long n);
static void on_app_message(const unsigned char *b, long n);
static void on_app_connected(void);
static void refresh_all(void);
static const char *const kMpkPorts[4] = { "MPK mini IV MIDI Port", "MPK mini IV DAW Port",
                                          "MPK mini IV Software Control Port", "MPK mini IV Plugin Port" };
enum { TYPE_HARDWARE = 1 << 16, TYPE_PORT = 1 << 19, CAP_DUPLEX = 1 << 4 };

static int open_mpk(void) {
    if (A.open(&mpk, "default", SEQ_OPEN_DUPLEX, SEQ_NONBLOCK) < 0) { mpk = NULL; logmsg("MPK: snd_seq_open failed"); return 0; }
    A.set_client_name(mpk, "MPK mini IV");
    mpk_client = A.client_id(mpk);
    for (int i = 0; i < 4; i++) {
        int p = A.create_simple_port(mpk, kMpkPorts[i], CAP_READ | CAP_WRITE | CAP_SUBS_READ | CAP_SUBS_WRITE | CAP_DUPLEX,
                                     TYPE_MIDI_GENERIC | TYPE_HARDWARE | TYPE_PORT);
        logmsg("MPK: port %d:%d '%s'", mpk_client, p, kMpkPorts[i]);
    }
    return 1;
}

/* Probe: "echo '1 b0 46 40' > /tmp/mpkinject" sends those bytes out of MPK port 1 (to the app). */
static void mpk_send(int port, const unsigned char *b, long n) {
    pthread_mutex_lock(&out_lock);
    while (n > 0) {
        seq_event_t ev;
        memset(&ev, 0, sizeof ev);
        long used = A.midi_event_encode(enc, b, n, &ev);
        if (used <= 0) break;
        b += used;
        n -= used;
        if (!ev.type) continue;
        ev.queue = QUEUE_DIRECT;
        ev.source.port = (unsigned char)port;
        ev.dest.client = ADDRESS_SUBSCRIBERS;
        ev.dest.port = ADDRESS_UNKNOWN;
        A.event_output_direct(mpk, &ev);
    }
    A.midi_event_reset_encode(enc);
    pthread_mutex_unlock(&out_lock);
}

static void poll_inject(void) {
    static int fd = -1;
    if (fd < 0) {
        mkfifo("/tmp/mpkinject", 0666);
        fd = open("/tmp/mpkinject", O_RDONLY | O_NONBLOCK);
        if (fd < 0) return;
    }
    static char line[1024];
    static int len;
    int r = (int)read(fd, line + len, sizeof line - 1 - len);
    if (r <= 0) return;
    len += r;
    line[len] = 0;
    char *nl;
    while ((nl = strchr(line, '\n'))) {
        *nl = 0;
        unsigned char b[300];
        int n = 0, port = 1, off = 0, v;
        char *s = line;
        if (!strcmp(s, "refresh")) refresh_all();
        if (sscanf(s, "%d%n", &port, &off) == 1) s += off;
        while (n < (int)sizeof b && sscanf(s, " %x%n", &v, &off) == 1) { b[n++] = (unsigned char)v; s += off; }
        if (n) { loghex(port == 9 ? "inject MCU" : "inject MPK", b, (size_t)n); if (port == 9) mcu_send(b, n); else mpk_send(port, b, n); }
        int rest = len - (int)(nl + 1 - line);
        memmove(line, nl + 1, (size_t)rest + 1);
        len = rest;
    }
}

enum { EV_PORT_SUBSCRIBED = 66, EV_PORT_UNSUBSCRIBED = 67 };

static void handle_mpk_input(void) {
    seq_event_t *ev;
    while (A.event_input(mpk, &ev) >= 0 && ev) {
        if (ev->type == EV_PORT_SUBSCRIBED || ev->type == EV_PORT_UNSUBSCRIBED) {
            logmsg("MPK: %s", ev->type == EV_PORT_SUBSCRIBED ? "subscribed" : "unsubscribed");
            if (ev->type == EV_PORT_SUBSCRIBED) on_app_connected();
            continue;
        }
        unsigned char buf[512];
        long n = A.midi_event_decode(dec, buf, sizeof buf, ev);
        char what[48];
        snprintf(what, sizeof what, "MPK port %d <- %d:%d", ev->dest.port, ev->source.client, ev->source.port);
        if (n > 0 && ev->dest.port == 1) on_app_message(buf, n);
        if (n > 0 && access("/tmp/mcutrace", F_OK) == 0) loghex(what, buf, (size_t)n);
    }
}

/* ---- finding the MCU and keeping the app off it ---- */

static int find_mcu(void) {
    snd_seq_client_info_t *ci;
    A.client_info_malloc(&ci);   /* small, kept: called once a second */
    A.client_info_set_client(ci, -1);
    int found = -1;
    while (A.query_next_client(seq, ci) >= 0) {
        const char *name = A.client_info_get_name(ci);
        if (name && !strncmp(name, mcu_client_name, strlen(mcu_client_name))) { found = A.client_info_get_client(ci); break; }
    }
    free(ci);
    return found;
}

/* A project being loaded (the app opens its .xpj): the mixer is read again once the load is through. Set from the
 * app's threads, acted on in ours. */
static _Atomic int64_t project_opened_at;

static void watch_open(const char *path, const char *via) {
    size_t n = path ? strlen(path) : 0;
    if (!logfile || n <= 4 || strcmp(path + n - 4, ".xpj")) return;
    atomic_store(&project_opened_at, now_ms());
    static int logged;
    if (!logged++) logmsg("project file opened via %s: %s", via, path);
}

#define OPEN_HOOK(fn, ...)                                                                                     \
    int fn(__VA_ARGS__, int flags, ...) {                                                                     \
        static int (*real)(__VA_ARGS__, int, ...);                                                            \
        va_list ap;                                                                                           \
        va_start(ap, flags);                                                                                  \
        int mode = (flags & O_CREAT) ? va_arg(ap, int) : 0;                                                   \
        va_end(ap);                                                                                           \
        if (!(flags & (O_WRONLY | O_RDWR))) watch_open(path, #fn);                                            \
        if (!real) real = dlsym(RTLD_NEXT, #fn);                                                              \
        return OPEN_CALL;                                                                                     \
    }
#define OPEN_CALL real(path, flags, mode)
OPEN_HOOK(open, const char *path)
OPEN_HOOK(open64, const char *path)
#undef OPEN_CALL
#define OPEN_CALL real(dirfd, path, flags, mode)
OPEN_HOOK(openat, int dirfd, const char *path)
OPEN_HOOK(openat64, int dirfd, const char *path)
#undef OPEN_CALL

int __open_2(const char *path, int flags) {
    static int (*real)(const char *, int);
    if (!(flags & (O_WRONLY | O_RDWR))) watch_open(path, "__open_2");
    return (real ? real : (real = dlsym(RTLD_NEXT, "__open_2")))(path, flags);
}

int __open64_2(const char *path, int flags) {
    static int (*real)(const char *, int);
    if (!(flags & (O_WRONLY | O_RDWR))) watch_open(path, "__open64_2");
    return (real ? real : (real = dlsym(RTLD_NEXT, "__open64_2")))(path, flags);
}

FILE *fopen(const char *path, const char *mode) {
    static FILE *(*real)(const char *, const char *);
    if (mode && mode[0] == 'r') watch_open(path, "fopen");
    return (real ? real : (real = dlsym(RTLD_NEXT, "fopen")))(path, mode);
}

FILE *fopen64(const char *path, const char *mode) {
    static FILE *(*real)(const char *, const char *);
    if (mode && mode[0] == 'r') watch_open(path, "fopen64");
    return (real ? real : (real = dlsym(RTLD_NEXT, "fopen64")))(path, mode);
}

int snd_seq_open(snd_seq_t **handle, const char *name, int streams, int mode) {
    int r = (real_seq_open ? real_seq_open : (real_seq_open = dlsym(RTLD_NEXT, "snd_seq_open")))(handle, name, streams, mode);
    if (r == 0 && handle && *handle && logfile) {
        pthread_mutex_lock(&app_seq_lock);
        if (app_seq_count < 8) app_seqs[app_seq_count++] = *handle;
        pthread_mutex_unlock(&app_seq_lock);
    }
    return r;
}

int snd_seq_close(snd_seq_t *handle) {
    pthread_mutex_lock(&app_seq_lock);
    for (int i = 0; i < app_seq_count; i++) {
        if (app_seqs[i] == handle) { app_seqs[i] = app_seqs[--app_seq_count]; break; }
    }
    pthread_mutex_unlock(&app_seq_lock);
    return (real_seq_close ? real_seq_close : (real_seq_close = dlsym(RTLD_NEXT, "snd_seq_close")))(handle);
}

/* Cut with the handle of the client that owns one end of the connection. */
static void unsubscribe(int owner, int sc, int sp, int dc, int dp) {
    static snd_seq_port_subscribe_t *sub;
    if (!sub) A.port_subscribe_malloc(&sub);
    seq_addr_t s = { (unsigned char)sc, (unsigned char)sp }, d = { (unsigned char)dc, (unsigned char)dp };
    A.port_subscribe_set_sender(sub, &s);
    A.port_subscribe_set_dest(sub, &d);
    pthread_mutex_lock(&app_seq_lock);
    for (int i = 0; i < app_seq_count; i++) {
        if (app_seqs[i] == seq || A.client_id(app_seqs[i]) != owner) continue;
        if (A.unsubscribe_port(app_seqs[i], sub) == 0) logmsg("cut %d:%d -> %d:%d", sc, sp, dc, dp);
    }
    pthread_mutex_unlock(&app_seq_lock);
}

/* Every port of every other seq client in this process (the app's, the shim's) loses its link to the MCU's port. */
static void keep_app_off(void) {
    snd_seq_client_info_t *ci;
    snd_seq_port_info_t *pi;
    A.client_info_malloc(&ci);
    A.port_info_malloc(&pi);
    A.client_info_set_client(ci, -1);
    while (A.query_next_client(seq, ci) >= 0) {
        int c = A.client_info_get_client(ci);
        if (c == my_client || A.client_info_get_pid(ci) != getpid()) continue;
        A.port_info_set_client(pi, c);
        A.port_info_set_port(pi, -1);
        while (A.query_next_port(seq, pi) >= 0) {
            int p = A.port_info_get_port(pi);
            unsubscribe(c, mcu_client, mcu_port_no, c, p);
            unsubscribe(c, c, p, mcu_client, mcu_port_no);
        }
    }
    free(pi);
    free(ci);
}

/* ---- MCU output ---- */

static void mcu_send(const unsigned char *b, long n) {
    if (mcu_client < 0) return;
    pthread_mutex_lock(&out_lock);
    while (n > 0) {
        seq_event_t ev;
        memset(&ev, 0, sizeof ev);
        long used = A.midi_event_encode(enc, b, n, &ev);
        if (used <= 0) break;
        b += used;
        n -= used;
        if (!ev.type) continue;   /* message not complete yet */
        ev.queue = QUEUE_DIRECT;
        ev.source.port = (unsigned char)my_port;
        ev.dest.client = ADDRESS_SUBSCRIBERS;
        ev.dest.port = ADDRESS_UNKNOWN;
        A.event_output_direct(seq, &ev);
    }
    A.midi_event_reset_encode(enc);
    pthread_mutex_unlock(&out_lock);
}

/* LCD: 2 rows x 56 characters, offset 0..111. Sent for both device ids: 0x14 = Mackie Control, 0x10 = Logic Control. */
static void mcu_lcd(int offset, const char *text) {
    static const unsigned char ids[2] = { 0x14, 0x10 };
    for (int k = 0; k < 2; k++) {
        unsigned char msg[8 + 112];
        int n = 0;
        msg[n++] = 0xf0; msg[n++] = 0x00; msg[n++] = 0x00; msg[n++] = 0x66; msg[n++] = ids[k]; msg[n++] = 0x12;
        msg[n++] = (unsigned char)offset;
        for (const char *c = text; *c && n < (int)sizeof msg - 1; c++) msg[n++] = (unsigned char)(*c & 0x7f);
        msg[n++] = 0xf7;
        mcu_send(msg, n);
    }
}

/* Fader 0..7 (8 = master), position 0..16383 as pitch bend on the fader's channel. */
static void mcu_fader(int fader, int pos) {
    unsigned char msg[3] = { (unsigned char)(0xe0 | fader), (unsigned char)(pos & 0x7f), (unsigned char)(pos >> 7 & 0x7f) };
    mcu_send(msg, 3);
}

/* ---- the mixer: MCU faders <-> the MPK's knobs ----
 * The MPK integration turns knob n (CC 0x18 + n, ch 1, relative two's complement) into the volume of mixer channel
 * n (tracks first, then submixes) and answers on the MPK's display (SysEx 0x10): line 0 "3: Plugin 001", line 2
 * the level ("-6.2dB", "-INFdB"; a MIDI track: "128"), first the old value, then the new one. The level moves on a
 * scale of 288 steps, gain = 2 * v^2 with v = steps / 288 (measured 2026-11-03): an MCU fader maps linearly onto v,
 * which puts 0 dB at ~70 % of the travel, as on a console. */
enum { STRIPS = 8, STEPS = 288, KNOB_CC = 0x18, TOUCH_NOTE = 0x68, NOTE_GLOBAL_VIEW = 0x33 };

struct strip {
    int known;               /* v is from the app */
    double v;                /* 0..1 */
    char name[32], value[16];
    int inf;                 /* the app shows -INF */
    int touched;
    double fader;            /* where the MCU fader is (as far as we know), 0..1 */
    int has_target;
    double target;           /* where the MCU fader wants v */
    int64_t target_at;
    int64_t last_sent;       /* our last knob turn */
    int awaiting;            /* turned, the app hasn't answered yet */
    int has_pending;         /* a value from the app, held back while our turns are under way */
    double pending;
    int pending_inf;
    int64_t snap_at;         /* when to move the motor fader to v */
    int seen;                /* refresh: values seen since the probe */
    char first[16];
};
static struct strip strips[STRIPS];
static int shown = -1;        /* the channel on the MPK's display */
static int refresh_strip = -1, refresh_phase;
static int64_t refresh_at;

static double value_to_v(const char *text, int *ok) {
    *ok = 1;
    if (strstr(text, "INF")) return 0;
    char *end;
    double x = strtod(text, &end);
    if (end == text) { *ok = 0; return 0; }
    if (strstr(end, "dB")) {
        double v = sqrt(pow(10.0, x / 20.0) / 2.0);
        return v > 1 ? 1 : v;
    }
    return x / 128.0;   /* a MIDI track: 0..128 */
}

/* "Plugin 002" -> "Plugi02": 6 characters, digits at the end kept. */
static void short_name(const char *name, char *out) {
    char t[32];
    int n = 0;
    for (const char *c = name; *c && n < 31; c++) if (*c != ' ') t[n++] = *c;
    t[n] = 0;
    if (n <= 6) { strcpy(out, t); return; }
    int digits = 0;
    while (digits < n && t[n - 1 - digits] >= '0' && t[n - 1 - digits] <= '9') digits++;
    if (digits > 3) digits = 3;
    memcpy(out, t, (size_t)(6 - digits));
    memcpy(out + 6 - digits, t + n - digits, (size_t)digits);
    out[6] = 0;
}

static void lcd_strip(int i) {
    char cell[8], sn[8];
    short_name(strips[i].name, sn);
    snprintf(cell, sizeof cell, "%-6.6s ", sn);
    mcu_lcd(7 * i, cell);
    snprintf(cell, sizeof cell, "%-6.6s ", strips[i].value);
    mcu_lcd(56 + 7 * i, cell);
}

static void motor(int i) {
    struct strip *s = &strips[i];
    if (s->touched) return;
    mcu_fader(i, (int)(s->v * 16383 + 0.5));
    s->fader = s->v;
}

static void turn_knob(int i, int steps) {
    unsigned char msg[3] = { 0xb0, (unsigned char)(KNOB_CC + i), (unsigned char)(steps & 0x7f) };
    mpk_send(1, msg, 3);
    strips[i].last_sent = now_ms();
    strips[i].awaiting = 1;
}

static void on_value(int i, const char *text) {
    struct strip *s = &strips[i];
    if (s->seen++ == 0) snprintf(s->first, sizeof s->first, "%s", text);
    int ok;
    double v = value_to_v(text, &ok);
    if (!ok) return;
    snprintf(s->value, sizeof s->value, "%s", text);
    char *db = strstr(s->value, "dB");
    if (db && !strstr(s->value, "INF")) *db = 0;   /* "-6.2dB" -> "-6.2" fits the strip */
    if (strstr(s->value, "INF")) snprintf(s->value, sizeof s->value, "-inf");
    s->pending = v;
    s->pending_inf = strstr(text, "INF") != NULL;
    s->has_pending = 1;
    lcd_strip(i);
}

static void on_app_message(const unsigned char *b, long n) {
    /* f0 47 00 5d 10 <len hi> <len lo> 03 <line> <text> 00 f7 */
    if (n < 11 || b[0] != 0xf0 || b[1] != 0x47 || b[3] != 0x5d || b[4] != 0x10 || b[7] != 0x03) return;
    char text[64];
    int len = 0;
    for (long k = 9; k < n - 1 && b[k] && len < 63; k++) text[len++] = (char)b[k];
    text[len] = 0;
    int line = b[8];
    if (line == 0) {
        int ch = atoi(text);
        const char *colon = strchr(text, ':');
        if (ch >= 1 && colon) {
            shown = (ch - 1) % STRIPS;
            const char *name = colon + 1;
            while (*name == ' ') name++;
            if (strcmp(strips[shown].name, name)) {
                snprintf(strips[shown].name, sizeof strips[shown].name, "%s", name);
                lcd_strip(shown);
            }
        }
    } else if (line == 2 && shown >= 0) {
        on_value(shown, text);
    }
}

/* Read every channel: +1 and back. A value at its end stops there (old == new): no step back then, it would move. */
static void mcu_led(int note, int on) {
    unsigned char msg[3] = { 0x90, (unsigned char)note, (unsigned char)(on ? 0x7f : 0x00) };
    mcu_send(msg, 3);
}

static void refresh_all(void) {
    mcu_led(NOTE_GLOBAL_VIEW, 1);
    refresh_strip = 0;
    refresh_phase = 0;
    refresh_at = now_ms() + 300;
}

static void on_app_connected(void) { refresh_all(); }

static void refresh_tick(void) {
    if (refresh_strip < 0 || now_ms() < refresh_at) return;
    struct strip *s = &strips[refresh_strip];
    if (refresh_phase == 0) {
        s->seen = 0;
        turn_knob(refresh_strip, 1);
        refresh_phase = 1;
        refresh_at = now_ms() + 150;
        return;
    }
    if (refresh_phase == 1) {
        int clamped = s->seen >= 2 && !strcmp(s->first, s->value);
        if (s->seen && !clamped) turn_knob(refresh_strip, -1);
        refresh_phase = 2;
        refresh_at = now_ms() + 150;
        return;
    }
    s->known = s->seen > 0;
    if (!s->known) { s->name[0] = 0; snprintf(s->value, sizeof s->value, " "); s->v = 0; lcd_strip(refresh_strip); }
    s->snap_at = now_ms() + 100;
    logmsg("strip %d: '%s' %s%s", refresh_strip + 1, s->name, s->value, s->known ? "" : " (none)");
    refresh_phase = 0;
    refresh_at = now_ms();
    if (++refresh_strip == STRIPS) {
        refresh_strip = -1;
        mcu_led(NOTE_GLOBAL_VIEW, 0);
    }
}

/* A fader move is a target that stays until the app confirms it: turns get lost when they come fast, so every
 * answer resyncs v and the rest is sent again. The motor moves only for a confirmed value off the fader. */
static void mixer_tick(void) {
    int64_t now = now_ms();
    for (int i = 0; i < STRIPS; i++) {
        struct strip *s = &strips[i];
        if (s->has_pending && now - s->last_sent >= 60) {
            s->v = s->pending;
            s->inf = s->pending_inf;
            s->has_pending = 0;
            s->awaiting = 0;
            s->known = 1;
            if (!s->has_target && fabs(s->v - s->fader) > 0.004) s->snap_at = now;
        }
        if (s->awaiting && now - s->last_sent >= 200) s->awaiting = 0;   /* no answer: at the end of the range */
        if (s->has_target && s->known && refresh_strip < 0 && !s->awaiting) {
            int steps = (int)lround((s->target - s->v) * STEPS);
            if (s->target < 0.5 / STEPS && !s->inf) steps = -63;              /* all the way down: until -INF */
            if (s->target > 1 - 0.5 / STEPS && s->v < 1) steps = steps < 1 ? 1 : steps;
            if (steps == 0 || (s->target < 0.5 / STEPS && s->inf)) {
                if (!s->touched) { s->has_target = 0; s->fader = s->target; }
            } else {
                if (steps > 63) steps = 63;
                if (steps < -63) steps = -63;
                turn_knob(i, steps);
                s->v += (double)steps / STEPS;
            }
        }
        if (s->has_target && !s->touched && now - s->target_at > 3000) s->has_target = 0;   /* give up */
        if (s->snap_at && now >= s->snap_at && !s->touched && !s->has_target) {
            motor(i);
            s->snap_at = 0;
        }
    }
}

static void hello(void) {
    char row[113];
    snprintf(row, sizeof row, "%-112s", "MPC X - reading the mixer ...");
    mcu_lcd(0, row);
    for (int i = 0; i < STRIPS; i++) mcu_fader(i, 0);
    refresh_all();
    logmsg("hello sent");
}

/* ---- MCU input ---- */

static void on_mcu(const unsigned char *b, long n) {
    if (n == 3 && (b[0] & 0xf0) == 0xe0 && (b[0] & 0x0f) < STRIPS) {
        struct strip *s = &strips[b[0] & 0x0f];
        s->target = (b[1] | b[2] << 7) / 16383.0;
        s->fader = s->target;
        s->has_target = 1;
        s->target_at = now_ms();
        /* Echo it, as a DAW does: on release the MCU drives the fader back to the last position the host sent. */
        mcu_send(b, 3);
    } else if (n == 3 && (b[0] & 0xf0) == 0x90 && b[1] >= TOUCH_NOTE && b[1] < TOUCH_NOTE + STRIPS) {
        struct strip *s = &strips[b[1] - TOUCH_NOTE];
        s->touched = b[2] != 0;
        if (!s->touched) s->target_at = now_ms();   /* the target still holds until the app confirms it */
    } else if (n == 3 && b[0] == 0x90 && b[1] == NOTE_GLOBAL_VIEW && b[2]) {
        /* the app tells the MPK nothing about changes made on the MPC itself: read the mixer on request */
        logmsg("GLOBAL VIEW: reading the mixer");
        refresh_all();
    } else if (access("/tmp/mcutrace", F_OK) == 0) {
        loghex("in ", b, (size_t)n);
    }
}

static void handle_input(void) {
    seq_event_t *ev;
    while (A.event_input(seq, &ev) >= 0 && ev) {
        unsigned char buf[512];
        long n = A.midi_event_decode(dec, buf, sizeof buf, ev);
        if (n > 0) on_mcu(buf, n);
    }
}

static void *bridge_thread(void *arg) {
    (void)arg;
    sleep(5);   /* let the app open its devices and ports first */
    if (!load_alsa() || !open_seq()) return NULL;
    open_mpk();
    logmsg("looking for seq client '%s' port %d", mcu_client_name, mcu_port_no);
    int64_t next_check = 0;
    for (;;) {
        if (now_ms() >= next_check) {
            next_check = now_ms() + 1000;
            int c = find_mcu();
            if (c != mcu_client) {
                mcu_client = c;
                if (c >= 0) {
                    logmsg("MCU = client %d port %d", c, mcu_port_no);
                    keep_app_off();
                    A.connect_from(seq, my_port, c, mcu_port_no);
                    A.connect_to(seq, my_port, c, mcu_port_no);
                    hello();
                } else {
                    logmsg("MCU gone");
                }
            } else if (c >= 0) {
                keep_app_off();
            }
        }
        struct pollfd p[8];
        int np = A.poll_descriptors(seq, p, 4, POLLIN);
        int nm = mpk ? A.poll_descriptors(mpk, p + np, 4, POLLIN) : 0;
        if (np + nm > 0 && poll(p, (nfds_t)(np + nm), 10) > 0) {
            handle_input();
            if (mpk) handle_mpk_input();
        } else if (np + nm <= 0) {
            usleep(50000);
        }
        if (mpk) poll_inject();
        /* a project load: read the mixer 3 s after the last .xpj open */
        int64_t opened = atomic_load(&project_opened_at);
        if (opened && now_ms() - opened >= 3000) {
            atomic_store(&project_opened_at, 0);
            logmsg("project loaded, reading the mixer");
            refresh_all();
        }
        refresh_tick();
        mixer_tick();
    }
    return NULL;
}

__attribute__((constructor)) static void init(void) {
    /* Loaded via LD_PRELOAD into the launcher's children too: only the MPC app does anything. */
    char comm[32] = { 0 };
    int f = open("/proc/self/comm", O_RDONLY);
    if (f >= 0) {
        if (read(f, comm, sizeof comm - 1) > 0) comm[strcspn(comm, "\n")] = 0;
        close(f);
    }
    if (strcmp(comm, "MPC")) return;
    logfile = fopen("/data/hacks/mcubridge.log", "a");
    logmsg("=== mcubridge pid %d ===", getpid());
    read_config();
    pthread_t t;
    if (pthread_create(&t, NULL, bridge_thread, NULL) == 0) pthread_detach(t);
}
