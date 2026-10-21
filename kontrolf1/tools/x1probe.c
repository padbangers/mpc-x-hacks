/* x1probe: talk to a Traktor Kontrol X1 Mk1 (USB 17cc:2305) over usbfs on the MPC X (the kernel has no caiaq driver).
   The protocol is NI's "caiaq" (see linux sound/usb/caiaq): commands as bulk on EP 1 (first byte the command),
   replies on EP 0x81, controller data on EP 0x84.

   EP 0x84 is a byte stream of 24-byte reports (flags; 7 io bytes: 5 button bytes, 2 bytes of four 4-bit encoder
   counters; 8 pots u16 big-endian, 0..4095), but the transfers split them anywhere: the reader resyncs on the report
   shape. The buttons only come after an LED write (x1-mk1-usb2midi writes the LEDs and reads EP 0x81 after every
   report); here a thread keeps EP 0x81 drained and the LEDs are written every 100 ms.

   The map, found on the device (2026-10-21). Button n = report byte 1 + n / 8, bit n % 8; 34 buttons (= the device
   info's digital inputs), bits 4-7, 10, 11 unused. LEDs: the linux caiaq control.c table (23 = Deck A PLAY checked).
                         Deck A           Deck B
     FX buttons 1-4      28 29 30 31      32 33 34 35
     FX 1 / FX 2         9 8              38 37        (Deck B: FX 3 / FX 4)
     IN OUT              20 3             12 19
     BEAT< BEAT>         2 21             18 13
     CUE CUP             1 22             17 14
     PLAY SYNC           0 23             16 15
     data encoder 1      enc 0, push 24   enc 1, push 25
     data encoder 2      enc 2, push 26   enc 3, push 27
     FX knobs 1-4        pot 4 6 7 5      pot 2 1 0 3
     SHIFT 36, HOTCUE 39. Encoders: 4-bit counters (enc 0 = byte 6 low nibble, 1 = byte 6 high, 2/3 = byte 7),
     +-1 per detent.

     x1probe info      the device info reply to GET_DEVICE_INFO
     x1probe in        decoded: buttons, encoders, pots (only real changes), and resyncs
     x1probe raw       every EP 0x84 transfer that changes (hex)
     x1probe led N V   set LED byte N (0..62) to V (0..127), rest off
     x1probe walk      light each LED byte in turn, printing its index (1 s per step)
     x1probe off       all LEDs off
   Build (static, arm32): docker run --rm --platform linux/arm/v7 -v "$PWD":/w -w /w arm32v7/gcc:12 \
                            gcc -O2 -static -pthread -o x1probe x1probe.c */
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/usbdevice_fs.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

enum { CMD_GET_DEVICE_INFO = 0x01, CMD_DIMM_LEDS = 0x0c };
enum { kReport = 24 };

static int g_fd;
static unsigned char led_state[63];
static pthread_mutex_t g_ep1 = PTHREAD_MUTEX_INITIALIZER;

static int read_sys(const char* dir, const char* name, char* out, int len) {
  char path[300];
  snprintf(path, sizeof path, "%s/%s", dir, name);
  FILE* f = fopen(path, "r");
  if (!f) return -1;
  int ok = fgets(out, len, f) != NULL;
  fclose(f);
  if (!ok) return -1;
  out[strcspn(out, "\n")] = 0;
  return 0;
}

// The X1's /dev/bus/usb node, found through sysfs.
static int find_node(char* node, int len) {
  DIR* d = opendir("/sys/bus/usb/devices");
  if (!d) return -1;
  struct dirent* e;
  int found = -1;
  while ((e = readdir(d)) && found < 0) {
    char dir[300], vid[16], pid[16], bus[16], dev[16];
    snprintf(dir, sizeof dir, "/sys/bus/usb/devices/%s", e->d_name);
    if (read_sys(dir, "idVendor", vid, sizeof vid) || read_sys(dir, "idProduct", pid, sizeof pid)) continue;
    if (strcmp(vid, "17cc") || strcmp(pid, "2305")) continue;
    if (read_sys(dir, "busnum", bus, sizeof bus) || read_sys(dir, "devnum", dev, sizeof dev)) continue;
    snprintf(node, len, "/dev/bus/usb/%03d/%03d", atoi(bus), atoi(dev));
    found = 0;
  }
  closedir(d);
  return found;
}

static int bulk(unsigned ep, void* data, unsigned len, unsigned timeout_ms) {
  struct usbdevfs_bulktransfer b = { ep, len, timeout_ms, data };
  return ioctl(g_fd, USBDEVFS_BULK, &b);
}

static int command(int cmd, const unsigned char* payload, int len) {
  unsigned char out[64] = { (unsigned char)cmd };
  if (len > 63) len = 63;
  if (len > 0) memcpy(out + 1, payload, len);
  pthread_mutex_lock(&g_ep1);
  int r = bulk(0x01, out, 1 + len, 200);
  pthread_mutex_unlock(&g_ep1);
  return r;
}

static int leds(void) { return command(CMD_DIMM_LEDS, led_state, sizeof led_state); }

static void hex(const unsigned char* p, int n) {
  for (int i = 0; i < n; ++i) printf("%02x%s", p[i], i % 8 == 7 ? "  " : " ");
  printf("\n");
}

static void* drain_ep81(void* arg) {   // the replies to our commands
  (void)arg;
  unsigned char buf[64];
  for (;;) if (bulk(0x81, buf, sizeof buf, 1000) < 0 && errno != ETIMEDOUT) usleep(10000);
  return NULL;
}

