# Requirements Document

## Introduction

A custom C++ memory management library for educational purposes, demonstrating low-level memory allocation strategies over pre-allocated pools. The library implements two core allocators — an Arena (bump) allocator and a Free-List allocator — with an optional Slab allocator extension, a C++ standard library adapter, comprehensive tests, and benchmarks. The goal is correctness, measurable performance, and clean architecture using modern C++20.

## Glossary

- **Arena**: A contiguous pre-allocated memory region with a bump pointer for O(1) sequential allocation and bulk reset.
- **Free_List**: An allocator that tracks freed blocks inside the managed pool, supporting arbitrary allocation/deallocation order, block splitting, and coalescing.
- **Slab**: An allocator optimized for fixed-size objects using a free-slot linked list within a pre-allocated pool.
- **MemoryManager**: The top-level owner that creates and exposes Arena and Free_List allocator instances.
- **CustomAllocator**: A C++ standard library-compatible allocator adapter wrapping an underlying allocator strategy.
- **Block_Header**: Metadata stored inside the managed pool describing a free-list block's size, free status, and navigation links.
- **Bump_Pointer**: The current offset into the Arena pool, advanced on each allocation.
- **Coalescing**: Merging adjacent free blocks in the Free_List to reduce fragmentation.
- **Splitting**: Dividing a free block into an allocated portion and a smaller free remainder.
- **Pool**: The backing memory region owned by an allocator, obtained via `std::malloc` or `mmap`.
- **Alignment**: The byte boundary to which an allocation's start address must be a multiple.
- **OOM**: Out-of-memory condition when the pool cannot satisfy an allocation request.
- **ASan**: AddressSanitizer, a runtime memory error detector.
- **UBSan**: UndefinedBehaviorSanitizer, a runtime undefined-behavior detector.

---

## Requirements

### Requirement 1: Arena Allocator — Core Allocation

**User Story:** As a developer, I want an Arena allocator that allocates memory from a pre-allocated pool with O(1) cost, so that I can efficiently allocate short-lived objects without per-object overhead.

#### Acceptance Criteria

1. WHEN `Arena` is constructed with a capacity in bytes, THE `Arena` SHALL pre-allocate a contiguous backing pool of exactly that capacity using `std::malloc` or `mmap`.
2. WHEN `Arena::allocate(size, alignment)` is called with a valid size and alignment, THE `Arena` SHALL return a pointer whose address is a multiple of `alignment` and whose allocation does not overlap any prior live allocation.
3. WHEN `Arena::allocate` is called with `alignment` not provided, THE `Arena` SHALL default to `alignof(std::max_align_t)`.
4. WHEN `Arena::allocate` is called and the requested size plus alignment padding would exceed the remaining capacity, THE `Arena` SHALL return `nullptr`.
5. WHEN `Arena::allocate` is called with `size == 0`, THE `Arena` SHALL return `nullptr`.
6. WHEN `Arena::deallocate(ptr)` is called, THE `Arena` SHALL perform no action (bulk deallocation only).
7. WHEN `Arena::reset()` is called, THE `Arena` SHALL reset the bump pointer to the start of the pool, making all previously allocated memory available for reuse.
8. THE `Arena` SHALL expose `capacity()`, `used()`, and `remaining()` returning the total pool size, bytes consumed, and bytes available respectively, satisfying `used() + remaining() == capacity()` at all times.
9. WHEN the `Arena` destructor runs, THE `Arena` SHALL release the backing pool memory.
10. THE `Arena` SHALL NOT perform any dynamic allocation inside the allocator itself beyond the initial pool acquisition.

---

### Requirement 2: Arena Allocator — Alignment

**User Story:** As a developer, I want the Arena allocator to support arbitrary power-of-two alignments, so that I can allocate SIMD buffers, cache-line-aligned data, and hardware-constrained structures.

#### Acceptance Criteria

