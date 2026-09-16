# Design Document: Custom Memory Manager

## Overview

This library provides two core allocator strategies — Arena and Free-List — plus an optional Slab allocator, a C++ standard library adapter, and a top-level `MemoryManager` owner. The design targets C++20, correctness-first, with measurable performance and zero external dynamic allocation inside the allocators themselves after pool acquisition.

The primary educational goal is to demonstrate how low-level memory strategies differ in trade-offs: the Arena sacrifices individual deallocation for throughput; the Free-List trades some throughput for flexibility; the Slab achieves both for fixed-size objects. The STL adapter bridges these strategies into standard containers.

All allocators operate on a pre-allocated contiguous pool obtained once at construction. No `new`/`delete` or `malloc`/`free` occurs inside the hot path.

---

## Architecture

```
┌─────────────────────────────────────────────────────┐
│                   MemoryManager                     │
│  ┌─────────────────────┐  ┌────────────────────┐    │
│  │       Arena         │  │     FreeList       │    │
│  │  (bump allocator)   │  │ (first-fit + coale)│    │
│  └─────────────────────┘  └────────────────────┘    │
└─────────────────────────────────────────────────────┘
             │                        │
  ┌──────────▼──────────┐   ┌─────────▼──────────┐
  │   CustomAllocator   │   │  CustomAllocator   │
  │   <T, Arena&>       │   │  <T, FreeList&>    │
  └─────────────────────┘   └────────────────────┘
             │
  ┌──────────▼──────────┐
  │   std::vector/list/ │
  │   unordered_map ... │
  └─────────────────────┘

  Optional:
  ┌──────────────────────┐
  │        Slab          │
  │ (fixed-size, O(1))   │
  └──────────────────────┘
```

### Key Architectural Decisions

1. **Policy-based, not virtual dispatch.** Allocators are concrete classes. `CustomAllocator<T, Alloc>` is templated on the underlying allocator type, giving zero-overhead abstraction.
2. **Metadata lives inside the pool.** `Block_Header` records are embedded at the start of each managed region; no external bookkeeping is needed.
3. **RAII throughout.** Each allocator owns its pool; the pool is released in the destructor. `MemoryManager` owns allocator instances.
4. **Sanitizer-clean.** Pointer arithmetic uses `std::byte*` to avoid aliasing violations; `std::align` is used where applicable.

---

## Components and Interfaces

### Arena

```cpp
namespace memory {

class Arena {
public:
    explicit Arena(std::size_t capacity);
    ~Arena();

    // Non-copyable, movable
    Arena(const Arena&) = delete;
    Arena& operator=(const Arena&) = delete;
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
    std::byte*   pool_     = nullptr;
    std::size_t  capacity_ = 0;
    std::size_t  offset_   = 0;   // bump pointer as byte offset
};

} // namespace memory
```

### FreeList

```cpp
namespace memory {

class FreeList {
public:
    explicit FreeList(std::size_t capacity);
    ~FreeList();

    FreeList(const FreeList&) = delete;
    FreeList& operator=(const FreeList&) = delete;
    FreeList(FreeList&&) noexcept;
    FreeList& operator=(FreeList&&) noexcept;

    [[nodiscard]] void* allocate(std::size_t size,
                                 std::size_t alignment = alignof(std::max_align_t)) noexcept;
    void deallocate(void* ptr) noexcept;

    [[nodiscard]] std::size_t capacity()  const noexcept;
    [[nodiscard]] std::size_t used()      const noexcept;
    [[nodiscard]] std::size_t remaining() const noexcept;

private:
    struct BlockHeader;              // defined in .cpp, layout below
    std::byte*   pool_     = nullptr;
    std::size_t  capacity_ = 0;

    BlockHeader* first_free_ = nullptr;  // head of free list

    // Internal helpers
    BlockHeader* find_first_fit(std::size_t needed) noexcept;
    void         split(BlockHeader* block, std::size_t size) noexcept;
    void         coalesce(BlockHeader* block) noexcept;
    static bool  is_power_of_two(std::size_t v) noexcept;
};

} // namespace memory
```

### Slab

```cpp
namespace memory {

class Slab {
public:
    Slab(std::size_t object_size, std::size_t alignment, std::size_t capacity);
    ~Slab();

    Slab(const Slab&) = delete;
    Slab& operator=(const Slab&) = delete;

    [[nodiscard]] void* allocate()  noexcept;
    void deallocate(void* ptr)      noexcept;

    [[nodiscard]] std::size_t capacity() const noexcept;  // max objects
    [[nodiscard]] std::size_t used()     const noexcept;

private:
    std::byte*   pool_        = nullptr;
    std::size_t  slot_size_   = 0;  // padded to alignment
    std::size_t  capacity_    = 0;  // object count
    std::size_t  used_        = 0;
    void**       free_head_   = nullptr;  // intrusive free-slot list
};

} // namespace memory
```

