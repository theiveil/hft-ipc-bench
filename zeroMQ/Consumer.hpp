#pragma once

#include <string>
#include <zmq.hpp>
#include <cstdint>
#include <x86intrin.h>

#include "Message.hpp"

static inline uint64_t rdtsc_now() {
    _mm_lfence();
    return __rdtsc();
}

class Consumer {
public:
    explicit Consumer(const std::string& endpoint = "ipc:///tmp/zmq_benchmark");

    bool recv(Message& msg, uint64_t& recv_tsc);

private:
    zmq::context_t context_;
    zmq::socket_t socket_;
};