1. WHEN `Arena::allocate(size, alignment)` is called with `alignment` equal to 8, 16, 32, or 64, THE `Arena` SHALL return a pointer whose numeric address is divisible by that alignment value.
2. WHEN `Arena::allocate` is called with an `alignment` that is not a power of two, THE `Arena` SHALL return `nullptr`.
3. WHEN alignment padding is required to satisfy an alignment request, THE `Arena` SHALL advance the bump pointer past the padding bytes before returning the allocation.
4. THE `Arena` SHALL use `std::align` or equivalent pointer arithmetic to compute aligned addresses without invoking undefined behavior.

---

### Requirement 3: Free-List Allocator — Core Allocation and Deallocation

**User Story:** As a developer, I want a Free-List allocator that supports arbitrary allocation and deallocation order from a fixed pool, so that I can manage objects with varied lifetimes without using the system heap.

#### Acceptance Criteria

1. WHEN `FreeList` is constructed with a capacity in bytes, THE `FreeList` SHALL pre-allocate a contiguous backing pool of exactly that capacity and initialize it as a single free block.
2. WHEN `FreeList::allocate(size, alignment)` is called, THE `FreeList` SHALL search the free list using First-Fit strategy and return the first block large enough to satisfy the request.
3. WHEN `FreeList::allocate` is called and no free block is large enough, THE `FreeList` SHALL return `nullptr`.
4. WHEN `FreeList::allocate` is called with `size == 0`, THE `FreeList` SHALL return `nullptr`.
5. WHEN `FreeList::deallocate(ptr)` is called with a pointer previously returned by `FreeList::allocate`, THE `FreeList` SHALL mark that block as free and make it available for future allocations.
6. WHEN `FreeList::deallocate` is called and the freed block is adjacent to one or more free blocks, THE `FreeList` SHALL coalesce those blocks into a single larger free block.
7. THE `FreeList` SHALL store all `Block_Header` metadata inside the managed pool without external dynamic allocation.
8. THE `FreeList` SHALL NOT perform any dynamic allocation inside the allocator itself beyond the initial pool acquisition.

---

### Requirement 4: Free-List Allocator — Block Splitting and Coalescing

**User Story:** As a developer, I want the Free-List allocator to split oversized blocks and merge adjacent free blocks, so that the pool is used efficiently and fragmentation is minimized over time.

#### Acceptance Criteria

1. WHEN `FreeList::allocate` finds a free block larger than the requested size plus the minimum `Block_Header` overhead, THE `FreeList` SHALL split the block, allocating the requested portion and returning the remainder to the free list as a separate block.
2. WHEN `FreeList::allocate` finds a free block that is large enough but not large enough to hold a remainder block after splitting, THE `FreeList` SHALL allocate the entire block without splitting.
3. WHEN `FreeList::deallocate` is called, THE `FreeList` SHALL check both the immediately preceding and following blocks in the pool and coalesce any that are free.
4. WHEN all previously allocated blocks are freed, THE `FreeList` SHALL coalesce back to a single free block covering the entire pool (minus initial header overhead).
5. THE `Block_Header` SHALL contain at minimum: block size, free/allocated status, a pointer or offset to the next block in pool order, and a pointer or offset to the previous block in pool order.

---

### Requirement 5: Slab Allocator (Optional Extension)

**User Story:** As a developer, I want a Slab allocator for fixed-size objects, so that I can achieve O(1) allocation and deallocation for high-frequency same-size objects with minimal fragmentation.

#### Acceptance Criteria

1. WHEN `Slab` is constructed with an object size, alignment, and capacity, THE `Slab` SHALL pre-allocate a contiguous pool sufficient to hold the specified number of objects.
2. WHEN `Slab::allocate()` is called and a free slot is available, THE `Slab` SHALL return a pointer to a free slot in O(1) time by following the free-slot linked list.
3. WHEN `Slab::allocate()` is called and no free slot is available, THE `Slab` SHALL return `nullptr`.
4. WHEN `Slab::deallocate(ptr)` is called with a valid pointer returned by `Slab::allocate()`, THE `Slab` SHALL return that slot to the free-slot linked list in O(1) time.
5. WHEN `Slab::deallocate(ptr)` is called with a pointer not belonging to the slab's pool range, THE `Slab` SHALL assert or signal an error in debug builds.
6. THE `Slab` SHALL store the free-slot linked list inside the managed pool (within free slot memory) without external dynamic allocation.
7. WHEN `Slab::allocate()` returns a pointer, THE `Slab` SHALL ensure the pointer is aligned to the configured alignment.

