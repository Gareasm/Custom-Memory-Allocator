# Implementation Plan: Custom Memory Manager

## Overview

Implement a C++20 custom memory management library in 8 phases: Arena allocator, Free-List allocator with splitting/coalescing, Slab allocator, STL adapter + MemoryManager, comprehensive tests with sanitizer support, benchmarks, optional thread safety, and documentation.

## Tasks

- [x] 1. CMake skeleton and build system
  - Create `CMakeLists.txt` with C++20 standard, `memory` static library target linking `src/*.cpp`
  - Add `FetchContent` declarations for GoogleTest v1.14.0, Google Benchmark v1.8.3, and RapidCheck (master)
  - Define `memory_tests` executable target linking GoogleTest + RapidCheck against `memory`
  - Define `memory_benchmarks` executable target linking Google Benchmark against `memory`
  - Add `CMakePresets.json` with a `debug-sanitizers` preset setting `-fsanitize=address,undefined -fno-omit-frame-pointer -g`
  - _Requirements: 17.1, 17.2, 17.3, 17.4, 17.5_

- [x] 2. Arena allocator
  - [x] 2.1 Implement `include/memory/arena.hpp` and `src/arena.cpp`
    - Declare `Arena` class: `pool_` (`std::byte*`), `capacity_`, `offset_` fields; non-copyable, movable
    - Implement constructor allocating pool via `std::malloc`; destructor freeing pool
    - Implement move constructor/assignment (transfer ownership, null out source)
    - Implement `allocate(size, alignment)`: guard `size==0`, non-power-of-two alignment, use `std::align` for aligned pointer, overflow-safe capacity check, advance `offset_`
    - Implement `deallocate` as no-op; `reset()` sets `offset_=0`
    - Implement `capacity()`, `used()`, `remaining()` satisfying `used()+remaining()==capacity()`
    - _Requirements: 1.1–1.10, 2.1–2.4, 8.1–8.4, 9.1–9.6_

  - [x] 2.2 Write property test for Arena allocate alignment
    - **Property 1: For any valid (size, alignment) pair where alignment is a power of two and the allocation succeeds, the returned pointer address is divisible by alignment**
    - **Validates: Requirements 2.1, 8.1, 8.2**

  - [ ]* 2.3 Write property test for Arena capacity invariant
    - **Property 2: After any sequence of allocations, `used() + remaining() == capacity()` holds**
    - **Validates: Requirements 1.8**

  - [ ]* 2.4 Write unit tests for Arena (`tests/arena_tests.cpp`)
    - Single allocation, multiple sequential allocations
    - Alignment correctness for 8/16/32/64 bytes
    - OOM returns `nullptr`; zero-size returns `nullptr`
    - Non-power-of-two alignment returns `nullptr`
    - `reset()` makes full pool available; post-reset allocation succeeds
    - Large single allocation up to pool capacity
    - _Requirements: 10.1–10.4_

