#pragma once

#include <string>
#include <string_view>

#include <hiredis/hiredis.h>

#include "Message.hpp"

class RedisProducer {
public:
    // PIPELINE_DEPTH: how many PUBLISH commands are queued before waiting for
    // replies. Raises throughput dramatically vs. synchronous redisCommand().
    // send_tsc is embedded in the message body, so latency accuracy is unaffected.
    static constexpr int PIPELINE_DEPTH = 64;

    RedisProducer(const std::string& socket_path = "/tmp/redis.sock",
                  const std::string& channel = "bench_channel");
    ~RedisProducer();

    RedisProducer(const RedisProducer&) = delete;
    RedisProducer& operator=(const RedisProducer&) = delete;

    void send(const Message& msg);
    void flush();   // drain remaining pipelined replies — call after send loop

private:
    redisContext* ctx_;
    std::string   channel_;
    int           pending_ = 0;   // number of commands awaiting reply
};