---

### Requirement 6: C++ Standard Allocator Adapter

**User Story:** As a developer, I want a `CustomAllocator<T>` that satisfies the C++ named requirement `Allocator`, so that I can use custom memory strategies with `std::vector`, `std::list`, `std::unordered_map`, and other standard containers.

#### Acceptance Criteria

1. THE `CustomAllocator<T>` SHALL implement `allocate(n)` and `deallocate(ptr, n)` satisfying the `std::allocator_traits` interface.
2. WHEN `CustomAllocator<T>::allocate(n)` is called, THE `CustomAllocator` SHALL request `n * sizeof(T)` bytes aligned to `alignof(T)` from its underlying allocator.
3. WHEN `n * sizeof(T)` would overflow `std::size_t`, THE `CustomAllocator` SHALL throw `std::bad_array_new_length`.
4. WHEN the underlying allocator returns `nullptr` for an allocation request, THE `CustomAllocator` SHALL throw `std::bad_alloc`.
5. THE `CustomAllocator<T>` SHALL be constructible from a reference to an underlying allocator instance so that container copies share the same pool.
6. THE `CustomAllocator<T>` SHALL define `rebind` (or rely on `std::allocator_traits` rebind) so it functions correctly with node-based containers such as `std::list` and `std::unordered_map`.
7. WHEN used with `std::vector<int, CustomAllocator<int>>`, THE `CustomAllocator` SHALL produce identical observable behavior to `std::allocator<int>` for all standard vector operations.

---

### Requirement 7: MemoryManager

**User Story:** As a developer, I want a `MemoryManager` that owns and exposes both allocators through a single entry point, so that I can manage allocator lifetimes and access strategies from one place.

#### Acceptance Criteria

1. WHEN `MemoryManager` is constructed with arena capacity and free-list capacity parameters, THE `MemoryManager` SHALL construct and own an `Arena` and a `FreeList` instance with those respective capacities.
2. THE `MemoryManager` SHALL expose `arena()` and `freeList()` accessors returning references to the owned allocator instances.
3. WHEN the `MemoryManager` destructor runs, THE `MemoryManager` SHALL destroy both allocator instances, releasing their backing pools.
4. THE `MemoryManager` SHALL use RAII so that no explicit cleanup call is needed beyond normal object destruction.

---

### Requirement 8: Alignment — General

**User Story:** As a developer, I want all allocators to support configurable alignment, so that I can safely allocate memory for types with strict alignment requirements.

#### Acceptance Criteria

1. THE `Arena`, `FreeList`, and `Slab` SHALL each accept an alignment parameter on `allocate` calls and return memory satisfying that alignment.
2. WHEN an alignment of 8, 16, 32, or 64 bytes is requested, THE allocators SHALL satisfy the request if capacity allows.
3. WHEN an alignment value of 0 is passed to any allocator, THE allocator SHALL return `nullptr` or treat it as an error.
4. THE allocators SHALL NOT invoke undefined behavior when computing aligned addresses, using only standard pointer arithmetic or `std::align`.

---

### Requirement 9: Error Handling and Safety

**User Story:** As a developer, I want all allocators to handle error conditions gracefully, so that invalid inputs and pool exhaustion do not cause undefined behavior or silent data corruption.

#### Acceptance Criteria

1. WHEN any allocator receives a request for more bytes than its pool capacity, THE allocator SHALL return `nullptr` rather than accessing out-of-bounds memory.
2. WHEN any allocator receives a `size` of 0, THE allocator SHALL return `nullptr`.
3. WHEN integer overflow would occur computing the total allocation size (e.g. size + alignment padding), THE allocator SHALL return `nullptr`.
4. WHEN a `deallocate` call is made with a pointer outside the allocator's pool range, THE `FreeList` and `Slab` SHALL assert or log an error in debug builds and take no action in release builds.
5. THE allocators SHALL contain no undefined behavior as defined by the C++ standard, and SHALL be compatible with AddressSanitizer and UndefinedBehaviorSanitizer.
6. WHERE debug assertions are enabled, THE allocators SHALL assert invariants such as valid alignment values, non-null pool pointers, and consistent header metadata.

