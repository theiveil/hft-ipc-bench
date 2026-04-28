// zeroMQ/main_consumer.cpp
//
// ZeroMQ PUB/SUB consumer benchmark (ipc:// transport).
//
// Usage:
//   ./consumer [consumer_id] [n_messages] [n_consumers] [out_dir] [target_mps]
//             (defaults: 0, 1000000, 1, /tmp, 0=unlimited)
//
// Start this BEFORE the producer. Subscribes to ipc:///tmp/zmq_bench and
// records per-message one-way latency (T2 - T1 in TSC cycles).
//
// Output (in out_dir):
//   latency_zmq_N{n}_C{c}_c<id>.bin    — binary array of uint64 latency cycles
//   bench_zmq_N{n}_C{c}_c<id>.tp       — 3 lines: s2_ns, n_recv, n_expected

#include <chrono>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

#include <x86intrin.h>

#include "Consumer.hpp"

// ── Config ─────────────────────────────────────────────────────────────────────
static constexpr uint64_t WARMUP_SENTINEL = 0xFFFF'FFFF'FFFF'0000ULL;
static constexpr const char* ENDPOINT    = "ipc:///tmp/zmq_bench";

// ── Helpers ────────────────────────────────────────────────────────────────────
// static inline uint64_t rdtsc_now() {
//     _mm_lfence();
//     return __rdtsc();
// }

static uint64_t now_ns() {
    return static_cast<uint64_t>(
        std::chrono::steady_clock::now().time_since_epoch() /
        std::chrono::nanoseconds(1));
}

// ── Main ───────────────────────────────────────────────────────────────────────
int main(int argc, char* argv[]) {
    const int         consumer_id = (argc > 1) ? std::stoi(argv[1]) : 0;
    const uint64_t    N_MESSAGES  = (argc > 2) ? std::stoull(argv[2]) : 1'000'000ULL;
    const int         n_consumers = (argc > 3) ? std::stoi(argv[3]) : 1;
    const std::string out_dir     = (argc > 4) ? argv[4] : "/tmp";
    const uint64_t    target_mps  = (argc > 5) ? std::stoull(argv[5]) : 0;
    const std::string n_tag = "_N" + std::to_string(N_MESSAGES)
                            + "_C" + std::to_string(n_consumers)
                            + "_MPS" + std::to_string(target_mps);

    try {
        Consumer consumer(ENDPOINT);
        std::cout << "Consumer " << consumer_id
                  << " subscribed to " << ENDPOINT << ", waiting...\n";

        std::vector<uint64_t> latencies;
        latencies.reserve(N_MESSAGES);

        uint64_t s2_ns  = 0;
        uint64_t n_recv = 0;

        // Stall detection: if no message arrives for STALL_SEC seconds after
        // the producer has presumably finished, give up gracefully.
        static constexpr int STALL_SEC = 5;
        auto last_msg_time = std::chrono::steady_clock::now();

        while (n_recv < N_MESSAGES) {
            Message msg{};
            uint64_t recv_tsc = 0;
            if (!consumer.recv(msg, recv_tsc)) {
                // recv() returned false: either a 5-s ZMQ timeout fired (rcvtimeo)
                // or an EAGAIN. Check total stall against wall clock.
                const auto idle = std::chrono::steady_clock::now() - last_msg_time;
                if (idle > std::chrono::seconds(STALL_SEC)) {
                    std::cerr << "Consumer " << consumer_id
                              << " stalled for " << STALL_SEC
                              << "s — producer likely done. Exiting early.\n";
                    if (s2_ns == 0) s2_ns = now_ns();
                    break;
                }
                continue;
            }

            last_msg_time = std::chrono::steady_clock::now();
            // const uint64_t recv_tsc = rdtsc_now();  //AI Solution

            // Skip warmup messages
            if (msg.seq_id >= WARMUP_SENTINEL) continue;

            latencies.push_back(recv_tsc - msg.send_tsc);
            ++n_recv;

            // Record wall-clock when we hit the expected last message OR when
            // we've received enough messages (guards against the last seq_id
            // being dropped by ZMQ HWM under heavy multi-consumer load).
            if (msg.seq_id == N_MESSAGES - 1 || n_recv >= N_MESSAGES) {
                s2_ns = now_ns();
                break;
            }
        }

        std::cout << "Consumer " << consumer_id
                  << " received " << n_recv << " / " << N_MESSAGES << " messages.\n";
        if (n_recv < N_MESSAGES)
            std::cerr << "  [warn] " << (N_MESSAGES - n_recv)
                      << " messages lost (ZMQ HWM drop under load)\n";

        // ── Write latency binary ───────────────────────────────────────────────
        {
            const std::string fname =
                out_dir + "/latency_zmq" + n_tag + "_c" + std::to_string(consumer_id) + ".bin";
            std::ofstream f(fname, std::ios::binary);
            f.write(reinterpret_cast<const char*>(latencies.data()),
                    static_cast<std::streamsize>(latencies.size() * sizeof(uint64_t)));
            std::cout << "Wrote " << fname << "\n";
        }

        // ── Write throughput stats ─────────────────────────────────────────────
        {
            const std::string fname =
                out_dir + "/bench_zmq" + n_tag + "_c" + std::to_string(consumer_id) + ".tp";
            std::ofstream f(fname);
            f << s2_ns      << "\n"
              << n_recv     << "\n"
              << N_MESSAGES << "\n";
            std::cout << "Wrote " << fname << "\n";
        }

    } catch (const zmq::error_t& e) {
        std::cerr << "ZMQ error: " << e.what() << "\n";
        return 1;
    } catch (const std::exception& e) {
        std::cerr << "Error: " << e.what() << "\n";
        return 1;
    }
    return 0;
}
