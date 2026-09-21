# hft-lob: Ultra-Low-Latency L2 Limit Order Book (C++20)

A production-style Level 2 order book reconstructor for NASDAQ ITCH 5.0:
O(1) add / cancel / execute / replace on a flat price ladder, with no dynamic
allocation and no locks on that path.

It replays NASDAQ's full historical ITCH 5.0 sample day for 2019-01-30
(368,366,634 messages into 8,695 order books) with no order dropped and no
price rounded. A deliberately naive reference model, run beside it over the
whole day, matched every book at all 76 checkpoints, level by level and order
by order: **zero mismatches** (*Recorded-day replay* below). That run is a
correctness run; this README reports no latency or throughput figure measured
on the recorded day.

The throughput figures in *Phase 5* come from a **synthetic** ITCH 5.0 feed
generated in-process by the mock in `src/`: end-to-end single-core throughput
measures **4.2-10.6M messages/second** across the two machines benchmarked
there. Throughput depends on the host as much as on the code: the slower of
the two machines measures about half the faster one's single-core rate, and
both vary run to run, so read the figures as measurements on the two named
machines rather than as a spec. No market data is stored in this repo.

## Layout

```
include/lob/common.hpp      byte-order shims, rdtsc/rdtscp, branch hints
include/lob/order_pool.hpp  Phase 1: Order (32B), slab OrderPool, flat OrderIdMap
include/lob/book.hpp        Phase 2: PriceLevel, BookSide, LimitOrderBook (ladder
                            only) and ExactOrderBook (ladder + exact overflow)
include/lob/itch.hpp        Phase 3: zero-copy ITCH 5.0 dispatch + FeedHandler
src/main.cpp                Phase 4: unit checks, differential fuzz, benchmarks,
                            ExactOrderBook checks and wire-unit fuzz
include/lob/spsc.hpp        Phase 5: wait-free SPSC ring (128B isolation,
                            cached indices, capped batches)
include/lob/engine.hpp      Phase 5: sharded multi-core engine (demux, rings,
                            pinned workers, locate % W sharding)
src/parallel_main.cpp       Phase 5: parallel-vs-sequential verification +
                            scaling benchmarks
src/spsc_stress.cpp         Phase 5: sequence-checked SPSC ring stress (the
                            ThreadSanitizer target in CI)
src/replay_main.cpp         recorded-day replay: chunked reader, pre-scan
                            sizing, per-locate ExactOrderBooks, differential
src/ref_market.hpp          naive whole-market reference model (own decoder)
src/book_diff.hpp           full book comparison: levels, queues, BBO
src/wire_gen.hpp            generated multi-symbol wire-unit ITCH stream (tests)
results/                    replay console log and the input data manifest
.github/workflows/ci.yml    g++ / clang++ / MinGW builds, ASan+UBSan and
                            TSan legs (correctness only, see Reproducibility)
```

## Architecture decisions

| Concern | Choice | Rejected alternative |
|---|---|---|
| Order storage | 32-byte POD in a pre-allocated slab, addressed by `u32` index (2 orders / cache line) | heap `Order*` (allocation + 8-byte pointers + fragmentation) |
| Free management | intrusive LIFO free list through `Order::next` (hottest slot reused first) | `std::deque` free queue |
| Order id lookup | flat open-addressing map, Fibonacci hash, linear probe, backward-shift erase (no tombstone decay over a 6.5h session) | `std::unordered_map` (node-based, ~2 misses + malloc per op) |
| Price ladder | flat tick-indexed array per side: `levels[price - base]`, O(1) unconditionally (`ExactOrderBook`: `(price - base) / tick`, the divide done as one multiply-high) | `std::map` (O(log n) + serialized pointer-chase misses) |
| Prices off the ladder | `ExactOrderBook`: an exact `std::map` overflow per side holding the same pool FIFO, merged with the ladder for BBO and depth; `LimitOrderBook`: dropped and counted | rounding to the nearest tick, or dropping them silently |
| Price units | `u32` in the caller's units: cents for the synthetic benchmarks, the ITCH wire unit ($0.0001) for recorded data | whole cents everywhere (merges sub-penny levels); `int32` (cannot hold prices above $214,748) |
| Best-price rediscovery | occupancy bitmap + `tzcnt`/`lzcnt` (64 prices/instruction; next level is almost always in the same word because activity clusters at the inside) | linear level scan |
| Queue at a level | intrusive doubly-linked FIFO via pool indices | `std::list` / `std::deque` per level |
| Side dispatch | `BookSide<IsBid>` template; comparison & scan direction compile-time | runtime branch per touch |
| Parsing | `#pragma pack(1)` wire-mirror structs + `reinterpret_cast` + one `bswap` per field | field-by-field copy-out |
| Threading | single writer per book (feed is inherently sequential); shard symbols across cores, SPSC queues at the edges | locks/atomics inside the book |

