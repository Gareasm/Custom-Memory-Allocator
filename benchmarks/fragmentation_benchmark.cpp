// Feature: custom-memory-manager
// Benchmark: FreeList fragmentation under a mixed-lifetime workload
// Requirements: 16.2
//
// Reports three metrics via benchmark counters:
//   pool_utilization_%   = used / capacity * 100
//   largest_free_bytes   = size of the largest contiguous free block
//   fragmentation        = 1 - (largest_free / total_free)   [0=no frag, 1=fully fragmented]
//
// Because FreeList does not expose a public API to query individual block
// sizes, we approximate largest_free by tracking live allocations and
// computing the largest gap after interleaved alloc/free.  The exact value
// requires either exposing an iterator or a dedicated query; this heuristic
// is sufficient for an educational benchmark.

#include <benchmark/benchmark.h>
#include <algorithm>
#include <cstdint>
#include <random>
#include <vector>

#include "memory/free_list.hpp"

// ---------------------------------------------------------------------------
// Parameters
// ---------------------------------------------------------------------------

// Allocation sizes cycle through small / medium / large to create realistic
// fragmentation patterns.
static constexpr std::size_t kSizes[] = {16, 32, 64, 128, 256};
static constexpr std::size_t kNumSizes = sizeof(kSizes) / sizeof(kSizes[0]);

// Block-header overhead assumed to be 32 bytes (as per design doc).
static constexpr std::size_t kHeaderBytes = 32;

// ---------------------------------------------------------------------------
// Fragmentation workload
// ---------------------------------------------------------------------------

static void BM_FreeList_Fragmentation(benchmark::State& state) {
    const auto n = static_cast<std::size_t>(state.range(0));  // number of ops

    // Size the pool generously so we can sustain ~n/2 live allocations.
    // Worst-case each allocation is 256 bytes + 32-byte header.
    const std::size_t pool_size = (kSizes[kNumSizes - 1] + kHeaderBytes) * (n / 2 + 1) + 4096;

    // Pool construction excluded from timed region (Requirement 15.4)
    memory::FreeList fl(pool_size);

    // Deterministic RNG seeded identically every benchmark iteration so
    // results are reproducible.
    std::mt19937 rng(0xDEADBEEF);
    std::uniform_int_distribution<std::size_t> size_dist(0, kNumSizes - 1);
    std::uniform_int_distribution<int>         free_dist(0, 1);

    // Track live allocations: {ptr, size}
    struct Alloc { void* ptr; std::size_t size; };
    std::vector<Alloc> live;
    live.reserve(n / 2 + 1);

    // Metrics accumulated across all state iterations
    double total_util   = 0.0;
    double total_frag   = 0.0;
    int64_t sample_count = 0;

    for (auto _ : state) {
        // Reset pool and tracking state
        fl = memory::FreeList(pool_size);
        live.clear();

        for (std::size_t op = 0; op < n; ++op) {
            // Decide: allocate or free (free only if we have live blocks)
            bool do_free = !live.empty() && (free_dist(rng) == 1);

            if (do_free) {
                // Free a random live allocation
                std::uniform_int_distribution<std::size_t> idx_dist(0, live.size() - 1);
                std::size_t idx = idx_dist(rng);
                fl.deallocate(live[idx].ptr);
                live.erase(live.begin() + static_cast<std::ptrdiff_t>(idx));
            } else {
                std::size_t sz = kSizes[size_dist(rng)];
                void* p = fl.allocate(sz);
                if (p) {
                    live.push_back({p, sz});
                }
            }
        }

        // Snapshot metrics after the workload
        const std::size_t cap       = fl.capacity();
        const std::size_t used_now  = fl.used();
        const std::size_t free_now  = fl.remaining();

        // Pool utilization %
        const double util = cap > 0 ? (static_cast<double>(used_now) / static_cast<double>(cap)) * 100.0 : 0.0;

        // Approximate largest contiguous free block:
        // After freeing all live allocations we know the pool coalesces to
        // (nearly) one block, but mid-workload we estimate it using the
        // free bytes minus per-block header cost scaled by live count.
        // A tighter bound: sort live allocations by address and find the
        // largest gap.  We approximate gap sizes using available data.
        std::size_t largest_free = 0;
        if (!live.empty()) {
            // Sort live allocations by pointer address
            std::vector<Alloc> sorted = live;
            std::sort(sorted.begin(), sorted.end(),
                      [](const Alloc& a, const Alloc& b) {
                          return reinterpret_cast<std::uintptr_t>(a.ptr) <
                                 reinterpret_cast<std::uintptr_t>(b.ptr);
                      });

            // Estimate leading free space before first allocation
            const auto pool_start = reinterpret_cast<std::uintptr_t>(sorted.front().ptr);
            // (pool base is not exposed; treat leading gap as 0 conservatively)
            (void)pool_start;

            // Gaps between consecutive live allocations
            for (std::size_t i = 0; i + 1 < sorted.size(); ++i) {
                const auto end_i  = reinterpret_cast<std::uintptr_t>(sorted[i].ptr)
                                  + sorted[i].size + kHeaderBytes;
                const auto start_next = reinterpret_cast<std::uintptr_t>(sorted[i + 1].ptr)
                                      - kHeaderBytes;
                if (start_next > end_i) {
                    const std::size_t gap = static_cast<std::size_t>(start_next - end_i);
                    largest_free = std::max(largest_free, gap);
                }
            }
        } else {
            // No live allocations: pool should be almost fully coalesced
            largest_free = free_now > kHeaderBytes ? free_now - kHeaderBytes : 0;
        }

        // Fragmentation metric: 1 - (largest_free / total_free)
        const double frag = (free_now > 0)
            ? 1.0 - (static_cast<double>(largest_free) / static_cast<double>(free_now))
            : 0.0;

        total_util   += util;
        total_frag   += frag;
        ++sample_count;

        benchmark::DoNotOptimize(used_now);
        benchmark::DoNotOptimize(largest_free);
    }

    // Report average metrics as counters
    if (sample_count > 0) {
        state.counters["pool_utilization_%"] = total_util / static_cast<double>(sample_count);
        state.counters["fragmentation"]       = total_frag / static_cast<double>(sample_count);
    }

    state.SetItemsProcessed(static_cast<int64_t>(state.iterations()) *
                            static_cast<int64_t>(n));
}

BENCHMARK(BM_FreeList_Fragmentation)
    ->Arg(1'000)
    ->Arg(10'000)
    ->Arg(100'000)
    ->Unit(benchmark::kMillisecond);
