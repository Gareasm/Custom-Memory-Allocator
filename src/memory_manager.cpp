#include "memory/memory_manager.hpp"

namespace memory {

MemoryManager::MemoryManager(std::size_t arena_capacity, std::size_t free_list_capacity)
    : arena_(arena_capacity), free_list_(free_list_capacity) {}

Arena& MemoryManager::arena() noexcept {
    return arena_;
}

FreeList& MemoryManager::freeList() noexcept {
    return free_list_;
}

} // namespace memory
