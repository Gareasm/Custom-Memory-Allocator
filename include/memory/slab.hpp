#pragma once

#include <cstddef>

namespace memory {

class Slab {
public:
    /// Construct a slab for fixed-size objects.
    /// @param object_size  Size in bytes of each object.
    /// @param alignment    Required alignment for each object (must be power-of-two).
    /// @param capacity     Maximum number of objects the slab can hold.
    Slab(std::size_t object_size, std::size_t alignment, std::size_t capacity);
    ~Slab();

    // Non-copyable
    Slab(const Slab&) = delete;
    Slab& operator=(const Slab&) = delete;

    /// Allocate one slot. Returns nullptr when the slab is full. O(1).
    [[nodiscard]] void* allocate()  noexcept;

    /// Return a slot to the free list. O(1).
    /// @param ptr  Must be a pointer previously returned by allocate().
    ///             In debug builds, asserts ptr is within the pool range.
    void deallocate(void* ptr) noexcept;

    /// Maximum number of objects the slab can hold.
    [[nodiscard]] std::size_t capacity() const noexcept;

    /// Number of currently allocated (live) objects.
    [[nodiscard]] std::size_t used() const noexcept;

private:
    std::byte*  pool_      = nullptr;  ///< Backing pool (malloc'd)
    std::size_t slot_size_ = 0;        ///< Padded slot size (bytes), >= alignment
    std::size_t capacity_  = 0;        ///< Object count
    std::size_t used_      = 0;        ///< Live object count
    void**      free_head_ = nullptr;  ///< Head of the intrusive free-slot list
};

} // namespace memory