### CustomAllocator

```cpp
namespace memory {

template<typename T, typename Alloc>
class CustomAllocator {
public:
    using value_type = T;

    explicit CustomAllocator(Alloc& alloc) noexcept : alloc_(&alloc) {}

    template<typename U>
    CustomAllocator(const CustomAllocator<U, Alloc>& other) noexcept
        : alloc_(other.alloc_) {}

    [[nodiscard]] T* allocate(std::size_t n);
    void deallocate(T* ptr, std::size_t n) noexcept;

    template<typename U>
    struct rebind { using other = CustomAllocator<U, Alloc>; };

    bool operator==(const CustomAllocator& o) const noexcept { return alloc_ == o.alloc_; }
    bool operator!=(const CustomAllocator& o) const noexcept { return !(*this == o); }

    // Allow rebind copy to access alloc_
    template<typename U, typename A> friend class CustomAllocator;

private:
    Alloc* alloc_;
};

} // namespace memory
```

### MemoryManager

```cpp
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
```

---

## Data Models

### Arena Pool Layout

```
┌──────────────────────────────────────────────────────────┐
│                    Arena Pool                            │
│  ┌──────────┬──────────┬─────────┬─────────┬──────────┐ │
│  │ alloc 0  │ padding  │ alloc 1 │ alloc 2 │  free    │ │
│  │ (n bytes)│(alignment│(m bytes)│(k bytes)│          │ │
│  └──────────┴──────────┴─────────┴─────────┴──────────┘ │
│  ^                                         ^            ^ │
│  pool_                              offset_            pool_+capacity_
└──────────────────────────────────────────────────────────┘
```

`offset_` is the bump pointer (byte count from `pool_`). On each `allocate`:
1. Compute aligned start = align_up(pool_ + offset_, alignment)
2. Check aligned_start + size <= pool_ + capacity_
3. Advance offset_ = (aligned_start - pool_) + size
4. Return aligned_start

### FreeList Pool Layout

Each region starts with an embedded `BlockHeader`, followed immediately by usable memory:

```
┌─────────────────────────────────────────────────────────┐
│                   FreeList Pool                         │
│  ┌────────────────┬──────────────────────────────────┐  │
│  │  BlockHeader   │    usable memory (size bytes)    │  │
│  │  (32 bytes)    │                                  │  │
│  └────────────────┴──────────────────────────────────┘  │
│  ┌────────────────┬──────────────────────────────────┐  │
│  │  BlockHeader   │    ...                           │  │
│  └────────────────┴──────────────────────────────────┘  │
└─────────────────────────────────────────────────────────┘
```

#### BlockHeader Structure

```cpp
struct FreeList::BlockHeader {
    std::size_t  size;      // bytes of usable payload (not including header)
    bool         is_free;
    std::uint8_t _pad[7];   // explicit padding for determinism
    BlockHeader* prev;      // previous block in pool order (nullptr = first)
    BlockHeader* next;      // next block in pool order (nullptr = last)
};
// sizeof(BlockHeader) == 32 on 64-bit systems (size:8 + is_free+pad:8 + prev:8 + next:8)
```

The `next`/`prev` pointers navigate pool-order (physical adjacency), not the free list. The free list is walked by scanning `next` pointers filtering for `is_free == true`. This simplifies coalescing.

#### Pointer to usable memory from header:
```cpp
void* user_ptr(BlockHeader* h) { return reinterpret_cast<std::byte*>(h) + sizeof(BlockHeader); }
BlockHeader* header_of(void* p)  { return reinterpret_cast<BlockHeader*>(static_cast<std::byte*>(p) - sizeof(BlockHeader)); }
```

### Slab Pool Layout

```
┌─────────────────────────────────────────────────────────┐
│                     Slab Pool                           │
│  ┌──────────┬──────────┬──────────┬──────────────────┐  │
│  │  slot 0  │  slot 1  │  slot 2  │   ...  slot N-1  │  │
│  │ (aligned │          │          │                  │  │
│  │slot_size)│          │          │                  │  │
│  └──────────┴──────────┴──────────┴──────────────────┘  │
│   ^                                                     │
│  free_head_ points into free slots; each free slot's    │
│  first sizeof(void*) bytes store pointer to next free   │
└─────────────────────────────────────────────────────────┘
```

Free slots embed the intrusive list pointer in the slot itself (valid because the slot is not in use). `slot_size_` is `std::max(object_size, sizeof(void*))` rounded up to `alignment`.

