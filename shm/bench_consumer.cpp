// shm/bench_consumer.cpp
//
// SPMC lock-free shared memory consumer benchmark.
//
// Usage:
//   ./bench_consumer [consumer_id] [n_messages] [n_consumers] [out_dir] [target_mps]
//                   (defaults: 0, 1000000, 1, /tmp, 0=unlimited)
//
// Opens the single /bench_shm segment created by bench_producer.
// Reads from its own cursor slot (tails[consumer_id]) via SeqLock-validated pops.
//
// Torn reads and lapped frames are detected and skipped automatically.
// The consumer reports how many messages it actually received vs. expected.
//
// Start this BEFORE bench_producer.
//
// Output (in out_dir):
//   latency_shm_N{n}_C{c}_c<id>.bin    — binary uint64 array of TSC-cycle latencies
//   bench_shm_N{n}_C{c}_c<id>.tp       — 3 lines: s2_ns, n_recv, n_expected

#include <chrono>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

#include <fcntl.h>
#include <sched.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#include <x86intrin.h>

#include "ring_buffer.hpp"

// ── Config ─────────────────────────────────────────────────────────────────────
static constexpr uint64_t WARMUP_SENTINEL = 0xFFFF'FFFF'FFFF'0000ULL;
static volatile uint64_t pre_touch_sink = 0;

// ── Helpers ────────────────────────────────────────────────────────────────────
static inline uint64_t rdtsc_now() {
    _mm_lfence();
    return __rdtsc();
}

static void pre_touch_ring(RingBuffer* rb) {
    uint64_t acc = 0;
    for (uint32_t i = 0; i < RING_SIZE; ++i) {
        acc ^= rb->slots[i].seq.load(std::memory_order_relaxed);
    }
    pre_touch_sink = acc;
}

static uint64_t now_ns() {
    return static_cast<uint64_t>(
        std::chrono::steady_clock::now().time_since_epoch() /
        std::chrono::nanoseconds(1));
}

