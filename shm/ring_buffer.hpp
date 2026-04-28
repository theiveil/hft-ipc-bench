#pragma once

#include <atomic>
#include <array>
#include <cstdint>
#include <cstring>

// ── Message layout (64 bytes, one cache line) ──────────────────────────────────
struct alignas(64) Message {
    std::uint64_t seq_id   = 0;
    std::uint64_t send_tsc = 0;
    std::array<std::uint8_t, 48> payload{};
};
static_assert(sizeof(Message) == 64);

struct MsgMeta {
    uint64_t seq_id;
    uint64_t send_tsc;
};

// ── Ring constants ─────────────────────────────────────────────────────────────
static constexpr uint32_t RING_SIZE     = 1u << 17;  // 131 072 slots, power-of-2
static constexpr uint32_t RING_MASK     = RING_SIZE - 1;
static constexpr int      MAX_CONSUMERS = 4;

// ── Slot: Message + SeqLock counter (128 bytes = 2 cache lines) ───────────────
//
// seq encoding (monotonically increasing per slot, never wraps in practice):
//
//   seq < 2*my_tail+2   → slot not yet written for this tail position  (empty)
//   seq = 2*h+1 (odd)   → producer is currently writing generation h   (in-flight)
//   seq = 2*my_tail+2   → stable, this is exactly the message we want  (valid)
//   seq > 2*my_tail+2   → slot overwritten by a later generation       (lapped)
//
// Because expected = 2*my_tail+2 is always even, "in-flight" (odd) is caught by
// the seq < expected branch: 2*h+1 < 2*h+2 = expected, so we simply spin-wait.
//
struct alignas(128) Slot {
    std::atomic<uint64_t> seq{0};
    // 56 bytes implicit padding inserted by compiler to align Message to 64
    Message msg;
    // Total layout: seq(8) + pad(56) + msg(64) = 128 bytes exactly
};
static_assert(sizeof(Slot) == 128);

// ── Per-consumer cursor — each on its own cache line (no false sharing) ────────
struct alignas(64) ConsumerCursor {
    std::atomic<uint64_t> tail{0};
    char _pad[64 - sizeof(std::atomic<uint64_t>)];
};
static_assert(sizeof(ConsumerCursor) == 64);

// ── SPMC RingBuffer ────────────────────────────────────────────────────────────
//
// One producer, up to MAX_CONSUMERS independent consumers.
// The producer NEVER blocks: it overwrites the oldest slot if the ring is full.
// Consumers detect overwrites (laps) and torn reads via the SeqLock in each slot.
//
// SHM size ≈ 64 + 16×64 + 131 072×128 ≈ 16 MB
//
struct alignas(64) RingBuffer {
    alignas(64) std::atomic<uint64_t> head{0};
    char _pad_head[64 - sizeof(std::atomic<uint64_t>)];

    ConsumerCursor tails[MAX_CONSUMERS];  // one 64-byte cursor per consumer

    Slot slots[RING_SIZE];
};

// ── Producer push — O(1), never blocks ────────────────────────────────────────
//
// Protocol:
//   1. stamp seq = 2h+1  (odd  → in-flight, consumers see "not yet ready")
//   2. memcpy message data
//   3. stamp seq = 2h+2  (even → stable, generation h is visible)
//   4. publish head = h+1
//
// If the ring is full the oldest slot is silently overwritten.
//
inline void rb_push(RingBuffer* rb, const Message& msg) {
    const uint64_t h   = rb->head.load(std::memory_order_relaxed);
    const uint64_t idx = h & RING_MASK;
    Slot& slot = rb->slots[idx];

    slot.seq.store(2 * h + 1, std::memory_order_release);   // mark in-flight
    std::memcpy(&slot.msg, &msg, sizeof(Message));
    slot.seq.store(2 * h + 2, std::memory_order_release);   // mark stable
    rb->head.store(h + 1,     std::memory_order_release);   // publish
}

