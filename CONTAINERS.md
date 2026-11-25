# Pushable Memory — container contracts (proposed v0.2)

## Purpose and scope

Explore how an ordered byte-cell machine with persistent identities can simplify real data structures while retaining useful throughput, memory efficiency and native interoperability. These are proposed interfaces and acceptance contracts, not claims about implemented functionality. The initial scope is four distinct containers:

| Container | Application | Principal capability |
| --- | --- | --- |
| FlexSequence | Packed arrays, lists, deques, strings, queues | Indexed and handle-based sequence operations |
| FlexMap | Ordered indexes, sorted records | Ordered key lookup, insertion and traversal |
| FlexHashMap | Fast dictionaries, caches, symbol tables | Hash-based lookup and resizing |
| FlexDocument | Hierarchical, variable-size data | Mutable portable binary representation |

A set can be an adapter over either map with a unit value; stacks and queues are adapters over FlexSequence. A priority queue can be built on a sequence. Separate implementations of list, vector, deque, string and these adapters are not part of this scope. A byte sequence is not required to provide Unicode-aware text operations.

## Shared rules

- C++20, namespace `pushable_memory`. Public interfaces below are illustrative C++ declarations defining required operations and semantics. Implementations may add convenience methods, but must preserve these operations.
- Each container owns an initially empty FlexibleBuffer (FlexDocument starts with one null cell). Constructor capacity is a fixed logical **byte** budget, including persistent metadata stored in the machine. If multiple buffers are used, the sum of their capacities must not exceed this budget. Single-threaded; not copyable or movable in the initial contract.
- Container implementation uses only the public machine API. No private representation, numerical address arithmetic, pointers to internal metadata, external STL storage, or hidden persistent variable-size side indexes. All variable-size persistent payload and metadata must be held in FlexibleBuffer. Fixed-size scalars, handles, functors and bounded local scratch are permitted. No implementation-owned heap allocations during successful operations after construction, including auxiliary storage. Caller-owned inputs and outputs are not included.
- Fixed-width generic types must be trivially copyable, non-cv object types with valid byte-copy semantics; pointer-containing values are discouraged and have no persistence guarantee. Values are transferred by representation using read/write or memcpy, never by assuming machine-native pointer alignment or C++ object lifetime. Compare, hash and equality functors must be deterministic, nonthrowing, and consistent (equivalent keys have equal hashes). No implicit allocation by functors.
- Handles are opaque, trivially copyable, equality comparable, non-owning; copied handles require no registration. A handle belongs to exactly one container and identifies one live element, entry or node; numeric ordering is not assumed. A permanent container-specific end sentinel is never a live value. Handles to surviving elements/entries/nodes remain valid across structural edits, including rehash and internal relocation. Erased identities are never reused within that container lifetime. Updating a value is not erasure; do not overwrite a surviving identity with a different entry merely to fill a hole.
- Checked container operations reject foreign, stale and non-element/non-node handles with InvalidAddress, and invalid ranges or indices with OutOfBounds. Missing lookup returns end. Resource exhaustion uses std::bad_alloc or AddressSpaceExhausted as applicable. Inputs with invalid native memory are caller errors. No publicly accessible writable internal lease in this version; any internal c_ptr acquisition must be released before returning or propagating an exception.
- Failed operations with defined inputs/errors leave contents, size, ordering and all pre-existing handles unchanged. A failed composite operation may consume address identities or change internal layout, but may not leak live storage or invalidate surviving handles. No expectation of byte-identical internal metadata. Validate before destructive operations; use transactions, preflight, or bounded rollback as needed. A failed construction produces no object.
- Complexity goals below are engineering objectives, not guaranteed asymptotic constraints or calibrated pass/fail thresholds. Measure with realistic workloads and report physical memory including metadata, scratch, lease storage and all machine instances.

## 1. FlexSequence<T>

