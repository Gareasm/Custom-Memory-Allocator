// Feature: custom-memory-manager
// Benchmark: std::vector push-back throughput — std::allocator vs CustomAllocator<Arena>
// Requirements: 16.1

#include <benchmark/benchmark.h>
#include <vector>

#include "memory/arena.hpp"
#include "memory/allocator.hpp"

// Each int is 4 bytes; reserve enough pool for the largest workload (10^7 ints)
// plus some headroom for alignment padding.
static constexpr std::size_t kMaxElems   = 10'000'000;
static constexpr std::size_t kPoolBytes  = kMaxElems * sizeof(int) + 4096;

// ---------------------------------------------------------------------------
// Baseline: std::vector with the default std::allocator
// ---------------------------------------------------------------------------

static void BM_Vector_StdAlloc(benchmark::State& state) {
    const auto n = static_cast<std::size_t>(state.range(0));

    for (auto _ : state) {
        std::vector<int> v;
        v.reserve(n);
        for (std::size_t i = 0; i < n; ++i) {
            v.push_back(static_cast<int>(i));
        }
        benchmark::DoNotOptimize(v.data());
        benchmark::ClobberMemory();
    }

    state.SetItemsProcessed(static_cast<int64_t>(state.iterations()) *
                            static_cast<int64_t>(n));
    state.counters["pushbacks/sec"] = benchmark::Counter(
        static_cast<double>(n), benchmark::Counter::kIsRate);
}
BENCHMARK(BM_Vector_StdAlloc)
    ->Arg(100'000)->Arg(1'000'000)->Arg(10'000'000)
    ->Unit(benchmark::kMillisecond);

// ---------------------------------------------------------------------------
// Custom: std::vector with CustomAllocator<int, Arena>
//
// The Arena is constructed outside the timed region.  Inside the timed loop
// we call arena.reset() to reclaim memory in O(1) so each iteration starts
// from a fresh (logically empty) pool without touching the OS allocator.
// ---------------------------------------------------------------------------

static void BM_Vector_CustomAlloc_Arena(benchmark::State& state) {
    const auto n = static_cast<std::size_t>(state.range(0));

    // Pool construction excluded from timed region (Requirement 15.4)
    memory::Arena arena(kPoolBytes);

    for (auto _ : state) {
        arena.reset();

        memory::CustomAllocator<int, memory::Arena> alloc(arena);
        std::vector<int, memory::CustomAllocator<int, memory::Arena>> v(alloc);
        v.reserve(n);
        for (std::size_t i = 0; i < n; ++i) {
            v.push_back(static_cast<int>(i));
        }
        benchmark::DoNotOptimize(v.data());
        benchmark::ClobberMemory();
    }

    state.SetItemsProcessed(static_cast<int64_t>(state.iterations()) *
                            static_cast<int64_t>(n));
    state.counters["pushbacks/sec"] = benchmark::Counter(
        static_cast<double>(n), benchmark::Counter::kIsRate);
}
BENCHMARK(BM_Vector_CustomAlloc_Arena)
    ->Arg(100'000)->Arg(1'000'000)->Arg(10'000'000)
    ->Unit(benchmark::kMillisecond);
