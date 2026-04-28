// zeroMQ/main_producer.cpp
//
// ZeroMQ PUB/SUB producer benchmark (ipc:// transport).
//
// Usage:
//   ./producer [n_messages] [n_consumers] [out_dir] [target_mps]
//             (defaults: 1000000, 1, /tmp, 0=unlimited)
//
// Publishes N_MESSAGES to ipc:///tmp/zmq_bench.
// Start all consumer processes BEFORE this producer (give them time to subscribe).
//
// Output (in out_dir):
//   bench_zmq_N{n}_C{c}_tsc_ghz.txt
//   bench_zmq_N{n}_C{c}_s1.ns

#include <chrono>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <iostream>
#include <thread>

#include <x86intrin.h>

#include "Producer.hpp"

// ── Config ─────────────────────────────────────────────────────────────────────
static constexpr uint64_t WARMUP_SENTINEL = 0xFFFF'FFFF'FFFF'0000ULL;
static constexpr int      N_WARMUP        = 10'000;
static constexpr const char* ENDPOINT    = "ipc:///tmp/zmq_bench";

// ── Helpers ────────────────────────────────────────────────────────────────────
static inline uint64_t rdtsc_now() {
    _mm_lfence();
    return __rdtsc();
}

static uint64_t now_ns() {
    return static_cast<uint64_t>(
        std::chrono::steady_clock::now().time_since_epoch() /
        std::chrono::nanoseconds(1));
}

static double estimate_tsc_ghz() {
    const auto t0     = std::chrono::steady_clock::now();
    const uint64_t c0 = rdtsc_now();
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    const uint64_t c1 = rdtsc_now();
    const auto t1     = std::chrono::steady_clock::now();
    const double ns   = std::chrono::duration<double, std::nano>(t1 - t0).count();
    return static_cast<double>(c1 - c0) / ns;
}

// ── Main ───────────────────────────────────────────────────────────────────────
int main(int argc, char* argv[]) {
    const uint64_t    N_MESSAGES  = (argc > 1) ? std::stoull(argv[1]) : 1'000'000ULL;
    const int         n_consumers = (argc > 2) ? std::stoi(argv[2]) : 1;
    const std::string out_dir     = (argc > 3) ? argv[3] : "/tmp";
    const uint64_t    target_mps  = (argc > 4) ? std::stoull(argv[4]) : 0;
    const std::string n_tag = "_N" + std::to_string(N_MESSAGES)
                            + "_C" + std::to_string(n_consumers)
                            + "_MPS" + std::to_string(target_mps);
    try {
        const double tsc_ghz = estimate_tsc_ghz();
        std::ofstream(out_dir + "/bench_zmq" + n_tag + "_tsc_ghz.txt") << tsc_ghz << "\n";
        std::cout << "TSC freq: " << tsc_ghz << " GHz\n";

        Producer producer(ENDPOINT);

        // Give subscribers time to connect before any messages are sent.
        // ZeroMQ PUB/SUB is "fire and forget" — messages sent before a SUB
        // connects are silently dropped.
        std::cout << "Waiting 1s for consumers to subscribe...\n";
        std::this_thread::sleep_for(std::chrono::seconds(1));

        // ── Warmup ─────────────────────────────────────────────────────────────
        std::cout << "Warming up (" << N_WARMUP << " msgs)...\n";
        for (int w = 0; w < N_WARMUP; ++w) {
            Message msg{};

            // Use a special seq_id range so consumers can identify and skip warmup
            msg.seq_id   = WARMUP_SENTINEL + static_cast<uint64_t>(w);
            msg.send_tsc = rdtsc_now();
            std::memset(msg.payload.data(), w & 0xFF, msg.payload.size());
            producer.send(msg);
        }
        // Let consumers drain warmup before the real clock starts
        std::this_thread::sleep_for(std::chrono::milliseconds(500));

        // ── Benchmark ──────────────────────────────────────────────────────────

        // Record benchmark start time in wall-clock nanoseconds
        const uint64_t s1 = now_ns();
        std::ofstream(out_dir + "/bench_zmq" + n_tag + "_s1.ns") << s1 << "\n";

        // Compute target spacing between messages in TSC ticks
        const uint64_t tsc_per_msg = (target_mps > 0)
            ? static_cast<uint64_t>(tsc_ghz * 1e9 / static_cast<double>(target_mps))
            : 0;

        std::cout << "Sending " << N_MESSAGES << " messages";
        if (target_mps > 0)
            std::cout << " @ " << target_mps << " MPS";
        std::cout << "...\n";

        // next_tsc = scheduled send time for the next message
        uint64_t next_tsc = rdtsc_now();
        for (uint64_t i = 0; i < N_MESSAGES; ++i) {
            if (tsc_per_msg > 0) {
                while (rdtsc_now() < next_tsc) { _mm_pause(); }
                next_tsc += tsc_per_msg;
            }
            Message msg{};
            msg.seq_id = i;

            // Fill payload with a simple deterministic byte pattern
            std::memset(msg.payload.data(), static_cast<int>(i & 0xFF),
                        msg.payload.size());
                        
            msg.send_tsc = rdtsc_now();     // T1: right before zmq_send
            producer.send(msg);
        }

        std::cout << "Producer done. Sent " << N_MESSAGES << " messages.\n";
    } catch (const zmq::error_t& e) {
        std::cerr << "ZMQ error: " << e.what() << "\n";
        return 1;
    } catch (const std::exception& e) {
        std::cerr << "Error: " << e.what() << "\n";
        return 1;
    }
    return 0;
}
