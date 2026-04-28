#pragma once

#include <string>

#include <hiredis/hiredis.h>

#include "Message.hpp"

class RedisConsumer {
public:
    RedisConsumer(const std::string& socket_path = "/tmp/redis.sock",
                  const std::string& channel = "bench_channel");
    ~RedisConsumer();

    RedisConsumer(const RedisConsumer&) = delete;
    RedisConsumer& operator=(const RedisConsumer&) = delete;

    // Blocking receive. Returns true on a valid message.
    bool recv(Message& msg);

private:
    redisContext* ctx_;
    std::string channel_;
};
