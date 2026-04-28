#include "Producer.hpp"

#include <stdexcept>

Producer::Producer(const std::string& endpoint)
    : context_(1),
      socket_(context_, zmq::socket_type::pub) {
    socket_.set(zmq::sockopt::sndhwm, 100000); //Set high water mark for outbound messages
    socket_.bind(endpoint);
}

void Producer::send(const Message& msg) {
    // Create a ZeroMQ message buffer with the same size as Message
    zmq::message_t zmq_msg(sizeof(Message));

    // Copy the raw bytes of msg into the ZeroMQ message buffer
    std::memcpy(zmq_msg.data(), &msg, sizeof(Message));

    // Send the message through the PUB socket
    const auto result = socket_.send(zmq_msg, zmq::send_flags::none);

    // Check whether the full message was sent successfully
    if (!result || *result != sizeof(Message)) {
        throw std::runtime_error("Producer::send failed");
    }
}