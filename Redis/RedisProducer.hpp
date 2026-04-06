#pragma once

#include <string>
#include <string_view>

#include <hiredis/hiredis.h>

#include "Message.hpp"

class RedisProducer {
public:
    RedisProducer(const std::string& socket_path = "/tmp/redis.sock",
                  const std::string& channel = "bench_channel");
    ~RedisProducer();

    RedisProducer(const RedisProducer&) = delete;
    RedisProducer& operator=(const RedisProducer&) = delete;

    void send(const Message& msg);

private:
    redisContext* ctx_;
    std::string channel_;
};