- [x] 3. Free-List allocator
  - [x] 3.1 Implement `include/memory/free_list.hpp` and `src/free_list.cpp`
    - Declare `FreeList` class with embedded `BlockHeader` struct (`size`, `is_free`, `_pad[7]`, `prev*`, `next*` — 32 bytes on 64-bit)
    - Constructor: allocate pool via `std::malloc`, initialize as single free `BlockHeader` spanning entire pool
    - Destructor: free pool; move constructor/assignment transfer ownership
    - Implement `find_first_fit(needed)`: linear scan of pool-order blocks filtering `is_free`
    - Implement `split(block, size)`: carve remainder into new `BlockHeader` if remainder ≥ `sizeof(BlockHeader) + 1`; link into pool-order chain
    - Implement `coalesce(block)`: merge with next if free, then merge with prev if free
    - Implement `allocate(size, alignment)`: guard `size==0`, non-power-of-two alignment; find first fit; split; mark allocated; return user pointer
    - Implement `deallocate(ptr)`: compute header via `ptr - sizeof(BlockHeader)`; assert ptr in pool range (debug); mark free; call `coalesce`
    - Implement `capacity()`, `used()`, `remaining()`
    - _Requirements: 3.1–3.8, 4.1–4.5, 8.1–8.4, 9.1–9.6_

  - [ ]* 3.2 Write property test for FreeList no-overlap
    - **Property 3: For any sequence of allocations that all succeed, no two live allocations overlap in address range**
    - **Validates: Requirements 3.2, 9.1**

  - [ ]* 3.3 Write property test for FreeList full coalesce
    - **Property 4: After allocating arbitrary blocks and freeing all of them, the free list coalesces to a single block whose usable size equals `capacity - sizeof(BlockHeader)`**
    - **Validates: Requirements 4.4**

  - [ ]* 3.4 Write property test for FreeList split correctness
    - **Property 5: When a block is split, the sum of the two resulting blocks' usable sizes plus one `sizeof(BlockHeader)` equals the original block's usable size**
    - **Validates: Requirements 4.1, 4.2**

  - [ ]* 3.5 Write unit tests for FreeList (`tests/free_list_tests.cpp`)
    - Basic allocate/deallocate; block reuse after free
    - First-Fit selection (verify block chosen is the first fitting one in pool order)
    - Splitting: remainder goes back on free list; subsequent smaller allocation succeeds
    - Coalescing: freeing all blocks produces single region ≥ pool − header overhead
    - Varied sizes and interleaved alloc/free order
    - OOM returns `nullptr`; zero-size returns `nullptr`
    - Alignment correctness for 8/16/32/64 bytes
    - _Requirements: 11.1–11.4_

- [x] 4. Checkpoint — ensure Arena and FreeList tests pass
  - Ensure all tests pass, ask the user if questions arise.

- [x] 5. Slab allocator
  - [x] 5.1 Implement `include/memory/slab.hpp` and `src/slab.cpp`
    - Declare `Slab` class: `pool_`, `slot_size_`, `capacity_` (object count), `used_`, `free_head_` (intrusive list)
    - Constructor: compute `slot_size_ = std::max(object_size, sizeof(void*))` rounded up to alignment; allocate pool; initialize intrusive free-slot list (each free slot stores pointer to next)
    - Implement `allocate()`: pop `free_head_`, advance `used_`, return slot (O(1)); return `nullptr` when full
    - Implement `deallocate(ptr)`: assert ptr in pool range (debug); push onto `free_head_`, decrement `used_` (O(1))
    - Implement `capacity()`, `used()`
    - _Requirements: 5.1–5.7, 8.1–8.2, 9.4_

  - [ ]* 5.2 Write property test for Slab O(1) reuse
    - **Property 6: For any sequence of Slab allocate/deallocate operations that stays within capacity, every allocation returns a pointer aligned to the configured alignment**
    - **Validates: Requirements 5.2, 5.7**

  - [ ]* 5.3 Write unit tests for Slab (`tests/slab_tests.cpp`)
    - Basic slot allocation and deallocation
    - Slot reuse after deallocation (same pointer returned)
    - Full slab returns `nullptr`; empty freshly constructed slab works
    - Alignment of returned slot pointers
    - _Requirements: 12.1–12.2_

- [x] 6. STL allocator adapter and MemoryManager
  - [x] 6.1 Implement `include/memory/allocator.hpp` — `CustomAllocator<T, Alloc>`
    - Template on `T` and `Alloc`; store `Alloc* alloc_`
    - Implement `allocate(n)`: overflow check → `throw std::bad_array_new_length`; call `alloc_->allocate(n*sizeof(T), alignof(T))`; nullptr → `throw std::bad_alloc`; return `static_cast<T*>(p)`
    - Implement `deallocate(ptr, n)`: forward to `alloc_->deallocate(ptr, n*sizeof(T))`
    - Implement copy constructor from `CustomAllocator<U, Alloc>` (rebind); `operator==`/`!=`
    - Add `rebind` nested struct
    - _Requirements: 6.1–6.7_

  - [x] 6.2 Implement `include/memory/memory_manager.hpp` and `src/memory_manager.cpp` — `MemoryManager`
    - Own `Arena arena_` and `FreeList free_list_` by value (RAII)
    - Constructor takes `arena_capacity` and `free_list_capacity`, initialises both members
    - Expose `arena()` and `freeList()` returning non-const references
    - _Requirements: 7.1–7.4_

  - [ ]* 6.3 Write unit tests for CustomAllocator and MemoryManager (`tests/allocator_tests.cpp`)
    - `std::vector<int, CustomAllocator<int, Arena>>` push/access/erase round-trip equals `std::vector<int>`
    - `std::list` or `std::unordered_map` round-trip with `CustomAllocator`
    - Overflow `n` throws `std::bad_array_new_length`
    - Underlying OOM (tiny pool) throws `std::bad_alloc`
    - `MemoryManager` constructs both allocators; accessors return usable references
    - _Requirements: 13.1–13.4_

