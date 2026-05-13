// uds/main_consumer.cpp
//
// Unix domain socket SPMC consumer benchmark.
//
// Usage:
//   ./consumer [consumer_id] [n_messages] [n_consumers] [out_dir] [target_mps]
//              (defaults: 0, 1000000, 1, /tmp, 0=unlimited)
//
// Each consumer connect()s to /tmp/bench_uds_N{n}_C{c}_MPS{mps}.sock. The
// producer accepts one dedicated SOCK_STREAM FD per consumer and sends the same
// 64-byte Message to each FD. Because streams do not preserve message boundaries,
// this consumer uses recv_exact() to reconstruct each fixed-size Message.
//
// Output (in out_dir):
//   latency_uds_N{n}_C{c}_MPS{mps}_c<id>.bin
//   bench_uds_N{n}_C{c}_MPS{mps}_c<id>.tp

#include <cerrno>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#include <sys/socket.h>
#include <unistd.h>
#include <x86intrin.h>

#include "common.hpp"

static inline uint64_t rdtsc_now() {
    _mm_lfence();
    return __rdtsc();
}

static uint64_t now_ns() {
    return static_cast<uint64_t>(
        std::chrono::steady_clock::now().time_since_epoch() /
        std::chrono::nanoseconds(1));
}

static void connect_with_retry(int fd, const sockaddr_un& server_addr) {
    while (true) {
        if (connect(fd, reinterpret_cast<const sockaddr*>(&server_addr),
                    sizeof(server_addr)) == 0) {
            return;
        }
        if (errno == EINTR)
            continue;
        if (errno == ENOENT || errno == ECONNREFUSED) {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
            continue;
        }
        throw std::runtime_error(std::string("connect failed: ") + std::strerror(errno));
    }
}

static bool recv_exact(int fd, void* data, size_t len) {
    char* p = static_cast<char*>(data);
    size_t got = 0;
    while (got < len) {
        const ssize_t rc = recv(fd, p + got, len - got, 0);
        if (rc > 0) {
            got += static_cast<size_t>(rc);
            continue;
        }
        if (rc == 0)
            return false;
        if (errno == EINTR)
            continue;
        throw std::runtime_error(std::string("recv failed: ") + std::strerror(errno));
    }
    return true;
}

int main(int argc, char* argv[]) {
    const int         consumer_id = (argc > 1) ? std::stoi(argv[1])   : 0;
    const uint64_t    N_MESSAGES  = (argc > 2) ? std::stoull(argv[2]) : 1'000'000ULL;
    const int         n_consumers = (argc > 3) ? std::stoi(argv[3])   : 1;
    const std::string out_dir     = (argc > 4) ? argv[4]              : "/tmp";
    const uint64_t    target_mps  = (argc > 5) ? std::stoull(argv[5]) : 0;
    const std::string n_tag = "_N" + std::to_string(N_MESSAGES)
                            + "_C" + std::to_string(n_consumers)
                            + "_MPS" + std::to_string(target_mps);

    if (n_consumers <= 0 || n_consumers > MAX_CONSUMERS) {
        std::cerr << "n_consumers " << n_consumers
                  << " must be in [1, " << MAX_CONSUMERS << "]\n";
        return 1;
    }
    if (consumer_id < 0 || consumer_id >= n_consumers) {
        std::cerr << "consumer_id " << consumer_id
                  << " must be in [0, " << (n_consumers - 1) << "]\n";
        return 1;
    }

    int fd = -1;
    std::vector<uint64_t> latencies;
    latencies.reserve(N_MESSAGES);
    uint64_t n_recv = 0;
    uint64_t s2_ns  = 0;

    try {
        fd = socket(AF_UNIX, SOCK_STREAM, 0);
        if (fd < 0)
            throw std::runtime_error(std::string("socket failed: ") + std::strerror(errno));

        const int buf_bytes = 256 * 1024;
        setsockopt(fd, SOL_SOCKET, SO_RCVBUF, &buf_bytes, sizeof(buf_bytes));

        const sockaddr_un server_addr = make_sockaddr(uds_server_path(n_tag));
        connect_with_retry(fd, server_addr);
        std::cout << "Consumer " << consumer_id
                  << " connected to " << uds_server_path(n_tag) << ", receiving...\n";

        while (n_recv < N_MESSAGES) {
            Message msg{};
            if (!recv_exact(fd, &msg, sizeof(msg))) {
                std::cerr << "Consumer " << consumer_id
                          << " saw producer close after " << n_recv
                          << " / " << N_MESSAGES << " messages.\n";
                if (s2_ns == 0)
                    s2_ns = now_ns();
                break;
            }

            const uint64_t recv_tsc = rdtsc_now();
            if (msg.seq_id >= UDS_WARMUP_SENTINEL)
                continue;

            latencies.push_back(recv_tsc - msg.send_tsc);
            ++n_recv;

            if (msg.seq_id == N_MESSAGES - 1 || n_recv >= N_MESSAGES) {
                s2_ns = now_ns();
                break;
            }
        }

        close(fd);
        fd = -1;

        std::cout << "Consumer " << consumer_id
                  << " received " << n_recv << " / " << N_MESSAGES << " messages.\n";
    } catch (const std::exception& e) {
        if (fd >= 0)
            close(fd);
        std::cerr << "UDS consumer " << consumer_id << " error: " << e.what() << "\n";
    }

    {
        const std::string fname =
            out_dir + "/latency_uds" + n_tag + "_c" + std::to_string(consumer_id) + ".bin";
        std::ofstream f(fname, std::ios::binary);
        f.write(reinterpret_cast<const char*>(latencies.data()),
                static_cast<std::streamsize>(latencies.size() * sizeof(uint64_t)));
        std::cout << "Wrote " << fname << "\n";
    }

    {
        const std::string fname =
            out_dir + "/bench_uds" + n_tag + "_c" + std::to_string(consumer_id) + ".tp";
        std::ofstream f(fname);
        f << s2_ns      << "\n"
          << n_recv     << "\n"
          << N_MESSAGES << "\n";
        std::cout << "Wrote " << fname << "\n";
    }

    return 0;
}