A single packed, logically ordered sequence of fixed-width values. It supports both vector-style indexing and list-style persistent element handles; it need not have conventional contiguous C++ iterators or expose T&. An element handle identifies the first byte of that element, or an equivalent opaque identity. Insertions/removals affect whole elements only.

```cpp
template<class T> class FlexSequence {
public:
    using Handle = /* opaque */;
    explicit FlexSequence(size_t byte_capacity);
    size_t size() const noexcept; // elements
    Handle first() const noexcept;
    Handle last() const noexcept;
    Handle end() const noexcept;
    Handle at(size_t index) const; // live element only
    Handle forward(Handle, size_t count) const;
    Handle backward(Handle, size_t count) const;
    T get(Handle) const;
    void set(Handle, const T&);
    Handle insert(Handle before, const T&);
    Handle insert_range(Handle before, std::span<const T>);
    void erase(Handle start, size_t count = 1);
    void clear();
};
```

- Empty first/last return end. Navigation follows current order, is non-wrapping, and may reach end; moving backward from end by one returns last on a nonempty sequence. Zero distance preserves a valid input. at(size()) is out of bounds.
- Insertion is immediately before the handle, or append at end. insert_range with an empty span returns before. Otherwise returns first inserted element. Order matches input. Erase removes count consecutive elements; count zero is valid at a live handle or end. clear invalidates all live handles. Multiplication of count by sizeof(T) must be checked for overflow.
- set changes a value but not its identity; get returns by value. Front/back, indexed insertion, push/pop and queue adapters can be expressed using these operations. FlexSequence<uint8_t> or FlexSequence<char> covers byte/string storage without another engine.
- Objectives: low-overhead indexed access and traversal, efficient front/middle/back mutation, bulk insert/erase, stable handles and dense storage. Avoid application-level per-element links if not needed.

Validation: differential tests against std::vector, retained handle ledger, alternating head/tail edits, random mid-sequence edits, repeated shrink/regrow, multi-element transfers, boundary and capacity failures, and zero-length operations. Measure indexed access, handle traversal, bulk operations and mixed mutations independently.

## 2. FlexMap<K,V,Compare>

An ordered unique-key dictionary. Keys are immutable for the lifetime of an entry. Values can be changed without replacing its handle. Iteration is sorted under Compare; key equivalence is !compare(a,b) && !compare(b,a).

```cpp
template<class K, class V, class Compare = std::less<K>>
class FlexMap {
public:
    using Handle = /* opaque */;
    explicit FlexMap(size_t byte_capacity, Compare compare = {});
    size_t size() const noexcept;
    Handle first() const noexcept;
    Handle last() const noexcept;
    Handle end() const noexcept;
    Handle find(const K&) const;
    Handle lower_bound(const K&) const;
    Handle upper_bound(const K&) const;
    Handle next(Handle) const;
    Handle previous(Handle) const;
    K key(Handle) const;
    V get(Handle) const;
    void set(Handle, const V&);
    std::pair<Handle,bool> insert(const K&, const V&);
    std::pair<Handle,bool> insert_or_assign(const K&, const V&);
    bool erase(const K&);
    void erase_entry(Handle);
    void clear();
};
```

- find returns end if absent. lower_bound returns the first key not less than the input, upper_bound the first strictly greater, or end. next(last()) returns end; previous(end()) returns last if nonempty. Other out-of-range navigation throws OutOfBounds.
- insert returns the existing handle and false for duplicate keys without modifying the value. insert_or_assign returns the existing handle and false on update, or new handle and true on insertion. Erasing a missing key returns false. Erasing an entry invalidates only its handle.
- Ordered iteration and bounds must remain correct after all edits. Internal ordering/index design is unrestricted; an implementation can exploit the machine's intrinsic ordering directly rather than necessarily use a tree.
- Objectives: efficient search, insert and erase, sorted scans and range lookup; benchmark memory per entry and comparison count.

Validation: differential tests against std::map with default and reverse order, duplicate/equivalent keys, ascending/descending/adversarial keys, randomized mutations, bound searches, handle survival and capacity failure.