- [x] 7. Stress test (`tests/stress_tests.cpp`)
  - Implement ≥100,000 randomised allocate/deallocate operations against `FreeList`
  - Track live allocations in a `std::vector<{ptr, size}>` reference structure; assert no overlapping regions on each allocation
  - On deallocation, remove from tracking and verify subsequent allocation can reuse the freed range
  - After all operations, free remaining live allocations and assert the coalesced free region ≥ pool − header overhead
  - Compile cleanly under ASan + UBSan (no suppression)
  - _Requirements: 14.1–14.6_

- [ ] 8. Checkpoint — ensure all unit and stress tests pass under sanitizers
  - Ensure all tests pass, ask the user if questions arise.

- [x] 9. Benchmarks
  - [x] 9.1 Implement `benchmarks/allocation_benchmark.cpp`
    - Register Google Benchmark cases for `malloc`/`free`, `new`/`delete`, `Arena`, `FreeList` at 1e5/1e6/1e7 ops
    - Exclude pool construction from timed region using `benchmark::DoNotOptimize`
    - Report elapsed time, allocations/sec, ns/allocation, ns/deallocation
    - _Requirements: 15.1–15.4_

  - [x] 9.2 Implement `benchmarks/container_benchmark.cpp`
    - Compare `std::vector<int, std::allocator<int>>` vs `std::vector<int, CustomAllocator<int, Arena>>` push-back throughput
    - _Requirements: 16.1_

  - [x] 9.3 Implement `benchmarks/fragmentation_benchmark.cpp`
    - Simulate mixed-lifetime workload (interleaved alloc/free of varying sizes) using `FreeList`
    - Report: pool utilization %, largest contiguous free block, fragmentation metric `1 - (largest_free / total_free)`
    - _Requirements: 16.2_

  - [x] 9.4 Implement `benchmarks/cache_benchmark.cpp`
    - Compare sequential vs random access timing on arrays allocated from `Arena`
    - _Requirements: 16.3_

- [ ]* 10. Optional: thread-safe and thread-local allocator extensions
  - [ ] 10.1 Add mutex-guarded wrappers (`ThreadSafeArena`, `ThreadSafeFreeList`) in new header files under `include/memory/`
    - Wrap `allocate`/`deallocate`/`reset` with `std::mutex` lock guards
    - Match the same interface so `CustomAllocator` can be templated on these wrappers

  - [ ]* 10.2 Write unit tests for thread-safe wrappers
    - Launch ≥4 threads concurrently allocating and deallocating from the same `ThreadSafeFreeList`
    - Assert no overlapping allocations across threads using an atomic reference tracker

- [x] 11. README and architecture documentation
  - Populate `README.md` with: project purpose, allocator summaries, alignment explanation, STL adapter usage, fragmentation management, build instructions (`cmake --preset debug-sanitizers`), test/benchmark commands, known limitations
  - Populate `docs/architecture.md` with ASCII memory layout diagrams for Arena pool, FreeList block header chain, and Slab slot layout (copy/adapt from design document)
  - _Requirements: 18.1–18.4_

- [ ] 12. Final checkpoint — all tests pass, benchmarks build
  - Ensure all tests pass, ask the user if questions arise.

## Notes

- Tasks marked with `*` are optional and can be skipped for a faster MVP
- Property tests use RapidCheck `rc::prop` integrated into GoogleTest; tag each with `// Feature: custom-memory-manager, Property N: <text>`
- The `debug-sanitizers` CMake preset should be used when running tests to catch memory errors early
- All allocators must never call `new`/`delete`/`malloc`/`free` after the initial pool construction
