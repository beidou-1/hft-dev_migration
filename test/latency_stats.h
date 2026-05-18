#pragma once

#include <string>
#include <vector>
#include <array>
#include <algorithm>
#include <cmath>
#include "common/logger.h"

struct LatencyStats {
    static constexpr int BUCKET_COUNT = 500'000;

    std::array<int64_t, BUCKET_COUNT> buckets{0};
    int64_t total_count{0};
    int64_t sum_latencies{0};
    int64_t sum_sq_latencies{0};
    int64_t min_latency{INT64_MAX};
    int64_t max_latency{0};

    void add(int64_t latency_ms) {
        if (latency_ms <= 0) {
            return;
        }

        sum_latencies += latency_ms;
        sum_sq_latencies += latency_ms * latency_ms;
        total_count++;
        min_latency = std::min(min_latency, latency_ms);
        max_latency = std::max(max_latency, latency_ms);

        int bucket = std::min(static_cast<int>(latency_ms), BUCKET_COUNT - 1);
        buckets[bucket]++;
    }

    int64_t getPercentile(double percentile) const {
        if (total_count == 0)
            return 0;

        int64_t target = static_cast<int64_t>(total_count * percentile / 100.0);
        int64_t count = 0;

        for (int i = 0; i < BUCKET_COUNT; ++i) {
            count += buckets[i];
            if (count >= target) {
                return i;
            }
        }
        return max_latency;
    }

    void print(const std::string& name, const std::string& unit) {
        if (total_count == 0) {
            INFRA_LOG_INFO("[{}] No data collected", name);
            return;
        }

        double avg = (sum_latencies * 1.0) / total_count;
        double stddev = std::sqrt((sum_sq_latencies * 1.0) / total_count - avg * avg);
        int64_t p50 = getPercentile(50);
        int64_t p95 = getPercentile(95);
        int64_t p99 = getPercentile(99);
        int64_t p999 = getPercentile(99.9);

        // 转换单位，输出成md格式
        avg /= 1000.0;
        stddev /= 1000.0;
        double min_ = min_latency / 1000.0;
        double max_ = max_latency / 1000.0;
        double p50_ = p50 / 1000.0;
        double p95_ = p95 / 1000.0;
        double p99_ = p99 / 1000.0;
        double p999_ = p999 / 1000.0;
        INFRA_LOG_INFO("| {} | {:.2f} | {:.2f} | {:.2f} | {:.2f} | {:.2f} | {:.2f} | {:.2f} | {:.2f} | {} |, total: {}",
                       name, avg, min_, max_, stddev, p50_, p95_, p99_, p999_, (unit == "ns") ? "us" : "ms",
                       total_count);
    }
};
