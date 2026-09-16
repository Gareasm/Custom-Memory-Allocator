// Feature: custom-memory-manager
// Benchmark: sequential vs random access timing on arrays allocated from Arena
// Requirements: 16.3
//
// Two access patterns are measured on an integer array sitting in the Arena pool:
//   Sequential — walk indices 0, 1, 2, ..., N-1  (cache-friendly, prefetcher-friendly)
//   Random     — walk a Fisher-Yates shuffled permutation of indices (cache-unfriendly)
//
// The pool is allocated once outside the timed region.  Inside the loop only
// the read/write pass is measured.  Each element is both read and written so
// that the access cannot be optimised away.

#include <benchmark/benchmark.h>
#include <algorithm>
#include <cstdint>
#include <numeric>
#include <random>
#include <vector>

#include "memory/arena.hpp"

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

static constexpr std::size_t kElemSize = sizeof(int);

// ---------------------------------------------------------------------------
// Sequential access
// ---------------------------------------------------------------------------

static void BM_Arena_Sequential(benchmark::State& state) {
    const auto n         = static_cast<std::size_t>(state.range(0));
    const std::size_t pool_size = n * kElemSize + 4096;

    // Pool + allocation outside timed region
    memory::Arena arena(pool_size);
    auto* data = static_cast<int*>(arena.allocate(n * kElemSize, alignof(int)));

    // Initialise data so we are reading defined values
    for (std::size_t i = 0; i < n; ++i) {
        data[i] = static_cast<int>(i);
    }

    int64_t checksum = 0; // prevent dead-code elimination

    for (auto _ : state) {
        int64_t sum = 0;
        for (std::size_t i = 0; i < n; ++i) {
            sum += data[i];
            data[i] = static_cast<int>(sum & 0xFF); // write to force cache dirtying
        }
        benchmark::DoNotOptimize(sum);
        checksum ^= sum;
    }

    benchmark::DoNotOptimize(checksum);

    state.SetBytesProcessed(static_cast<int64_t>(state.iterations()) *
                            static_cast<int64_t>(n * kElemSize));
    state.counters["GB/s"] = benchmark::Counter(
        static_cast<double>(n * kElemSize),
        benchmark::Counter::kIsRate,
        benchmark::Counter::kIs1024);
}
BENCHMARK(BM_Arena_Sequential)
    ->Arg(1 << 16)   //  64 Ki elements (~256 KB — fits in L2)
    ->Arg(1 << 20)   //   1 Mi elements (~4 MB  — fits in L3)
    ->Arg(1 << 24)   //  16 Mi elements (~64 MB — exceeds L3)
    ->Unit(benchmark::kMillisecond);

// ---------------------------------------------------------------------------
// Random access
// ---------------------------------------------------------------------------

static void BM_Arena_Random(benchmark::State& state) {
    const auto n         = static_cast<std::size_t>(state.range(0));
    const std::size_t pool_size = n * kElemSize + 4096;

    // Pool + allocation outside timed region
    memory::Arena arena(pool_size);
    auto* data = static_cast<int*>(arena.allocate(n * kElemSize, alignof(int)));

    // Build a permutation of [0, n) — deterministic seed for reproducibility
    std::vector<std::size_t> indices(n);
    std::iota(indices.begin(), indices.end(), std::size_t{0});
    std::mt19937_64 rng(0xC0FFEE);
    std::shuffle(indices.begin(), indices.end(), rng);

    // Initialise data
    for (std::size_t i = 0; i < n; ++i) {
        data[i] = static_cast<int>(i);
    }

    int64_t checksum = 0;

    for (auto _ : state) {
        int64_t sum = 0;
        for (std::size_t i = 0; i < n; ++i) {
            const std::size_t idx = indices[i];
            sum += data[idx];
            data[idx] = static_cast<int>(sum & 0xFF);
        }
        benchmark::DoNotOptimize(sum);
        checksum ^= sum;
    }

    benchmark::DoNotOptimize(checksum);

    state.SetBytesProcessed(static_cast<int64_t>(state.iterations()) *
                            static_cast<int64_t>(n * kElemSize));
    state.counters["GB/s"] = benchmark::Counter(
        static_cast<double>(n * kElemSize),
        benchmark::Counter::kIsRate,
        benchmark::Counter::kIs1024);
}
BENCHMARK(BM_Arena_Random)
    ->Arg(1 << 16)
    ->Arg(1 << 20)
    ->Arg(1 << 24)
    ->Unit(benchmark::kMillisecond);
