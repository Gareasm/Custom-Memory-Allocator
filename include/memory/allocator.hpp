#pragma once

#include <cstddef>
#include <limits>
#include <new>

namespace memory {

/// C++ named-requirement Allocator adapter that bridges CustomAllocator<T, Alloc>
/// to any of the library's pool allocators (Arena, FreeList, Slab).
///
/// The underlying allocator is held by pointer so that the STL can copy/rebind
/// this allocator without copying the pool itself. All instances that share the
/// same pool pointer compare equal.
template<typename T, typename Alloc>
class CustomAllocator {
public:
    // ---- STL requirements ------------------------------------------------
    using value_type = T;

    // Rebind support: CustomAllocator<T, Alloc>::rebind<U>::other
    //                  == CustomAllocator<U, Alloc>
    template<typename U>
    struct rebind { using other = CustomAllocator<U, Alloc>; };

    // ---- Construction ----------------------------------------------------

    /// Primary constructor: bind to an existing allocator instance.
    explicit CustomAllocator(Alloc& alloc) noexcept : alloc_(&alloc) {}

    /// Rebind copy constructor (e.g. CustomAllocator<char, Arena> from
    /// CustomAllocator<int, Arena>). The friend declaration below allows
    /// access to the private alloc_ member of the other specialisation.
    template<typename U>
    CustomAllocator(const CustomAllocator<U, Alloc>& other) noexcept
        : alloc_(other.alloc_) {}

    // Defaulted copy/move are fine — pointer semantics.
    CustomAllocator(const CustomAllocator&) noexcept = default;
    CustomAllocator& operator=(const CustomAllocator&) noexcept = default;

    // ---- Core interface --------------------------------------------------

    /// Allocate storage for n objects of type T.
    /// Throws std::bad_array_new_length on arithmetic overflow.
    /// Throws std::bad_alloc when the underlying pool returns nullptr.
    [[nodiscard]] T* allocate(std::size_t n) {
        // Overflow check: n * sizeof(T) must not wrap.
        if (n > std::numeric_limits<std::size_t>::max() / sizeof(T)) {
            throw std::bad_array_new_length{};
        }
        void* p = alloc_->allocate(n * sizeof(T), alignof(T));
        if (!p) {
            throw std::bad_alloc{};
        }
        return static_cast<T*>(p);
    }

    /// Return storage previously obtained from allocate(n).
    /// Forwards to the underlying allocator's single-argument deallocate
    /// because Arena, FreeList, and Slab all take only a void* pointer —
    /// the size parameter is not needed by any of the pool strategies.
    void deallocate(T* ptr, std::size_t /*n*/) noexcept {
        alloc_->deallocate(ptr);
    }

    // ---- Equality --------------------------------------------------------

    bool operator==(const CustomAllocator& o) const noexcept {
        return alloc_ == o.alloc_;
    }
    bool operator!=(const CustomAllocator& o) const noexcept {
        return !(*this == o);
    }

    // Allow any CustomAllocator<U, Alloc> to access our private alloc_
    // (needed by the rebind copy constructor above).
    template<typename U, typename A>
    friend class CustomAllocator;

private:
    Alloc* alloc_;
};

} // namespace memory