---

### Requirement 10: Unit Testing — Arena

**User Story:** As a developer, I want comprehensive unit tests for the Arena allocator, so that I can verify correctness of all allocation, alignment, capacity, and reset behaviors.

#### Acceptance Criteria

1. THE test suite SHALL include tests for: basic single allocation, multiple sequential allocations, alignment correctness for 8/16/32/64-byte alignments, capacity exhaustion returning `nullptr`, `reset()` making the full pool available again, allocation after reset succeeding, large single allocations up to pool capacity, and zero-byte allocation returning `nullptr`.
2. WHEN `Arena` alignment tests run, THE test SHALL assert that the returned pointer address modulo the requested alignment equals zero.
3. WHEN the capacity exhaustion test runs, THE test SHALL confirm that `Arena::allocate` returns `nullptr` after the pool is full.
4. THE Arena test suite SHALL be implemented using GoogleTest.

---

### Requirement 11: Unit Testing — Free-List

**User Story:** As a developer, I want comprehensive unit tests for the Free-List allocator, so that I can verify allocation, deallocation, splitting, coalescing, and edge-case behaviors.

#### Acceptance Criteria

1. THE test suite SHALL include tests for: basic allocate/deallocate, block reuse after free, First-Fit selection behavior, block splitting when a large block satisfies a smaller request, coalescing of adjacent free blocks, allocations of varied sizes, arbitrary interleaved alloc/free order, pool exhaustion, and alignment correctness.
2. WHEN the coalescing test runs, THE test SHALL verify that freeing all blocks results in a single free region covering the full pool (minus header overhead).
3. WHEN the splitting test runs, THE test SHALL verify that the unused remainder is placed back on the free list and available for a subsequent smaller allocation.
4. THE Free-List test suite SHALL be implemented using GoogleTest.

---

### Requirement 12: Unit Testing — Slab

**User Story:** As a developer, I want unit tests for the Slab allocator covering slot allocation, deallocation, reuse, and boundary conditions.

#### Acceptance Criteria

1. THE test suite SHALL include tests for: basic slot allocation, slot deallocation and return to free list, slot reuse after deallocation, allocation when the slab is full returning `nullptr`, behavior on an empty freshly constructed slab, and alignment of returned slot pointers.
2. THE Slab test suite SHALL be implemented using GoogleTest.

---

### Requirement 13: Unit Testing — STL Adapter

**User Story:** As a developer, I want unit tests for the CustomAllocator adapter verifying compatibility with standard containers.

#### Acceptance Criteria

1. THE test suite SHALL include a test using `std::vector<int, CustomAllocator<int>>` that pushes, accesses, and erases elements, producing the same results as a `std::vector<int>`.
2. THE test suite SHALL include a test using at least one additional standard container (e.g. `std::list` or `std::unordered_map`) with `CustomAllocator`.
3. WHEN `CustomAllocator::allocate` is called with a count that would overflow `std::size_t`, THE test SHALL verify that `std::bad_array_new_length` is thrown.
4. THE STL adapter test suite SHALL be implemented using GoogleTest.

---

### Requirement 14: Stress Testing

**User Story:** As a developer, I want a randomized stress test that exercises thousands of interleaved alloc/free operations, so that I can detect overlap, reuse errors, metadata corruption, and fragmentation regressions.

#### Acceptance Criteria

1. THE stress test SHALL execute a minimum of 100,000 randomized allocate and deallocate operations against the Free-List allocator.
2. WHEN an allocation succeeds, THE stress test SHALL record the returned pointer and size in a reference tracking structure and verify the allocation does not overlap any other live allocation.
3. WHEN a deallocation is performed, THE stress test SHALL remove the entry from the reference structure and verify the block becomes available for reuse in a subsequent allocation.
4. AFTER all stress-test operations complete, THE stress test SHALL verify that freeing all remaining live allocations causes the Free-List to coalesce back to a region no smaller than the original free pool minus header overhead.
5. THE stress test SHALL be compatible with AddressSanitizer and UndefinedBehaviorSanitizer (no suppression of sanitizer warnings).
6. THE stress test SHALL be implemented using GoogleTest.

