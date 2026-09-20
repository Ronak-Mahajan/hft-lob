# hft-lob: Ultra-Low-Latency L2 Limit Order Book (C++20)

A production-style Level 2 order book reconstructor for NASDAQ ITCH 5.0:
O(1) add / cancel / execute / replace, zero dynamic allocation and zero locks
on the critical path. On a **synthetic** ITCH 5.0 feed, end-to-end single-core
throughput measures **4.2-10.6M messages/second** across the two machines
benchmarked in *Phase 5*.

The feed is generated in-process by the mock in `src/`. This repo contains no
recorded NASDAQ data, and every number below comes from that generated stream.
Throughput depends on the host as much as on the code: the slower of the two
machines measures about half the faster one's single-core rate, and both vary
run to run, so read the figures as measurements on the two named machines
rather than as a spec.

## Layout

```
include/lob/common.hpp      byte-order shims, rdtsc/rdtscp, branch hints
include/lob/order_pool.hpp  Phase 1: Order (32B), slab OrderPool, flat OrderIdMap
include/lob/book.hpp        Phase 2: PriceLevel, BookSide<IsBid>, LimitOrderBook
include/lob/itch.hpp        Phase 3: zero-copy ITCH 5.0 dispatch + FeedHandler
src/main.cpp                Phase 4: unit checks, differential fuzz, benchmarks
include/lob/spsc.hpp        Phase 5: wait-free SPSC ring (128B isolation,
                            cached indices, capped batches)
include/lob/engine.hpp      Phase 5: sharded multi-core engine (demux, rings,
                            pinned workers, locate % W sharding)
src/parallel_main.cpp       Phase 5: parallel-vs-sequential verification +
                            scaling benchmarks
src/spsc_stress.cpp         Phase 5: sequence-checked SPSC ring stress (the
                            ThreadSanitizer target in CI)
.github/workflows/ci.yml    g++ / clang++ / MinGW builds, ASan+UBSan and
                            TSan legs (correctness only, see Reproducibility)
```

## Architecture decisions

| Concern | Choice | Rejected alternative |
|---|---|---|
| Order storage | 32-byte POD in a pre-allocated slab, addressed by `u32` index (2 orders / cache line) | heap `Order*` (allocation + 8-byte pointers + fragmentation) |
| Free management | intrusive LIFO free list through `Order::next` (hottest slot reused first) | `std::deque` free queue |
| Order id lookup | flat open-addressing map, Fibonacci hash, linear probe, backward-shift erase (no tombstone decay over a 6.5h session) | `std::unordered_map` (node-based, ~2 misses + malloc per op) |
| Price ladder | flat tick-indexed array per side: `levels[price - base]`, O(1) unconditionally | `std::map` (O(log n) + serialized pointer-chase misses) |
| Best-price rediscovery | occupancy bitmap + `tzcnt`/`lzcnt` (64 prices/instruction; next level is almost always in the same word because activity clusters at the inside) | linear level scan |
| Queue at a level | intrusive doubly-linked FIFO via pool indices | `std::list` / `std::deque` per level |
| Side dispatch | `BookSide<IsBid>` template; comparison & scan direction compile-time | runtime branch per touch |
| Parsing | `#pragma pack(1)` wire-mirror structs + `reinterpret_cast` + one `bswap` per field | field-by-field copy-out |
| Threading | single writer per book (feed is inherently sequential); shard symbols across cores, SPSC queues at the edges | locks/atomics inside the book |

## Phase 5: multi-core scale (single-writer shards, wait-free rings)

Whole-market processing: a demux thread peeks `stock_locate` (2 bytes, no
decode) and routes raw messages through wait-free SPSC rings to workers that
own disjoint instrument sets (`locate % W`). Per-instrument message order is
preserved end-to-end, so the single-writer Phase 2 book is reused unchanged:
zero locks, zero atomics in the book itself.

Key mechanics (`spsc.hpp`):
- head/tail publications isolated on **128-byte** boundaries (Intel's
  adjacent-line prefetcher moves cache-line pairs, so 64B padding still
  false-shares)
- each side keeps a private **cached copy** of the other's index; the shared
  line is touched ~once per batch, not once per message
