// Redis/main_producer.cpp
//
// Redis Pub/Sub producer benchmark (Unix Domain Socket).
//
// Usage:
//   ./producer
//
// Publishes N_MESSAGES to channel "bench" via UDS.
// Start all consumer processes BEFORE this producer.
//
// Output:
//   /tmp/bench_redis_tsc_ghz.txt
//   /tmp/bench_redis_s1.ns

#include <chrono>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <iostream>
#include <thread>

#include <x86intrin.h>

#include "RedisProducer.hpp"

// Benchmark configuration constants: total messages, warmup marker, Redis socket path, and channel name.
static constexpr uint64_t WARMUP_SENTINEL = 0xFFFF'FFFF'FFFF'0000ULL;
static constexpr int      N_WARMUP        = 10'000;
static constexpr const char* SOCK_PATH   = "/tmp/redis_bench.sock";
static constexpr const char* CHANNEL     = "bench";

// Return the current CPU timestamp counter value.
static inline uint64_t rdtsc_now() {
    _mm_lfence();
    return __rdtsc();
}

// Return the current steady-clock time as nanoseconds.
// Used it to calculate throughput
static uint64_t now_ns() {
    return static_cast<uint64_t>(
        std::chrono::steady_clock::now().time_since_epoch() /
        std::chrono::nanoseconds(1));
}

// Estimate the TSC frequency in GHz using a ~200 ms sleep interval.
static double estimate_tsc_ghz() {
    const auto t0     = std::chrono::steady_clock::now();
    const uint64_t c0 = rdtsc_now();
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    const uint64_t c1 = rdtsc_now();
    const auto t1     = std::chrono::steady_clock::now();
    const double ns   = std::chrono::duration<double, std::nano>(t1 - t0).count();
    return static_cast<double>(c1 - c0) / ns;
}

// Main
int main(int argc, char* argv[]) {
    const uint64_t n_messages = (argc > 1) ? std::stoull(argv[1]) : 1'000'000ULL;
    try {

        const double tsc_ghz = estimate_tsc_ghz();
        std::ofstream("/tmp/bench_redis_tsc_ghz.txt") << tsc_ghz << "\n";
        std::cout << "TSC freq: " << tsc_ghz << " GHz\n";

        RedisProducer producer(SOCK_PATH, CHANNEL);

        // ── Warmup ─────────────────────────────────────────────────────────────
        std::cout << "Warming up (" << N_WARMUP << " msgs)...\n";
        for (int w = 0; w < N_WARMUP; ++w) {
            Message msg{};
            msg.seq_id   = WARMUP_SENTINEL + static_cast<uint64_t>(w);
            msg.send_tsc = rdtsc_now();
            std::memset(msg.payload.data(), w & 0xFF, msg.payload.size());
            producer.send(msg);
        }
        // Let consumers drain warmup before the real clock starts
        std::this_thread::sleep_for(std::chrono::milliseconds(500));

        // ── Benchmark ──────────────────────────────────────────────────────────
        // Record S1 (wall-clock) right before the first real send
        const uint64_t s1 = now_ns();
        std::ofstream("/tmp/bench_redis_s1.ns") << s1 << "\n";

        std::cout << "Sending " << n_messages << " messages...\n";
        for (uint64_t i = 0; i <n_messages; ++i) {
            Message msg{};
            msg.seq_id = i;
            std::memset(msg.payload.data(), static_cast<int>(i & 0xFF),
                        msg.payload.size());
            msg.send_tsc = rdtsc_now();     // T1: right before PUBLISH
            producer.send(msg);
        }

        std::cout << "Producer done. Sent " << n_messages << " messages.\n";
    } catch (const std::exception& e) {
        std::cerr << "Producer error: " << e.what() << "\n";
        return 1;
    }
    return 0;
}