static void* led_loop(void* arg) {
  (void)arg;
  for (;;) { leds(); usleep(100000); }
  return NULL;
}

// A report starts at p: a flag byte 1..7, the pots' high bytes <= 0x0f.
static int report_shape(const unsigned char* p) {
  if (p[0] == 0 || p[0] > 7) return 0;
  for (int i = 0; i < 8; ++i) if (p[8 + 2 * i] > 0x0f) return 0;
  return 1;
}

static void decode_loop(void) {
  unsigned char stream[4096], io[7] = {0};
  int len = 0, have_io = 0, pot[8] = {0}, have_pot = 0, synced = 0;
  long reports = 0, resyncs = 0;
  for (;;) {
    int n = bulk(0x84, stream + len, sizeof stream - len, 1000);
    if (n < 0) {
      if (errno == ETIMEDOUT) continue;
      perror("EP 0x84");
      return;
    }
    len += n;
    int at = 0;
    while (len - at >= 2 * kReport || (synced && len - at >= kReport)) {
      if (!synced) {   // two reports in a row with the right shape
        if (report_shape(stream + at) && report_shape(stream + at + kReport)) {
          synced = 1;
          if (reports) printf("(resync %ld after %ld reports)\n", ++resyncs, reports);
        } else {
          ++at;
          continue;
        }
      }
      const unsigned char* p = stream + at;
      if (!report_shape(p)) { synced = 0; continue; }
      at += kReport;
      ++reports;
      if (p[0] & 0x03) {
        if (have_io) {
          for (int b = 0; b < 5 * 8; ++b) {
            int was = io[b / 8] >> (b % 8) & 1, now = p[1 + b / 8] >> (b % 8) & 1;
            if (was != now) printf("button %2d %s   (byte %d bit %d)\n", b, now ? "down" : "up", 1 + b / 8, b % 8);
          }
          for (int e = 0; e < 4; ++e) {
            int was = io[5 + e / 2] >> (e % 2 * 4) & 0xf, now = p[6 + e / 2] >> (e % 2 * 4) & 0xf;
            int d = (now - was + 24) % 16 - 8;
            if (d) printf("encoder %d %+d (%d)\n", e, d, now);
          }
        }
        memcpy(io, p + 1, 7);
        have_io = 1;
      }
      if (p[0] & 0x04) {
        for (int i = 0; i < 8; ++i) {
          int v = p[8 + 2 * i] << 8 | p[9 + 2 * i];
          if (!have_pot || abs(v - pot[i]) > 12) {
            if (have_pot) printf("pot %d %4d\n", i, v);
            pot[i] = v;
          }
        }
        have_pot = 1;
      }
      fflush(stdout);
    }
    memmove(stream, stream + at, len - at);
    len -= at;
  }
}

int main(int argc, char** argv) {
  const char* cmd = argc > 1 ? argv[1] : "in";
  char node[64];
  if (find_node(node, sizeof node)) { fprintf(stderr, "no X1 (17cc:2305) found\n"); return 1; }
  g_fd = open(node, O_RDWR);
  if (g_fd < 0) { perror(node); return 1; }
  unsigned iface = 0;
  if (ioctl(g_fd, USBDEVFS_CLAIMINTERFACE, &iface) < 0) { perror("claim interface 0"); return 1; }

  unsigned char buf[512];
  int r = command(CMD_GET_DEVICE_INFO, NULL, 0);
  if (r < 0) fprintf(stderr, "GET_DEVICE_INFO: %s\n", strerror(errno));
  r = bulk(0x81, buf, 64, 500);
  if (r < 0) fprintf(stderr, "EP 0x81 reply: %s\n", strerror(errno));
  if (!strcmp(cmd, "info")) {
    if (r >= 0) { printf("EP 0x81 (%d): ", r); hex(buf, r); }
    return 0;
  }

  if (!strcmp(cmd, "led") && argc > 3) {
    int i = atoi(argv[2]);
    if (i >= 0 && i < (int)sizeof led_state) led_state[i] = (unsigned char)atoi(argv[3]);
    printf("send %d\n", leds());
  } else if (!strcmp(cmd, "off")) {
    printf("send %d\n", leds());
  } else if (!strcmp(cmd, "walk")) {
    for (int i = 0; i < 32; ++i) {
      memset(led_state, 0, sizeof led_state);
      led_state[i] = 127;
      printf("led %d: send %d\n", i, leds());
      fflush(stdout);
      sleep(1);
    }
    memset(led_state, 0, sizeof led_state);
    leds();
  } else if (!strcmp(cmd, "raw")) {
    unsigned char last[512];
    int last_n = -1;
    for (;;) {
      int n = bulk(0x84, buf, sizeof buf, 1000);
      if (n < 0) {
        if (errno == ETIMEDOUT) continue;
        perror("EP 0x84");
        return 1;
      }
      if (n == last_n && !memcmp(buf, last, n)) continue;
      printf("%3d: ", n);
      hex(buf, n);
      fflush(stdout);
      memcpy(last, buf, n);
      last_n = n;
    }
  } else {   // in
    pthread_t t1, t2;
    pthread_create(&t1, NULL, drain_ep81, NULL);
    if (!getenv("X1_NO_LEDS")) pthread_create(&t2, NULL, led_loop, NULL);
    decode_loop();
  }
  return 0;
}
