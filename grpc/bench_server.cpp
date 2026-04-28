// grpc/bench_server.cpp
//
// gRPC benchmark server (= producer).
//
// Usage:
//   ./bench_server [n_consumers] [n_messages] [out_dir] [target_mps]
//                 (defaults: 1, 1000000, /tmp, 0=unlimited)
//
// The server waits for all n_consumers to call Subscribe() before beginning
// the real benchmark, acting as a synchronization barrier. All streams then
// receive the same N_MESSAGES in parallel, each time-stamped right before
// writer->Write() (T1).
//
// Output (in out_dir):
//   bench_grpc_N{n}_C{c}_tsc_ghz.txt
//   bench_grpc_N{n}_C{c}_s1.ns      (wall-clock ns before first real send)

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <fstream>
#include <iostream>
#include <memory>
#include <mutex>
#include <queue>
#include <string>
#include <thread>
#include <vector>

#include <x86intrin.h>

#include <grpcpp/grpcpp.h>
#include "bench.grpc.pb.h"
#include "bench.pb.h"

// ── Config ─────────────────────────────────────────────────────────────────────
static constexpr uint64_t WARMUP_SENTINEL = 0xFFFF'FFFF'FFFF'0000ULL;
static constexpr int      N_WARMUP        = 10'000;
static constexpr const char* ADDR        = "unix:///tmp/bench_grpc.sock";

static uint64_t      G_N_MESSAGES = 1'000'000ULL;
static uint64_t      G_TARGET_MPS = 0;
static std::string   G_OUT_DIR    = "/tmp";
static std::string   G_N_TAG;
static grpc::Server* G_SERVER     = nullptr;

// Backpressure: max messages buffered per consumer before producer blocks.
static constexpr size_t MAX_QUEUE_DEPTH = 8192;

// Tracks how many Subscribe handlers have fully finished.
// producer_thread waits on this before calling Shutdown().
static std::atomic<int>        g_n_done{0};
static std::mutex              g_done_mtx;
static std::condition_variable g_done_cv;

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

// ── Per-client queue ───────────────────────────────────────────────────────────
struct ClientSlot {
    std::mutex              mtx;
    std::condition_variable cv;
    std::queue<bench::BenchMsg> q;
    bool done = false;
};

// ── Global broadcast state ─────────────────────────────────────────────────────
static int                                         g_n_expected = 1;
static std::mutex                                  g_reg_mtx;
static std::vector<std::shared_ptr<ClientSlot>>    g_slots;
static std::atomic<int>                            g_n_connected{0};
static std::mutex                                  g_barrier_mtx;
static std::condition_variable                     g_barrier_cv;
static bool                                        g_s1_written = false;

// Push one message to every connected client's queue.
// Blocks (backpressure) if any consumer queue is full — prevents OOM for large N.
static void broadcast(const bench::BenchMsg& tmpl) {
    std::lock_guard<std::mutex> reg_lock(g_reg_mtx);
    for (auto& slot : g_slots) {
        bench::BenchMsg m = tmpl;
        m.set_send_tsc(rdtsc_now()); // T1: stamp per-stream just before queuing
        std::unique_lock<std::mutex> cq_lock(slot->mtx);
        // Wait until the consumer has drained enough to make room
        slot->cv.wait(cq_lock,
            [&] { return slot->q.size() < MAX_QUEUE_DEPTH || slot->done; });
        if (!slot->done) {
            slot->q.push(std::move(m));
            slot->cv.notify_one();
        }
    }
}

// ── Producer thread ────────────────────────────────────────────────────────────
static void producer_thread() {
    // Wait until all expected consumers are connected (barrier)
    {
        std::unique_lock<std::mutex> lock(g_barrier_mtx);
        const bool ok = g_barrier_cv.wait_for(
            lock, std::chrono::seconds(60),
            [] { return g_n_connected.load() >= g_n_expected; });
        if (!ok) {
            std::cerr << "Barrier timeout: only "
                      << g_n_connected.load() << " / " << g_n_expected
                      << " consumers connected.\n";
        }
    }
    std::cout << "All " << g_n_expected << " consumer(s) connected. Starting.\n";

    // Warmup
    for (int w = 0; w < N_WARMUP; ++w) {
        bench::BenchMsg msg;
        msg.set_seq_id(WARMUP_SENTINEL + static_cast<uint64_t>(w));
        msg.set_payload(std::string(48, static_cast<char>(w & 0xFF)));
        broadcast(msg);
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(500));

    // Record S1 (wall-clock) before first real broadcast
    const uint64_t s1 = now_ns();
    std::ofstream(G_OUT_DIR + "/bench_grpc" + G_N_TAG + "_s1.ns") << s1 << "\n";

    const double tsc_ghz_local = []{
        const auto t0 = std::chrono::steady_clock::now();
        const uint64_t c0 = rdtsc_now();
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        const uint64_t c1 = rdtsc_now();
        const double ns = std::chrono::duration<double,std::nano>(
            std::chrono::steady_clock::now() - t0).count();
        return static_cast<double>(c1 - c0) / ns;
    }();
    const uint64_t tsc_per_msg = (G_TARGET_MPS > 0)
        ? static_cast<uint64_t>(tsc_ghz_local * 1e9 / static_cast<double>(G_TARGET_MPS))
        : 0;

    std::cout << "Sending " << G_N_MESSAGES << " messages";
    if (G_TARGET_MPS > 0)
        std::cout << " @ " << G_TARGET_MPS << " MPS";
    std::cout << "...\n";

    uint64_t next_tsc = rdtsc_now();
    for (uint64_t i = 0; i < G_N_MESSAGES; ++i) {
        if (tsc_per_msg > 0) {
            while (rdtsc_now() < next_tsc) { _mm_pause(); }
            next_tsc += tsc_per_msg;
        }
        bench::BenchMsg msg;
        msg.set_seq_id(i);
        msg.set_payload(std::string(48, static_cast<char>(i & 0xFF)));
        // send_tsc is stamped inside broadcast() per-stream
        broadcast(msg);
    }

    // Signal all streams that we're done
    {
        std::lock_guard<std::mutex> lock(g_reg_mtx);
        for (auto& slot : g_slots) {
            std::lock_guard<std::mutex> cq_lock(slot->mtx);
            slot->done = true;
            slot->cv.notify_all();
        }
    }
    std::cout << "Producer done.\n";

    // Wait for all Subscribe handlers to exit, then shut down from this
    // non-gRPC thread.  Calling Shutdown() from inside an RPC handler
    // deadlocks because Shutdown() waits for all active RPCs to finish.
    {
        std::unique_lock<std::mutex> lock(g_done_mtx);
        g_done_cv.wait(lock,
            [] { return g_n_done.load() >= g_n_expected; });
    }
    if (G_SERVER) G_SERVER->Shutdown();
}