- consumer batches capped at 256 slots so `head` publication stays fresh;
  uncapped draining of a full ring head-of-line-blocks the producer
- exponential backoff on empty polls: a spinning consumer holds `tail_` in
  Shared state and taxes every producer push with an RFO
- 64-byte message slots: one line per message, hardware-prefetch friendly

**Scope of the concurrency claim.** Nothing in this repo takes a mutex, and
the ring's `try_push` / `consume_batch` are wait-free: each finishes in a
bounded number of steps, with no retry loop inside the operation. The book
itself is not a lock-free data structure and does not need to be. It is
**single-writer**, which is why it needs no synchronization at all, and
sharding by `stock_locate` is what makes it scale. The pipeline as a whole is
not non-blocking: the rings are bounded, so a full ring makes the demux spin
until its worker drains it. `spsc_stress` prints that full-stall count on
every run, and it is not small.

Verification: the same generated 16M-message / 128-instrument stream is run
through the parallel engine (8 workers) and a single thread; **the full-band
depth of every instrument must be identical**, level by level, along with the
processed-message count and the drop / bad-length counters (proves the
sharding invariant).

Measured on the **synthetic** stream above (Core Ultra 9 275HX, 8P+16E,
Windows 11). Single core, end to end: 8.5-10.6M msgs/s across six idle runs,
best recorded run 13.8M. Throughput is sensitive to turbo and DRAM contention,
so read 10M as this machine's round number and not as a floor anywhere else.
A single demux feeding 8 workers reaches ~49M msgs/s and is demux-bound beyond
that. **Only with the feed pre-split per shard** (exactly how NASDAQ
distributes ITCH across parallel MoldUDP channels, with the split done offline,
outside the timed region) do 23 workers measure **113-120M msgs/s aggregate**,
best recorded run 148M. Through a single demux thread, the same machine does
not reach that figure.

The second machine shows how much the host matters. On a Core Ultra 7 265H
(16 cores, Windows 11, other work running) the same binaries measure
4.2-6.4M msgs/s single core, 10.5-15M through a demux with 8 workers, and
40-54M aggregate across 15 pre-split channels. Those spans are repeated runs
on a machine that was not idle, and the low end of each is what a loaded
laptop gives you.

## Build & run

Three single-translation-unit binaries, no build system. Windows with
MinGW-w64 g++ (`winget install BrechtSanders.WinLibs.POSIX.UCRT`;
`.\build.ps1 [-Run] [-Quick]` runs these same lines):

```powershell
g++ -std=c++20 -O3 -march=native -DNDEBUG -Wall -Wextra -pthread -static `
    -I include src/main.cpp -o lob_bench.exe
g++ -std=c++20 -O3 -march=native -DNDEBUG -Wall -Wextra -pthread -static `
    -I include src/parallel_main.cpp -o lob_parallel.exe
g++ -std=c++20 -O3 -march=native -DNDEBUG -Wall -Wextra -pthread -static `
    -I include src/spsc_stress.cpp -o spsc_stress.exe
