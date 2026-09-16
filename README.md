# Custom Memory Manager

A C++20 library that implements three pool-based memory allocation strategies,
a C++ standard library adapter, and a top-level owner class. Built for
educational purposes — the goal is to show clearly how Arena, Free-List, and
Slab allocators differ in trade-offs, and to verify those trade-offs with
property-based tests and benchmarks.

---

## What this project does

The library provides drop-in alternatives to the system heap for situations
where you control object lifetimes and want predictable, low-overhead
allocation:

| Allocator | Best for | Deallocation |
|---|---|---|
| `Arena` | Short-lived objects with the same lifetime | Bulk reset only |
| `FreeList` | Mixed-lifetime objects of varying sizes | Individual, O(n) search |
| `Slab` | High-frequency fixed-size objects | Individual, O(1) |

A `CustomAllocator<T, Alloc>` adapter makes any of these usable with
`std::vector`, `std::list`, `std::unordered_map`, and other standard containers.

---

## Why custom allocators?

- **Throughput.** The Arena bump-pointer path is a few instructions; `malloc`
  must navigate thread-local caches, bins, and OS calls.
- **Fragmentation control.** General-purpose allocators can leave the heap in
  a state where many small holes prevent large allocations. An Arena has zero
  fragmentation; a Slab has zero fragmentation for fixed-size objects.
- **Determinism.** Embedded and real-time code often needs allocations that
  cannot block or fail mid-run. A pre-allocated pool gives a hard guarantee on
  memory availability.
- **Cache locality.** Objects allocated sequentially from an Arena end up
  contiguous in memory, which the CPU prefetcher can exploit.

---

## Allocator details

### Arena (bump allocator)

Maintains a single offset (`offset_`) into a contiguous pool. Each allocation
advances the offset by the required alignment padding plus the requested size.
Deallocation is a no-op; the entire pool is reclaimed in O(1) via `reset()`.

```
Pool:  [ alloc0 | pad | alloc1 | alloc2 | ............. free ............ ]
        ^                               ^                                  ^
        pool_                        offset_                   pool_+capacity_
```

- Allocation: O(1)
- Deallocation: O(1) no-op; O(1) bulk via `reset()`
- Fragmentation: none

### Free-List allocator

Each region in the pool begins with a `BlockHeader` (32 bytes on 64-bit).
Headers form a doubly-linked pool-order chain. Allocation walks the chain
for the first free block that fits (First-Fit), splits it if a remainder
exists, and marks it allocated. Deallocation marks the block free and
coalesces adjacent free neighbours.

```
Pool:  [ Header | payload 0 ][ Header | payload 1 ][ Header | free ....... ]
        ^                     ^                     ^
        first_block_          block->next           block->next->next
```

- Allocation: O(n) scan
- Deallocation: O(1) + O(1) coalesce of up to 2 neighbours
- Fragmentation: reduced by coalescing; First-Fit may leave small holes

### Slab allocator

All slots are the same size (`max(object_size, sizeof(void*))` rounded up to
alignment). Free slots form an intrusive singly-linked list stored inside the
slot memory itself (safe because free slots are not in use).

```
Pool:  [ slot0 | slot1 | slot2 | ... | slotN ]
         ^--- free_head_ points to the first free slot;
              each free slot's first bytes hold a pointer to the next free slot
```

- Allocation: O(1) pop from free list
- Deallocation: O(1) push to free list
- Fragmentation: none (fixed-size slots)

---

## Alignment

All allocators accept a `size_t alignment` parameter (must be a non-zero
power of two, e.g. 8, 16, 32, 64).

- `Arena::allocate` uses `std::align` to advance the bump pointer to the next
  aligned address without undefined behaviour.
- `FreeList::allocate` searches for a block whose usable region contains an
  address aligned to the request; the alignment padding is included in the
  allocated size.
- `Slab` alignment is fixed at construction: every slot is aligned to the
  configured value.
- Passing a non-power-of-two (or zero) alignment returns `nullptr`.

---

## STL adapter

`CustomAllocator<T, Alloc>` satisfies the C++ named requirement *Allocator*:

```cpp
memory::Arena arena(1 << 20);   // 1 MiB pool
memory::CustomAllocator<int, memory::Arena> alloc(arena);
std::vector<int, decltype(alloc)> v(alloc);
v.push_back(42);
```

- `allocate(n)` requests `n * sizeof(T)` bytes aligned to `alignof(T)` from
  the underlying pool. Throws `std::bad_array_new_length` on overflow;
  `std::bad_alloc` when the pool is exhausted.
