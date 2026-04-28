#include "RedisProducer.hpp"

#include <stdexcept>
#include <cstring>
#include <sys/time.h>

RedisProducer::RedisProducer(const std::string& socket_path,
                             const std::string& channel)
    : ctx_(nullptr), channel_(channel) {
    timeval timeout{};
    timeout.tv_sec = 1;
    timeout.tv_usec = 0;

    ctx_ = redisConnectUnixWithTimeout(socket_path.c_str(), timeout);
    if (ctx_ == nullptr) {
        throw std::runtime_error("redisConnectUnixWithTimeout returned nullptr");
    }
    if (ctx_->err) {
        std::string err = ctx_->errstr;
        redisFree(ctx_);
        ctx_ = nullptr;
        throw std::runtime_error("Redis connect error: " + err);
    }

    // UDS 下不涉及 TCP keepalive / TCP_NODELAY。
    // 低延迟场景下尽量避免额外包装和多余命令。
}

RedisProducer::~RedisProducer() {
    if (ctx_ != nullptr) {
        redisFree(ctx_);
        ctx_ = nullptr;
    }
}

void RedisProducer::send(const Message& msg) {
    // Append command to the local send buffer — does NOT block waiting for reply.
    int rc = redisAppendCommand(ctx_,
                                "PUBLISH %s %b",
                                channel_.c_str(),
                                reinterpret_cast<const char*>(&msg),
                                sizeof(Message));
    if (rc != REDIS_OK)
        throw std::runtime_error(
            ctx_ ? std::string("redisAppendCommand failed: ") + ctx_->errstr
                 : "redisAppendCommand failed: null context");
    ++pending_;

    // Drain replies in batches to keep the pipeline from growing unbounded.
    if (pending_ >= PIPELINE_DEPTH)
        flush();
}

void RedisProducer::flush() {
    while (pending_ > 0) {
        void* raw = nullptr;
        if (redisGetReply(ctx_, &raw) != REDIS_OK)
            throw std::runtime_error(
                ctx_ ? std::string("redisGetReply failed: ") + ctx_->errstr
                     : "redisGetReply failed");
        if (raw) freeReplyObject(raw);
        --pending_;
    }
}
