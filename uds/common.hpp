#pragma once

#include <cstdint>
#include <cstring>
#include <stdexcept>
#include <string>

#include <sys/un.h>

#include "../shm/ring_buffer.hpp"

static_assert(sizeof(Message) == 64, "UDS benchmark requires 64-byte Message");

static constexpr uint64_t UDS_WARMUP_SENTINEL = 0xFFFF'FFFF'FFFF'0000ULL;
static constexpr int      UDS_N_WARMUP        = 10'000;

inline std::string uds_server_path(const std::string& n_tag) {
    return "/tmp/bench_uds" + n_tag + ".sock";
}

inline sockaddr_un make_sockaddr(const std::string& path) {
    sockaddr_un addr{};
    addr.sun_family = AF_UNIX;
    if (path.size() >= sizeof(addr.sun_path)) {
        throw std::runtime_error("UDS path too long: " + path);
    }
    std::memcpy(addr.sun_path, path.c_str(), path.size() + 1);
    return addr;
}
