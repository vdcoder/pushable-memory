# Pushable Memory — proposed machine contract (v0.1 reference)

## Model

A buffer is an initially empty ordered sequence of byte cells, with a fixed maximum number of live cells. A cell has a stable identity and a byte value. No public numeric position API or address arithmetic exists. `Address` is opaque, trivially copyable, equality-comparable and non-owning. Copies of a handle require no registration. An address remains valid until its cell is sunk or its buffer is destroyed. A sunk address is never issued again by that buffer, even when physical storage is recycled. An address from another buffer is invalid. No operation requires retaining the history of dead cells.

The machine is single-threaded; externally synchronize all use, including raw pointer access, if sharing it across threads. The reference buffer is non-copyable and non-movable. Native pointers must not outlive the buffer.

## Instructions

```cpp
template<bool BoundsChecked = true>
class FlexibleBuffer {
public:
    using Address = /* opaque */;
    explicit FlexibleBuffer(size_t capacity);
    size_t size() const noexcept;
    size_t capacity() const noexcept;
    Address first() const noexcept;
    Address last() const noexcept;
    Address end() const noexcept;
    Address separate(Address start, size_t count);
    void sink(Address start, size_t count);
    Address forward(Address from, size_t count) const;
    Address backward(Address from, size_t count) const;
    uint8_t get(Address address) const;
    void set(Address address, uint8_t value);
    void read(Address start, void* dst, size_t count) const;
    void write(Address start, const void* src, size_t count);
    void copy(Address dst, Address src, size_t count);
    uint8_t* c_ptr(Address start, size_t count);
    void c_ptr_release(uint8_t* ptr);
};
```

- `end()` is a permanent one-past-end sentinel. `first()` and `last()` return it when empty. Traversal is linear, never wraps. `forward(last(),1)` returns end; `backward(end(),1)` returns last when nonempty. A move before the first or beyond end is out of bounds. A zero-distance move returns its input.
- `separate(start,n)` inserts n new cells immediately **before** start and returns the first new address. `start=end()` appends. `separate(start,0)` returns start. New bytes have unspecified values; write before reading for deterministic behavior. The reference initializes its physical byte array once at construction, not on insertion.
- `sink(start,n)` removes n consecutive cells starting at start; all other addresses and values survive. Storage is reusable; identities are not.
- `read`, `write`, `copy`, and `sink` act on the next n cells beginning at start. A zero-length range is valid at any live address or end. A positive range must contain n live cells before end. External `read`/`write` memory must be valid and must not alias buffer storage; use `copy` for intra-buffer copies. Zero-length external ranges permit null pointers.
- `copy` has memmove semantics, including overlaps in either direction, and does not change identities or size. Both ranges are validated before any writes in the checked version.

## Native memory leases

`c_ptr(start,n)` makes precisely n bytes (at least n physically accessible bytes; only the first n are authorized) contiguous in logical order and returns a writable native byte pointer. `c_ptr(start,0)` returns null and creates no lease. Releasing null is a no-op. Alignment beyond that needed for bytes is not guaranteed; use memcpy for typed data.

- Many simultaneous leases, including disjoint ranges and compatible overlaps. All native and abstract accesses to a cell see the same byte immediately. The pointer and its mapping to cells remain stable until release. No delayed copy-back.
- Reads, writes, copies and traversal are allowed while leases are active. Insertions before or after a lease are allowed, but inserting strictly inside a leased range throws `PinnedRangeConflict`. Sinking any leased cell also throws it. Insertion between two adjacent independent leases is allowed unless another lease spans that boundary.
- For overlapping acquisition, pinned cells must imply the same physical base, and the destination may not overwrite a different pinned cell. Compatible requests succeed if the lease table has room. For a disjoint range, an already contiguous location or a long enough physical run without pins is needed in this reference. If no such placement exists, `PinnedRangeConflict` is thrown, even for logically disjoint ranges. No existing native pointer can be relocated. The exact cross-implementation policy for which acquisitions must succeed remains to be finalized; tests should not expect an implementation-specific physical placement.
- `c_ptr_release` releases one acquisition for the exact pointer originally returned. Distinct pointers may be released in any order. If identical pointer values were acquired multiple times, release matches the most recent still-active acquisition for that value (LIFO). This rule is necessary because the release API does not carry a length or lease token. Never use a pointer after releasing its lease, even if the numeric pointer is later reissued. Unlike Address, native pointers are not permanent identities. Caller must pair each positive acquisition with one release.

## Failure semantics

- Checked: invalid, foreign or stale handles throw `InvalidAddress`; invalid ranges throw `OutOfBounds`; unmatched non-null pointer release throws `InvalidPointer`.
- Unchecked: violating those preconditions is outside the contract (undefined behavior permitted). No guaranteed diagnostic. The same valid operations must produce the same abstract results.
- Both: exceeding live capacity or native lease resource capacity throws `std::bad_alloc`; exhausting the never-reused address identity space throws `AddressSpaceExhausted`; operations incompatible with live native leases throw `PinnedRangeConflict`. These resource and pin checks are not optional even in unchecked mode.
- Failed operations (with defined inputs and defined errors) leave all logical data, handles, and outstanding native leases unchanged. When multiple errors apply, error precedence is not standardized.
- No implementation-owned dynamic heap allocation after construction for successful operations. Exception runtime internals are not included in this guarantee. The reference pre-reserves all storage, including native lease records. Resource exhaustion is not permitted to silently corrupt or invalidate any existing allocation.

## Reference-only configuration

The optional second constructor argument `ReferenceOptions` is a test hook, not a mandated candidate API. `max_leases=0` reserves max(capacity,16) lease records; a positive value sets an explicit resource ceiling. `address_limit` limits the number of issued byte identities (default UINT64_MAX); it permits a practical test of exhaustion. Capacity zero is valid. The reference stores the same generation on a slot until that slot is reissued with a new generation. All successful positive separate calls consume one identity per byte.

## Open assignment decisions

- Set practical physical metadata and scratch budgets only after measuring a feasible optimized design. The reference is intentionally memory hungry.
- Decide a portable minimum simultaneous lease requirement and a portable progress rule for fragmented pinned memory. No reference-specific placement should be used as ground truth for candidate acceptance without settling that contract.
- Specify a candidate-independent address-space-exhaustion test hook if required, since Address is intentionally opaque.
- Decide if external read/write aliasing should be supported, and whether aligned native pointer requests deserve another API.
- Define APIs and independent model tests for vector, list, deque, string, map/set, hash map, heap, record map and FlexDocument. The binary demo is not a finalized wire format. Serialized documents contain portable values and framing, never raw addresses; loading creates fresh handles. A real loader must validate malformed input.
- Freeze performance and memory thresholds on the grading hardware, after correctness validation. The reference is not a proven performance floor or production allocator.
