#include "memory/slab.hpp"

#include <algorithm>   // std::max
#include <cassert>
#include <cstdlib>     // std::malloc, std::free
#include <new>         // placement new (not used, but good practice header)

namespace memory {

namespace {

/// Round `value` up to the nearest multiple of `alignment`.
/// `alignment` must be a non-zero power of two.
constexpr std::size_t round_up(std::size_t value, std::size_t alignment) noexcept {
    // alignment is a power of two, so (alignment - 1) is a valid bit-mask.
    return (value + alignment - 1u) & ~(alignment - 1u);
}

} // namespace

// ---------------------------------------------------------------------------
// Constructor
// ---------------------------------------------------------------------------

Slab::Slab(std::size_t object_size, std::size_t alignment, std::size_t capacity)
    : capacity_(capacity)
{
    // Each free slot must be able to store a void* for the intrusive list,
    // AND the slot must satisfy the requested alignment.
    // slot_size_ = round_up(max(object_size, sizeof(void*)), alignment)
    const std::size_t min_size = std::max(object_size, sizeof(void*));
    slot_size_ = round_up(min_size, alignment);

    // Allocate the backing pool.
    pool_ = static_cast<std::byte*>(std::malloc(slot_size_ * capacity_));

    // Initialize the intrusive free-slot linked list.
    // Each slot's first sizeof(void*) bytes hold a pointer to the next free slot
    // (or nullptr for the last slot in the list).
    for (std::size_t i = 0; i < capacity_; ++i) {
        std::byte* slot      = pool_ + i * slot_size_;
        std::byte* next_slot = (i + 1 < capacity_) ? pool_ + (i + 1) * slot_size_ : nullptr;
        *reinterpret_cast<void**>(slot) = next_slot;
    }

    free_head_ = reinterpret_cast<void**>(pool_);
}

// ---------------------------------------------------------------------------
// Destructor
// ---------------------------------------------------------------------------

Slab::~Slab() {
    std::free(pool_);
    pool_      = nullptr;
    free_head_ = nullptr;
}

// ---------------------------------------------------------------------------
// allocate
// ---------------------------------------------------------------------------

void* Slab::allocate() noexcept {
    if (free_head_ == nullptr) {
        return nullptr;  // Slab is full (Req 5.3)
    }

    // Pop the head slot from the free list.
    void* slot     = free_head_;
    free_head_     = reinterpret_cast<void**>(*free_head_);  // advance to next free slot
    ++used_;
    return slot;  // O(1) (Req 5.2, 5.7)
}

// ---------------------------------------------------------------------------
// deallocate
// ---------------------------------------------------------------------------

void Slab::deallocate(void* ptr) noexcept {
    // Debug-mode range check (Req 5.5, 9.4).
    assert(ptr >= static_cast<void*>(pool_) &&
           ptr <  static_cast<void*>(pool_ + slot_size_ * capacity_) &&
           "Slab::deallocate: pointer is outside the slab's pool range");

    // Push ptr onto the front of the free list.
    *reinterpret_cast<void**>(ptr) = free_head_;
    free_head_ = reinterpret_cast<void**>(ptr);
    --used_;  // O(1) (Req 5.4)
}

// ---------------------------------------------------------------------------
// capacity / used
// ---------------------------------------------------------------------------

std::size_t Slab::capacity() const noexcept {
    return capacity_;  // object count, not bytes (Req 5.1)
}

std::size_t Slab::used() const noexcept {
    return used_;
}

} // namespace memory
