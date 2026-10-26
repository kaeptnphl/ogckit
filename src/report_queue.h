// Bounded hand-off from asynchronous report callbacks to the blocking Backend::read contract.
#pragma once

#include <stdint.h>
#include <string.h>

#include <chrono>
#include <condition_variable>
#include <mutex>

namespace ogc {

class ReportQueue {
public:
    static constexpr int kMaxReport = 512;
    static constexpr int kDepth = 8;

    // Producer side (callback thread). Oversized or empty reports are rejected; when full, the
    // oldest report is dropped because only the newest state matters to the reader.
    // Returns false if the report was rejected or the queue already ended.
    bool push(const uint8_t* data, int len) {
        std::lock_guard<std::mutex> lock(m_);
        if (gone_ || len <= 0 || len > kMaxReport)
            return false;
        if (count_ == kDepth) {
            head_ = (head_ + 1) % kDepth;
            count_--;
            dropped_++;
        }
        Slot& s = slots_[(head_ + count_) % kDepth];
        memcpy(s.data, data, static_cast<size_t>(len));
        s.len = len;
        count_++;
        cv_.notify_all();
        return true;
    }

    // The device was removed or failed: pending and future reads fail once queued reports drain.
    void markGone() {
        std::lock_guard<std::mutex> lock(m_);
        gone_ = true;
        cv_.notify_all();
    }

    // Consumer side, same result contract as Backend::read: >0 bytes, 0 timeout, <0 gone.
    // A report longer than `len` is truncated to `len` bytes.
    int pop(uint8_t* buf, int len, int timeoutMs) {
        std::unique_lock<std::mutex> lock(m_);
        cv_.wait_for(lock, std::chrono::milliseconds(timeoutMs < 0 ? 0 : timeoutMs),
                     [this] { return count_ > 0 || gone_; });
        if (count_ == 0)
            return gone_ ? -1 : 0;
        const Slot& s = slots_[head_];
        const int n = s.len < len ? s.len : len;
        if (n > 0)
            memcpy(buf, s.data, static_cast<size_t>(n));
        head_ = (head_ + 1) % kDepth;
        count_--;
        return n;
    }

    // Signalled once the asynchronous source guarantees no further callbacks.
    void markCancelled() {
        std::lock_guard<std::mutex> lock(m_);
        cancelled_ = true;
        cv_.notify_all();
    }

    // Returns false on timeout; the source may then still call into this queue.
    bool waitCancelled(int timeoutMs) {
        std::unique_lock<std::mutex> lock(m_);
        return cv_.wait_for(lock, std::chrono::milliseconds(timeoutMs), [this] { return cancelled_; });
    }

    unsigned dropped() {
        std::lock_guard<std::mutex> lock(m_);
        return dropped_;
    }

private:
    struct Slot {
        uint8_t data[kMaxReport];
        int len;
    };

    std::mutex m_;
    std::condition_variable cv_;
    Slot slots_[kDepth] = {};
    int head_ = 0;
    int count_ = 0;
    unsigned dropped_ = 0;
    bool gone_ = false;
    bool cancelled_ = false;
};

}  // namespace ogc
