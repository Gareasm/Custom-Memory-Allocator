# Architecture: Custom Memory Manager

## Component overview

```
┌─────────────────────────────────────────────────────┐
│                   MemoryManager                     │
│  ┌─────────────────────┐  ┌────────────────────┐   │
│  │       Arena         │  │     FreeList       │   │
│  │  (bump allocator)   │  │ (first-fit+coalesce│   │
│  └─────────────────────┘  └────────────────────┘   │
└─────────────────────────────────────────────────────┘
             │                        │
  ┌──────────▼──────────┐   ┌─────────▼──────────┐
  │   CustomAllocator   │   │  CustomAllocator   │
  │  <T, Arena>         │   │  <T, FreeList>     │
  └─────────────────────┘   └────────────────────┘
             │
  ┌──────────▼──────────┐
  │  std::vector/list/  │
  │  unordered_map ...  │
  └─────────────────────┘

  Optional:
  ┌──────────────────────┐
  │        Slab          │
  │  (fixed-size, O(1))  │
  └──────────────────────┘
```

`MemoryManager` owns both allocator instances by value (RAII). Its
constructors and destructor handle pool acquisition and release automatically.
`CustomAllocator<T, Alloc>` holds a pointer to the underlying allocator so
that STL rebind copies share the same pool.

---

## Arena pool layout

The Arena pool is a flat contiguous byte array. A single `offset_` field
(byte count from `pool_`) acts as the bump pointer.

```
 pool_                                   offset_       pool_+capacity_
   │                                        │                │
   ▼                                        ▼                ▼
   ┌──────────┬─────────┬──────────┬────────┬───────────────┐
   │ alloc 0  │ padding │ alloc 1  │ alloc2 │  free space   │
   │ (n bytes)│(0..a-1) │ (m bytes)│(k bytes│               │
   └──────────┴─────────┴──────────┴────────┴───────────────┘
```

- `padding` = 0..`alignment-1` bytes inserted by `std::align` to satisfy the
  alignment request of `alloc 1`.
- After `reset()`, `offset_` is set to 0; the pool bytes are untouched but
  logically available for reuse.
- `used()` returns `offset_`; `remaining()` returns `capacity_ - offset_`.
  The invariant `used() + remaining() == capacity()` always holds.

### allocate(size, alignment) — step by step

```
1. Guard: size == 0  →  return nullptr
2. Guard: !is_power_of_two(alignment)  →  return nullptr
3. current = pool_ + offset_
   space   = capacity_ - offset_
4. std::align(alignment, size, current, space)
      adjusts current to the next aligned address
      reduces space by the padding consumed
5. If std::align returns nullptr (no room)  →  return nullptr
6. offset_ = (current - pool_) + size
7. return current
```

---

## FreeList pool layout

Every region in the pool starts with an embedded `BlockHeader` followed
immediately by the usable payload.

```
 pool_                                              pool_+capacity_
   │                                                       │
   ▼                                                       ▼
   ┌────────────────┬─────────────────┐ ┌────────────────┬──────────────────┐
   │  BlockHeader   │  payload        │ │  BlockHeader   │  payload / free  │
   │   (32 bytes)   │  (h0->size B)   │ │   (32 bytes)   │  (h1->size B)    │
   └────────────────┴─────────────────┘ └────────────────┴──────────────────┘
         h0                                    h1
    h0->next = h1                        h1->prev = h0
    h0->prev = nullptr                   h1->next = nullptr (last)
```

### BlockHeader structure

```
 Offset  Size   Field
 ──────  ────   ──────────────────────────────────────────────────────────
 0       8 B    size      — usable payload bytes (not including this header)
 8       1 B    is_free   — 1 = free, 0 = allocated
 9       7 B    _pad[7]   — explicit padding (deterministic layout)
 16      8 B    prev*     — previous block in pool order (nullptr = first)
 24      8 B    next*     — next block in pool order (nullptr = last)
 ──────  ────
 Total: 32 bytes on 64-bit systems
```

`prev`/`next` are pool-order pointers (physical adjacency), not a separate
free-list chain. Coalescing is O(1) because adjacent blocks are always
`h->prev` and `h->next`.

### Pointer conversion

```cpp
// header → user pointer
void* user_ptr(BlockHeader* h) {
    return reinterpret_cast<std::byte*>(h) + sizeof(BlockHeader);
}

// user pointer → header
BlockHeader* header_of(void* p) {
    return reinterpret_cast<BlockHeader*>(
        static_cast<std::byte*>(p) - sizeof(BlockHeader));
}
```

### allocate — First-Fit walk