## Recorded-day replay: NASDAQ ITCH 5.0, 2019-01-30

`lob_replay` (`src/replay_main.cpp`) streams NASDAQ's public ITCH 5.0 sample
file for 2019-01-30 (11,245,883,092 bytes decompressed) in 256 MB chunks and
builds one `ExactOrderBook` per `stock_locate`, fed through the same
`itch::dispatch_checked` as the benchmarks. The file is not in this repo;
[`results/manifest_20190130.md`](results/manifest_20190130.md) gives its
source, sizes and SHA-256.

`ExactOrderBook` shares every line of the benchmark book's ladder, bitmap,
FIFO, pool and id-map code, with three differences. Prices stay in ITCH wire
units ($0.0001), so a sub-penny price is its own level and prices above
$214,748 fit. Each book has its own ladder base, width and tick. A price the
ladder does not hold (below it, above it, or between two of its grid points)
rests in an exact per-side `std::map` overflow instead of being dropped or
rounded; the ladder path allocates nothing, the overflow allocates one node
per new level.

**Sizing comes from a pre-scan.** The replay reads the file twice. Pass 1
counts and length-checks every message, reads the Stock Directory, finds each
symbol's peak resting-order count and keeps its add prices; pass 2 is the
replay. A book's tick is one cent when the symbol's median add price is at
least $1.00 (Reg NMS keeps displayed quotes there in whole cents) and $0.0001
below that. Ladder widths, from 64 to 131,072 ticks on this day, come from a
greedy split of a 1,024 MB ladder budget by adds covered per tick, each ladder
placed on the window where that symbol's adds arrived; 99.898% of the day's
adds landed on a ladder. The pre-scan sets sizes only: an add that misses the
ladder rests in the overflow, so the reconstructed books do not depend on it.

The reference (`src/ref_market.hpp`) is deliberately naive and shares no code
with the books: it decodes every message from the specification's byte
offsets rather than the packed structs, keeps exact prices as `std::map` keys
with no ladder or tick, holds every order of the market in one
`std::unordered_map`, and keeps each level's queue as a `std::list`. With
`--differential` it processes the same messages, and at every checkpoint
every book is compared with it in full (`src/book_diff.hpp`): both sides level
by level (price, aggregate shares, order count), every queue order by order
(reference number, remaining shares), the BBO and the resting-order count.

One run, [`results/replay_20190130_differential.log`](results/replay_20190130_differential.log)
(Core Ultra 7 265H laptop, Windows 11, g++ 16.1.0):

| | |
|---|---:|
| messages | 368,366,634 |
| add orders `A` / `F` | 162,970,455 / 1,725,898 |
| executions `E` / `C` | 8,096,995 / 158,886 |
| cancels `X`, deletes `D`, replaces `U` | 4,669,874 / 158,273,361 / 27,222,746 |
| stock_locates in the directory / with order messages | 8,713 / 8,695 |
| bad-length messages, orders dropped, unknown order ids | 0 / 0 / 0 |
| adds on a ladder / in the overflow | 191,722,488 / 196,611 |
| checkpoints (every 5M messages, 12:00 and 16:00 ET, end of file) | 76 |
| price levels / queued orders compared | 49,562,417 / 119,295,162 |
| **mismatches** | **0** |
| resting orders at end of file | 0 |

