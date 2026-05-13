// uds/main_producer.cpp
//
// Unix domain socket SPMC producer benchmark.
//
// Usage:
//   ./producer [n_messages] [n_consumers] [out_dir] [target_mps]
//              (defaults: 1000000, 1, /tmp, 0=unlimited)
//
// Uses AF_UNIX + SOCK_STREAM. Stream mode gives the producer a dedicated FD per
// consumer via listen()/accept(), which makes multi-client registration explicit:
// every consumer connect()s to /tmp/bench_uds_N{n}_C{c}_MPS{mps}.sock and the
// producer stores the accepted FDs in client_fds. Because SOCK_STREAM has no
// message boundaries, every 64-byte Message is sent with send_all().
//
// Output (in out_dir):
//   bench_uds_N{n}_C{c}_MPS{mps}_tsc_ghz.txt
//   bench_uds_N{n}_C{c}_MPS{mps}_s1.ns

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

#ifndef MSG_NOSIGNAL
#define MSG_NOSIGNAL 0
#endif

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

static void send_all(int fd, const void* data, size_t len) {
    const char* p = static_cast<const char*>(data);
    size_t sent = 0;
    while (sent < len) {
        const ssize_t rc = send(fd, p + sent, len - sent, MSG_NOSIGNAL);
        if (rc > 0) {
            sent += static_cast<size_t>(rc);
            continue;
        }
        if (rc < 0 && errno == EINTR)
            continue;
        throw std::runtime_error(std::string("send failed: ") + std::strerror(errno));
    }
}

static int accept_one(int listen_fd) {
    while (true) {
        const int fd = accept(listen_fd, nullptr, nullptr);
        if (fd >= 0)
            return fd;
        if (errno == EINTR)
            continue;
        throw std::runtime_error(std::string("accept failed: ") + std::strerror(errno));
    }
}

static void broadcast(const std::vector<int>& client_fds, const Message& msg) {
    for (int fd : client_fds) {
        send_all(fd, &msg, sizeof(msg));
    }
}

int main(int argc, char* argv[]) {
    const uint64_t    N_MESSAGES  = (argc > 1) ? std::stoull(argv[1]) : 1'000'000ULL;
    const int         n_consumers = (argc > 2) ? std::stoi(argv[2])   : 1;
    const std::string out_dir     = (argc > 3) ? argv[3]              : "/tmp";
    const uint64_t    target_mps  = (argc > 4) ? std::stoull(argv[4]) : 0;
    const std::string n_tag = "_N" + std::to_string(N_MESSAGES)
                            + "_C" + std::to_string(n_consumers)
                            + "_MPS" + std::to_string(target_mps);

    if (n_consumers <= 0 || n_consumers > MAX_CONSUMERS) {
        std::cerr << "n_consumers " << n_consumers
                  << " must be in [1, " << MAX_CONSUMERS << "]\n";
        return 1;
    }

    const std::string server_path = uds_server_path(n_tag);
    int listen_fd = -1;
    std::vector<int> client_fds;

    try {
        listen_fd = socket(AF_UNIX, SOCK_STREAM, 0);
        if (listen_fd < 0)
            throw std::runtime_error(std::string("socket failed: ") + std::strerror(errno));

        const int buf_bytes = 256 * 1024;
        setsockopt(listen_fd, SOL_SOCKET, SO_SNDBUF, &buf_bytes, sizeof(buf_bytes));
        setsockopt(listen_fd, SOL_SOCKET, SO_RCVBUF, &buf_bytes, sizeof(buf_bytes));

        unlink(server_path.c_str());
        const sockaddr_un server_addr = make_sockaddr(server_path);
        if (bind(listen_fd, reinterpret_cast<const sockaddr*>(&server_addr),
                 sizeof(server_addr)) < 0) {
            throw std::runtime_error(std::string("bind failed: ") + std::strerror(errno));
        }
        if (listen(listen_fd, n_consumers) < 0) {
            throw std::runtime_error(std::string("listen failed: ") + std::strerror(errno));
        }

        std::cout << "UDS producer listening on " << server_path
                  << ", accepting " << n_consumers << " consumer(s)...\n";
        client_fds.reserve(static_cast<size_t>(n_consumers));
        for (int i = 0; i < n_consumers; ++i) {
            const int client_fd = accept_one(listen_fd);
            setsockopt(client_fd, SOL_SOCKET, SO_SNDBUF, &buf_bytes, sizeof(buf_bytes));
            client_fds.push_back(client_fd);
            std::cout << "Accepted consumer " << i << "\n";
        }

        const double tsc_ghz = estimate_tsc_ghz();
        std::ofstream(out_dir + "/bench_uds" + n_tag + "_tsc_ghz.txt") << tsc_ghz << "\n";
        std::cout << "TSC freq: " << tsc_ghz << " GHz\n";

        std::cout << "Warming up (" << UDS_N_WARMUP << " msgs)...\n";
        for (int w = 0; w < UDS_N_WARMUP; ++w) {
            Message msg{};
            msg.seq_id   = UDS_WARMUP_SENTINEL;
            msg.send_tsc = rdtsc_now();
            std::memset(msg.payload.data(), w & 0xFF, msg.payload.size());
            broadcast(client_fds, msg);
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(500));

        const uint64_t s1 = now_ns();
        std::ofstream(out_dir + "/bench_uds" + n_tag + "_s1.ns") << s1 << "\n";

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
            msg.send_tsc = rdtsc_now();
            std::memset(msg.payload.data(), static_cast<int>(i & 0xFF), msg.payload.size());
            broadcast(client_fds, msg);
        }

        std::cout << "Producer done. Sent " << N_MESSAGES << " messages.\n";
        for (int fd : client_fds)
            close(fd);
        close(listen_fd);
        unlink(server_path.c_str());
        return 0;
    } catch (const std::exception& e) {
        for (int fd : client_fds)
            close(fd);
        if (listen_fd >= 0)
            close(listen_fd);
        unlink(server_path.c_str());
        std::cerr << "UDS producer error: " << e.what() << "\n";
        return 1;
    }
}
