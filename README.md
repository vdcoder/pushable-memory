# Pushable Memory

An experimental C++20 memory architecture built around ordered byte cells with persistent identities rather than numerical offsets. Insert or remove cells anywhere without invalidating handles to surviving cells. Temporarily borrow contiguous native memory for existing C and C++ code. The project investigates whether a single flexible memory primitive can simplify application data structures while approaching conventional memory performance.

This repository contains a **deliberately simple reference implementation** that defines executable semantics and establishes an initial correctness baseline. The long-term objective is a production-oriented, capacity-bounded memory subsystem with low overhead, efficient structural mutations and predictable native interoperability. The current version is experimental, not production-ready. No external dependencies.

## Build

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel
ctest --test-dir build --output-on-failure
```

ASan + UBSan (GCC/Clang):

```sh
cmake -S . -B build-san -DCMAKE_BUILD_TYPE=Debug -DPUSHABLE_SANITIZE=ON
cmake --build build-san --parallel
ctest --test-dir build-san --output-on-failure
```

## Example

```cpp
#include <pushable_memory/flexible_buffer.hpp>
using pushable_memory::FlexibleBuffer;

FlexibleBuffer<> b(1024); // empty, fixed logical capacity
const auto a = b.separate(b.end(), 5);
b.write(a, "hello", 5);
const auto e = b.forward(a, 4); // persistent handle to 'o'
const auto inserted = b.separate(e, 2);
b.write(inserted, "!!", 2); // logical sequence hell!!o

// Borrow a contiguous native range; modify through either interface.
auto* p = b.c_ptr(b.first(), b.size());
p[0] = 'H';
b.set(e, 'O'); // p[6] immediately reads 'O'
b.c_ptr_release(p);
b.sink(inserted, 2); // HellO; e still identifies the last cell
```

`FlexibleBuffer<false>` omits address/range checks for callers satisfying preconditions. See [SPEC.md](SPEC.md) for the machine contract, resource semantics and open engineering decisions.

## Implementation

- Fixed physical byte array. Its address never changes while the buffer lives.
- Vector of slot IDs recording logical order. Structural changes shift the vector, not the byte array.
- Fixed address table: slot → logical position, physical position, generation. The handle also carries a buffer identity. A global-per-buffer increasing generation prevents stale handles from becoming valid on reuse; no tombstone history is needed.
- Reverse physical mapping enables swaps. The reference makes a range contiguous by swapping **unpinned** physical bytes and updating both mappings.
- Per-physical-byte pin counts and a pre-reserved native lease vector. Overlaps are supported when the physical constraints can be satisfied. All native writes are immediately visible; no copy-back stage.

The design intentionally spends substantial metadata (roughly 73 bytes per logical-capacity byte on a typical 64-bit implementation, at default lease reservation; actual allocation sizes may vary). Structural operations and range materialization can scan large portions of the buffer. This is a correctness-first design, not a recommended production representation or a physical memory budget for optimized implementations. Insertion is O(capacity + count), deletion O(size + count), random access and navigation O(1), range operations O(count), and native acquisition O(capacity + count) worst case; insertion also scans active leases. Release scans leases and pin counts. These bounds describe the reference only; alternative implementations are encouraged to improve them while preserving semantics.

## Container architecture

[CONTAINERS.md](CONTAINERS.md) specifies four complementary application-level containers built exclusively on the public FlexibleBuffer instruction set:

| Container | Purpose |
| --- | --- |
| `FlexSequence<T>` | One packed sequence for vector, list, deque, byte/string and queue-style use |
| `FlexMap<K,V>` | Ordered unique-key dictionary and sorted traversal |
| `FlexHashMap<K,V>` | Hash-based dictionary with stable entry handles across rehash |
| `FlexDocument` | Mutable nested objects and arrays whose live bytes are their portable serialized representation |

The container contracts emphasize stable handles, bounded storage, no post-construction heap allocation, correctness under structural changes, and realistic performance. Set, stack and queue adapters can reuse these primitives instead of introducing redundant storage engines.

FlexDocument is a particularly important experiment: a self-contained binary encoding can be modified in place logically, saved without reconstructing an object graph, and loaded with fresh runtime handles. Save/load still incur data transfer; physical memory may not be contiguous.

## Engineering roadmap

1. Freeze machine semantics and resolve portable native lease progress and resource guarantees.
2. Implement and independently validate the four container contracts.
3. Compare optimized machine architectures under identical machine and container workloads, including actual memory footprint and native pointer acquisition/release.
4. Establish repeatable performance targets and expand portability, diagnostics and robustness before considering production deployment.

## Repository layout

- `include/pushable_memory/flexible_buffer.hpp` — complete header-only reference.
- `tests/reference_tests.cpp` — directed tests plus 96,000 randomized operations across checked and unchecked variants, comparing to an independent logical vector model.
- `tests/no_heap.cpp` — counts operator new calls during successful machine operations (ordinary build only).
- `examples/flex_document_demo.cpp` — nested binary document mutation, surviving handle, exact byte-stream save/load. Demonstration only, not a full FlexDocument container or parser.
- `SPEC.md` — machine contract and open engineering decisions.
- `CONTAINERS.md` — proposed container APIs, portable document format and validation plan.

The container suite, independent conformance harness, performance calibration and production-grade FlexDocument remain future work. The reference is not thread-safe.
