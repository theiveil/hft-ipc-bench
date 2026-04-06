#include "RedisConsumer.hpp"

#include <cstring>
#include <stdexcept>
#include <string>
#include <sys/time.h>

RedisConsumer::RedisConsumer(const std::string& socket_path,
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

    // Send the SUBSCRIBE command to Redis.
    redisReply* reply = static_cast<redisReply*>(
        redisCommand(ctx_, "SUBSCRIBE %s", channel_.c_str()));

    if (reply == nullptr) {
        throw std::runtime_error(
            ctx_ ? std::string("SUBSCRIBE failed: ") + ctx_->errstr
                 : "SUBSCRIBE failed: null context");
    }

    freeReplyObject(reply);
}

RedisConsumer::~RedisConsumer() {
    if (ctx_ != nullptr) {
        redisFree(ctx_);
        ctx_ = nullptr;
    }
}


bool RedisConsumer::recv(Message& msg) {
    void* raw_reply = nullptr;

    if (redisGetReply(ctx_, &raw_reply) != REDIS_OK || raw_reply == nullptr) {
        return false;
    }

    redisReply* reply = static_cast<redisReply*>(raw_reply);

    // Pub/Sub message format:
    // ["message", <channel>, <payload>]
    bool ok = false;

    if (reply->type == REDIS_REPLY_ARRAY &&
        reply->elements == 3 &&
        reply->element[0] != nullptr &&
        reply->element[1] != nullptr &&
        reply->element[2] != nullptr &&
        reply->element[0]->type == REDIS_REPLY_STRING &&
        reply->element[1]->type == REDIS_REPLY_STRING &&
        reply->element[2]->type == REDIS_REPLY_STRING) {

        const redisReply* kind = reply->element[0];
        const redisReply* ch   = reply->element[1];
        const redisReply* data = reply->element[2];

        if (std::string_view(kind->str, kind->len) == "message" &&
            std::string_view(ch->str, ch->len) == channel_ &&
            data->len == sizeof(Message)) {
            std::memcpy(&msg, data->str, sizeof(Message));
            ok = true;
        }
    }

    freeReplyObject(reply);
    return ok;
}
