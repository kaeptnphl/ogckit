#include <CoreFoundation/CoreFoundation.h>
#include <IOKit/hid/IOHIDKeys.h>
#include <IOKit/hid/IOHIDManager.h>
#include <fcntl.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/select.h>
#include <time.h>
#include <unistd.h>

enum { kReportBufferSize = 1024, kMaxCaptureSeconds = 300, kBlockCount = 3 };

static volatile sig_atomic_t g_stop = 0;
static FILE* g_rawFile;
static FILE* g_markerFile;
static unsigned g_reportCount;
static uint64_t g_totalReportBytes;
static unsigned g_nextMarker = 1;
static int g_callbackFailed;
static uint8_t* g_reportBuffer;

static void stopCapture(int signalNumber) {
    (void)signalNumber;
    g_stop = 1;
}

static int getNumberProperty(IOHIDDeviceRef device, CFStringRef key, int* value) {
    CFTypeRef property = IOHIDDeviceGetProperty(device, key);
    if (!property || CFGetTypeID(property) != CFNumberGetTypeID())
        return 0;
    return CFNumberGetValue((CFNumberRef)property, kCFNumberIntType, value);
}

static void printStringProperty(FILE* out, IOHIDDeviceRef device, CFStringRef key,
                                const char* label) {
    CFTypeRef property = IOHIDDeviceGetProperty(device, key);
    if (!property || CFGetTypeID(property) != CFStringGetTypeID())
        return;
    char value[256];
    if (CFStringGetCString((CFStringRef)property, value, sizeof(value), kCFStringEncodingUTF8))
        fprintf(out, "%s: %s\n", label, value);
}

static void printDevice(FILE* out, IOHIDDeviceRef device) {
    int vendor = 0, product = 0, maxInput = 0, usagePage = 0, usage = 0;
    getNumberProperty(device, CFSTR(kIOHIDVendorIDKey), &vendor);
    getNumberProperty(device, CFSTR(kIOHIDProductIDKey), &product);
    getNumberProperty(device, CFSTR(kIOHIDMaxInputReportSizeKey), &maxInput);
    getNumberProperty(device, CFSTR(kIOHIDPrimaryUsagePageKey), &usagePage);
    getNumberProperty(device, CFSTR(kIOHIDPrimaryUsageKey), &usage);
    printStringProperty(out, device, CFSTR(kIOHIDProductKey), "Product");
    printStringProperty(out, device, CFSTR(kIOHIDTransportKey), "Transport");
    fprintf(out, "VID:PID: %04X:%04X\nUsagePage: 0x%04X\nUsage: 0x%04X\nMaxInputReportSize: %d\n",
            vendor, product, usagePage, usage, maxInput);

    CFTypeRef descriptor = IOHIDDeviceGetProperty(device, CFSTR(kIOHIDReportDescriptorKey));
    if (descriptor && CFGetTypeID(descriptor) == CFDataGetTypeID()) {
        CFDataRef data = (CFDataRef)descriptor;
        const UInt8* bytes = CFDataGetBytePtr(data);
        const CFIndex length = CFDataGetLength(data);
        fprintf(out, "ReportDescriptor (%ld bytes):", (long)length);
        for (CFIndex i = 0; i < length; ++i)
            fprintf(out, " %s%02X", i ? " " : "", bytes[i]);
        fputc('\n', out);
    } else {
        fputs("ReportDescriptor: unavailable as an IOHIDDevice property\n", out);
    }
}

static void printDeviceFromSet(const void* value, void* context) {
    printDevice((FILE*)context, (IOHIDDeviceRef)value);
}

static CFMutableDictionaryRef createMatching(const char* transport, int useIdentity, int vendor,
                                             int product) {
    CFMutableDictionaryRef matching =
        CFDictionaryCreateMutable(kCFAllocatorDefault, 0, &kCFTypeDictionaryKeyCallBacks,
                                  &kCFTypeDictionaryValueCallBacks);
    if (!matching)
        return NULL;
    CFStringRef transportValue = strcmp(transport, "USB") == 0 ? CFSTR("USB")
                                                               : CFSTR("Bluetooth");
    CFDictionarySetValue(matching, CFSTR(kIOHIDTransportKey), transportValue);
    if (useIdentity) {
        CFNumberRef vendorNumber = CFNumberCreate(kCFAllocatorDefault, kCFNumberIntType, &vendor);
        CFNumberRef productNumber = CFNumberCreate(kCFAllocatorDefault, kCFNumberIntType, &product);
        if (!vendorNumber || !productNumber) {
            if (vendorNumber)
                CFRelease(vendorNumber);
            if (productNumber)
                CFRelease(productNumber);
            CFRelease(matching);
            return NULL;
        }
        CFDictionarySetValue(matching, CFSTR(kIOHIDVendorIDKey), vendorNumber);
        CFDictionarySetValue(matching, CFSTR(kIOHIDProductIDKey), productNumber);
        CFRelease(vendorNumber);
        CFRelease(productNumber);
    }
    return matching;
}