The log also prints the books of AAPL, MSFT, AMZN, BKNG and WFT at 12:00 and
16:00 ET, and the opening and closing cross prices the file carries (AAPL's
closing cross: 165.25). BRK.A is not in this day's stock directory, so the
file carries no book for it.

Not measured here: per-message latency, and throughput on this data. The
replay times only its dispatch loop over a chunk already in memory, never the
file reads, but in a differential run the reference processes each segment
between the timed ones, so the time that run prints is not a measurement.

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

Four single-translation-unit binaries, no build system. Windows with
MinGW-w64 g++ (`winget install BrechtSanders.WinLibs.POSIX.UCRT`;
`.\build.ps1 [-Run] [-Quick]` runs these same lines):

```powershell
g++ -std=c++20 -O3 -march=native -DNDEBUG -Wall -Wextra -pthread -static `
    -I include src/main.cpp -o lob_bench.exe
g++ -std=c++20 -O3 -march=native -DNDEBUG -Wall -Wextra -pthread -static `
    -I include src/parallel_main.cpp -o lob_parallel.exe
g++ -std=c++20 -O3 -march=native -DNDEBUG -Wall -Wextra -pthread -static `
    -I include src/spsc_stress.cpp -o spsc_stress.exe
g++ -std=c++20 -O3 -march=native -DNDEBUG -Wall -Wextra -pthread -static `
    -I include src/replay_main.cpp -o lob_replay.exe
./lob_bench.exe       # unit checks, differential fuzz, latency percentiles, single-core throughput
./lob_parallel.exe    # parallel-vs-sequential verification, multi-core scaling tables
./spsc_stress.exe     # 2M sequence-checked messages through the SPSC ring
./lob_replay.exe --selftest                          # generated day, 4 KB chunks, differential
./lob_replay.exe 01302019.NASDAQ_ITCH50 --differential   # the recorded day (see results/)
```

Linux with g++ >= 11 or clang++ >= 14 (`std::latch` needs libstdc++ 11+):

```bash
g++ -std=c++20 -O3 -march=native -DNDEBUG -Wall -Wextra -pthread -I include src/main.cpp          -o lob_bench
g++ -std=c++20 -O3 -march=native -DNDEBUG -Wall -Wextra -pthread -I include src/parallel_main.cpp -o lob_parallel
g++ -std=c++20 -O3 -march=native -DNDEBUG -Wall -Wextra -pthread -I include src/spsc_stress.cpp   -o spsc_stress
g++ -std=c++20 -O3 -march=native -DNDEBUG -Wall -Wextra -pthread -I include src/replay_main.cpp   -o lob_replay
./lob_bench && ./lob_parallel && ./spsc_stress && ./lob_replay --selftest
```

`lob_bench` and `lob_parallel` accept `--quick` (the mode CI runs): every
correctness check still runs, only the benchmark sizes shrink, and the
numbers a `--quick` run prints are not measurements.

`lob_replay FILE` takes the decompressed BinaryFILE. Options:
`--differential` (run the reference and compare at every checkpoint),
`--checkpoint N` (default 5,000,000 messages), `--at HH:MM:SS,...` (extra
checkpoints that also print the named books; default 12:00:00,16:00:00 ET),
`--symbols A,B,...`, `--depth N`, `--chunk-mb N` (default 256),
`--ladder-mb N` (default 1024), `--cpu N`. It exits non-zero unless every
check passes. `--selftest` writes a generated 40-symbol day and replays it
with the differential through 4,103-byte chunks, so that messages and length
prefixes straddle chunk boundaries thousands of times.

