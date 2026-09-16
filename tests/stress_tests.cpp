#include <gtest/gtest.h>

#include <algorithm>
#include <random>
#include <vector>

#include "memory/free_list.hpp"

// ---------------------------------------------------------------------------
// Helper types and utilities
// ---------------------------------------------------------------------------

struct Alloc {
    void*       ptr;
    std::size_t size;
};

/// Returns true if the region [ptr, ptr+size) overlaps with any live allocation.
static bool overlaps(const std::vector<Alloc>& live, void* ptr, std::size_t size) {
    const auto* p_begin = static_cast<const std::byte*>(ptr);
    const auto* p_end   = p_begin + size;

    for (const auto& a : live) {
        const auto* a_begin = static_cast<const std::byte*>(a.ptr);
        const auto* a_end   = a_begin + a.size;
        if (p_begin < a_end && p_end > a_begin) {
            return true;
        }
    }
    return false;
}

// ---------------------------------------------------------------------------
// Stress Test
// ---------------------------------------------------------------------------

/// Validates: Requirements 14.1–14.6
///
/// Runs ≥100,000 randomised allocate/deallocate operations against FreeList.
/// Maintains a reference vector of live allocations and asserts:
///   - no two live allocations overlap in address space
///   - after full deallocation the pool is fully coalesced (remaining ≈ capacity)
TEST(StressTest, FreeListRandomOps) {
    // Feature: custom-memory-manager
    // Requirements 14.1–14.6: stress / randomised / overlap / coalesce

    constexpr std::size_t POOL_SIZE = 4u * 1024u * 1024u;  // 4 MiB
    constexpr std::size_t NUM_OPS   = 100'000;
    constexpr std::size_t MAX_LIVE  = 500;   // cap on simultaneous live allocs
    constexpr std::size_t MIN_SIZE  = 8;
    constexpr std::size_t MAX_SIZE  = 256;

    memory::FreeList fl(POOL_SIZE);

    // Verify the pool was created successfully.
    ASSERT_GT(fl.capacity(), std::size_t{0}) << "FreeList pool creation failed";
    ASSERT_GT(fl.remaining(), std::size_t{0}) << "FreeList pool has no free space";

    std::vector<Alloc> live;
    live.reserve(MAX_LIVE);

    // Fixed-seed PRNG for reproducibility.
    std::mt19937 rng(42);
    std::uniform_int_distribution<std::size_t> size_dist(MIN_SIZE, MAX_SIZE);
    std::uniform_int_distribution<int>         op_dist(0, 1);  // 0 = alloc, 1 = dealloc

    std::size_t successful_allocs   = 0;
    std::size_t successful_deallocs = 0;

    for (std::size_t op = 0; op < NUM_OPS; ++op) {
        // Decide whether to allocate or deallocate.
        // Force allocate if live list is empty; force deallocate if at cap.
        const bool do_alloc =
            live.empty() ||
            (live.size() < MAX_LIVE && op_dist(rng) == 0);

        if (do_alloc) {
            const std::size_t sz  = size_dist(rng);
            void* const       ptr = fl.allocate(sz);

            if (ptr) {
                // Core invariant: no overlap with any existing live allocation.
                ASSERT_FALSE(overlaps(live, ptr, sz))
                    << "New allocation [" << ptr << ", +" << sz
                    << ") overlaps with an existing live allocation";

                live.push_back({ptr, sz});
                ++successful_allocs;
            }
            // A nullptr return is acceptable (pool may be fragmented/full).
        } else {
            // Pick a random live allocation to free.
            std::uniform_int_distribution<std::size_t> idx_dist(0, live.size() - 1u);
            const std::size_t idx = idx_dist(rng);

            fl.deallocate(live[idx].ptr);
            live.erase(live.begin() + static_cast<std::ptrdiff_t>(idx));
            ++successful_deallocs;
        }
    }

    // Ensure we exercised both paths meaningfully.
    EXPECT_GT(successful_allocs,   std::size_t{1000})
        << "Too few successful allocations — test may not be meaningful";
    EXPECT_GT(successful_deallocs, std::size_t{100})
        << "Too few successful deallocations — test may not be meaningful";

    // -----------------------------------------------------------------------
    // Phase 2: free all remaining live allocations and verify full coalesce.
    // -----------------------------------------------------------------------
    for (const auto& a : live) {
        fl.deallocate(a.ptr);
    }
    live.clear();

    // After freeing everything, the allocator should coalesce back to (nearly)
    // the full pool capacity.  BlockHeader is 32 bytes on 64-bit; allow a
    // generous tolerance of 256 bytes for 1–2 residual headers.
    //
    // remaining() returns sum of all free block payloads (headers excluded).
    // With a single free block the payload == capacity - sizeof(BlockHeader)
    // == POOL_SIZE - 32.  We check for >= capacity - 256 to be robust to
    // minor implementation variations without being able to access sizeof
    // (BlockHeader) directly from here.
    EXPECT_GT(fl.remaining(), std::size_t{0})
        << "FreeList has zero remaining space after freeing all allocations";

    EXPECT_GE(fl.remaining(), fl.capacity() - 256u)
        << "FreeList did not coalesce fully after freeing all allocations; "
        << "remaining=" << fl.remaining() << " capacity=" << fl.capacity();
}

// ---------------------------------------------------------------------------
// Verify that a freed region can be immediately reused
// ---------------------------------------------------------------------------

/// Validates: Requirements 14.3 (dealloc → reuse)
///
/// Allocates a block, frees it, then checks that a subsequent allocation of
/// the same size succeeds (i.e., the freed range is genuinely reusable).
TEST(StressTest, FreedRegionIsReusable) {
    constexpr std::size_t POOL_SIZE  = 64u * 1024u;  // 64 KiB is plenty
    constexpr std::size_t BLOCK_SIZE = 128;

    memory::FreeList fl(POOL_SIZE);

    void* first = fl.allocate(BLOCK_SIZE);
    ASSERT_NE(first, nullptr);

    const std::size_t remaining_after_alloc = fl.remaining();
    fl.deallocate(first);

    // After dealloc, remaining should be greater than it was while allocated.
    EXPECT_GT(fl.remaining(), remaining_after_alloc);

    // A new allocation of the same size must succeed.
    void* second = fl.allocate(BLOCK_SIZE);
    ASSERT_NE(second, nullptr)
        << "Could not reallocate after freeing a block — freed range not reused";

    fl.deallocate(second);
}

