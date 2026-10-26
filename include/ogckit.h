/* OGCKit: Open Game Controller Kit.
 *
 * USB game controller input for console homebrew and emulators. A plain C API: devices are
 * found and dropped again while running (hotplug), each is served by a small driver that turns
 * the device's input reports into one common state. Read-only: nothing is ever sent to a pad.
 *
 * Drivers so far: Xbox 360-class XInput and Switch Pro USB reports (including the Miadore
 * receiver in its Mode A setting).
 * Backends (how USB is reached): PS4 (OpenOrbis, sceUsbd) and libusb (PC, for development).
 * Lifecycle calls are serialized; ogc_get() may run concurrently from any thread.
 */
#ifndef OGCKIT_H
#define OGCKIT_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define OGC_MAX_PADS 8

/* Button bits (Xbox layout names; A is the bottom face button). */
enum {
    OGC_BTN_A = 1u << 0,
    OGC_BTN_B = 1u << 1,
    OGC_BTN_X = 1u << 2,
    OGC_BTN_Y = 1u << 3,
    OGC_BTN_LB = 1u << 4,
    OGC_BTN_RB = 1u << 5,
    OGC_BTN_BACK = 1u << 6,
    OGC_BTN_START = 1u << 7,
    OGC_BTN_GUIDE = 1u << 8,
    OGC_BTN_L3 = 1u << 9,
    OGC_BTN_R3 = 1u << 10,
    OGC_BTN_DPAD_UP = 1u << 11,
    OGC_BTN_DPAD_DOWN = 1u << 12,
    OGC_BTN_DPAD_LEFT = 1u << 13,
    OGC_BTN_DPAD_RIGHT = 1u << 14,
};

typedef struct {
    uint32_t buttons;       /* OGC_BTN_* */
    int16_t lx, ly, rx, ry; /* -32768..32767, up and right are positive */
    uint8_t lt, rt;         /* analog triggers, 0..255 */
    uint32_t sequence;      /* increments with every report received */
} ogc_state;

typedef struct {
    uint16_t vid, pid;
    char driver[16];        /* name of the driver serving this pad, e.g. "xinput" */
} ogc_info;

/* Optional; set before ogc_init() or after ogc_shutdown(). Changes while initialized are
 * ignored and reported to the current logger. Callbacks can run concurrently on library
 * threads and the calling thread; they must be thread-safe and must not call ogc_init(),
 * ogc_shutdown(), or ogc_set_log(). Calling ogc_get() from a callback is allowed. */
void ogc_set_log(void (*fn)(const char* line));

/* Starts the USB backend and device scanning. Returns 0 on success, negative on failure.
 * Repeated init/shutdown calls are harmless. This is one process-wide instance, not
 * reference-counted: any shutdown stops it. Concurrent lifecycle calls are serialized.
 * Shutdown waits for an in-flight scan/open and all reader threads to finish. */
int ogc_init(void);
void ogc_shutdown(void);

/* Slot 0..OGC_MAX_PADS-1; a pad keeps its slot while it stays plugged in.
 * Returns 1 and fills a coherent snapshot (either output may be NULL) if a pad is
 * connected there, else 0 and leaves outputs unchanged. Once shutdown begins, returns 0.
 * Sequence and state reset on reconnection; sequence is not a device identity. */
int ogc_get(int slot, ogc_info* info, ogc_state* state);

#ifdef __cplusplus
}
#endif

#endif