static void onInputReport(void* context, IOReturn result, void* sender, IOHIDReportType type,
                          uint32_t reportID, uint8_t* report, CFIndex length) {
    (void)context;
    (void)sender;
    if (result != kIOReturnSuccess) {
        fprintf(stderr, "ERROR: input report callback failed: 0x%08X\n", result);
        g_callbackFailed = 1;
        g_stop = 1;
        return;
    }
    if (type != kIOHIDReportTypeInput || !report || length < 0 ||
        length > kReportBufferSize) {
        fprintf(stderr, "ERROR: unexpected report type/length: %u/%ld\n", type, (long)length);
        g_callbackFailed = 1;
        g_stop = 1;
        return;
    }

    fprintf(g_rawFile, "report[%u] id=%u length=%ld data=", ++g_reportCount, reportID,
            (long)length);
    for (CFIndex i = 0; i < length; ++i)
        fprintf(g_rawFile, "%s%02X", i ? " " : "", report[i]);
    fputc('\n', g_rawFile);
    if (fflush(g_rawFile) != 0) {
        fputs("ERROR: failed writing raw report file\n", stderr);
        g_callbackFailed = 1;
        g_stop = 1;
        return;
    }
    g_totalReportBytes += (uint64_t)length;
    if (g_reportCount == 1)
        fprintf(stderr, "FIRST_REPORT_RECEIVED: id=%u length=%ld; raw capture is active.\n",
                reportID, (long)length);
}

static int createExclusiveFile(const char* path, FILE** file) {
    int fd = open(path, O_WRONLY | O_CREAT | O_EXCL, 0600);
    if (fd < 0) {
        perror(path);
        return 0;
    }
    *file = fdopen(fd, "w");
    if (!*file) {
        perror("fdopen");
        close(fd);
        return 0;
    }
    return 1;
}

static int parseHexId(const char* text, int* value) {
    char* end = NULL;
    long parsed = strtol(text, &end, 16);
    if (!text[0] || !end || *end || parsed < 0 || parsed > 0xFFFF)
        return 0;
    *value = (int)parsed;
    return 1;
}

static int recordMarker(const char* command) {
    static const char* const names[kBlockCount] = {"buttons", "dpad-sticks", "triggers"};
    const unsigned boundary = g_nextMarker - 1;
    unsigned block = boundary / 2;
    const char* edge = (boundary % 2) ? "end" : "start";
    char expected[32];
    if (g_reportCount == 0) {
        fputs("Wait for FIRST_REPORT_RECEIVED before marking a block.\n", stderr);
        return 0;
    }
    if (block >= kBlockCount) {
        fprintf(stderr, "Rejected marker '%s'; all %u block boundaries are already recorded.\n",
                command, kBlockCount * 2);
        return 0;
    }
    snprintf(expected, sizeof(expected), "%u-%s", block + 1, edge);
    if (strcmp(command, expected) != 0) {
        fprintf(stderr, "Rejected marker '%s'; expected '%s'. No marker was recorded.\n",
                command, expected);
        return -1;
    }
    if (fprintf(g_markerFile, "MARKER block=%s edge=%s last_report=%u total_report_bytes=%llu\n",
                names[block], edge, g_reportCount,
                (unsigned long long)g_totalReportBytes) < 0 ||
        fflush(g_markerFile) != 0) {
        fputs("ERROR: failed writing marker file\n", stderr);
        g_callbackFailed = 1;
        g_stop = 1;
        return -1;
    }
    fprintf(stderr, "Recorded %s boundary at report %u, byte offset %llu\n",
            expected, g_reportCount, (unsigned long long)g_totalReportBytes);
    ++g_nextMarker;
    return 1;
}

static int processCommands(void) {
    fd_set input;
    FD_ZERO(&input);
    FD_SET(STDIN_FILENO, &input);
    struct timeval timeout = {0, 0};
    int ready = select(STDIN_FILENO + 1, &input, NULL, NULL, &timeout);
    if (ready <= 0)
        return 1;

    char command[64];
    if (!fgets(command, sizeof(command), stdin)) {
        fputs("ERROR: marker input closed before capture finished\n", stderr);
        return 0;
    }
    command[strcspn(command, "\r\n")] = '\0';
    if (strcmp(command, "q") == 0) {
        g_stop = 1;
        return 1;
    }
    recordMarker(command);
    return !g_callbackFailed;
}

