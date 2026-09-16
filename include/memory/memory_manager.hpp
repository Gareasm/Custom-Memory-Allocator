#pragma once

#include "memory/arena.hpp"
#include "memory/free_list.hpp"

namespace memory {

class MemoryManager {
public:
    MemoryManager(std::size_t arena_capacity, std::size_t free_list_capacity);

    [[nodiscard]] Arena&    arena()    noexcept;
    [[nodiscard]] FreeList& freeList() noexcept;

private:
    Arena    arena_;
    FreeList free_list_;
};

} // namespace memory