- `deallocate(ptr, n)` forwards to the underlying allocator's `deallocate`.
- `rebind` and the rebind copy-constructor let node-based containers
  (`std::list`, `std::unordered_map`) work correctly.
- Two `CustomAllocator` instances compare equal iff they point to the same
  underlying pool.

---

## Fragmentation management

The Free-List allocator uses two mechanisms:

1. **Splitting** — when a free block is larger than needed by at least
   `sizeof(BlockHeader) + 1` bytes, it is divided into the allocated portion
   and a new, smaller free block. This avoids wasting the remainder.

2. **Coalescing** — `deallocate` immediately checks both the preceding and
   following pool-order blocks. If either is free it is merged into a single
   larger block. After freeing all allocations the pool coalesces back to a
   single free region.

The `fragmentation_benchmark` measures this behaviour under a mixed-lifetime
workload and reports pool utilisation %, largest contiguous free block, and
the metric `1 - (largest_free / total_free)`.

---

## Build instructions

**Requirements:** CMake ≥ 3.20, a C++20 compiler (GCC ≥ 11 or Clang ≥ 13),
Ninja (for the sanitizer preset), and internet access (CMake fetches
GoogleTest v1.14.0, Google Benchmark v1.8.3, and RapidCheck on first build).

```bash
# Configure with AddressSanitizer + UBSan (recommended for development)
cmake --preset debug-sanitizers

# Build everything
cmake --build build/debug-sanitizers

# Or build a specific target
cmake --build build/debug-sanitizers --target memory_tests
cmake --build build/debug-sanitizers --target memory_benchmarks
```

For a plain release build without a preset:

```bash
cmake -B build/release -DCMAKE_BUILD_TYPE=Release
cmake --build build/release
```

---

## Running tests

```bash
# Run all tests via CTest (from the build directory)
ctest --preset debug-sanitizers --output-on-failure

# Or run the test binary directly
./build/debug-sanitizers/memory_tests
```

The test suite includes:
- Property-based tests (RapidCheck + GoogleTest) for alignment invariants
- Unit tests for Arena, FreeList, Slab, and the STL adapter
- A stress test of 100,000+ randomised FreeList alloc/dealloc operations with
  overlap detection and full-coalesce verification

---

## Running benchmarks

```bash
# Release build recommended for benchmarks (no sanitizer overhead)
cmake -B build/release -DCMAKE_BUILD_TYPE=Release
cmake --build build/release --target memory_benchmarks
./build/release/memory_benchmarks
```

Available benchmark suites:

| Binary name | What it measures |
|---|---|
| `BM_Malloc_Alloc` / `BM_New_Alloc` | System allocator baseline |
| `BM_Arena_Alloc` | Arena bump-pointer throughput |
| `BM_FreeList_AllocDealloc` | FreeList alloc + dealloc throughput |
| `BM_Vector_StdAlloc` vs `BM_Vector_CustomAlloc_Arena` | Container push-back |
| `BM_FreeList_Fragmentation` | Pool utilisation & fragmentation metrics |
| `BM_Arena_Sequential` vs `BM_Arena_Random` | Cache-locality comparison |

Benchmark results with measured numbers will be added here once benchmarks
have been run on the target hardware. Performance claims in this document are
architecture-dependent and supported by benchmark output, not theoretical.

---

## Known limitations and trade-offs

- **No thread safety.** All three allocators are single-threaded. Concurrent
  access requires external synchronisation (see optional task 10).
- **No individual deallocation in Arena.** Objects must either share a
  lifetime or be moved to FreeList. `deallocate` on Arena is a no-op.
- **FreeList is O(n) on allocation.** First-Fit scans the pool in order.
  Under heavy fragmentation with many small holes this degrades. A production
  variant would maintain a separate free-list pointer chain.
- **Fixed pool size.** Pools are sized at construction and cannot grow.
  Exceeding capacity returns `nullptr`; the caller must handle OOM.
- **BlockHeader overhead.** Each FreeList allocation costs 32 bytes of header
  metadata. For allocations smaller than ~64 bytes this is a significant
  relative overhead.
- **Slab requires uniform object size.** Mixing object sizes requires multiple
  Slab instances or switching to FreeList.
- **`std::malloc` used for pool acquisition.** The initial pool is obtained
  from the system allocator. `mmap` could be used instead for large pools to
  avoid competing with the general heap.
