// Hardware-free tests for the asynchronous report hand-off used by the IOHID backend.
#include <stdio.h>
#include <string.h>

#include <chrono>
#include <thread>

#include "../src/report_queue.h"

using ogc::ReportQueue;
using Clock = std::chrono::steady_clock;

static int g_failed = 0;

#define CHECK(cond)                                                    \
    do {                                                               \
        if (!(cond)) {                                                 \
            fprintf(stderr, "%s:%d: CHECK(%s)\n", __FILE__, __LINE__, #cond); \
            g_failed++;                                                \
        }                                                              \
    } while (0)

static long elapsedMs(Clock::time_point since) {
    return static_cast<long>(
        std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - since).count());
}

static void testTimeoutAndOrder() {
    ReportQueue q;
    uint8_t buf[ReportQueue::kMaxReport];
    CHECK(q.pop(buf, sizeof buf, 0) == 0);
    const uint8_t a[] = {1, 2, 3}, b[] = {4, 5};
    CHECK(q.push(a, 3));
    CHECK(q.push(b, 2));
    CHECK(q.pop(buf, sizeof buf, 0) == 3 && buf[0] == 1 && buf[2] == 3);
    CHECK(q.pop(buf, sizeof buf, 0) == 2 && buf[0] == 4);
    CHECK(q.pop(buf, sizeof buf, 0) == 0);
}

static void testBoundsAndTruncation() {
    ReportQueue q;
    uint8_t big[ReportQueue::kMaxReport + 1] = {};
    CHECK(!q.push(big, 0));
    CHECK(!q.push(big, -1));
    CHECK(!q.push(big, ReportQueue::kMaxReport + 1));
    CHECK(q.push(big, ReportQueue::kMaxReport));
    uint8_t small[4];
    CHECK(q.pop(small, sizeof small, 0) == 4);

    uint8_t switchReport[362];
    uint8_t received[sizeof(switchReport)];
    for (size_t i = 0; i < sizeof(switchReport); ++i)
        switchReport[i] = static_cast<uint8_t>(i);
    CHECK(q.push(switchReport, sizeof(switchReport)));
    CHECK(q.pop(received, sizeof(received), 0) == sizeof(switchReport));
    CHECK(memcmp(switchReport, received, sizeof(switchReport)) == 0);
}

static void testOverflowDropsOldest() {
    ReportQueue q;
    for (int i = 0; i < ReportQueue::kDepth + 3; i++) {
        const uint8_t v = static_cast<uint8_t>(i);
        CHECK(q.push(&v, 1));
    }
    CHECK(q.dropped() == 3);
    uint8_t buf[1];
    for (int i = 3; i < ReportQueue::kDepth + 3; i++)
        CHECK(q.pop(buf, 1, 0) == 1 && buf[0] == i);
    CHECK(q.pop(buf, 1, 0) == 0);
}

static void testGoneDrainsThenFails() {
    ReportQueue q;
    const uint8_t a = 7;
    CHECK(q.push(&a, 1));
    q.markGone();
    CHECK(!q.push(&a, 1));
    uint8_t buf[1];
    CHECK(q.pop(buf, 1, 0) == 1 && buf[0] == 7);
    CHECK(q.pop(buf, 1, 1000) == -1);
}

// A reader blocked with a long timeout must wake for a report and for removal, not sleep it out.
static void testBlockedReaderWakes() {
    ReportQueue q;
    int got = 99;
    uint8_t buf[4] = {};
    auto start = Clock::now();
    std::thread reader([&] { got = q.pop(buf, sizeof buf, 10000); });
    const uint8_t r[] = {0, 20};
    CHECK(q.push(r, 2));
    reader.join();
    CHECK(got == 2 && buf[1] == 20);
    CHECK(elapsedMs(start) < 5000);

    start = Clock::now();
    std::thread reader2([&] { got = q.pop(buf, sizeof buf, 10000); });
    q.markGone();
    reader2.join();
    CHECK(got == -1);
    CHECK(elapsedMs(start) < 5000);
}

static void testCancelWait() {
    ReportQueue q;
    CHECK(!q.waitCancelled(0));
    std::thread canceller([&] { q.markCancelled(); });
    CHECK(q.waitCancelled(10000));
    canceller.join();
    CHECK(q.waitCancelled(0));
}

// Concurrent producer/removal/consumer: every result honours the read contract and the reader ends.
static void testConcurrentProducer() {
    ReportQueue q;
    std::thread producer([&] {
        for (int i = 0; i < 20000; i++) {
            const uint8_t v[2] = {static_cast<uint8_t>(i), 2};
            q.push(v, 2);
        }
        q.markGone();
    });
    uint8_t buf[2];
    int reports = 0;
    bool contract = true;
    for (;;) {
        const int n = q.pop(buf, sizeof buf, 10000);
        if (n < 0)
            break;
        contract = contract && n == 2 && buf[1] == 2;
        reports++;
    }
    producer.join();
    CHECK(contract);
    CHECK(reports > 0 && reports <= 20000);
    CHECK(static_cast<unsigned>(reports) + q.dropped() == 20000u);
}

int main() {
    testTimeoutAndOrder();
    testBoundsAndTruncation();
    testOverflowDropsOldest();
    testGoneDrainsThenFails();
    testBlockedReaderWakes();
    testCancelWait();
    testConcurrentProducer();
    if (g_failed) {
        fprintf(stderr, "%d check(s) failed\n", g_failed);
        return 1;
    }
    puts("report queue tests passed");
    return 0;
}
