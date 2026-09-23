---
title: 'GravTree: A Local-Compaction Tree for Predictable Write Latency'
tags:
  - C++
  - storage engine
  - key-value store
  - database systems
  - LSM-tree
  - compaction
authors:
  - name: Subodh Wandile
    affiliation: 1
affiliations:
  - name: Independent Researcher, India
    index: 1
date: 23 September 2026
bibliography: paper.bib
---

# Summary

GravTree is a C++17 key-value storage engine in which every node is an
immutable Sorted String Table (SSTable) file and compaction is strictly
*local*: a single compaction event involves exactly one parent node and
its direct children, and nothing else. This invariant bounds the I/O of
any single compaction event to $O(K \cdot B)$ bytes — a constant
independent of dataset size — so that the maximum work done between any
two writes is fixed by configuration rather than by data volume.

Writes accumulate in an in-memory skip-list at the root; the critical
write path performs no disk I/O. A dedicated background thread runs
LocalFlush to push data from the root buffer down the tree, splitting
leaves and internal nodes as they fill. The prototype includes per-node
Bloom filters, a crash-safe atomic manifest with garbage collection of
superseded nodes, a sparse index per SSTable, CRC-64 integrity
verification, and a two-level lock design that allows concurrent reads
during flushes.

# Statement of Need

Log-Structured Merge-trees (LSM-trees) [@oneil1996lsm], the dominant
write-optimised storage engine design used in RocksDB [@rocksdb],
LevelDB [@leveldb], Cassandra, and TiKV, achieve high write throughput
by buffering writes in memory and periodically merging sorted runs on
disk. The central cost of this design is compaction, which can cascade
across levels and produce write stall events where p99.9 latency spikes
100–1000× above median in production deployments [@lsm-survey].

Existing approaches reduce write amplification [@wisckey; @pebblesdb;
@dostoevsky] or restructure the level hierarchy [@pebblesdb], but none
bound the *duration* of a single compaction event to a constant
independent of dataset size. GravTree addresses this directly: its
LocalFlush algorithm reads and writes at most $B + K \cdot C_{\max}$
bytes per event, where $B$ is the root buffer size and $C_{\max}$ is the
largest child SSTable size — both bounded by configuration constants.

GravTree is closest in spirit to the B$^\varepsilon$-tree
[@bender2007cache] and SplinterDB [@splinterdb], which also propagate
writes downward through per-node buffers. The key difference is the
storage model: every GravTree node is a complete, immutable SSTable
rather than a mutable fixed-size block. This enables per-node Bloom
filters with no staleness concern, deterministic crash recovery via
atomic file rename without a write-ahead log for tree structure, and a
natural mapping onto Zoned Namespace (ZNS) SSDs [@zns] where each node
occupies one sequentially-written zone.

# Functionality

GravTree exposes a simple key-value API (`insert`, `remove`, `get`,
`range_scan`, `flush`) and is configured via a `GravTreeOptions` struct.
Key parameters include:

- `buffer_threshold` (default 4 MB): soft threshold that triggers
  background flush.
- `max_fanout` (default 8): maximum number of children per internal
  node.
- `data_dir`: directory for SSTable node files and the manifest.

The engine is validated with a correctness harness that inserts 1M keys,
reads every key back, closes the engine, re-opens it from the manifest,
and reads every key again (0 missing, 0 mismatched in all runs).

# Performance

Evaluated against RocksDB 11.8.1 at $N = 10$M operations per workload
under equal durability settings (no WAL on either engine) on a Linux
x86\_64 VM (8 vCPU Intel Xeon Platinum 8559C, 31 GB RAM, NVMe-backed
disk):

- GravTree achieves **1.3–2.6× lower p99 latency** on all four
  workloads (sequential write, random write, point read, mixed).
- **p99.99 write latency stays below 20 μs** on every workload, versus
  31–2117 μs for RocksDB.
- **Point reads are 2.6× faster at p99** and 2.4× higher in throughput.
- Write throughput is 21–80× lower than RocksDB due to whole-node
  rewrites on every LocalFlush (measured write amplification: 9.4×
  sequential, 180× random at 10M keys). This is the primary limitation
  of the current prototype and the subject of planned future work.

# Acknowledgements

The author thanks the open-source RocksDB team for providing a
well-documented benchmark baseline.

# References