```
for each h in pool order:
    if h->is_free:
        candidate = user_ptr(h)
        aligned   = align_up(candidate, alignment)
        adjustment = aligned - candidate
        total_needed = size + adjustment
        if h->size >= total_needed:
            split(h, total_needed)   // optional
            h->is_free = false
            return aligned
return nullptr
```

### split(block, needed_size)

```
new_h position = (byte*)block + sizeof(BlockHeader) + needed_size

new_h->size    = block->size - needed_size - sizeof(BlockHeader)
new_h->is_free = true
new_h->prev    = block
new_h->next    = block->next

if block->next: block->next->prev = new_h
block->next = new_h
block->size = needed_size
```

Only executed when `block->size - needed_size >= sizeof(BlockHeader) + 1`.

### coalesce(block)

```
// Merge with next if free
if block->next && block->next->is_free:
    absorbed = block->next
    block->size += sizeof(BlockHeader) + absorbed->size
    block->next  = absorbed->next
    if absorbed->next: absorbed->next->prev = block

// Merge with prev if free
if block->prev && block->prev->is_free:
    pred = block->prev
    pred->size += sizeof(BlockHeader) + block->size
    pred->next  = block->next
    if block->next: block->next->prev = pred
```

---

## Slab pool layout

All slots are the same size: `slot_size_ = round_up(max(object_size, sizeof(void*)), alignment)`.

```
 pool_                                                    pool_+slot_size_*capacity_
   │                                                               │
   ▼                                                               ▼
   ┌────────────┬────────────┬────────────┬─── ... ───┬───────────┐
   │   slot 0   │   slot 1   │   slot 2   │           │  slot N-1 │
   │ (slot_size_│ (slot_size_│ (slot_size_│           │           │
   │  bytes)    │  bytes)    │  bytes)    │           │           │
   └────────────┴────────────┴────────────┴─── ... ───┴───────────┘
```

Free slots embed the intrusive list pointer in the first `sizeof(void*)` bytes
of the slot itself:

```
   free_head_
       │
       ▼
   ┌──────────┬───────────────┐     ┌──────────┬───────────────┐
   │ next ptr │  (unused)     │ ──► │ next ptr │  (unused)     │ ──► nullptr
   │ (8 bytes)│               │     │ (8 bytes)│               │
   └──────────┴───────────────┘     └──────────┴───────────────┘
       slot 2 (free)                    slot 0 (free)
```

### allocate / deallocate

```
allocate():
    if free_head_ == nullptr: return nullptr
    slot      = free_head_
    free_head_ = *reinterpret_cast<void**>(slot)   // advance
    ++used_
    return slot

deallocate(ptr):
    assert ptr in [pool_, pool_ + slot_size_ * capacity_)
    *reinterpret_cast<void**>(ptr) = free_head_    // push
    free_head_ = ptr
    --used_
```

Both operations are O(1) with no memory allocation.

---

## CustomAllocator<T, Alloc>

A thin template wrapper that satisfies the C++ named requirement *Allocator*.
Holds a non-owning `Alloc*` pointer so copies and rebind instances all share
the same pool.

```
CustomAllocator<int, Arena>          CustomAllocator<char, Arena>
        alloc_  ──────────────────────────── alloc_
               \                            /
                ▼                          ▼
              arena_ (owned by MemoryManager or caller)
```

`allocate(n)`:
```
if n > SIZE_MAX / sizeof(T): throw std::bad_array_new_length
void* p = alloc_->allocate(n * sizeof(T), alignof(T))
if p == nullptr: throw std::bad_alloc
return static_cast<T*>(p)
```

`deallocate(ptr, n)`:
```
alloc_->deallocate(ptr)   // size not needed by pool strategies
```

---

## File layout

```
include/memory/
  arena.hpp           — Arena class declaration
  free_list.hpp       — FreeList class declaration
  slab.hpp            — Slab class declaration
  allocator.hpp       — CustomAllocator<T, Alloc> (header-only)
  memory_manager.hpp  — MemoryManager declaration

src/
  arena.cpp           — Arena implementation
  free_list.cpp       — FreeList + BlockHeader implementation
  slab.cpp            — Slab implementation
  memory_manager.cpp  — MemoryManager implementation

tests/
  arena_property_tests.cpp  — RapidCheck property tests for Arena
  stress_tests.cpp          — 100 000-op FreeList stress + overlap checks
  placeholder_test.cpp      — minimal smoke test

benchmarks/
  allocation_benchmark.cpp   — malloc/new/Arena/FreeList throughput
  container_benchmark.cpp    — std::vector with std vs custom allocator
  fragmentation_benchmark.cpp— FreeList fragmentation metrics
  cache_benchmark.cpp        — sequential vs random access on Arena array
```
