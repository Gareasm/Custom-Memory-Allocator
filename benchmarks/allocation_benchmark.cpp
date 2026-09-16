// Feature: custom-memory-manager
// Benchmarks: malloc/free, new/delete, Arena, FreeList allocation throughput
// Requirements: 15.1-15.4

#include <benchmark/benchmark.h>
#include <cstdlib>
#include <vector>

#include "memory/arena.hpp"
#include "memory/free_list.hpp"

// ---------------------------------------------------------------------------
// Constants
// ---------------------------------------------------------------------------

static constexpr std::size_t kAllocSize  = 64;   // bytes per allocation
static constexpr std::size_t kPoolSize1e5 = kAllocSize * 100'000 + 4096;
static constexpr std::size_t kPoolSize1e6 = kAllocSize * 1'000'000 + 4096;
static constexpr std::size_t kPoolSize1e7 = kAllocSize * 10'000'000 + 4096;

// ---------------------------------------------------------------------------
// malloc / free
// ---------------------------------------------------------------------------

static void BM_Malloc_Alloc(benchmark::State& state) {
    const auto n = static_cast<std::size_t>(state.range(0));
    std::vector<void*> ptrs(n);

    for (auto _ : state) {
        for (std::size_t i = 0; i < n; ++i) {
            ptrs[i] = std::malloc(kAllocSize);
            benchmark::DoNotOptimize(ptrs[i]);
        }
        for (std::size_t i = 0; i < n; ++i) {
            std::free(ptrs[i]);
        }
    }

    state.SetItemsProcessed(static_cast<int64_t>(state.iterations()) *
                            static_cast<int64_t>(n));
    state.counters["allocs/sec"] = benchmark::Counter(
        static_cast<double>(n), benchmark::Counter::kIsRate);
    state.counters["ns/alloc"] = benchmark::Counter(
        static_cast<double>(n),
        benchmark::Counter::kIsRate | benchmark::Counter::kInvert,
        benchmark::Counter::kIs1000);
}
BENCHMARK(BM_Malloc_Alloc)->Arg(100'000)->Arg(1'000'000)->Arg(10'000'000)
    ->Unit(benchmark::kMillisecond);

// ---------------------------------------------------------------------------
// new / delete
// ---------------------------------------------------------------------------

static void BM_New_Alloc(benchmark::State& state) {
    const auto n = static_cast<std::size_t>(state.range(0));
    std::vector<std::byte*> ptrs(n);

    for (auto _ : state) {
        for (std::size_t i = 0; i < n; ++i) {
            ptrs[i] = new std::byte[kAllocSize];
            benchmark::DoNotOptimize(ptrs[i]);
        }
        for (std::size_t i = 0; i < n; ++i) {
            delete[] ptrs[i];
        }
    }

    state.SetItemsProcessed(static_cast<int64_t>(state.iterations()) *
                            static_cast<int64_t>(n));
    state.counters["allocs/sec"] = benchmark::Counter(
        static_cast<double>(n), benchmark::Counter::kIsRate);
    state.counters["ns/alloc"] = benchmark::Counter(
        static_cast<double>(n),
        benchmark::Counter::kIsRate | benchmark::Counter::kInvert,
        benchmark::Counter::kIs1000);
}
BENCHMARK(BM_New_Alloc)->Arg(100'000)->Arg(1'000'000)->Arg(10'000'000)
    ->Unit(benchmark::kMillisecond);


// ---------------------------------------------------------------------------
// Arena allocator
// ---------------------------------------------------------------------------

// Helper: pick pool size based on op count
static std::size_t arena_pool_size(int64_t n) {
    if (n <= 100'000)  return kPoolSize1e5;
    if (n <= 1'000'000) return kPoolSize1e6;
    return kPoolSize1e7;
}

static void BM_Arena_Alloc(benchmark::State& state) {
    const auto n        = static_cast<std::size_t>(state.range(0));
    const auto poolSize = arena_pool_size(state.range(0));

    // Pool construction excluded from timed region
    memory::Arena arena(poolSize);

    for (auto _ : state) {
        arena.reset();  // O(1) — resets bump pointer, no free()
        for (std::size_t i = 0; i < n; ++i) {
            void* p = arena.allocate(kAllocSize);
            benchmark::DoNotOptimize(p);
        }
        // Arena bulk-deallocates on reset; individual dealloc is a no-op
    }

    state.SetItemsProcessed(static_cast<int64_t>(state.iterations()) *
                            static_cast<int64_t>(n));
    state.counters["allocs/sec"] = benchmark::Counter(
        static_cast<double>(n), benchmark::Counter::kIsRate);
    state.counters["ns/alloc"] = benchmark::Counter(
        static_cast<double>(n),
        benchmark::Counter::kIsRate | benchmark::Counter::kInvert,
        benchmark::Counter::kIs1000);
}
BENCHMARK(BM_Arena_Alloc)->Arg(100'000)->Arg(1'000'000)->Arg(10'000'000)
    ->Unit(benchmark::kMillisecond);

// ---------------------------------------------------------------------------
// FreeList allocator
// ---------------------------------------------------------------------------

// FreeList pool needs BlockHeader overhead per block (~32 bytes) on top of payload
static constexpr std::size_t kBlockOverhead = 32;
static constexpr std::size_t kFLAlloc = kAllocSize + kBlockOverhead;

static std::size_t fl_pool_size(int64_t n) {
    if (n <= 100'000)  return kFLAlloc * 100'000 + 4096;
    if (n <= 1'000'000) return kFLAlloc * 1'000'000 + 4096;
    return kFLAlloc * 10'000'000 + 4096;
}

static void BM_FreeList_AllocDealloc(benchmark::State& state) {
    const auto n        = static_cast<std::size_t>(state.range(0));
    const auto poolSize = fl_pool_size(state.range(0));

    // Pool construction excluded from timed region
    memory::FreeList fl(poolSize);
    std::vector<void*> ptrs(n);

    for (auto _ : state) {
        // Allocate phase
        for (std::size_t i = 0; i < n; ++i) {
            ptrs[i] = fl.allocate(kAllocSize);
            benchmark::DoNotOptimize(ptrs[i]);
        }
        // Deallocate phase
        for (std::size_t i = 0; i < n; ++i) {
            fl.deallocate(ptrs[i]);
        }
    }

    state.SetItemsProcessed(static_cast<int64_t>(state.iterations()) *
                            static_cast<int64_t>(n));
    state.counters["allocs/sec"] = benchmark::Counter(
        static_cast<double>(n), benchmark::Counter::kIsRate);
    state.counters["ns/alloc"] = benchmark::Counter(
        static_cast<double>(n),
        benchmark::Counter::kIsRate | benchmark::Counter::kInvert,
        benchmark::Counter::kIs1000);
    state.counters["ns/dealloc"] = benchmark::Counter(
        static_cast<double>(n),
        benchmark::Counter::kIsRate | benchmark::Counter::kInvert,
        benchmark::Counter::kIs1000);
}
BENCHMARK(BM_FreeList_AllocDealloc)->Arg(100'000)->Arg(1'000'000)->Arg(10'000'000)
    ->Unit(benchmark::kMillisecond);