// ── gRPC service ───────────────────────────────────────────────────────────────
class BenchServiceImpl final : public bench::BenchService::Service {
public:
    grpc::Status Subscribe(grpc::ServerContext* ctx,
                           const bench::SubRequest* req,
                           grpc::ServerWriter<bench::BenchMsg>* writer) override {
        // Register this client
        auto slot = std::make_shared<ClientSlot>();
        {
            std::lock_guard<std::mutex> lock(g_reg_mtx);
            g_slots.push_back(slot);
        }
        const int prev = g_n_connected.fetch_add(1);
        std::cout << "Consumer " << req->consumer_id()
                  << " connected (" << prev + 1 << " / " << g_n_expected << ")\n";
        g_barrier_cv.notify_all();

        // Drain the queue and write to the stream
        while (!ctx->IsCancelled()) {
            bench::BenchMsg msg;
            {
                std::unique_lock<std::mutex> lock(slot->mtx);
                slot->cv.wait(lock,
                    [&] { return !slot->q.empty() || slot->done; });
                if (slot->q.empty()) break;     // done and queue drained
                msg = std::move(slot->q.front());
                slot->q.pop();
                slot->cv.notify_all();  // wake producer blocked on backpressure
            }
            if (!writer->Write(msg)) break;     // client disconnected
            if (msg.seq_id() == G_N_MESSAGES - 1) break;
        }

        // Notify producer_thread that this handler is done
        {
            std::lock_guard<std::mutex> lock(g_done_mtx);
            if (g_n_done.fetch_add(1) + 1 >= g_n_expected)
                g_done_cv.notify_one();
        }
        return grpc::Status::OK;
    }
};

// ── Main ───────────────────────────────────────────────────────────────────────
int main(int argc, char* argv[]) {
    g_n_expected = (argc > 1) ? std::stoi(argv[1]) : 1;
    G_N_MESSAGES = (argc > 2) ? std::stoull(argv[2]) : 1'000'000ULL;
    G_OUT_DIR    = (argc > 3) ? argv[3] : "/tmp";
    G_TARGET_MPS = (argc > 4) ? std::stoull(argv[4]) : 0;
    G_N_TAG      = "_N" + std::to_string(G_N_MESSAGES)
                 + "_C" + std::to_string(g_n_expected)
                 + "_MPS" + std::to_string(G_TARGET_MPS);

    const double tsc_ghz = estimate_tsc_ghz();
    std::ofstream(G_OUT_DIR + "/bench_grpc" + G_N_TAG + "_tsc_ghz.txt") << tsc_ghz << "\n";
    std::cout << "TSC freq: " << tsc_ghz << " GHz\n";

    // Remove stale socket file
    std::remove("/tmp/bench_grpc.sock");

    BenchServiceImpl service;
    grpc::ServerBuilder builder;
    builder.AddListeningPort(ADDR, grpc::InsecureServerCredentials());
    builder.RegisterService(&service);
    // Allow large messages (48-byte payload + overhead)
    builder.SetMaxSendMessageSize(4 * 1024 * 1024);
    builder.SetMaxReceiveMessageSize(4 * 1024 * 1024);

    auto server = builder.BuildAndStart();
    G_SERVER = server.get();
    std::cout << "gRPC server listening on " << ADDR << "\n";
    std::cout << "Waiting for " << g_n_expected << " consumer(s)...\n";

    std::thread prod(producer_thread);
    server->Wait();   // returns after G_SERVER->Shutdown() is called
    if (prod.joinable()) prod.join();

    return 0;
}
