#pragma once

#include <cstddef>
#include <cstdlib>

namespace memory {

class Arena {
public:
    explicit Arena(std::size_t capacity);
    ~Arena();

    // Non-copyable
    Arena(const Arena&) = delete;
    Arena& operator=(const Arena&) = delete;

    // Movable
    Arena(Arena&&) noexcept;
    Arena& operator=(Arena&&) noexcept;

    [[nodiscard]] void* allocate(std::size_t size,
                                 std::size_t alignment = alignof(std::max_align_t)) noexcept;
    void deallocate(void* ptr) noexcept; // no-op

    void reset() noexcept;

    [[nodiscard]] std::size_t capacity()  const noexcept;
    [[nodiscard]] std::size_t used()      const noexcept;
    [[nodiscard]] std::size_t remaining() const noexcept;

private:
    std::byte*  pool_     = nullptr;
    std::size_t capacity_ = 0;
    std::size_t offset_   = 0;   // bump pointer as byte offset from pool_
};

} // namespace memory