// ── Consumer pop — SeqLock validated, O(1) ─────────────────────────────────────
//
// Returns true  → out contains a valid message; tail has been advanced by 1.
// Returns false → one of:
//   (a) empty  — nothing written yet for this tail position; caller should retry.
//   (b) lapped — producer overran the consumer; tail is re-synced to the oldest
//                available slot automatically; caller should retry immediately.
//   (c) torn   — producer overwrote the slot mid-copy; tail re-synced; retry.
//
inline bool rb_pop(RingBuffer* rb, int cid, Message& out) {
    std::atomic<uint64_t>& tail_atom = rb->tails[cid].tail;
    uint64_t my_tail = tail_atom.load(std::memory_order_relaxed);

    const uint64_t idx      = my_tail & RING_MASK;
    Slot&          slot     = rb->slots[idx];
    const uint64_t expected = 2 * my_tail + 2;  // stable seq for this position

    // ── SeqLock: sample sequence before read ──────────────────────────────────
    const uint64_t seq_before = slot.seq.load(std::memory_order_acquire);

    if (seq_before < expected) {
        // Slot not yet written (or producer is currently writing it — odd seq
        // is also < expected because 2h+1 < 2h+2).  Buffer is empty; spin.
        return false;
    }

    if (seq_before > expected) {
        // Lapped: producer overwrote this slot with a later generation.
        // Jump tail to the oldest slot still guaranteed to be in the ring.
        const uint64_t h = rb->head.load(std::memory_order_acquire);
        my_tail = (h >= RING_SIZE) ? h - RING_SIZE : 0;
        tail_atom.store(my_tail, std::memory_order_relaxed);
        return false;
    }

    // seq_before == expected (even) → slot should be stable; copy it.
    std::memcpy(&out, &slot.msg, sizeof(Message));

    // ── SeqLock: sample sequence after read ───────────────────────────────────
    const uint64_t seq_after = slot.seq.load(std::memory_order_acquire);
    if (seq_after != seq_before) {
        // Producer overwrote this slot while we were copying — torn read.
        const uint64_t h = rb->head.load(std::memory_order_acquire);
        my_tail = (h >= RING_SIZE) ? h - RING_SIZE : 0;
        tail_atom.store(my_tail, std::memory_order_relaxed);
        return false;
    }

    // ── Success ───────────────────────────────────────────────────────────────
    tail_atom.store(my_tail + 1, std::memory_order_release);
    return true;
}

// new solution
inline bool rb_pop_meta(RingBuffer* rb, int cid, MsgMeta& out) {
    std::atomic<uint64_t>& tail_atom = rb->tails[cid].tail;
    uint64_t my_tail = tail_atom.load(std::memory_order_relaxed);

    const uint64_t idx      = my_tail & RING_MASK;
    Slot&          slot     = rb->slots[idx];
    const uint64_t expected = 2 * my_tail + 2;

    const uint64_t seq_before = slot.seq.load(std::memory_order_acquire);

    if (seq_before < expected) {
        return false;
    }

    if (seq_before > expected) {
        const uint64_t h = rb->head.load(std::memory_order_acquire);
        my_tail = (h >= RING_SIZE) ? h - RING_SIZE : 0;
        tail_atom.store(my_tail, std::memory_order_relaxed);
        return false;
    }

    out.seq_id   = slot.msg.seq_id; // No need to copy the whole message just to measure latency.
    out.send_tsc = slot.msg.send_tsc;

    const uint64_t seq_after = slot.seq.load(std::memory_order_acquire);
    if (seq_after != seq_before) {
        const uint64_t h = rb->head.load(std::memory_order_acquire);
        my_tail = (h >= RING_SIZE) ? h - RING_SIZE : 0;
        tail_atom.store(my_tail, std::memory_order_relaxed);
        return false;
    }

    tail_atom.store(my_tail + 1, std::memory_order_release);
    return true;
}