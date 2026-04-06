// Redis/main_consumer.cpp
//
// Redis Pub/Sub consumer benchmark (Unix Domain Socket).
//
// Usage:
//   ./consumer [consumer_id]   (default: 0)
//
// Start this BEFORE the producer. Subscribes to channel "bench" and records
// per-message one-way latency (T2 - T1 in TSC cycles).
//
// Output:
//   /tmp/latency_redis_c<id>.bin    — binary array of uint64 latency cycles
//   /tmp/bench_redis_c<id>.tp       — 3 lines: s2_ns, n_recv, n_expected

#include <chrono>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

#include <x86intrin.h>

#include "RedisConsumer.hpp"

// Benchmark configuration constants: total messages, warmup marker, Redis socket path, and channel name.
static constexpr uint64_t WARMUP_SENTINEL = 0xFFFF'FFFF'FFFF'0000ULL;
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

// Main
int main(int argc, char* argv[]) {
    const int consumer_id = (argc > 1) ? std::stoi(argv[1]) : 0;
    const uint64_t n_messages = (argc > 2) ? std::stoull(argv[2]) : 1'000'000ULL;
    try {
        RedisConsumer consumer(SOCK_PATH, CHANNEL);
        std::cout << "Consumer " << consumer_id
                  << " subscribed, waiting for messages...\n";

        std::vector<uint64_t> latencies;
        latencies.reserve(n_messages);

        uint64_t s2_ns  = 0;
        uint64_t n_recv = 0;

        while (n_recv < n_messages) {
            Message msg{};
            if (!consumer.recv(msg)) continue;

            const uint64_t recv_tsc = rdtsc_now();  // T2: right after recv

            // Skip warmup messages
            if (msg.seq_id >= WARMUP_SENTINEL) continue;

            latencies.push_back(recv_tsc - msg.send_tsc);
            ++n_recv;

            // Record wall clock on the final expected message
            if (msg.seq_id == n_messages - 1) {
                s2_ns = now_ns();
                break;
            }
        }

        std::cout << "Consumer " << consumer_id
                  << " received " << n_recv << " / " << n_messages << " messages.\n";

        // ── Write latency binary ───────────────────────────────────────────────
        {
            const std::string fname =
                "/tmp/latency_redis_c" + std::to_string(consumer_id) + ".bin";
            std::ofstream f(fname, std::ios::binary);
            f.write(reinterpret_cast<const char*>(latencies.data()),
                    static_cast<std::streamsize>(latencies.size() * sizeof(uint64_t)));
            std::cout << "Wrote " << fname << "\n";
        }

        // ── Write throughput stats ─────────────────────────────────────────────
        {
            const std::string fname =
                "/tmp/bench_redis_c" + std::to_string(consumer_id) + ".tp";
            std::ofstream f(fname);
            f << s2_ns      << "\n"
              << n_recv     << "\n"
              << n_messages << "\n";
            std::cout << "Wrote " << fname << "\n";
        }

    } catch (const std::exception& e) {
        std::cerr << "Consumer error: " << e.what() << "\n";
        return 1;
    }
    return 0;
}
