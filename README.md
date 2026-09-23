# GravTree

**A local-compaction key-value storage engine with predictable write-tail latency.**

GravTree is a C++17 key-value store where every node is an immutable SSTable file and compaction is strictly *local* — a single flush event touches exactly one parent and its direct children, bounding per-compaction I/O to a constant independent of dataset size.

---

## Key Results (10M operations vs RocksDB 11.8)

| Workload | GT p99 | RDB p99 | GT wins |
|---|---|---|---|
| SeqWrite | 0.80 μs | 1.28 μs | 1.6× |
| RandWrite | 2.43 μs | 3.20 μs | 1.3× |
| PointRead | 2.42 μs | 6.21 μs | 2.6× |
| Mixed | 4.33 μs | 9.81 μs | 2.3× |

- **p99.99 write latency stays below 20 μs** on every workload (vs 31–2117 μs for RocksDB)
- **Point reads: 2.4× higher throughput** than RocksDB
- Write throughput is 21–80× lower due to whole-node rewrites — see [Limitations](#limitations)

---

## How It Works

```
Writes → in-memory skip-list (root buffer)
              ↓  [background thread, soft threshold 4 MB]
         LocalFlush: partition buffer by child key ranges,
                     merge-sort with child SSTable,
                     write new immutable child file,
                     invalidate old file, update manifest
```

Every node file is written once and never modified. This enables:
- Per-node Bloom filters with no staleness
- Crash recovery via atomic manifest rename (no WAL needed for tree structure)
- Natural fit for ZNS SSDs (sequential writes, bounded zone pressure)

---

## Build

```bash
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j$(nproc)
```

**Requirements:** C++17 compiler, CMake ≥ 3.14

---

## Usage

```cpp
#include "gravtree/tree.h"

GravTreeOptions opts;
opts.data_dir        = "/tmp/gravtree_data";
opts.buffer_threshold = 4 * 1024 * 1024;  // 4 MB soft limit

auto db = GravTree::open(opts);

db->insert("hello", "world");

auto val = db->get("hello");   // returns std::optional<std::string>

db->remove("hello");

auto entries = db->range_scan("a", "z");

db->flush();   // drain buffer to disk
```

---

## Benchmarks

```bash
# Latency benchmark (vs RocksDB)
./build/bench/latency_bench

# Write-amplification measurement
./build/bench/wa_bench

# Correctness verification
./build/bench/verify_bench
```

---

## Tests

```bash
ctest --test-dir build --output-on-failure
```

---

## Limitations

- **Write amplification:** 9.4× sequential, 180× random at 10M keys. Whole-node rewrites mean every LocalFlush rewrites the full subtree for random-key workloads. Planned fix: partial-node rewrites + parallel flush across sibling children.
- **No WAL:** buffered writes are lost on crash before `flush()` is called. Tree structure survives via atomic manifest.
- **Single-threaded flush:** background flush thread holds the tree lock during merge + file write. Parallel flush is the highest-leverage future work item.
- **Single client:** concurrent writers are serialised by `write_buf_mu`. Per-shard skip lists are future work.

---

## Paper

The research paper is available on SSRN:

> Subodh Wandile, *GravTree: A Local-Compaction Tree for Predictable Write Latency*, SSRN 7487218, 2026.

---

## License

GravTree is released under the [GNU Affero General Public License v3.0](LICENSE).
