# hft-lob — Ultra-Low-Latency L2 Limit Order Book (C++20)

A production-style Level 2 order book reconstructor for NASDAQ ITCH 5.0:
O(1) add / cancel / execute / replace, zero dynamic allocation and zero locks
on the critical path, ~10 M+ messages/second end-to-end on commodity hardware.

## Layout

```
include/lob/common.hpp      byte-order shims, rdtsc/rdtscp, branch hints
include/lob/order_pool.hpp  Phase 1: Order (32B), slab OrderPool, flat OrderIdMap
include/lob/book.hpp        Phase 2: PriceLevel, BookSide<IsBid>, LimitOrderBook
include/lob/itch.hpp        Phase 3: zero-copy ITCH 5.0 dispatch + FeedHandler
src/main.cpp                Phase 4: unit checks, differential fuzz, benchmarks
include/lob/spsc.hpp        Phase 5: wait-free SPSC ring (128B isolation,
                            cached indices, capped batches)
include/lob/engine.hpp      Phase 5: sharded multi-core engine (demux → rings
                            → pinned workers, locate % W sharding)
src/parallel_main.cpp       Phase 5: parallel-vs-sequential verification +
                            scaling benchmarks
```

## Architecture decisions

| Concern | Choice | Rejected alternative |
|---|---|---|
| Order storage | 32-byte POD in a pre-allocated slab, addressed by `u32` index (2 orders / cache line) | heap `Order*` (allocation + 8-byte pointers + fragmentation) |
| Free management | intrusive LIFO free list through `Order::next` — hottest slot reused first | `std::deque` free queue |
| ID → order lookup | flat open-addressing map, Fibonacci hash, linear probe, backward-shift erase (no tombstone decay over a 6.5h session) | `std::unordered_map` (node-based, ~2 misses + malloc per op) |
| Price ladder | flat tick-indexed array per side: `levels[price - base]` — O(1) unconditionally | `std::map` (O(log n) + serialized pointer-chase misses) |
| Best-price rediscovery | occupancy bitmap + `tzcnt`/`lzcnt` (64 prices/instruction; next level is almost always in the same word because activity clusters at the inside) | linear level scan |
| Queue at a level | intrusive doubly-linked FIFO via pool indices | `std::list` / `std::deque` per level |
| Side dispatch | `BookSide<IsBid>` template — comparison & scan direction compile-time | runtime branch per touch |
| Parsing | `#pragma pack(1)` wire-mirror structs + `reinterpret_cast` + one `bswap` per field | field-by-field copy-out |
| Threading | single writer per book (feed is inherently sequential); shard symbols across cores, SPSC queues at the edges | locks/atomics inside the book |

## Phase 5 — multi-core scale (lock-free sharding)

Whole-market processing: a demux thread peeks `stock_locate` (2 bytes, no
decode) and routes raw messages through wait-free SPSC rings to workers that
own disjoint instrument sets (`locate % W`). Per-instrument message order is
preserved end-to-end, so the single-writer Phase 2 book is reused unchanged —
zero locks, zero atomics in the book itself.

Key mechanics (`spsc.hpp`):
- head/tail publications isolated on **128-byte** boundaries (Intel's
  adjacent-line prefetcher moves cache-line pairs — 64B padding still
  false-shares)
- each side keeps a private **cached copy** of the other's index; the shared
  line is touched ~once per batch, not once per message
- consumer batches capped at 256 slots so `head` publication stays fresh —
  uncapped draining of a full ring head-of-line-blocks the producer
- exponential backoff on empty polls: a spinning consumer holds `tail_` in
  Shared state and taxes every producer push with an RFO
- 64-byte message slots: one line per message, hardware-prefetch friendly

Verification: the same 16M-message / 128-instrument stream is run through the
parallel engine and a single thread; **full-band depth of every instrument
must be byte-identical** (proves the sharding invariant).

Measured (Core Ultra 9 275HX, 8P+16E, Windows 11): single demux feeding 8
workers reaches ~49M msgs/s, demux-bound beyond that. With the feed pre-split
per shard — exactly how NASDAQ distributes ITCH across parallel MoldUDP
channels — 23 workers reach **~148M msgs/s aggregate**.

## Build & run

```powershell
g++ -std=c++20 -O3 -march=native -DNDEBUG -Wall -Wextra -static `
    -I include src/main.cpp -o lob_bench.exe
./lob_bench.exe
```

The binary self-verifies before benchmarking:
1. deterministic unit checks (FIFO priority, BBO transitions, replace semantics)
2. **differential fuzz** — 2M random ITCH messages replayed simultaneously into
   this book and a naive `std::map` reference; BBO compared after *every*
   message, full depth audited every 50k
3. per-op latency percentiles (`rdtscp`-serialized, timer overhead subtracted)
4. end-to-end binary-stream throughput through the feed handler

## Notes & limits

- One instrument per `LimitOrderBook` (standard sharding unit). Multi-symbol =
  `stock_locate → book` table in front of the handler.
- Prices are integer cents inside a configurable band (default $0.01–$1310.72,
  3 MB of ladder per side). Out-of-band adds are dropped, as a prod handler
  would route them to a slow path.
- The book does not match crossing orders — it is a *reconstructor*: crossings
  are resolved by the venue and arrive as Execute messages, per ITCH semantics.
- Latency outliers (max ≈ 100 µs) are OS preemption; on a tuned host you'd pin
  to an isolated core (`isolcpus`), disable SMT on that core, and use huge
  pages for the slab and ID map.
