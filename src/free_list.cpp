#include "memory/free_list.hpp"

#include <cassert>
#include <cstring>
#include <limits>

namespace memory {

// ---------------------------------------------------------------------------
// BlockHeader definition (private to this translation unit)
// ---------------------------------------------------------------------------
//
// Layout on 64-bit systems (total: 32 bytes):
//   size     : 8 bytes  — usable payload bytes (not including this header)
//   is_free  : 1 byte   — allocation status
//   _pad[7]  : 7 bytes  — explicit padding for determinism
//   prev     : 8 bytes  — previous block in pool order (nullptr == first)
//   next     : 8 bytes  — next block in pool order (nullptr == last)
//
struct FreeList::BlockHeader {
    std::size_t  size;        // bytes of usable payload (not including header)
    bool         is_free;
    std::uint8_t _pad[7];     // explicit padding
    BlockHeader* prev;        // previous block in pool order
    BlockHeader* next;        // next block in pool order
};

// ---------------------------------------------------------------------------
// Helper: pointer to the usable region that follows a header
// ---------------------------------------------------------------------------
static inline void* user_ptr(FreeList::BlockHeader* h) noexcept {
    return reinterpret_cast<std::byte*>(h) + sizeof(FreeList::BlockHeader);
}

// ---------------------------------------------------------------------------
// Helper: header of the block whose user region starts at p
// ---------------------------------------------------------------------------
static inline FreeList::BlockHeader* header_of(void* p) noexcept {
    return reinterpret_cast<FreeList::BlockHeader*>(
        static_cast<std::byte*>(p) - sizeof(FreeList::BlockHeader));
}

// ---------------------------------------------------------------------------
// Helper: align a pointer upward to the given (power-of-two) alignment
// ---------------------------------------------------------------------------
static inline std::byte* align_up(std::byte* ptr, std::size_t alignment) noexcept {
    const std::size_t addr = reinterpret_cast<std::size_t>(ptr);
    const std::size_t mask = alignment - 1u;
    return reinterpret_cast<std::byte*>((addr + mask) & ~mask);
}

// ---------------------------------------------------------------------------
// FreeList::is_power_of_two
// ---------------------------------------------------------------------------
bool FreeList::is_power_of_two(std::size_t v) noexcept {
    return v != 0 && (v & (v - 1u)) == 0;
}

// ---------------------------------------------------------------------------
// Constructor
// ---------------------------------------------------------------------------
FreeList::FreeList(std::size_t capacity)
    : pool_(nullptr), capacity_(0), first_block_(nullptr)
{
    if (capacity < sizeof(BlockHeader)) {
        // Pool too small to hold even one header — leave in a null/empty state.
        return;
    }

    pool_ = static_cast<std::byte*>(std::malloc(capacity));
    if (!pool_) {
        return;  // malloc failed; object remains in null state
    }

    capacity_ = capacity;

    // Initialise the single free block that spans the entire pool.
    auto* h      = reinterpret_cast<BlockHeader*>(pool_);
    h->size      = capacity - sizeof(BlockHeader);
    h->is_free   = true;
    std::memset(h->_pad, 0, sizeof(h->_pad));
    h->prev      = nullptr;
    h->next      = nullptr;

    first_block_ = h;
}

// ---------------------------------------------------------------------------
// Destructor
// ---------------------------------------------------------------------------
FreeList::~FreeList() {
    std::free(pool_);
    pool_        = nullptr;
    capacity_    = 0;
    first_block_ = nullptr;
}

// ---------------------------------------------------------------------------
// Move constructor
// ---------------------------------------------------------------------------
FreeList::FreeList(FreeList&& other) noexcept
    : pool_(other.pool_),
      capacity_(other.capacity_),
      first_block_(other.first_block_)
{
    other.pool_        = nullptr;
    other.capacity_    = 0;
    other.first_block_ = nullptr;
}

// ---------------------------------------------------------------------------
// Move assignment
// ---------------------------------------------------------------------------
FreeList& FreeList::operator=(FreeList&& other) noexcept {
    if (this != &other) {
        std::free(pool_);

        pool_        = other.pool_;
        capacity_    = other.capacity_;
        first_block_ = other.first_block_;

        other.pool_        = nullptr;
        other.capacity_    = 0;
        other.first_block_ = nullptr;
    }
    return *this;
}

// ---------------------------------------------------------------------------
// find_first_fit
//
// Linear scan of all blocks in pool order; returns the first that is both
// free and has a payload of at least `needed` bytes.
// ---------------------------------------------------------------------------
FreeList::BlockHeader* FreeList::find_first_fit(std::size_t needed) noexcept {
    for (BlockHeader* h = first_block_; h != nullptr; h = h->next) {
        if (h->is_free && h->size >= needed) {
            return h;
        }
    }
    return nullptr;
}

// ---------------------------------------------------------------------------
// split
//
// Carve `size` bytes from the front of `block`.  If the remainder is large
// enough to hold another BlockHeader plus at least 1 byte of payload, insert
// a new free BlockHeader for the remainder and link it into the pool-order
// chain.
// ---------------------------------------------------------------------------
void FreeList::split(BlockHeader* block, std::size_t size) noexcept {
    // MIN_SPLIT: remainder must hold a header AND at least 1 payload byte.
    const std::size_t min_split = sizeof(BlockHeader) + 1u;

    if (block->size < size + sizeof(BlockHeader) + min_split) {
        // Not enough room to split — leave the block at its current size.
        return;
    }

    // Position the new header immediately after the allocated region.
    auto* new_h = reinterpret_cast<BlockHeader*>(
        reinterpret_cast<std::byte*>(block) + sizeof(BlockHeader) + size);

    new_h->size    = block->size - size - sizeof(BlockHeader);
    new_h->is_free = true;
    std::memset(new_h->_pad, 0, sizeof(new_h->_pad));
    new_h->prev    = block;
    new_h->next    = block->next;

    if (block->next) {
        block->next->prev = new_h;
    }

    block->next = new_h;
    block->size = size;
}

// ---------------------------------------------------------------------------
// coalesce
//
// Merge `block` with its immediate pool-order neighbour(s) if they are free.
// Merges forward (with next) first, then backward (with prev).
// ---------------------------------------------------------------------------
void FreeList::coalesce(BlockHeader* block) noexcept {
    // Merge with next if it is free.
    if (block->next && block->next->is_free) {
        BlockHeader* absorbed = block->next;
        block->size += sizeof(BlockHeader) + absorbed->size;
        block->next  = absorbed->next;
        if (absorbed->next) {
            absorbed->next->prev = block;
        }
    }

    // Merge with prev if it is free.
    if (block->prev && block->prev->is_free) {
        BlockHeader* pred = block->prev;
        pred->size += sizeof(BlockHeader) + block->size;
        pred->next   = block->next;
        if (block->next) {
            block->next->prev = pred;
        }
    }
}

// ---------------------------------------------------------------------------
// allocate
//
// 1. Reject size == 0.
// 2. Reject alignment that is not a power of two.
// 3. Walk pool-order blocks searching for the first free block that can
//    satisfy the aligned request.
// 4. Split, mark allocated, and return the aligned user pointer.
// ---------------------------------------------------------------------------
void* FreeList::allocate(std::size_t size, std::size_t alignment) noexcept {
    if (size == 0) {
        return nullptr;
    }
    if (!is_power_of_two(alignment)) {
        return nullptr;
    }

    for (BlockHeader* h = first_block_; h != nullptr; h = h->next) {
        if (!h->is_free) {
            continue;
        }

        // The user region starts immediately after the header.
        auto* candidate = static_cast<std::byte*>(user_ptr(h));

        // Compute the aligned start address within the user region.
        std::byte* aligned = align_up(candidate, alignment);

        // How many bytes of padding are needed to reach the aligned address?
        const std::size_t adjustment = static_cast<std::size_t>(aligned - candidate);

        // Guard against overflow when computing total_needed.
        if (adjustment > std::numeric_limits<std::size_t>::max() - size) {
            continue;  // overflow — skip this block
        }
        const std::size_t total_needed = size + adjustment;

        if (h->size < total_needed) {
            continue;  // block too small
        }

        // This block fits — split it (using total_needed as the allocation
        // unit so that the aligned return address lands within the block's
        // payload without corrupting the header of a remainder block).
        split(h, total_needed);
        h->is_free = false;

        return static_cast<void*>(aligned);
    }

    return nullptr;  // no suitable block found
}

// ---------------------------------------------------------------------------
// deallocate
//
// Recover the BlockHeader via pointer arithmetic, assert it is within the
// pool (debug builds), mark the block free, and coalesce neighbours.
// ---------------------------------------------------------------------------
void FreeList::deallocate(void* ptr) noexcept {
    if (!ptr) {
        return;
    }

#if !defined(NDEBUG)
    // Assert the pointer is within the pool's user region.
    auto* byte_ptr = static_cast<std::byte*>(ptr);
    assert(byte_ptr >= pool_ + sizeof(BlockHeader) &&
           byte_ptr <  pool_ + capacity_ &&
           "FreeList::deallocate: pointer is outside the pool range");
#endif

    BlockHeader* block = header_of(ptr);
    block->is_free = true;
    coalesce(block);
}

// ---------------------------------------------------------------------------
// capacity / used / remaining
//
// Walk the entire pool-order list and sum payload sizes by free/used status.
// This is simple, consistent, and requires no extra tracking fields.
// ---------------------------------------------------------------------------
std::size_t FreeList::capacity() const noexcept {
    return capacity_;
}

std::size_t FreeList::used() const noexcept {
    std::size_t total = 0;
    for (const BlockHeader* h = first_block_; h != nullptr; h = h->next) {
        if (!h->is_free) {
            total += h->size;
        }
    }
    return total;
}

std::size_t FreeList::remaining() const noexcept {
    std::size_t total = 0;
    for (const BlockHeader* h = first_block_; h != nullptr; h = h->next) {
        if (h->is_free) {
            total += h->size;
        }
    }
    return total;
}

} // namespace memory