---

### Requirement 15: Benchmarking — Allocation Throughput

**User Story:** As a developer, I want throughput benchmarks comparing system allocators to custom allocators, so that I can measure and document the performance characteristics of each strategy.

#### Acceptance Criteria

1. THE benchmark suite SHALL measure allocation and deallocation throughput for: `malloc`/`free`, `new`/`delete`, `Arena`, `FreeList`, and optionally `Slab` at operation counts of 10^5, 10^6, and 10^7.
2. THE benchmark SHALL report: total elapsed time, allocations per second, nanoseconds per allocation, and nanoseconds per deallocation.
3. THE benchmark suite SHALL use Google Benchmark or `std::chrono` with a stable clock.
4. THE benchmark SHALL avoid measuring setup time (pool construction) within the timed region.

---

### Requirement 16: Benchmarking — Container and Fragmentation

**User Story:** As a developer, I want benchmarks for STL container usage and fragmentation behavior, so that I can compare CustomAllocator overhead and measure pool efficiency under mixed workloads.

#### Acceptance Criteria

1. THE container benchmark SHALL compare `std::vector<int, std::allocator<int>>` against `std::vector<int, CustomAllocator<int>>` for push-back throughput across equal element counts.
2. THE fragmentation benchmark SHALL simulate a mixed-lifetime workload (interleaved alloc/free of varying sizes) and report: pool utilization percentage, size of the largest contiguous free block, and a fragmentation metric defined as `1 - (largest_free_block / total_free_bytes)`.
3. THE cache locality benchmark SHALL compare sequential vs. random access patterns on allocated arrays and report timing differences.
4. THE benchmark suite SHALL be buildable independently from the test suite via CMake.

---

### Requirement 17: Build System

**User Story:** As a developer, I want a CMake build system targeting C++20, so that I can build, test, and benchmark the library on any supported platform.

#### Acceptance Criteria

1. THE `CMakeLists.txt` SHALL set the C++ standard to C++20 and enforce it.
2. THE `CMakeLists.txt` SHALL define separate build targets for: the library, the test suite, and the benchmark suite.
3. THE `CMakeLists.txt` SHALL include CMake presets or flags enabling AddressSanitizer and UndefinedBehaviorSanitizer for debug builds.
4. WHEN GoogleTest is not found locally, THE `CMakeLists.txt` SHALL fetch it via `FetchContent`.
5. WHEN Google Benchmark is used, THE `CMakeLists.txt` SHALL fetch it via `FetchContent` if not found locally.

---

### Requirement 18: Documentation

**User Story:** As a developer, I want a README and architecture document explaining the project, so that readers can understand the design, build the project, and interpret benchmark results.

#### Acceptance Criteria

1. THE `README.md` SHALL explain: what the project does, why custom allocators are useful, how each allocator works, how alignment is handled, how the STL adapter works, how fragmentation is managed, build instructions, test and benchmark instructions, and known limitations and tradeoffs.
2. THE `docs/architecture.md` SHALL include memory layout diagrams (ASCII or otherwise) for the Arena pool layout and the Free-List block header layout.
3. THE `README.md` SHALL document benchmark results with measured numbers once benchmarks have been run.
4. THE `README.md` SHALL note that performance claims are benchmark-supported rather than theoretical.

---

### Requirement 19: Parser/Serializer — Configuration Round-Trip (if applicable)

**User Story:** As a developer, I want any configuration or metadata serialization to be verifiable via round-trip testing, so that serialization bugs are caught early.

#### Acceptance Criteria

1. WHERE the library exposes a mechanism to serialize or snapshot allocator state (e.g. for debugging or logging), THE serializer SHALL produce output that, when parsed back, yields an equivalent state representation.
2. FOR ALL valid serialized state objects, parsing then printing then parsing SHALL produce an equivalent object (round-trip property).