---

## Algorithms

### Arena: allocate(size, alignment)

```
1. If size == 0 → return nullptr
2. If !is_power_of_two(alignment) → return nullptr
3. current = pool_ + offset_
4. space = capacity_ - offset_
5. aligned_ptr = std::align(alignment, size, current, space)
   // std::align adjusts current to first aligned address; space is reduced by padding
6. If aligned_ptr == nullptr (would overflow or insufficient) → return nullptr
7. offset_ = (static_cast<std::byte*>(aligned_ptr) - pool_) + size
8. return aligned_ptr
```

### FreeList: allocate(size, alignment)

```
1. If size == 0 → return nullptr
2. If !is_power_of_two(alignment) → return nullptr
3. needed = size  (alignment handled by adjusting within block's usable region)
4. Walk free list (pool-order scan of is_free blocks):
   a. For each free BlockHeader h:
      - candidate = user_ptr(h)  (just past header)
      - Compute aligned = align_up(candidate, alignment)
      - adjustment = aligned - candidate
      - total_needed = size + adjustment
      - If h->size >= total_needed:
          - If adjustment > 0: shuffle header to accommodate alignment padding
            (handled by treating adjusted region as the allocation start)
          - split(h, total_needed) if remainder large enough
          - Mark h->is_free = false
          - return aligned
5. return nullptr (no fit found)
```

> Note: Alignment within a free-list block is handled by searching for blocks where the aligned start plus size fits within the block's payload. For simplicity in the initial implementation, `allocate` always aligns to the default `alignof(std::max_align_t)` for block start addresses (since `BlockHeader` is always at a known alignment), and extra per-call alignment is achieved by adjusting the returned pointer within the block.

### FreeList: split(block, needed_size)

```
MIN_SPLIT = sizeof(BlockHeader) + 1

If block->size - needed_size >= sizeof(BlockHeader) + MIN_SPLIT:
    new_header = reinterpret_cast<BlockHeader*>(
        reinterpret_cast<std::byte*>(block) + sizeof(BlockHeader) + needed_size)
    new_header->size    = block->size - needed_size - sizeof(BlockHeader)
    new_header->is_free = true
    new_header->prev    = block
    new_header->next    = block->next

    If block->next:
        block->next->prev = new_header
    block->next = new_header
    block->size = needed_size
```

### FreeList: coalesce(block)

```
// Merge with next if free
if block->next && block->next->is_free:
    absorbed = block->next
    block->size += sizeof(BlockHeader) + absorbed->size
    block->next  = absorbed->next
    if absorbed->next:
        absorbed->next->prev = block

// Merge with prev if free
if block->prev && block->prev->is_free:
    pred = block->prev
    pred->size += sizeof(BlockHeader) + block->size
    pred->next  = block->next
    if block->next:
        block->next->prev = pred
```

### Slab: allocate / deallocate

```
allocate():
  if free_head_ == nullptr → return nullptr
  slot = free_head_
  free_head_ = *reinterpret_cast<void**>(slot)
  ++used_
  return slot

deallocate(ptr):
  assert ptr in [pool_, pool_ + capacity_ * slot_size_)
  *reinterpret_cast<void**>(ptr) = free_head_
  free_head_ = ptr
  --used_
```

### CustomAllocator: allocate(n)

```
if n > std::numeric_limits<std::size_t>::max() / sizeof(T):
    throw std::bad_array_new_length{}
void* p = alloc_->allocate(n * sizeof(T), alignof(T))
if p == nullptr:
    throw std::bad_alloc{}
return static_cast<T*>(p)
```

---

## Error Handling

| Condition | Arena | FreeList | Slab | CustomAllocator |
|---|---|---|---|---|
| size == 0 | nullptr | nullptr | — | bad_array_new_length or nullptr |
| OOM | nullptr | nullptr | nullptr | throws bad_alloc |
| alignment not power-of-two | nullptr | nullptr | — (fixed at construction) | — |
| alignment == 0 | nullptr | nullptr | — | — |
| deallocate out-of-range ptr | no-op | assert+no-op (debug) | assert (debug) | — |
| size*sizeof(T) overflow | — | — | — | throws bad_array_new_length |
| integer overflow in padding calc | nullptr | nullptr | — | — |

All overflow checks use the pattern `if (size > capacity_ - offset_)` (unsigned subtraction is safe after verifying `offset_ <= capacity_`), avoiding signed overflow UB. Alignment padding overflow uses `if (padding > std::numeric_limits<std::size_t>::max() - size)`.

---

## Testing Strategy

### Dual Testing Approach

