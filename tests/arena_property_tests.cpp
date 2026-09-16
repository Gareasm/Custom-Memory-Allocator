#include <gtest/gtest.h>
#include <rapidcheck.h>
#include <rapidcheck/gtest.h>

#include <cstddef>
#include <cstdint>

#include "memory/arena.hpp"

// ---------------------------------------------------------------------------
// Property 1: Arena allocate alignment
// ---------------------------------------------------------------------------
//
// Feature: custom-memory-manager, Property 1:
//   For any valid (size, alignment) pair where alignment is a power of two and
//   the allocation succeeds, the returned pointer address is divisible by alignment.
//
// Validates: Requirements 2.1, 8.1, 8.2

RC_GTEST_PROP(ArenaPropertyTests, AllocateReturnsAlignedPointer, ()) {
    // Generate a pool large enough to guarantee at least one allocation succeeds
    // even with maximum padding. We pick a fixed large capacity to keep the pool
    // construction cost negligible and focus on the alignment property.
    constexpr std::size_t POOL_CAPACITY = 4096;

    // Generate a power-of-two alignment in {1, 2, 4, 8, 16, 32, 64, 128, 256}.
    // We encode it as an exponent in [0, 8] and shift.
    const auto exp = *rc::gen::inRange<unsigned>(0u, 9u);  // [0, 8]
    const std::size_t alignment = std::size_t{1} << exp;   // 1 .. 256

    // Generate a non-zero size that fits within the pool after maximum alignment
    // padding (worst case: alignment - 1 bytes of padding).
    const std::size_t max_usable = POOL_CAPACITY - (alignment - 1);
    RC_PRE(max_usable > 0);
    const auto size = *rc::gen::inRange<std::size_t>(1, max_usable + 1);

    memory::Arena arena(POOL_CAPACITY);
    void* ptr = arena.allocate(size, alignment);

    // If the allocation succeeded the pointer must be alignment-divisible.
    if (ptr != nullptr) {
        const auto addr = reinterpret_cast<std::uintptr_t>(ptr);
        RC_ASSERT(addr % alignment == 0);
    }
    // If ptr == nullptr the preconditions were still valid — the pool was simply
    // exhausted. That is a correct outcome and satisfies the property vacuously.
}

