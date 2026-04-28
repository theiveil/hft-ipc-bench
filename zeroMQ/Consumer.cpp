#include "Consumer.hpp"

#include <cstring>
#include <cstdint>

Consumer::Consumer(const std::string& endpoint)
    : context_(1),
      socket_(context_, zmq::socket_type::sub) {
    socket_.set(zmq::sockopt::rcvhwm, 100000); // receive high water mark
    socket_.set(zmq::sockopt::rcvtimeo, 5000);  // 5s timeout — unblocks if producer exits
    socket_.set(zmq::sockopt::subscribe, ""); // Subscribe to all messages
    socket_.connect(endpoint);
}

// AI solution: 
// bool Consumer::recv(Message& msg) {
//     zmq::message_t zmq_msg;
//     try {
//         const auto result = socket_.recv(zmq_msg, zmq::recv_flags::none);
//         if (!result || *result != sizeof(Message))
//             return false;
//         std::memcpy(&msg, zmq_msg.data(), sizeof(Message));
//         return true;
//     } catch (const zmq::error_t& e) {
//         if (e.num() == EAGAIN) return false;   // timeout — caller decides what to do
//         throw;
//     }
// }

bool Consumer::recv(Message& msg, uint64_t& recv_tsc) {
    zmq::message_t zmq_msg;
    try {
        const auto result = socket_.recv(zmq_msg, zmq::recv_flags::none);
        if (!result || *result != sizeof(Message))
            return false;

        recv_tsc = rdtsc_now();  // T2: immediately after recv succeeds

        std::memcpy(&msg, zmq_msg.data(), sizeof(Message));
        return true;
    } catch (const zmq::error_t& e) {
        if (e.num() == EAGAIN) return false;
        throw;
    }
}