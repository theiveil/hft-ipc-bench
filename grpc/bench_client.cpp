// grpc/bench_client.cpp
//
// gRPC benchmark client (= consumer).
//
// Usage:
//   ./bench_client [consumer_id] [n_consumers] [n_messages] [out_dir] [target_mps]
//                 (defaults: 0, 1, 1000000, /tmp, 0=unlimited)
//
// Start this BEFORE bench_server (so the server can detect all consumers).
//
// Output (in out_dir):
//   latency_grpc_N{n}_C{c}_c<id>.bin    — binary array of uint64 latency cycles
//   bench_grpc_N{n}_C{c}_c<id>.tp       — 3 lines: s2_ns, n_recv, n_expected

#include <chrono>
#include <cstdint>
#include <fstream>
#include <iostream>
#include <memory>
#include <string>
#include <vector>

#include <x86intrin.h>

#include <grpcpp/grpcpp.h>
#include "bench.grpc.pb.h"
#include "bench.pb.h"

// ── Config ─────────────────────────────────────────────────────────────────────
static constexpr uint64_t WARMUP_SENTINEL = 0xFFFF'FFFF'FFFF'0000ULL;
static constexpr const char* ADDR        = "unix:///tmp/bench_grpc.sock";

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

// ── Main ───────────────────────────────────────────────────────────────────────
int main(int argc, char* argv[]) {
    const int         consumer_id = (argc > 1) ? std::stoi(argv[1]) : 0;
    const int         n_consumers = (argc > 2) ? std::stoi(argv[2]) : 1;
    const uint64_t    N_MESSAGES  = (argc > 3) ? std::stoull(argv[3]) : 1'000'000ULL;
    const std::string out_dir     = (argc > 4) ? argv[4] : "/tmp";
    const uint64_t    target_mps  = (argc > 5) ? std::stoull(argv[5]) : 0;
    const std::string n_tag = "_N" + std::to_string(N_MESSAGES)
                            + "_C" + std::to_string(n_consumers)
                            + "_MPS" + std::to_string(target_mps);

    auto channel = grpc::CreateChannel(ADDR,
                                       grpc::InsecureChannelCredentials());
    auto stub    = bench::BenchService::NewStub(channel);

    grpc::ClientContext ctx;
    bench::SubRequest req;
    req.set_consumer_id(consumer_id);
    req.set_n_consumers(n_consumers);

    auto stream = stub->Subscribe(&ctx, req);
    std::cout << "Consumer " << consumer_id << " connected to " << ADDR << "\n";

    std::vector<uint64_t> latencies;
    latencies.reserve(N_MESSAGES);

    uint64_t s2_ns  = 0;
    uint64_t n_recv = 0;

    bench::BenchMsg msg;
    while (stream->Read(&msg)) {
        const uint64_t recv_tsc = rdtsc_now();  // T2: right after Read returns

        // Skip warmup messages
        if (msg.seq_id() >= WARMUP_SENTINEL) continue;

        latencies.push_back(recv_tsc - msg.send_tsc());
        ++n_recv;

        if (msg.seq_id() == N_MESSAGES - 1) {
            s2_ns = now_ns();
            break;
        }
    }

    const grpc::Status status = stream->Finish();
    if (!status.ok()) {
        std::cerr << "Stream finished with error: " << status.error_message() << "\n";
    }

    std::cout << "Consumer " << consumer_id
              << " received " << n_recv << " / " << N_MESSAGES << " messages.\n";

    // ── Write latency binary ───────────────────────────────────────────────────
    {
        const std::string fname =
            out_dir + "/latency_grpc" + n_tag + "_c" + std::to_string(consumer_id) + ".bin";
        std::ofstream f(fname, std::ios::binary);
        f.write(reinterpret_cast<const char*>(latencies.data()),
                static_cast<std::streamsize>(latencies.size() * sizeof(uint64_t)));
        std::cout << "Wrote " << fname << "\n";
    }

    // ── Write throughput stats ─────────────────────────────────────────────────
    {
        const std::string fname =
            out_dir + "/bench_grpc" + n_tag + "_c" + std::to_string(consumer_id) + ".tp";
        std::ofstream f(fname);
        f << s2_ns      << "\n"
          << n_recv     << "\n"
          << N_MESSAGES << "\n";
        std::cout << "Wrote " << fname << "\n";
    }

    return 0;
}
