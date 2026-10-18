// Terminal demo for the libusb backend: prints pad state changes. Ctrl+C to quit.
#include <stdio.h>
#include <unistd.h>

#include "ogckit.h"

static void logLine(const char* line) {
    fprintf(stderr, "%s\n", line);
}

int main() {
    ogc_set_log(logLine);
    if (ogc_init() != 0) {
        fprintf(stderr, "ogc_init failed\n");
        return 1;
    }
    ogc_state last[OGC_MAX_PADS] = {};
    for (;;) {
        for (int i = 0; i < OGC_MAX_PADS; i++) {
            ogc_info info;
            ogc_state s;
            if (!ogc_get(i, &info, &s) || s.sequence == last[i].sequence)
                continue;
            if (s.buttons != last[i].buttons || s.lt != last[i].lt || s.rt != last[i].rt ||
                s.lx / 2048 != last[i].lx / 2048 || s.ly / 2048 != last[i].ly / 2048 ||
                s.rx / 2048 != last[i].rx / 2048 || s.ry / 2048 != last[i].ry / 2048) {
                printf("pad %d %04X:%04X btn=%04X L(%d,%d) R(%d,%d) LT=%u RT=%u\n", i, info.vid,
                       info.pid, s.buttons, s.lx, s.ly, s.rx, s.ry, s.lt, s.rt);
                fflush(stdout);
                last[i] = s;
            }
            last[i].sequence = s.sequence;
        }
        usleep(5000);
    }
}
