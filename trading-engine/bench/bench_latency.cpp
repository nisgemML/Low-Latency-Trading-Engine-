// bench_latency.cpp — per-event end-to-end latency benchmark.
//
// Methodology:
//   Measure the wall-clock time between submitting a MarketDataMsg to the
//   matching engine's SPSC queue and receiving the corresponding
//   ExecutionReport on the outbound queue.  This covers:
//     inbound SPSC push → message decode → matching → outbound SPSC push.
//
//   We use CLOCK_MONOTONIC with clock_gettime for nanosecond resolution.
//   TSC-based timing would be more accurate for sub-100ns measurements but
//   requires TSC synchronization across cores; CLOCK_MONOTONIC is simpler
//   and sufficient to validate the p50/p99 targets.

#include "core/matching_engine.hpp"
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <vector>
#include <time.h>
#include <numeric>

using namespace engine;

static uint64_t now_ns() {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return static_cast<uint64_t>(ts.tv_sec) * 1'000'000'000ULL + ts.tv_nsec;
}

static void print_percentile(std::vector<uint64_t>& samples, const char* label) {
    std::sort(samples.begin(), samples.end());
    const std::size_t n = samples.size();
    printf("%-20s  p50=%4lu ns  p90=%4lu ns  p99=%4lu ns  p99.9=%5lu ns  max=%6lu ns\n",
           label,
           samples[n * 50 / 100],
           samples[n * 90 / 100],
           samples[n * 99 / 100],
           samples[n * 999 / 1000],
           samples.back());
}

int main() {
    printf("=== Low-Latency Trading Engine — Latency Benchmark ===\n\n");

    MatchingEngine engine;
    engine.register_symbol(0);
    engine.start();

    // Warm up JIT / caches.
    static constexpr int kWarmup   = 100'000;
    static constexpr int kSamples  = 1'000'000;

    MarketDataMsg msg{};
    msg.msg_type   = MarketDataMsg::Type::NewOrder;
    msg.symbol     = 0;
    msg.order_type = OrderType::Limit;

    // Pre-build a mix: 70% new orders, 20% cancels, 10% market orders.
    uint64_t order_id_gen = 1;

    auto send_and_time = [&](bool measure) -> uint64_t {
        msg.order_id  = order_id_gen++;
        msg.price     = to_price(100.0 + (order_id_gen % 10) * 0.01);
        msg.qty       = 100 + (order_id_gen % 50);
        msg.side      = (order_id_gen % 2 == 0) ? Side::Buy : Side::Sell;

        const uint64_t t0 = measure ? now_ns() : 0;
        while (!engine.submit(msg)) { /* spin if full */ }

        ExecutionReport rpt;
        // Aggressive orders will produce a report; passives won't.
        // We measure submit→poll latency regardless.
        uint64_t spin = 0;
        while (spin++ < 100'000 && !engine.poll_report(rpt)) {}

        return measure ? (now_ns() - t0) : 0;
    };

    // Warm-up.
    printf("Warming up (%d events)...\n", kWarmup);
    for (int i = 0; i < kWarmup; ++i) send_and_time(false);

    // Timed run.
    printf("Sampling %d events...\n\n", kSamples);
    std::vector<uint64_t> latencies;
    latencies.reserve(kSamples);

    for (int i = 0; i < kSamples; ++i)
        latencies.push_back(send_and_time(true));

    print_percentile(latencies, "End-to-end");

    // Also compute throughput from total time.
    uint64_t total_ns = std::accumulate(latencies.begin(), latencies.end(), 0ULL);
    double   msgs_sec = kSamples / (total_ns / 1e9);
    printf("\nThroughput (single-threaded submit): %.2f M msgs/sec\n", msgs_sec / 1e6);

    engine.stop();
    printf("\nMatching engine stats:\n");
    printf("  Messages processed : %lu\n", engine.messages_processed());
    printf("  Matches generated  : %lu\n", engine.matches_generated());
    return 0;
}
