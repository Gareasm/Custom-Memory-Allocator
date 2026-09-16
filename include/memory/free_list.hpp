#pragma once

#include <cstddef>
#include <cstdint>
#include <cstdlib>

namespace memory {

class FreeList {
public:
    explicit FreeList(std::size_t capacity);
    ~FreeList();

    FreeList(const FreeList&)            = delete;
    FreeList& operator=(const FreeList&) = delete;
    FreeList(FreeList&&) noexcept;
    FreeList& operator=(FreeList&&) noexcept;

    [[nodiscard]] void* allocate(std::size_t size,
                                 std::size_t alignment = alignof(std::max_align_t)) noexcept;
    void deallocate(void* ptr) noexcept;

    [[nodiscard]] std::size_t capacity()  const noexcept;
    [[nodiscard]] std::size_t used()      const noexcept;
    [[nodiscard]] std::size_t remaining() const noexcept;

private:
    // BlockHeader is defined in the .cpp to keep its layout private.
    // Forward-declared here so the private member functions can use it.
    struct BlockHeader;

    std::byte*   pool_     = nullptr;
    std::size_t  capacity_ = 0;

    // Head of the pool-order linked list (always the first physical block).
    BlockHeader* first_block_ = nullptr;

    // Internal helpers
    BlockHeader* find_first_fit(std::size_t needed) noexcept;
    void         split(BlockHeader* block, std::size_t size) noexcept;
    void         coalesce(BlockHeader* block) noexcept;

    static bool  is_power_of_two(std::size_t v) noexcept;
};

} // namespace memory
