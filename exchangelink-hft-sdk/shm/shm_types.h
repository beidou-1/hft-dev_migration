#pragma once

#include <atomic>
#include <cstdint>
#include <cstring>
#include <new>
#include <type_traits>

namespace hft {

// ── 通用长度常量 ──────────────────────────────────────────────
static constexpr uint32_t LEN_32  = 32;
static constexpr uint32_t LEN_64  = 64;
static constexpr uint32_t LEN_128 = 128;

static constexpr uint32_t MAX_SPREADS     = 256;
static constexpr uint32_t MAX_CUSTOM_INFO = 8;

// ── SPSC 无锁环形队列 ─────────────────────────────────────────
// - 单写单读，无锁
// - head_/tail_ 各自独占 cache line，避免 false sharing
// - T 必须是 trivially_copyable
template <typename T, uint32_t N>
struct SpscQueue {
    static_assert((N & (N - 1)) == 0, "N must be power of 2");
    static_assert(std::is_trivially_copyable_v<T>, "T must be trivially copyable");

    // 写端：push，失败返回 false（队列满）
    bool push(const T& val) noexcept {
        const uint32_t tail = tail_.load(std::memory_order_relaxed);
        const uint32_t next = (tail + 1) & (N - 1);
        if (next == head_.load(std::memory_order_acquire))
            return false;
        std::memcpy(&slots_[tail], &val, sizeof(T));
        tail_.store(next, std::memory_order_release);
        return true;
    }

    // 读端：pop，失败返回 false（队列空）
    bool pop(T& val) noexcept {
        const uint32_t head = head_.load(std::memory_order_relaxed);
        if (head == tail_.load(std::memory_order_acquire))
            return false;
        std::memcpy(&val, &slots_[head], sizeof(T));
        head_.store((head + 1) & (N - 1), std::memory_order_release);
        return true;
    }

    bool empty() const noexcept {
        return head_.load(std::memory_order_acquire) ==
               tail_.load(std::memory_order_acquire);
    }

private:
    static constexpr std::size_t kCacheLine = 64;

    alignas(kCacheLine) std::atomic<uint32_t> head_{0};
    alignas(kCacheLine) std::atomic<uint32_t> tail_{0};
    alignas(kCacheLine) T slots_[N];
};

} // namespace hft
