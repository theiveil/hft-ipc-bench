#pragma once

#include <array>
#include <cstdint>
#include <type_traits>

struct alignas(64) Message {
    std::uint64_t seq_id;
    std::uint64_t send_tsc;
    std::array<std::uint8_t, 48> payload{};
};

static_assert(sizeof(Message) == 64, "Message must be exactly 64 bytes");
static_assert(std::is_trivially_copyable_v<Message>,
              "Message must be trivially copyable");
