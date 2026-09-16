#include "memory/arena.hpp"

#include <cassert>
#include <cstdlib>
#include <memory>   // std::align
#include <utility>  // std::exchange

namespace memory {

// ---------------------------------------------------------------------------
// Helper
// ---------------------------------------------------------------------------

static constexpr bool is_power_of_two(std::size_t v) noexcept {
    // 0 is not a power of two
    return v != 0 && (v & (v - 1)) == 0;
}

// ---------------------------------------------------------------------------
// Constructor / Destructor
// ---------------------------------------------------------------------------

Arena::Arena(std::size_t capacity)
    : pool_(static_cast<std::byte*>(std::malloc(capacity)))
    , capacity_(capacity)
    , offset_(0)
{
    // If capacity == 0, std::malloc may return nullptr or a unique pointer.
    // Either way, no allocation can be satisfied, so this is valid.
    assert(capacity == 0 || pool_ != nullptr && "std::malloc failed during Arena construction");
}

Arena::~Arena() {
    std::free(pool_);
    pool_     = nullptr;
    capacity_ = 0;
    offset_   = 0;
}

// ---------------------------------------------------------------------------
// Move semantics
// ---------------------------------------------------------------------------

Arena::Arena(Arena&& other) noexcept
    : pool_(std::exchange(other.pool_, nullptr))
    , capacity_(std::exchange(other.capacity_, 0))
    , offset_(std::exchange(other.offset_, 0))
{}

Arena& Arena::operator=(Arena&& other) noexcept {
    if (this != &other) {
        // Release currently owned pool
        std::free(pool_);

        pool_     = std::exchange(other.pool_, nullptr);
        capacity_ = std::exchange(other.capacity_, 0);
        offset_   = std::exchange(other.offset_, 0);
    }
    return *this;
}

// ---------------------------------------------------------------------------
// allocate
// ---------------------------------------------------------------------------

void* Arena::allocate(std::size_t size, std::size_t alignment) noexcept {
    // Requirement 1.5 / 9.2: size == 0 → nullptr
    if (size == 0) {
        return nullptr;
    }

    // Requirement 2.2 / 8.3: alignment must be a non-zero power of two
    if (!is_power_of_two(alignment)) {
        return nullptr;
    }

    // Compute current position and available space for std::align
    void*       current = pool_ + offset_;
    std::size_t space   = capacity_ - offset_;

    // Requirement 2.4: use std::align to compute the aligned pointer without UB.
    // std::align adjusts `current` to the next aligned address and reduces `space`
    // by the padding consumed.
    void* aligned_ptr = std::align(alignment, size, current, space);

    // Requirement 1.4 / 9.1: OOM or overflow → nullptr
    if (aligned_ptr == nullptr) {
        return nullptr;
    }

    // Advance the bump pointer past this allocation.
    // (static_cast<std::byte*>(aligned_ptr) - pool_) gives the byte offset of the
    // aligned address; adding size gives the new offset.
    offset_ = static_cast<std::size_t>(static_cast<std::byte*>(aligned_ptr) - pool_) + size;

    return aligned_ptr;
}

// ---------------------------------------------------------------------------
// deallocate — no-op (requirement 1.6)
// ---------------------------------------------------------------------------

void Arena::deallocate(void* /*ptr*/) noexcept {
    // Intentionally empty: Arena supports only bulk reset, not individual frees.
}

// ---------------------------------------------------------------------------
// reset — requirement 1.7
// ---------------------------------------------------------------------------

void Arena::reset() noexcept {
    offset_ = 0;
}

// ---------------------------------------------------------------------------
// Capacity queries — requirement 1.8
// ---------------------------------------------------------------------------

std::size_t Arena::capacity() const noexcept {
    return capacity_;
}

std::size_t Arena::used() const noexcept {
    return offset_;
}

std::size_t Arena::remaining() const noexcept {
    // Invariant: used() + remaining() == capacity()  (requirement 1.8)
    return capacity_ - offset_;
}

} // namespace memory
