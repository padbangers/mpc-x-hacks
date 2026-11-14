/* f1probe: talk to a Traktor Kontrol F1 over /dev/hidraw on the MPC X.
     f1probe in        print every input report that changes (buttons, encoder, knobs, faders)
     f1probe led N V   set LED byte N (0..79) of output report 0x80 to V (0..127), rest off
     f1probe walk      light each LED byte in turn, printing its index (1 s per step)
     f1probe off       all LEDs off */
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static const char* dev = "/dev/hidraw0";

static int leds(int fd, int index, int value) {
  unsigned char out[81] = {0x80};
  if (index >= 0) out[1 + index] = (unsigned char)value;
  return (int)write(fd, out, sizeof out);
}

int main(int argc, char** argv) {
  if (getenv("F1DEV")) dev = getenv("F1DEV");
  const char* cmd = argc > 1 ? argv[1] : "in";
  int fd = open(dev, O_RDWR);
  if (fd < 0) { perror(dev); return 1; }

  if (!strcmp(cmd, "led") && argc > 3) {
    printf("write %d\n", leds(fd, atoi(argv[2]), atoi(argv[3])));
  } else if (!strcmp(cmd, "off")) {
    printf("write %d\n", leds(fd, -1, 0));
  } else if (!strcmp(cmd, "walk")) {
    for (int i = 0; i < 80; i++) {
      printf("led %d\n", i);
      fflush(stdout);
      leds(fd, i, 127);
      sleep(1);
    }
    leds(fd, -1, 0);
  } else {
    unsigned char buf[64], last[64] = {0};
    int lastn = 0;
    for (;;) {
      int n = (int)read(fd, buf, sizeof buf);
      if (n <= 0) { perror("read"); return 1; }
      if (n == lastn && !memcmp(buf, last, n)) continue;
      memcpy(last, buf, n);
      lastn = n;
      /* report 0x01: id, 4 bytes buttons, encoder, 4x knob u16le, 4x fader u16le */
      if (buf[0] == 0x01 && n >= 22) {
        printf("btn %02x%02x%02x%02x enc %3u knob", buf[1], buf[2], buf[3], buf[4], buf[5]);
        for (int i = 0; i < 4; i++) printf(" %4u", buf[6 + 2 * i] | buf[7 + 2 * i] << 8);
        printf(" fader");
        for (int i = 0; i < 4; i++) printf(" %4u", buf[14 + 2 * i] | buf[15 + 2 * i] << 8);
        printf("\n");
      } else {
        for (int i = 0; i < n; i++) printf("%02x", buf[i]);
        printf("\n");
      }
      fflush(stdout);
    }
  }
  close(fd);
  return 0;
}