// Open and mmap the run-specific SHM segment. Retries until the producer creates
// it AND ftruncate has sized it to at least sizeof(RingBuffer).
static RingBuffer* open_shm(const std::string& shm_name) {
    for (int retry = 0; retry < 200; ++retry) {
        int fd = shm_open(shm_name.c_str(), O_RDWR, 0666);
        if (fd < 0) { usleep(100'000); continue; }

        struct stat st{};
        if (fstat(fd, &st) == 0 && static_cast<size_t>(st.st_size) >= sizeof(RingBuffer)) {
            void* ptr = mmap(nullptr, sizeof(RingBuffer),
                             PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
            close(fd);
            if (ptr != MAP_FAILED) return static_cast<RingBuffer*>(ptr);
        }
        close(fd);
        usleep(100'000);    // 100 ms between retries
    }
    throw std::runtime_error("shm_open timed out: " + shm_name);
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
    const int         consumer_id = (argc > 1) ? std::stoi(argv[1])   : 0;
    const uint64_t    N_MESSAGES  = (argc > 2) ? std::stoull(argv[2]) : 1'000'000ULL;
    const int         n_consumers = (argc > 3) ? std::stoi(argv[3])   : 1;
    const std::string out_dir     = (argc > 4) ? argv[4]              : "/tmp";
    const uint64_t    target_mps  = (argc > 5) ? std::stoull(argv[5]) : 0;
    const std::string n_tag = "_N" + std::to_string(N_MESSAGES)
                            + "_C" + std::to_string(n_consumers)
                            + "_MPS" + std::to_string(target_mps);

    if (consumer_id >= MAX_CONSUMERS) {
        std::cerr << "consumer_id " << consumer_id
                  << " exceeds MAX_CONSUMERS " << MAX_CONSUMERS << "\n";
        return 1;
    }

    // Declare all result variables before the try-block so they remain
    // accessible when writing output files even after an exception.
    std::vector<uint64_t> latencies;
    latencies.reserve(N_MESSAGES);
    uint64_t n_recv        = 0;
    uint64_t n_dropped     = 0;
    uint64_t s2_ns         = 0;

    try {
        const std::string shm_name = "/bench_shm" + n_tag;
        RingBuffer* rb = open_shm(shm_name);  // throws if SHM never appears
        std::cout << "Consumer " << consumer_id
                  << " attached to " << shm_name
                  << " (cursor slot " << consumer_id << "), spin-polling...\n";

        pin_to_core(consumer_id + 1);  // consumer 0→core1, 1→core2, 2→core3, 3→core4
        pre_touch_ring(rb);
        rb->ready_count.fetch_add(1, std::memory_order_acq_rel);
        while (!rb->start_flag.load(std::memory_order_acquire)) {
            _mm_pause();
        }

        uint64_t last_seq_seen = UINT64_MAX;

        // Stall detection: break if idle for >3 s after the first message,
        // or >45 s from process start (covers slow open_shm + long runs).
        auto     last_any_time = std::chrono::steady_clock::now();
        auto     proc_start    = last_any_time;
        bool     any_received  = false;

        while (true) {
            MsgMeta meta;
            if (!rb_pop_meta(rb, consumer_id, meta)) {
                const auto   now      = std::chrono::steady_clock::now();
                const double idle_any = std::chrono::duration<double>(now - last_any_time).count();
                const double total    = std::chrono::duration<double>(now - proc_start).count();
                if ((any_received && idle_any > 3.0) || total > 45.0) {
                    std::cerr << "Consumer " << consumer_id
                              << " stall (idle=" << idle_any
                              << "s total=" << total
                              << "s after seq " << last_seq_seen << ") — breaking.\n";
                    break;
                }
                continue;
            }
            last_any_time = std::chrono::steady_clock::now();
            any_received  = true;

            const uint64_t recv_tsc = rdtsc_now();

            // Skip warmup messages
            if (meta.seq_id >= WARMUP_SENTINEL)
                continue;

            // Detect dropped frames (gaps in seq_id)
            if (last_seq_seen != UINT64_MAX && meta.seq_id > last_seq_seen + 1)
                n_dropped += meta.seq_id - last_seq_seen - 1;
            last_seq_seen = meta.seq_id;

            latencies.push_back(recv_tsc - meta.send_tsc);
            ++n_recv;

            // Termination: received the last message, or counted enough
            if (meta.seq_id == N_MESSAGES - 1 || n_recv >= N_MESSAGES) {
                s2_ns = now_ns();
                break;
            }
        }

        munmap(rb, sizeof(RingBuffer));

        std::cout << "Consumer " << consumer_id
                  << ": received " << n_recv << " / " << N_MESSAGES
                  << "  dropped " << n_dropped << "\n";

    } catch (const std::exception& e) {
        std::cerr << "Consumer " << consumer_id << " fatal: " << e.what() << "\n";
        // Fall through — still write output files so wait_consumers doesn't hang.
    }

    // ── Write latency binary ───────────────────────────────────────────────────
    {
        const std::string fname =
            out_dir + "/latency_shm" + n_tag + "_c" + std::to_string(consumer_id) + ".bin";
        std::ofstream f(fname, std::ios::binary);
        f.write(reinterpret_cast<const char*>(latencies.data()),
                static_cast<std::streamsize>(latencies.size() * sizeof(uint64_t)));
        std::cout << "Wrote " << fname << "\n";
    }

    // ── Write throughput stats ─────────────────────────────────────────────────
    {
        const std::string fname =
            out_dir + "/bench_shm" + n_tag + "_c" + std::to_string(consumer_id) + ".tp";
        std::ofstream f(fname);
        f << s2_ns      << "\n"
          << n_recv     << "\n"
          << N_MESSAGES << "\n";
        std::cout << "Wrote " << fname << "\n";
    }

    return 0;
}
