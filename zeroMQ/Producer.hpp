#pragma once

#include <string>
#include <zmq.hpp>

#include "Message.hpp"

class Producer {
public:
    explicit Producer(const std::string& endpoint = "ipc:///tmp/zmq_benchmark");

    void send(const Message& msg);

private:
    zmq::context_t context_;
    zmq::socket_t socket_;
};