`lob_bench` runs, in order (a failed check makes it exit non-zero):
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
5. `ExactOrderBook` unit checks: the first and last ladder ticks and one past
   them, overflow below, above and between grid points, replaces that move an
   order onto and off the ladder, a FIFO inside one overflow level, sub-penny
   and past-int32 prices, and the reciprocal division behind the ladder index
6. `ExactOrderBook` differential fuzz: 2M generated wire-unit messages over
   five symbols whose ladders the prices straddle, through `ExactOrderBook`s
   and the `ref::Market` reference used by the replay, with a full audit of
   depth and queue order every 50,000 messages and at the end (41 per run)

`lob_parallel` runs the same multi-instrument stream through one thread and
through the sharded engine and requires every instrument's full-band depth,
the processed-message count and the drop / bad-length counters to agree
before it prints any scaling table.

## Reproducibility

- Every figure in *Phase 5* above is printed by `lob_bench` (single-core,
  `src/main.cpp`) or `lob_parallel` (demux and multi-channel scaling,
  `src/parallel_main.cpp`), built with the `-O3 -march=native -DNDEBUG` lines
  in *Build & run* and run without `--quick`, on the synthetic stream those
  binaries generate in-process; they read no market data. The runs behind the
  figures are recorded in the commit messages of `67d6b20` (first
  measurements: 13.8M single-core, 148M across 23 cores) and `554bbd0`
  (ranges over six repeated idle runs). No console log is committed for
  them: build, run, and expect different absolute numbers.
- Every figure in *Recorded-day replay* is in
  `results/replay_20190130_differential.log`, the console output of
  `lob_replay 01302019.NASDAQ_ITCH50 --differential` built from this tree, and
  the input's sizes and hashes are in `results/manifest_20190130.log`. The
  data itself is NASDAQ's and is not redistributed here;
  `results/manifest_20190130.md` says where to get it and how to check a copy.
- `.github/workflows/ci.yml` builds all three binaries single-TU with
  `-std=c++20 -O2 -Wall -Wextra -pthread` on ubuntu (g++-13, clang++-18) and
  Windows (MinGW g++) and runs `lob_bench --quick`, `lob_parallel --quick` and
  `spsc_stress`. An ASan+UBSan leg runs the same three, and a TSan leg runs
  `spsc_stress`. CI verifies **correctness** (unit checks, differential fuzz,
  parallel-vs-sequential depth equality, sanitizers, TSan) and **not
  throughput**: the msgs/s a shared 4-vCPU runner prints under `--quick` are
  not measurements and are not the numbers in this README. The `ExactOrderBook`
  checks and fuzz run in CI as part of `lob_bench --quick`; the workflow does
  not yet build `lob_replay`, whose `--selftest` runs locally through
  `build.ps1 -Run`.
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

- One instrument per book (standard sharding unit). Multi-symbol support is
  a `stock_locate`-indexed book table in front of the handler, as in
  `lob_replay` and the parallel engine.
- `LimitOrderBook`, the benchmark book, keeps integer cents inside a
  configurable band (default $0.01-$1310.72, 3 MB of ladder per side) and
  drops and counts out-of-band adds. `ExactOrderBook`, the replay's book,
  holds any u32 wire price exactly, routing what its ladder does not cover to
  the overflow.
- `lob_replay` sizes its books from a pre-scan of the same file, which suits a
  historical replay. A live feed handler would size from the previous day or
  a reserve, since it cannot read the day ahead.
- The book does not match crossing orders; it is a *reconstructor*, and crossings
  are resolved by the venue and arrive as Execute messages, per ITCH semantics.
- Latency outliers (the `max` column: tens to hundreds of microseconds on an
  idle desktop, milliseconds on one doing other work) are OS preemption rather
  than book work; the p99 sits orders of magnitude below them. On a tuned host
  you'd pin to an isolated core (`isolcpus`), disable SMT on that core, and
  use huge pages for the slab and ID map.
