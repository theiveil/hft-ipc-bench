// shm/bench_producer.cpp
//
// SPMC lock-free shared memory producer benchmark.
//
// Usage:
//   ./bench_producer [n_consumers] [n_messages] [out_dir] [target_mps]
//                   (defaults: 1, 1000000, /tmp, 0=unlimited)
//
// Creates ONE POSIX SHM segment /bench_shm_N{n}_C{c} that holds the SPMC ring.
// All consumers attach to the same segment and read from their own cursor slot.
// The producer NEVER blocks: it overwrites the oldest slot if the ring is full.
// Start all bench_consumer processes BEFORE this producer.
//
// Output (in out_dir):
//   bench_shm_N{n}_C{c}_tsc_ghz.txt
//   bench_shm_N{n}_C{c}_s1.ns

#include <chrono>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <thread>

#include <fcntl.h>
#include <sched.h>
#include <sys/mman.h>
#include <unistd.h>
#include <x86intrin.h>

#include "ring_buffer.hpp"

// ── Config ─────────────────────────────────────────────────────────────────────
static constexpr uint64_t WARMUP_SENTINEL = 0xFFFF'FFFF'FFFF'0000ULL;
static constexpr uint32_t N_WARMUP        = RING_SIZE;

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
    const auto     t0 = std::chrono::steady_clock::now();
    const uint64_t c0 = rdtsc_now();
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    const uint64_t c1 = rdtsc_now();
    const auto     t1 = std::chrono::steady_clock::now();
    const double   ns = std::chrono::duration<double, std::nano>(t1 - t0).count();
    return static_cast<double>(c1 - c0) / ns;
}

// Create, size, and mmap the single SPMC SHM segment.
// Name is unique per (N, C) run so consumers never attach to a stale segment.
static RingBuffer* create_shm(const std::string& shm_name) {
    shm_unlink(shm_name.c_str());   // remove any stale segment
    const int fd = shm_open(shm_name.c_str(), O_CREAT | O_RDWR, 0666);
    if (fd < 0) throw std::runtime_error("shm_open failed");
    if (ftruncate(fd, sizeof(RingBuffer)) < 0)
        throw std::runtime_error("ftruncate failed");
    void* ptr = mmap(nullptr, sizeof(RingBuffer),
                     PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    close(fd);
    if (ptr == MAP_FAILED) throw std::runtime_error("mmap failed");
    std::memset(ptr, 0, sizeof(RingBuffer));   // zero-init (all seq = 0, head = 0)
    return static_cast<RingBuffer*>(ptr);
}

// ── CPU affinity ───────────────────────────────────────────────────────────────
static void pin_to_core(int core_id) {
    cpu_set_t mask;
    CPU_ZERO(&mask);
    CPU_SET(core_id, &mask);
    if (sched_setaffinity(0, sizeof(mask), &mask) != 0)
        perror("sched_setaffinity");
}

// ── Main ───────────────────────────────────────────────────────────────────────
int main(int argc, char* argv[]) {
    const int         n_consumers = (argc > 1) ? std::stoi(argv[1])   : 1;
    const uint64_t    N_MESSAGES  = (argc > 2) ? std::stoull(argv[2]) : 1'000'000ULL;
    const std::string out_dir     = (argc > 3) ? argv[3]              : "/tmp";
    const uint64_t    target_mps  = (argc > 4) ? std::stoull(argv[4]) : 0;
    const std::string n_tag = "_N" + std::to_string(N_MESSAGES)
                            + "_C" + std::to_string(n_consumers)
                            + "_MPS" + std::to_string(target_mps);

    pin_to_core(0);  // producer pinned to core 0

    if (n_consumers > MAX_CONSUMERS) {
        std::cerr << "n_consumers " << n_consumers
                  << " exceeds MAX_CONSUMERS " << MAX_CONSUMERS << "\n";
        return 1;
    }

    const std::string shm_name = "/bench_shm" + n_tag;
    RingBuffer* rb = create_shm(shm_name);
    std::cout << "Created " << shm_name
              << "  (" << sizeof(RingBuffer) / (1 << 20) << " MB)\n";

    std::cout << "Waiting for " << n_consumers << " consumer(s) to get ready...\n";
    while (rb->ready_count.load(std::memory_order_acquire) <
           static_cast<uint32_t>(n_consumers)) {
        _mm_pause();
    }

    const double tsc_ghz = estimate_tsc_ghz();
    std::ofstream(out_dir + "/bench_shm" + n_tag + "_tsc_ghz.txt") << tsc_ghz << "\n";
    std::cout << "TSC freq: " << tsc_ghz << " GHz\n";

    // ── Warmup ─────────────────────────────────────────────────────────────────
    for (uint32_t w = 0; w < N_WARMUP; ++w) {
        Message msg{};
        msg.seq_id   = WARMUP_SENTINEL;
        msg.send_tsc = rdtsc_now();
        std::memset(msg.payload.data(), w & 0xFF, msg.payload.size());
        rb_push(rb, msg);
    }
    const uint64_t start_tail = rb->head.load(std::memory_order_acquire);
    for (int cid = 0; cid < n_consumers; ++cid) {
        rb->tails[cid].tail.store(start_tail, std::memory_order_release);
    }
    rb->start_flag.store(true, std::memory_order_release);

    // ── Benchmark ──────────────────────────────────────────────────────────────
    const uint64_t s1 = now_ns();
    std::ofstream(out_dir + "/bench_shm" + n_tag + "_s1.ns") << s1 << "\n";

    const uint64_t tsc_per_msg = (target_mps > 0)
        ? static_cast<uint64_t>(tsc_ghz * 1e9 / static_cast<double>(target_mps))
        : 0;

    std::cout << "Sending " << N_MESSAGES << " messages to "
              << n_consumers << " consumer(s)";
    if (target_mps > 0)
        std::cout << " @ " << target_mps << " MPS";
    std::cout << "...\n";

    uint64_t next_tsc = rdtsc_now();
    for (uint64_t i = 0; i < N_MESSAGES; ++i) {
        if (tsc_per_msg > 0) {
            while (rdtsc_now() < next_tsc) { _mm_pause(); }
            next_tsc += tsc_per_msg;
        }
        Message msg{};
        msg.seq_id   = i;
        msg.send_tsc = rdtsc_now();   // T1: right before push
        std::memset(msg.payload.data(), static_cast<int>(i & 0xFF), msg.payload.size());
        rb_push(rb, msg);
    }

    std::cout << "Producer done. Sent " << N_MESSAGES << " messages.\n";
    munmap(rb, sizeof(RingBuffer));
    shm_unlink(shm_name.c_str());
    return 0;
}
