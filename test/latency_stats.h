#pragma once

#include <string>
#include <vector>
#include <array>
#include <algorithm>
#include <cmath>
#include "common/logger.h"

using namespace infra;

struct MktdataLatencyStats {
    static constexpr int BUCKET_COUNT = 80000; // 分成80000个桶，每个桶跨度为1ns

    std::array<int64_t, BUCKET_COUNT> buckets{0};
    int64_t total_count{0};
    int64_t success_count{0};
    int64_t error_count{0};
    int64_t sum_latencies{0};
    int64_t sum_sq_latencies{0};
    int64_t min_latency{INT64_MAX};
    int64_t max_latency{0};

    void add(int64_t latency_ms, bool success = true) {
        if (latency_ms <= 0) {
            return;
        }

        sum_latencies += latency_ms;
        sum_sq_latencies += latency_ms * latency_ms;
        total_count++;
        min_latency = std::min(min_latency, latency_ms);
        max_latency = std::max(max_latency, latency_ms);

        if (success)
            success_count++;
        else
            error_count++;

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
        auto p50 = getPercentile(50);
        // auto p90 = getPercentile(90);
        auto p95 = getPercentile(95);
        auto p99 = getPercentile(99);
        // auto p999 = getPercentile(99.9);

        INFRA_LOG_INFO(
            "[{}], stat({}) - Avg: {:.1f}, Min: {}, Max: {}, Stddev: {:.1f}, P50: {}, P95: {}, P99: {}, Total: {}",
            name, unit, avg, min_latency, max_latency, stddev, p50, p95, p99, total_count);
    }
};