./lob_bench.exe       # unit checks, differential fuzz, latency percentiles, single-core throughput
./lob_parallel.exe    # parallel-vs-sequential verification, multi-core scaling tables
./spsc_stress.exe     # 2M sequence-checked messages through the SPSC ring
```

Linux with g++ >= 11 or clang++ >= 14 (`std::latch` needs libstdc++ 11+):

```bash
g++ -std=c++20 -O3 -march=native -DNDEBUG -Wall -Wextra -pthread -I include src/main.cpp          -o lob_bench
g++ -std=c++20 -O3 -march=native -DNDEBUG -Wall -Wextra -pthread -I include src/parallel_main.cpp -o lob_parallel
g++ -std=c++20 -O3 -march=native -DNDEBUG -Wall -Wextra -pthread -I include src/spsc_stress.cpp   -o spsc_stress
./lob_bench && ./lob_parallel && ./spsc_stress
```

`lob_bench` and `lob_parallel` accept `--quick` (the mode CI runs): every
correctness check still runs, only the benchmark sizes shrink, and the
numbers a `--quick` run prints are not measurements.

`lob_bench` self-verifies before benchmarking:
1. deterministic unit checks (FIFO priority, BBO transitions, replace semantics)
2. **differential fuzz**: 2M generated ITCH messages from a fixed seed
   (`A`/`F`/`E`/`C`/`X`/`D`/`U`, all seven types present and counted in the
   printed mix) replayed simultaneously into this book and a naive
   `std::map` + `std::unordered_map` reference. The fast book reads the wire
   bytes through the length-validated dispatch, so the parser is on trial too,
   while the reference replays the generator's decoded op list. BBO is
   compared after *every* one of the 2M messages, and every price level in the
   whole 131072-tick band is audited against the reference every 50k messages
   (40 full-band audits per run). A corrupt-length frame must be refused and
   counted, never parsed. The reference replay is then timed next to the flat
   book and the ratio printed as a reference-implementation comparison on that
   stream
3. per-op latency percentiles (`rdtscp`-serialized, timer overhead subtracted)
4. end-to-end binary-stream throughput through the feed handler, with the
   out-of-band-drop / bad-length / live-order counters (all zero drops on a
   well-formed in-band stream)

`lob_parallel` runs the same multi-instrument stream through one thread and
through the sharded engine and requires every instrument's full-band depth,
the processed-message count and the drop / bad-length counters to agree
before it prints any scaling table.

## Reproducibility

- Every figure in *Phase 5* above is printed by `lob_bench` (single-core,
  `src/main.cpp`) or `lob_parallel` (demux and multi-channel scaling,
  `src/parallel_main.cpp`), built with the `-O3 -march=native -DNDEBUG` lines
  in *Build & run* and run without `--quick`, on the synthetic stream those
  binaries generate in-process. No market data is read from disk, because none
  is distributed here. The runs behind the figures are recorded in the commit
  messages of `67d6b20` (first measurements: 13.8M single-core, 148M across
  23 cores) and `554bbd0` (ranges over six repeated idle runs). No console log
  is committed: build, run, and expect different absolute numbers.
- `.github/workflows/ci.yml` builds all three binaries single-TU with
  `-std=c++20 -O2 -Wall -Wextra -pthread` on ubuntu (g++-13, clang++-18) and
  Windows (MinGW g++) and runs `lob_bench --quick`, `lob_parallel --quick` and
  `spsc_stress`. An ASan+UBSan leg runs the same three, and a TSan leg runs
  `spsc_stress`. CI verifies **correctness** (unit checks, differential fuzz,
  parallel-vs-sequential depth equality, sanitizers, TSan) and **not
  throughput**: the msgs/s a shared 4-vCPU runner prints under `--quick` are
  not measurements and are not the numbers in this README.
- Guards make a bad run loud. A length prefix that disagrees with the per-type
  ITCH table is refused before any cast and counted (`bad_length`), and
  out-of-band adds are counted (`dropped_out_of_band`). The id map aborts
  instead of probing forever when full, the book checks pool <= idmap / 2 at
  construction, and the SPSC ring checks that a message fits its slot. Those
  three are `LOB_ASSERT`, a branch to `std::abort()` that `-DNDEBUG` does
  *not* compile out, so they hold in the release builds benchmarked here. All
  counters are printed at the end of every run, and CI requires the
  bad-length count to be zero.

## Notes & limits

- One instrument per `LimitOrderBook` (standard sharding unit). Multi-symbol
  support is a `stock_locate`-indexed book table in front of the handler.
- Prices are integer cents inside a configurable band (default $0.01-$1310.72,
  3 MB of ladder per side). Out-of-band adds are dropped and counted, as a
  prod handler would route them to a slow path.
- The book does not match crossing orders; it is a *reconstructor*, and crossings
  are resolved by the venue and arrive as Execute messages, per ITCH semantics.
- Latency outliers (the `max` column: tens to hundreds of microseconds on an
  idle desktop, milliseconds on one doing other work) are OS preemption rather
  than book work; the p99 sits orders of magnitude below them. On a tuned host
  you'd pin to an isolated core (`isolcpus`), disable SMT on that core, and
  use huge pages for the slab and ID map.