static int listDevices(const char* transport) {
    IOHIDManagerRef manager = IOHIDManagerCreate(kCFAllocatorDefault, kIOHIDOptionsTypeNone);
    if (!manager) {
        fputs("ERROR: IOHIDManagerCreate failed\n", stderr);
        return 1;
    }
    CFMutableDictionaryRef matching = createMatching(transport, 0, 0, 0);
    if (!matching) {
        fprintf(stderr, "ERROR: unable to allocate %s matching filter\n", transport);
        CFRelease(manager);
        return 1;
    }
    IOHIDManagerSetDeviceMatching(manager, matching);
    CFRelease(matching);
    CFRunLoopRef runLoop = CFRunLoopGetCurrent();
    IOHIDManagerScheduleWithRunLoop(manager, runLoop, kCFRunLoopDefaultMode);
    IOReturn result = IOHIDManagerOpen(manager, kIOHIDOptionsTypeNone);
    if (result != kIOReturnSuccess) {
        fprintf(stderr, "ERROR: IOHIDManagerOpen failed: 0x%08X\n", result);
        IOHIDManagerUnscheduleFromRunLoop(manager, runLoop, kCFRunLoopDefaultMode);
        CFRelease(manager);
        return 1;
    }
    CFSetRef devices = IOHIDManagerCopyDevices(manager);
    CFIndex count = devices ? CFSetGetCount(devices) : 0;
    printf("%s HID devices visible to macOS: %ld\n", transport, (long)count);
    if (devices)
        CFSetApplyFunction(devices, printDeviceFromSet, stdout);
    if (devices)
        CFRelease(devices);
    IOHIDManagerClose(manager, kIOHIDOptionsTypeNone);
    IOHIDManagerUnscheduleFromRunLoop(manager, runLoop, kCFRunLoopDefaultMode);
    CFRelease(manager);
    return 0;
}