## 3. FlexHashMap<K,V,Hash,Equal>

An unordered unique-key dictionary. Keys are immutable while live; equal keys are required to hash equally. Rehashing must preserve entry identity and contents.

```cpp
template<class K, class V, class Hash = std::hash<K>,
         class Equal = std::equal_to<K>>
class FlexHashMap {
public:
    using Handle = /* opaque */;
    explicit FlexHashMap(size_t byte_capacity, Hash hash = {}, Equal equal = {});
    size_t size() const noexcept;
    Handle end() const noexcept;
    Handle find(const K&) const;
    K key(Handle) const;
    V get(Handle) const;
    void set(Handle, const V&);
    std::pair<Handle,bool> insert(const K&, const V&);
    std::pair<Handle,bool> insert_or_assign(const K&, const V&);
    bool erase(const K&);
    void erase_entry(Handle);
    void reserve(size_t entry_count);
    void clear();
};
```

- Lookup, insert/update and erase semantics match FlexMap, using Equal rather than Compare. No ordering or iteration guarantee in this initial interface.
- reserve requests sufficient internal index capacity for entry_count total live entries without another index growth (subject to remaining payload/storage budget). It cannot reduce capacity; reserve(size()) is permitted. Automatic growth is allowed. reserve does not change size or invalidate handles. No public bucket layout or required load factor.
- All index/bucket/tombstone data is charged to the machine budget. Collisions must not cause incorrect lookup or nontermination. No claim of constant worst-case performance; average throughput and adversarial degradation are measured separately.

Validation: differential tests against std::unordered_map, deliberately constant and low-entropy hashes, duplicate/equal keys, growth/rehash with retained handles, collision-chain deletion and reinsertion, reserve failures and long churn at nearly fixed live size.

## 4. FlexDocument

A mutable JSON-style tree with native binary values and stable node handles. The **live logical byte sequence is the serialized document**, not an object graph that is serialized later. An implementation may have fixed-size state outside the sequence. No process addresses, opaque machine handles, padding or transient indexes appear in the serialized bytes. This initial version uses one machine for the document and no persistent auxiliary index; navigation can scan the encoding.

### Portable wire format, version 1

The document is exactly one encoded root node; no file header or trailing bytes. All multi-byte numbers are little-endian. One-byte tags:

| Tag (hex) | Value | Following bytes |
| --- | --- | --- |
| 00 | null | none |
| 01 | false | none |
| 02 | true | none |
| 03 | signed integer | 8-byte two's-complement integer |
| 04 | floating point | 8-byte IEEE 754 binary64, finite only |
| 05 | UTF-8 string | u64 byte length, then bytes |
| 06 | binary blob | u64 byte length, then bytes |
| 07 | array | zero or more encoded nodes, then FF |
| 08 | object | zero or more (u64 key byte length, UTF-8 key bytes, encoded node), then FF |
| FF | end marker | closes an array/object; never a standalone node |

For objects, FF is ambiguous with the first byte of an untagged key length. Therefore **each object member MUST begin with a 09 member marker** before its u64 key length. At an object member boundary only 09 and FF are legal. The object row above is read with this mandatory marker. Array boundaries need no member marker. Keys are unique by exact UTF-8 bytes, preserve insertion order and may be empty; no Unicode normalization. Strings and keys must be well-formed UTF-8 (embedded NUL allowed); blobs are arbitrary bytes. Maximum node nesting is 256 (root depth 1). No indefinite lengths other than container terminators, no references/cycles/shared subtrees. Version changes require explicit format agreement.

### Proposed API