Both unit tests and property-based tests are required. They are complementary:
- Unit tests cover known examples, edge cases, and error conditions.
- Property-based tests verify universal invariants across randomly generated inputs.

### Unit Testing (GoogleTest)

Each allocator has a dedicated test file. Tests cover:
- **Arena**: single allocation, sequential allocations, alignment (8/16/32/64), OOM, reset, post-reset allocation, zero-size allocation.
- **FreeList**: basic alloc/dealloc, block reuse, First-Fit selection, splitting, coalescing, varied sizes, interleaved order, OOM, alignment.
- **Slab**: basic slot alloc/dealloc, reuse, full-slab nullptr, empty slab, alignment.
- **CustomAllocator**: `std::vector` round-trip, `std::list` or `std::unordered_map` round-trip, overflow throws.
- **Stress**: 100,000+ randomized FreeList operations, overlap detection, full coalesce verification.

### Property-Based Testing

The library uses **[RapidCheck](https://github.com/emil-e/rapidcheck)** (header-only, CMake-fetchable) for C++ property-based testing. Each property test runs a minimum of 100 iterations.

Each test is tagged with a comment in the form:
```
// Feature: custom-memory-manager, Property N: <property text>
```

### Build Targets

```
ctest            → runs all unit + stress tests
ctest -R bench   → runs benchmarks (separate target)
```

### Benchmark Suite (Google Benchmark)

| Benchmark | Measures |
|---|---|
| `allocation_benchmark` | malloc/free vs new/delete vs Arena vs FreeList throughput at 1e5, 1e6, 1e7 ops |
| `container_benchmark` | std::vector with std::allocator vs CustomAllocator push-back throughput |
| `fragmentation_benchmark` | pool utilization %, largest contiguous free block, fragmentation metric |
| `cache_benchmark` | sequential vs random access timing on allocated arrays |

Benchmarks exclude setup (pool construction) from the timed region using `benchmark::DoNotOptimize` and state iteration.

---

## CMake Build System Design

```
cmake_minimum_required(VERSION 3.20)
project(custom-memory-manager CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
```

### Targets

| Target | Type | Description |
|---|---|---|
| `memory` | STATIC library | Core library (arena, free_list, slab, memory_manager) |
| `memory_tests` | Executable | GoogleTest suite (all unit + stress tests) |
| `memory_benchmarks` | Executable | Google Benchmark suite |

### Sanitizer Preset

A CMake preset `debug-sanitizers` adds:
```
-fsanitize=address,undefined
-fno-omit-frame-pointer
-g
```

### FetchContent

```cmake
include(FetchContent)

FetchContent_Declare(googletest
  GIT_REPOSITORY https://github.com/google/googletest.git
  GIT_TAG        v1.14.0)

FetchContent_Declare(benchmark
  GIT_REPOSITORY https://github.com/google/benchmark.git
  GIT_TAG        v1.8.3)

FetchContent_Declare(rapidcheck
  GIT_REPOSITORY https://github.com/emil-e/rapidcheck.git
  GIT_TAG        master)
```

---

## Key Design Decisions and Tradeoffs

### 1. Pool-order linked list in FreeList (not a separate free list)

Using `prev`/`next` pointers in pool order (not free-list order) simplifies coalescing dramatically: adjacent blocks are always `header->next` and `header->prev`. The cost is that first-fit requires scanning all blocks, not just free ones. For an educational library this is acceptable; a production variant would maintain a separate free-list pointer chain.

### 2. BlockHeader embedded at block start (not end)

Storing the header at the start of each block means `header_of(ptr) = ptr - sizeof(BlockHeader)` is a simple subtraction. End-of-block tags (boundary tags) would improve coalescing speed but double the header overhead.

### 3. Arena uses byte offset, not raw pointer

Storing `offset_` as a `std::size_t` rather than a `std::byte*` avoids pointer arithmetic producing values outside the allocation range (UB), and makes serialization of state trivial.

### 4. CustomAllocator templated on Alloc type (not virtual interface)

Virtual dispatch would add indirection on every allocation. The template approach is zero-cost and allows the compiler to inline the underlying allocator call. The tradeoff is that `CustomAllocator<T, Arena>` and `CustomAllocator<T, FreeList>` are distinct types.

### 5. Slab intrusive free list

Using the slot memory itself to store the next-free pointer (when free) eliminates all external bookkeeping. The constraint is that `slot_size >= sizeof(void*)`, which is always true for any practically useful object.

### 6. RapidCheck for property-based testing

RapidCheck is well-maintained, header-only (mostly), and uses a shrinking strategy similar to Haskell's QuickCheck. It integrates with GoogleTest via `rc::prop`, allowing property tests to live alongside unit tests in the same binary.