static int capture(const char* transport, int vendor, int product, int seconds,
                   const char* rawPath, const char* markerPath) {
    if (!isatty(STDIN_FILENO)) {
        fputs("ERROR: capture markers require an interactive terminal.\n", stderr);
        return 1;
    }
    IOHIDManagerRef manager = IOHIDManagerCreate(kCFAllocatorDefault, kIOHIDOptionsTypeNone);
    if (!manager) {
        fputs("ERROR: IOHIDManagerCreate failed\n", stderr);
        return 1;
    }
    CFMutableDictionaryRef matching = createMatching(transport, 1, vendor, product);
    if (!matching) {
        fputs("ERROR: unable to allocate device matching filter\n", stderr);
        CFRelease(manager);
        return 1;
    }
    IOHIDManagerSetDeviceMatching(manager, matching);
    CFRelease(matching);
    IOHIDManagerRegisterInputReportCallback(manager, onInputReport, NULL);

    CFRunLoopRef runLoop = CFRunLoopGetCurrent();
    IOHIDManagerScheduleWithRunLoop(manager, runLoop, kCFRunLoopDefaultMode);
    IOReturn result = IOHIDManagerOpen(manager, kIOHIDOptionsTypeNone);
    if (result != kIOReturnSuccess) {
        fprintf(stderr, "ERROR: IOHIDManagerOpen failed: 0x%08X\n", result);
        IOHIDManagerUnscheduleFromRunLoop(manager, runLoop, kCFRunLoopDefaultMode);
        CFRelease(manager);
        return 1;
    }

    CFSetRef devices = IOHIDManagerCopyDevices(manager);
    CFIndex count = devices ? CFSetGetCount(devices) : 0;
    if (count != 1) {
        fprintf(stderr, "ERROR: expected exactly one %s HID device %04X:%04X; found %ld\n",
                transport, vendor, product, (long)count);
        if (devices)
            CFRelease(devices);
        IOHIDManagerClose(manager, kIOHIDOptionsTypeNone);
        IOHIDManagerUnscheduleFromRunLoop(manager, runLoop, kCFRunLoopDefaultMode);
        CFRelease(manager);
        return 1;
    }
    const void* deviceValues[1] = {NULL};
    CFSetGetValues(devices, deviceValues);
    IOHIDDeviceRef device = (IOHIDDeviceRef)deviceValues[0];
    CFRetain(device);
    fprintf(stderr, "Selected %s HID device metadata:\n", transport);
    printDevice(stderr, device);
    int maxInput = 0;
    getNumberProperty(device, CFSTR(kIOHIDMaxInputReportSizeKey), &maxInput);
    CFRelease(devices);
    if (maxInput > kReportBufferSize) {
        fprintf(stderr, "ERROR: advertised input report size %d exceeds capture limit %d\n",
                maxInput, kReportBufferSize);
        CFRelease(device);
        IOHIDManagerClose(manager, kIOHIDOptionsTypeNone);
        IOHIDManagerUnscheduleFromRunLoop(manager, runLoop, kCFRunLoopDefaultMode);
        CFRelease(manager);
        return 1;
    }

    if (!createExclusiveFile(rawPath, &g_rawFile) ||
        !createExclusiveFile(markerPath, &g_markerFile)) {
        if (g_rawFile)
            fclose(g_rawFile);
        if (g_markerFile)
            fclose(g_markerFile);
        CFRelease(device);
        IOHIDManagerClose(manager, kIOHIDOptionsTypeNone);
        IOHIDManagerUnscheduleFromRunLoop(manager, runLoop, kCFRunLoopDefaultMode);
        CFRelease(manager);
        return 1;
    }
    signal(SIGINT, stopCapture);
    signal(SIGTERM, stopCapture);
    fprintf(stderr,
            "CAPTURE_READY: selected %s HID %04X:%04X; waiting for first report, max %d seconds.\n"
            "Only after report reception is confirmed, enter 1-start, 1-end, 2-start, "
            "2-end, 3-start, 3-end at the terminal. Blocks: buttons; D-pad/both sticks; "
            "L/R analog/end-clicks. Enter q to stop. Raw and marker files are separate.\n",
            transport, vendor, product, seconds);
    fflush(stderr);

    const double deadline = CFAbsoluteTimeGetCurrent() + seconds;
    while (!g_stop && CFAbsoluteTimeGetCurrent() < deadline) {
        if (!processCommands()) {
            g_callbackFailed = 1;
            break;
        }
        CFRunLoopRunInMode(kCFRunLoopDefaultMode, 0.05, true);
    }
    if (!g_stop && CFAbsoluteTimeGetCurrent() >= deadline)
        fputs("Capture reached its time limit.\n", stderr);
    if (g_nextMarker <= kBlockCount * 2)
        fprintf(stderr, "WARNING: only %u of %u block boundaries were marked.\n",
                g_nextMarker - 1, kBlockCount * 2);

    IOHIDManagerClose(manager, kIOHIDOptionsTypeNone);
    IOHIDManagerUnscheduleFromRunLoop(manager, runLoop, kCFRunLoopDefaultMode);
    CFRelease(device);
    CFRelease(manager);
    fprintf(stderr, "Capture stopped; reports=%u bytes=%llu\n", g_reportCount,
            (unsigned long long)g_totalReportBytes);
    fclose(g_rawFile);
    fclose(g_markerFile);
    return g_callbackFailed ? 1 : 0;
}

static void usage(const char* name) {
    fprintf(stderr,
            "Usage:\n"
            "  %s --list\n"
            "  %s --capture VID PID SECONDS RAW_FILE MARKER_FILE\n"
            "  %s --list-usb\n"
            "  %s --capture-usb VID PID SECONDS RAW_FILE MARKER_FILE\n"
            "IDs are hexadecimal as shown by the matching --list command; SECONDS must be 1..%d.\n"
            "USB commands observe USB HID devices exposed to macOS IOHID; they do not "
            "capture arbitrary libusb interfaces.\n",
            name, name, name, name, kMaxCaptureSeconds);
}

int main(int argc, char** argv) {
    if (argc == 2 && strcmp(argv[1], "--list") == 0)
        return listDevices("Bluetooth");
    if (argc == 2 && strcmp(argv[1], "--list-usb") == 0)
        return listDevices("USB");
    if (argc == 7 && (strcmp(argv[1], "--capture") == 0 ||
                      strcmp(argv[1], "--capture-usb") == 0)) {
        const char* transport = strcmp(argv[1], "--capture-usb") == 0 ? "USB" : "Bluetooth";
        int vendor, product;
        char* end = NULL;
        long seconds = strtol(argv[4], &end, 10);
        if (!parseHexId(argv[2], &vendor) || !parseHexId(argv[3], &product) ||
            !argv[4][0] || !end || *end || seconds < 1 || seconds > kMaxCaptureSeconds) {
            usage(argv[0]);
            return 2;
        }
        return capture(transport, vendor, product, (int)seconds, argv[5], argv[6]);
    }
    usage(argv[0]);
    return 2;
}