```cpp
enum class Kind { Null, Boolean, Integer, Real, String, Binary, Array, Object };

class FlexDocument {
public:
    using Node = /* opaque */;
    explicit FlexDocument(size_t byte_capacity); // root is null
    FlexDocument(size_t byte_capacity, std::span<const uint8_t> encoded);
    Node root() const noexcept;
    Node end() const noexcept; // missing node
    Kind kind(Node) const;
    size_t child_count(Node) const;
    Node child(Node array, size_t index) const;
    Node find(Node object, std::string_view key) const;
    bool boolean(Node) const;
    int64_t integer(Node) const;
    double real(Node) const;
    size_t payload_size(Node) const; // string/blob byte count
    void read_payload(Node, std::span<uint8_t> out) const;
    void replace(Node, std::span<const uint8_t> encoded_node);
    Node insert(Node array, size_t index, std::span<const uint8_t> encoded_node);
    Node put(Node object, std::string_view key, std::span<const uint8_t> encoded_node);
    void erase(Node array, size_t index);
    bool erase(Node object, std::string_view key);
    size_t encoded_size() const noexcept;
    void save(std::span<uint8_t> out) const;
};
```

- Inputs encoded_node are complete single-node encodings using the wire format above. This makes scalar construction and nested subtree insertion uniform; convenience typed setters/builders may be layered on top.
- root always exists; an empty construction needs at least one byte of budget. Loading via the second constructor validates the complete stream, capacity, depth, keys and all scalar encodings before exposing a document. Malformed or type-incompatible inputs throw std::invalid_argument. Invalid array index/range or insufficient output span throws OutOfBounds. Missing object key returns end. child_count applies only to arrays/objects. Typed accessors reject wrong kinds. read_payload and save write exactly the payload/encoding prefix of the output span, leaving any remaining bytes unchanged.
- insert accepts indices 0..child_count inclusive, preserves sibling order and returns a new node. put adds a missing key at the end of the object, or replaces the existing value in place and returns its existing node handle. replace preserves the target node identity (including root), but destroys all prior descendant identities; all unrelated handles survive. Array erase removes the indexed node and descendants; object erase removes the key/value subtree, returning false for absent keys. Node handles must not refer to key bytes or structural markers.
- Every mutation leaves a well-formed document. Check input length arithmetic, depth of the resulting tree, finite doubles, UTF-8, duplicate keys and exact input consumption. No mutation on invalid input, including insufficient capacity. The input/output spans are caller-owned and do not alias the internal machine.
- save is a copy of the existing logical wire sequence, without walking nodes to reconstruct or re-encode values. Implementations may use native memory leases or machine range reads; physical contiguity is not required. Loading creates fresh runtime identities. A saved node handle cannot be reused after loading. Saving is O(n) data transfer, not zero-cost I/O. The format is binary, not JSON text and not a claim of compatibility with any standard binary JSON format.

Validation: independent parser/model; nested insertion and replacement, handle ledger across unrelated edits, empty arrays/objects, arbitrary binary bytes, UTF-8 keys and strings, malformed tags/truncation/length overflow/duplicate keys/depth, bit-exact save/load, canonical input byte preservation, deterministic failure and full-capacity edits. Measure deep and wide mutation, large blobs, traversal, memory footprint, save and load separately.

## Evaluation and engineering priorities

1. Establish an independent semantic model and deterministic randomized differential tests for each container. Tests should compare contents, stable identities and failures, not private layouts.
2. Cross-test containers against the reference machine and optimized machines using interchangeable public APIs. Keep machine-specific test hooks out of container code. Test ASan/UBSan and no-heap behavior separately, and exercise checked/unchecked machines with valid inputs.
3. Measure machine and end-to-end container operations with identical traces and validated output. Report elapsed time, memory budget, actual reserved memory, peak scratch, handle sizes, and failure rates separately. Include native acquisition and release in timing.
4. Correctness is a prerequisite for performance comparisons. Do not sacrifice specified semantics for throughput. Calibrate thresholds only after a feasible implementation and repeatable measurements exist. These specifications do not establish a production-readiness claim.

The current repository implements the reference machine and a small illustrative binary document demo. It does not yet implement these container APIs; the demo uses its own illustrative format rather than the proposed wire format above.
