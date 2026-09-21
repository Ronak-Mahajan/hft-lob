# hft-lob: NASDAQ ITCH 5.0 limit order book (C++20)

Level 2 order book reconstruction for NASDAQ TotalView-ITCH 5.0. Add, cancel,
execute and replace are O(1) on a flat per-symbol price ladder (when the best
level empties, a bitmap scan finds the next one), with no allocation and no
locks on that path. A price the ladder does not hold rests, exactly, in a
per-side overflow, so no order is ever dropped or rounded.

## Result: NASDAQ's full trading day of 2019-01-30

`lob_replay` replays NASDAQ's public historical TotalView-ITCH 5.0 sample file
for 2019-01-30, the whole day: 368,366,634 messages (11,245,883,092 bytes
decompressed) into 8,695 order books, one per `stock_locate` that carries
orders.

- **Every book reconstructed, nothing dropped or rounded.** 0 orders dropped,
  0 unknown order ids, 0 malformed messages. Prices stay in ITCH's native
  $0.0001 units, so sub-penny levels stay separate, and the 587,096 adds
  priced outside $0.01-$1,310.72 (AMZN and BKNG trade above that range) are
  held exactly.
- **Zero mismatches against a reference.** A deliberately naive reference
  model that shares no code with the book processed the same messages, one
  at a time. After every one of the 363,118,215 order messages, the book's
  resting-order count and BBO, the orders the message named and the price
  levels it touched were compared with the model; at all 76 checkpoints of
  the day every book was compared with it in full: 660,820 book comparisons
  covering 49,562,417 price levels and 119,295,162 queued orders, order by
  order. Mismatches: **0** in both.
- **The file's closing crosses match the official closes.** For **10 of 10**
  symbols checked (AAPL, MSFT, AMZN, GOOGL, FB, INTC, CSCO, NVDA, TSLA,
  NFLX), the closing cross price in the file equals the official closing
  price to the cent, as served by Nasdaq and, separately, by Yahoo Finance.
  An independent decoder that shares no code with the book counts the same
  messages by type and prints the same books for those ten symbols at 16:00
  ET.
- **Per-message latency over the whole day.** Every one of the 368,366,634
  messages timed on its own (the book lookup, decode and book update, on a
  file already in memory), on one P-core: **p50 183 ns, p90 469 ns, p99
  1,082 ns, p99.9 1,516 ns**.
- **Throughput over the whole day.** **8.91M messages/second** on one P-core
  (median of five runs, 112.22 ns/message; range 8.85-9.06M), **52.54M**
  through one demux thread feeding five workers, and **99.80M** aggregate
  with the input pre-split across 13 workers. File reads are outside the
  timed loops.

Machine: Intel Core Ultra 7 265H laptop (6 P-cores, 8 E-cores, 2 low-power
E-cores, no SMT), Windows 11, on AC power; g++ 16.1.0, `-O3 -march=native`.
One recorded day. Every book's capacity, and where its price ladder sits,
comes from a pre-scan of the same file (see *Recorded-day replay*), which a
live feed handler cannot do: it would size from the previous day. Placed
instead at each symbol's first add, the ladders catch fewer than half the
adds and one P-core applies the day at 4.04M messages/second (see
*Recorded-day performance*). Not measured: memory use, the core clock during
the runs, file-read time, and any other machine or OS. The multi-core
configurations ran once each.

| Figures | Committed console output |
|---|---|
| replay, drops, reference comparison | [`results/replay_20190130_differential.log`](results/replay_20190130_differential.log) |
| closing crosses vs official closes | [`results/closing_cross_20190130.md`](results/closing_cross_20190130.md), from [`itch_count_20190130.log`](results/itch_count_20190130.log), [`closing_cross_replay_20190130.log`](results/closing_cross_replay_20190130.log), [`official_close_nasdaq_20190130.log`](results/official_close_nasdaq_20190130.log) and [`official_close_yahoo_20190130.log`](results/official_close_yahoo_20190130.log) |
| per-message latency | [`results/perf_20190130_run_latency_1.log`](results/perf_20190130_run_latency_1.log) (runs 2 and 3 alongside, and a rebuilt rerun in [`perf_20190130_repro_latency.log`](results/perf_20190130_repro_latency.log)) |
| throughput | `results/perf_20190130_run_throughput_{1..5}.log`, `results/perf_20190130_run_multicore_{demux,presplit}.log`, all collected in [`results/perf_20190130.json`](results/perf_20190130.json) |
| the input file, and the command behind every result | [`data/MANIFEST.md`](data/MANIFEST.md) |

## Recorded-day replay

`lob_replay` (`src/replay_main.cpp`, the run itself in `src/replay_run.hpp`)
streams the decompressed file in 256 MB chunks, carrying a message that
straddles two chunks into the next, and applies every message through the
same `itch::dispatch_checked` as the benchmarks into one `ExactOrderBook` per
`stock_locate`. The file is not in this repo; `data/MANIFEST.md` gives its
source, sizes and SHA-256.

**Sizing comes from a pre-scan.** The replay reads the file twice. Pass 1
counts and length-checks every message, reads the Stock Directory, finds each
symbol's peak resting-order count and keeps its add prices; pass 2 is the
replay. A book's tick is one cent when the symbol's median add price is at
least $1.00 (Reg NMS Rule 612 keeps displayed quotes there in whole cents) and
$0.0001 below that: 8,323 books on a $0.01 grid and 372 on $0.0001. Ladder
widths, from 64 to 131,072 ticks, come from a greedy split of a 1,024 MB
ladder budget by adds covered per tick, each ladder placed on the window
where that symbol's adds arrived. 191,722,488 of the day's 191,919,099 adds
(99.898%) landed on a ladder and the other 196,611 in the overflow, exactly
as the pre-scan predicted. The pre-scan sets sizes only: an add that misses
the ladder rests in the overflow, so the reconstructed books do not depend
on it.

The reference (`src/ref_market.hpp`) decodes every message from the
specification's byte offsets rather than the packed structs, keeps exact
prices as `std::map` keys with no ladder or tick, holds every order of the
market in one `std::unordered_map`, and keeps each level's queue as a
`std::list`. With `--differential` the books and the reference take the file
one message at a time, and two comparisons run (`src/book_diff.hpp`). After
every order message, what it touched: the book's resting-order count and
BBO; the order it names (both orders of a replace), resting in both or in
neither with the same shares, price and side; that an order just added is
last in its level's queue in both; and the aggregate shares and order count
at every price it changed. At every checkpoint, every book in full: both
sides level by level (price, aggregate shares, order count), every queue
order by order (reference number, remaining shares), the BBO and the
resting-order count. The feed also checks the replay on its own terms: every
one of the day's executions, cancels, deletes and replaces named a resting
order (0 unknown order ids, in the books and in the model), none took more
shares than rested (the model counts that), and the day ends with 0 resting
orders in both.

From [`results/replay_20190130_differential.log`](results/replay_20190130_differential.log):

| | |
|---|---:|
| messages | 368,366,634 |
| add orders `A` / `F` | 162,970,455 / 1,725,898 |
| executions `E` / `C` | 8,096,995 / 158,886 |
| cancels `X`, deletes `D`, replaces `U` | 4,669,874 / 158,273,361 / 27,222,746 |
| stock_locates in the directory / with order messages | 8,713 / 8,695 |
| bad-length messages, orders dropped, unknown order ids | 0 / 0 / 0 |
| adds on a ladder / in the overflow | 191,722,488 / 196,611 |
| order messages checked right after they were applied | 363,118,215 |
| orders / price levels compared in those checks | 390,340,961 / 390,340,961 |
| **mismatches after a message** | **0** |
| checkpoints (every 5M messages, 12:00 and 16:00 ET, end of file) | 76 |
| price levels / queued orders compared at the checkpoints | 49,562,417 / 119,295,162 |
| **mismatches at a checkpoint** | **0** |
| resting orders at end of file | 0 |

The log also prints the books of AAPL, MSFT, AMZN, BKNG and WFT (a sub-dollar
stock on the $0.0001 grid) at 12:00 and 16:00 ET, and the opening and closing
cross prices the file carries. BRK.A is not in this day's stock directory.
After every 256 MB chunk the replay prints a digest of the full state of
every book (each level's price, shares and order count, and each queue order
by order; `books_digest()` in `src/replay_day.hpp`); the performance runs
below print the same digests, which is how they are tied to this run. The
dispatch time a differential run prints is not a measurement: the reference
runs between messages and evicts the books from cache.

`tools/itch_count.cpp` is a second, independent decoder: it includes nothing
from `include/lob` or `src` and decodes every field by byte offset. Its
counts ([`results/itch_count_20190130.log`](results/itch_count_20190130.log))
equal the replay's for every message type, and the file ends exactly on a
message boundary.

## Closing crosses vs official closes

From [`results/closing_cross_20190130.md`](results/closing_cross_20190130.md).
For a NASDAQ-listed stock the closing cross price is the NASDAQ Official
Closing Price. The file's closing cross ('Q' message, cross type 'C') is
compared with the official close from two sources fetched separately,
Nasdaq's own historical quotes and Yahoo Finance's daily bars, with their
later split adjustments undone
([`tools/official_close_nasdaq.sh`](tools/official_close_nasdaq.sh),
[`tools/official_close_yahoo.sh`](tools/official_close_yahoo.sh)). The two
sources agree with each other for all ten:

| | AAPL | MSFT | AMZN | GOOGL | FB | INTC | CSCO | NVDA | TSLA | NFLX |
|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| closing cross in the file | 165.25 | 106.38 | 1670.43 | 1097.99 | 150.42 | 47.54 | 46.71 | 137.39 | 308.77 | 340.66 |
| official close | 165.25 | 106.38 | 1670.43 | 1097.99 | 150.42 | 47.54 | 46.71 | 137.39 | 308.77 | 340.66 |

All ten are equal to the cent. The cross prices are data in the file, so
this checks the input; the reconstruction is checked against the reference
model above. At 16:00:00.000 ET `lob_replay` and the independent decoder
print identical books for all ten (resting orders, level counts, and the top
five levels with shares and order counts). At the moment each closing cross
is published, the independent decoder's book has bid <= cross price <= ask
for all ten.

## Recorded-day performance

`lob_perf` (`src/perf_main.cpp`) builds the same books as `lob_replay` from
the same pre-scan and streams the same file in 256 MB chunks. Every figure
here: the 265H laptop above, on AC power, Windows 11 on the Balanced power
plan, with a browser and other everyday applications open; g++ 16.1.0,
`-O3 -march=native -DNDEBUG`, binaries built from commit `d17968a` unless a
paragraph names another. Each run pins its thread (logical CPU 1, a P-core,
unless stated) and raises the process to high priority. The raw console
output of every run is a `results/perf_20190130_*.log` file, and
[`results/perf_20190130.json`](results/perf_20190130.json) collects every
number of the `d17968a` runs, generated from their logs by
`tools/perf_json.pl`.

**Single core, end to end.** Every message is framed, length-checked, routed
to its `stock_locate` book and applied (`dispatch_segment()`, the loop
`lob_replay` uses). Only that loop over a chunk already in memory is timed,
and the 42 chunk times are summed; file reads, the pre-scan and the digests
are outside the timed region. This build has no per-message instrumentation.
From `results/perf_20190130_run_throughput_{1..5}.log`:

| run | 1 | 2 | 3 | 4 | 5 |
|---|---:|---:|---:|---:|---:|
| M messages/s | 8.85 | 9.02 | 9.06 | 8.90 | 8.91 |
| ns/message | 112.97 | 110.82 | 110.33 | 112.34 | 112.22 |

Min / median / max: 8.85 / 8.91 / 9.06M messages/second. One run pinned to
logical CPU 2, an E-core, measured 6.30M messages/second (158.61 ns/message;
`results/perf_20190130_run_throughput_ecore.log`).

**Per-message latency.** A separate build, `lob_perf_latency`
(`-DLOB_PERF_LATENCY`), times every one of the day's 368,366,634 messages on
its own, with no sampling: `lfence; rdtsc`, the book lookup and
`dispatch_checked`, then `rdtscp; lfence`. The minimum of 100,000 empty timer
pairs, measured once before the pass (38 cycles in run 1), is subtracted from
every sample; later builds measure it throughout the pass (see below).
Cycles go into an exact histogram per message type, and percentiles are
nearest-rank. The TSC is invariant; CPUID 15h reports 3.686400 GHz, and the
tool measures it against `QueryPerformanceCounter`, in five 1 s windows and
over the whole pass (3.686398 GHz in every run). Run 1
([`results/perf_20190130_run_latency_1.log`](results/perf_20190130_run_latency_1.log)),
in nanoseconds:

| type | messages | p50 | p90 | p99 | p99.9 | max |
|---|---:|---:|---:|---:|---:|---:|
| `A` add | 162,970,455 | 184 | 341 | 692 | 998 | 3,444,762 |
| `F` add with MPID | 1,725,898 | 193 | 417 | 763 | 1,500 | 752,360 |
| `E` execute | 8,096,995 | 185 | 614 | 1,271 | 1,638 | 612,511 |
| `C` execute at price | 158,886 | 110 | 425 | 1,200 | 1,635 | 116,607 |
| `X` partial cancel | 4,669,874 | 80 | 365 | 948 | 1,421 | 3,230,745 |
| `D` delete | 158,273,361 | 177 | 591 | 1,227 | 1,601 | 2,930,690 |
| `U` replace | 27,222,746 | 224 | 564 | 1,087 | 1,546 | 2,428,755 |
| other (not applied) | 5,248,419 | 7 | 26 | 42 | 139 | 171,168 |
| order messages (`A` to `U`) | 363,118,215 | 184 | 472 | 1,086 | 1,519 | 3,444,762 |
| **all messages** | **368,366,634** | **183** | **469** | **1,082** | **1,516** | 3,444,762 |

Run 3 gives, for all messages, p50 181, p99 1,072 and p99.9 1,508. In run 2
the empty-timer calibration read 76 cycles instead of 38, which
over-subtracts every sample; its log is committed and its p99 (1,073) and
p99.9 (1,512) agree, but it is not used for the headline. Each sample is one
message timed in isolation behind a serializing fence, so the mean (231 ns)
is higher than the 112.22 ns/message of the median throughput run, where
consecutive messages overlap. The maxima, in the milliseconds, are not the
book's work: messages the book skips after one table lookup (the "other"
row) also reached 171,168 ns. They fit the thread losing its core to the OS
mid-message, which these runs do not record.

**Rebuilt and rerun.** Built again from commit `18d6082`, whose `lob_perf`
sources differ from `d17968a` only in how log lines format timestamps, and
run on the same laptop the same day: the latency build measured, for all
messages, p50 182 ns, p90 462, p99 1,058 and p99.9 1,502
([`results/perf_20190130_repro_latency.log`](results/perf_20190130_repro_latency.log)),
and the throughput build 8.87M messages/second
([`results/perf_20190130_repro_throughput.log`](results/perf_20190130_repro_throughput.log)),
both with every chunk's book digest equal to the differential run's.

**Timer cost under a changing clock.** The empty timer pair takes a fixed
number of core cycles, so its cost in TSC cycles rises when the core clocks
down. From commit `9f9ee75` the latency build measures the pair before the
pass, after each of the 42 chunks and after the pass; within a single run
its minimum after a chunk ranged from 34 to 82 cycles, or from 38 to 116.
The build subtracts the smallest minimum seen, so a sample taken while the
core ran slower keeps part of the timer's cost, and none has more than the
fastest state's cost removed. Two runs of that build with the pre-scan
placement measured, for all messages, p50 181 and 182 ns and p99 1,075 and
1,060 ns
([`results/perf_20190130_placement_prescan_latency_1.log`](results/perf_20190130_placement_prescan_latency_1.log),
[`_2.log`](results/perf_20190130_placement_prescan_latency_2.log)), in line
with run 1 above.

**Ladder placement without look-ahead.** Every figure above uses the
pre-scan's placement: each ladder sits where that symbol's adds arrived
during the day, and its width follows how widely they spread.
`--placement first-add` uses only what is known when a symbol's first add
arrives: every ladder 2,048 ticks wide (the widest that fits the 1,024 MB
budget across 8,695 books), centered on that first add, on a one-cent grid at
or above $1.00 and $0.0001 below. The books are the same (every chunk's
digest equals the differential run's), but the first add is a poor anchor:
93,816,959 of the 191,919,099 adds land on a ladder and the rest take the
overflow's `std::map`. With the same binary, back to back, one P-core applies
the day at 4.04M messages/second against 9.02M with the pre-scan placement
([`results/perf_20190130_placement_firstadd_throughput.log`](results/perf_20190130_placement_firstadd_throughput.log),
[`results/perf_20190130_placement_prescan_throughput.log`](results/perf_20190130_placement_prescan_throughput.log)),
and per-message latency for all messages is p50 235 ns, p90 805, p99 1,813
and p99.9 2,626
([`results/perf_20190130_placement_firstadd_latency.log`](results/perf_20190130_placement_firstadd_latency.log),
run between the two pre-scan latency runs above). A live feed handler would
place its ladders from the previous day's prices, which a one-day replay
cannot test.

**Multi-core.** The books are sharded by `stock_locate % W`, as in
`engine.hpp`. *Demux*: one thread reads each chunk and routes every message
through W SPSC rings to W workers (`BasicParallelEngine<ExactOrderBook,
WireUnits>`), timed from the first push of a chunk until every ring is
drained. *Presplit*: each chunk is first split per shard outside the timed
region, then W workers apply their own buffers in parallel, timed from the
start signal until the last worker finishes; it measures the books without
any demux. The main thread is on logical CPU 0; workers take CPUs 1, 10, 11,
12 and 13 (P-cores), then 2 to 9 (E-cores). One run each, aggregate M
messages/second, from `results/perf_20190130_run_multicore_demux.log` and
`results/perf_20190130_run_multicore_presplit.log`:

| W | 1 | 2 | 3 | 4 | 5 | 8 | 13 |
|---|---:|---:|---:|---:|---:|---:|---:|
| demux | 8.71 | 18.68 | 30.19 | 40.16 | **52.54** | 43.15 | 36.92 |
| presplit | 8.81 | 19.14 | 31.26 | 42.14 | 58.64 | 64.76 | **99.80** |

Up to five workers, all on P-cores, the aggregate grows faster than the
worker count: at W = 5 the demux reaches 52.54M against 8.71M with one
worker, and the presplit 58.64M against 8.81M. Each worker then holds a
fifth of the books, and each P-core has its own L2; that is a likely cause,
not a measured one (no hardware counters were read). Beyond five workers the
demux configurations add E-cores and get slower; the busiest of 13 shards
carries 1.24 times the mean load.

**Same books in every run.** Each of the runs above (throughput, latency,
both multi-core modes at every W) printed the book digest after each of the
42 chunks, and every digest equals the one the differential run printed at
the same point. Every run also ends with 0 bad-length messages, 0 dropped
orders, 0 unknown order ids, and the number of adds in the overflow that its
placement predicted (196,611 with the pre-scan placement, 98,102,140 with
the first-add one).

## Architecture decisions

| Concern | Choice | Rejected alternative |
|---|---|---|
| Order storage | 32-byte POD in a pre-allocated slab, addressed by `u32` index (2 orders per cache line) | heap `Order*` (allocation, 8-byte pointers, fragmentation) |
| Free management | intrusive LIFO free list through `Order::next` (hottest slot reused first) | `std::deque` free queue |
| Order id lookup | flat open-addressing map, Fibonacci hash, linear probe, backward-shift erase (no tombstones to decay over a trading day) | `std::unordered_map` (node-based: a pointer chase per lookup and an allocation per insert) |
| Price ladder | flat tick-indexed array per side, `levels[(price - base) / tick]`; the divide is one multiply-high by a precomputed reciprocal, skipped when the tick is 1 | `std::map` (O(log n), serialized pointer-chase misses) |
| Price band | **per symbol**: every `stock_locate` gets its own ladder base and width (64 to 131,072 ticks on 2019-01-30), placed by a pre-scan where that symbol's adds arrive, within a 1,024 MB total ladder budget. *Trade-off*: the replay reads the file twice to size the books; a live feed handler would size from the previous day instead. | one band for every symbol: the benchmark book's $0.01-$1,310.72 cannot hold AMZN or BKNG |
| Price units and tick | ITCH's **native $0.0001 units**; a book's tick is $0.01 when its symbol's median add is at least $1.00 and $0.0001 below. *Trade-off*: a tick other than 1 costs one multiply-high per lookup, and a price between two grid points (a sub-penny price in a dollar stock) cannot use the ladder. | whole cents everywhere, which merges distinct sub-penny prices into one level |
| Prices off the ladder | **exact overflow**: a `std::map` per side from price to a level holding the same pool FIFO as a ladder level; BBO and depth merge the two by price, and a replace can move an order between them. *Trade-off*: that path is O(log n) and allocates one node per new level; on 2019-01-30 it took 196,611 of 191,919,099 adds (0.102%). | dropping off-band adds and counting them (what the ladder-only `LimitOrderBook` of the benchmarks does), or rounding them onto the ladder |
| Ladder-only vs exact book | one template, `BasicOrderBook<WithOverflow>`: `LimitOrderBook` compiles to exactly the ladder-only code the synthetic benchmarks time, `ExactOrderBook` adds the grid and the overflow | a runtime flag, which puts a branch in the benchmark book's hot path |
| Best-price rediscovery | occupancy bitmap + `tzcnt`/`lzcnt` (64 prices per instruction; the next level is almost always in the same word because activity clusters at the inside) | linear level scan |
| Queue at a level | intrusive doubly-linked FIFO via pool indices | `std::list` / `std::deque` per level |
| Side dispatch | `BookSide<IsBid>` template; comparison and scan direction compile-time | runtime branch per touch |
| Parsing | `#pragma pack(1)` wire-mirror structs + `reinterpret_cast` + one `bswap` per field, after the length prefix is checked against the ITCH 5.0 table | field-by-field copy-out |
| Threading | single writer per book (the feed is inherently sequential); shard symbols across cores, SPSC rings at the edges | locks/atomics inside the book |

**Multi-core mechanics** (`include/lob/spsc.hpp`, `include/lob/engine.hpp`).
A demux thread peeks `stock_locate` (2 bytes, no decode) and routes raw
messages through wait-free SPSC rings to workers that own disjoint instrument
sets (`locate % W`). Per-instrument message order is preserved end to end,
so the single-writer book is reused unchanged: no locks and no atomics in
the book itself. In the ring:
- head/tail publications are isolated on **128-byte** boundaries (Intel's
  adjacent-line prefetcher moves cache-line pairs, so 64-byte padding still
  false-shares)
- each side keeps a private **cached copy** of the other's index; the shared
  line is touched about once per batch, not once per message
- consumer batches are capped at 256 slots so `head` publication stays fresh;
  uncapped draining of a full ring head-of-line-blocks the producer
- empty polls back off exponentially: a spinning consumer holds `tail_` in
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
every run.

## Synthetic benchmarks: the book in isolation

`lob_bench` and `lob_parallel` generate an ITCH 5.0 stream in-process and read
no market data. They measure the ladder-only `LimitOrderBook` in isolation,
on streams whose prices stay inside its band, and they are what CI runs, in
`--quick` mode, for correctness (see *Tests and CI*). These figures are from
the same 265H laptop, built from commit `eaeb960` with the `-O3 -march=native
-DNDEBUG` lines in *Build & run*; the environment before each run is in
[`results/replay_synthetic_run_env.log`](results/replay_synthetic_run_env.log).

**`lob_bench`, one book, one core** (pinned with `--cpu 1`, a P-core; three
runs, `results/synthetic_lob_bench_run_{1,2,3}.log`). End to end is the
binary ITCH stream through the feed handler into the book, 10M generated
messages; the latency rows time single `add` and `cancel` calls with
`rdtscp`, timer overhead subtracted, with 1M orders resting and cancels in
random order.

| run | 1 | 2 | 3 |
|---|---:|---:|---:|
| end to end, M messages/s | 14.4 | 13.7 | 14.3 |
| end to end, ns/message | 69.4 | 73.0 | 70.0 |
| `add` p50 / p99 / p99.9, ns | 157 / 283 / 352 | 160 / 283 / 369 | 161 / 306 / 407 |
| `cancel` p50 / p99 / p99.9, ns | 311 / 471 / 585 | 315 / 471 / 584 | 316 / 476 / 595 |

The generated stream drives a single book. The recorded day spreads the same
work over 8,695 books with 1,024 MB of ladders, 73 MB of order pools and
106 MB of id maps (sizes from the differential log), and measures 8.91M
messages/second on the same logical CPU.

**`lob_parallel`, 128 books over W workers** (one run,
[`results/synthetic_lob_parallel_run_1.log`](results/synthetic_lob_parallel_run_1.log)):
a 16M-message stream over 128 instruments. Before any table is printed, the
sharded engine (W = 8) and one thread must produce identical full-band depth
for every instrument; they did. One unpinned thread applies the stream at
5.8M messages/second. The demux thread runs on CPU 0 and worker w on logical
CPU w + 1, which on the 265H mixes the core types: CPUs 1 and 10-13 are
P-cores, 2-9 E-cores, 14-15 low-power E-cores. Aggregate M messages/second:

| W | 1 | 2 | 4 | 8 | 12 | 15 |
|---|---:|---:|---:|---:|---:|---:|
| one demux thread feeding W workers | 6.3 | 5.0 | 13.2 | 32.2 | 38.9 | 19.4 |
| stream pre-split per worker, no demux | 6.1 | 7.4 | 22.0 | 49.8 | 65.8 | 63.0 |

In CI both binaries run in `--quick` mode on every push, at reduced message
counts; the rates a shared runner prints there are not measurements.

## Tests and CI

`lob_bench` runs, in order, and exits non-zero on any failed check:
1. deterministic unit checks (FIFO priority, BBO transitions, replace semantics)
2. **differential fuzz**: 2M generated ITCH messages from a fixed seed
   (`A`/`F`/`E`/`C`/`X`/`D`/`U`, all seven types present and counted in the
   printed mix) replayed simultaneously into this book and a naive
   `std::map` + `std::unordered_map` reference. The fast book reads the wire
   bytes through the length-validated dispatch, so the parser is on trial too,
   while the reference replays the generator's decoded op list. BBO is
   compared after *every* message, and every price level in the whole
   131,072-tick band is audited against the reference at regular intervals. A
   corrupt-length frame must be refused and counted, never parsed
3. per-op latency percentiles (`rdtscp`-serialized, timer overhead subtracted)
4. end-to-end binary-stream throughput through the feed handler, with the
   out-of-band-drop / bad-length / live-order counters
5. `ExactOrderBook` unit checks: the first and last ladder ticks and one past
   them, overflow below, above and between grid points, replaces that move an
   order onto and off the ladder, a FIFO inside one overflow level, sub-penny
   and past-int32 prices, and the reciprocal division behind the ladder index
6. `ExactOrderBook` differential fuzz: 2M generated wire-unit messages over
   five symbols whose ladders the prices straddle, through `ExactOrderBook`s
   and the `ref::Market` reference used by the replay, with a full audit of
   depth and queue order 41 times per run
7. **the recorded-day replay path on a crafted file** (`src/replay_fixture.hpp`,
   also `lob_replay --fixture`): a hand-built 65-message ITCH 5.0 BinaryFILE
   with sub-penny prices, prices far outside every ladder ($0.0001,
   $199,999.99, the largest u32 price, a $1,650 symbol and a $300,000 one),
   replaces that cross the ladder boundary both ways, and executions,
   cancels and deletes in the overflow. The `lob_replay` run (chunked reader,
   pre-scan, sizing, dispatch, reference differential) replays it with the
   whole file as one chunk and with 1-, 2-, 37- and 64-byte chunks, comparing
   every book with the reference after every message. Each run must end with
   0 mismatches and 0 drops, report exactly the chunk boundaries the file
   layout implies, and leave the books written out by hand in the test; as a
   negative control, the cent-denominated `LimitOrderBook` must drop adds and
   merge the sub-penny levels on the same file

`lob_parallel` runs one multi-instrument stream through one thread and
through the sharded engine and requires every instrument's full-band depth,
the processed-message count and the drop / bad-length counters to agree
before it prints any scaling table. `spsc_stress` pushes sequence-checked
messages through the ring.

CI (`.github/workflows/ci.yml`) builds `lob_bench`, `lob_parallel` and
`spsc_stress` single-TU with `-std=c++20 -O2 -Wall -Wextra -pthread` on
ubuntu (g++-13, clang++-18) and Windows (MinGW g++), with an ASan+UBSan leg,
and runs `lob_bench --quick`, `lob_parallel --quick` and `spsc_stress`; a TSan
leg runs `spsc_stress`. `--quick` shrinks the benchmark sizes only: every
check above runs, including check 7, so every leg except TSan replays the
crafted file through the `lob_replay` code on every push. The workflow does
not build the `lob_replay` or `lob_perf` binaries themselves; `build.ps1 -Run`
runs `lob_replay --fixture` and `lob_replay --selftest` locally. The selftest
replays a generated 40-symbol day of 1,500,044 messages through 4,103-byte
chunks with the differential; 11,352 of its chunk boundaries fall inside a
message ([`results/replay_selftest.log`](results/replay_selftest.log); the
fixture's output is [`results/replay_fixture.log`](results/replay_fixture.log)).
CI checks correctness, not speed: the messages/second a shared runner prints
under `--quick` are not measurements.

Guards make a bad run loud. A length prefix that disagrees with the per-type
ITCH table is refused before any cast and counted (`bad_length`). The id map
aborts instead of probing forever when full, the book checks pool <= idmap / 2
at construction, and the SPSC ring checks that a message fits its slot. Those
three are `LOB_ASSERT`, a branch to `std::abort()` that `-DNDEBUG` does *not*
compile out, so they hold in the release builds measured here.

## Layout

```
include/lob/common.hpp      byte-order shims, rdtsc/rdtscp, branch hints
include/lob/order_pool.hpp  Order (32 B), slab OrderPool, flat OrderIdMap
include/lob/book.hpp        PriceLevel, BookSide, LimitOrderBook (ladder only)
                            and ExactOrderBook (per-book grid + exact overflow)
include/lob/itch.hpp        zero-copy ITCH 5.0 dispatch, length table, FeedHandler
include/lob/spsc.hpp        wait-free SPSC ring
include/lob/engine.hpp      sharded multi-core engine (demux, rings, pinned workers)
src/main.cpp                lob_bench: unit checks, differential fuzz, benchmarks,
                            ExactOrderBook checks and fuzz, crafted-file replay
src/parallel_main.cpp       lob_parallel: parallel-vs-sequential check, scaling
src/spsc_stress.cpp         spsc_stress: sequence-checked ring stress (TSan target)
src/replay_main.cpp         lob_replay: command line, --fixture, --selftest
src/replay_run.hpp          the replay run: pre-scan, sizing, dispatch, differential
src/replay_day.hpp          shared by the recorded-day tools: chunked reader,
                            pre-scan, per-symbol sizing, timed loop, book digest
src/replay_fixture.hpp      the crafted 65-message file and its expected books
src/perf_main.cpp           lob_perf: single core, demux, presplit, latency build
src/ref_market.hpp          naive whole-market reference model (own decoder)
src/book_diff.hpp           full book comparison (levels, queues, BBO) and the
                            per-message check of what a message touched
src/wire_gen.hpp            generated multi-symbol wire-unit ITCH stream (tests)
tools/itch_count.cpp        independent recount of a BinaryFILE (no shared code)
tools/perf_json.pl          builds results/perf_20190130.json from the logs
tools/*.sh                  data manifest, official closes (Nasdaq, Yahoo Finance)
data/MANIFEST.md            the input file: source, sizes, hashes, commands
results/                    console output of every run cited in this README
```

## Build & run

Six single-translation-unit binaries (two from one source), no build system.
Windows with MinGW-w64 g++ (`winget install BrechtSanders.WinLibs.POSIX.UCRT`;
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
g++ -std=c++20 -O3 -march=native -DNDEBUG -Wall -Wextra -pthread -static `
    -I include src/perf_main.cpp -o lob_perf.exe
g++ -std=c++20 -O3 -march=native -DNDEBUG -Wall -Wextra -pthread -static `
    -DLOB_PERF_LATENCY -I include src/perf_main.cpp -o lob_perf_latency.exe
./lob_bench.exe       # checks 1-7, latency percentiles, single-core throughput
./lob_parallel.exe    # parallel-vs-sequential verification, multi-core scaling
./spsc_stress.exe     # sequence-checked messages through the SPSC ring
./lob_replay.exe --fixture                             # crafted file, 1-byte chunks up
./lob_replay.exe --selftest                            # generated day, 4 KB chunks
./lob_replay.exe 01302019.NASDAQ_ITCH50 --differential # the recorded day
./lob_perf.exe 01302019.NASDAQ_ITCH50                  # single-core throughput on the day
./lob_perf_latency.exe 01302019.NASDAQ_ITCH50          # every message timed
```

Linux with g++ >= 11 or clang++ >= 14 (`std::latch` needs libstdc++ 11+):

```bash
g++ -std=c++20 -O3 -march=native -DNDEBUG -Wall -Wextra -pthread -I include src/main.cpp          -o lob_bench
g++ -std=c++20 -O3 -march=native -DNDEBUG -Wall -Wextra -pthread -I include src/parallel_main.cpp -o lob_parallel
g++ -std=c++20 -O3 -march=native -DNDEBUG -Wall -Wextra -pthread -I include src/spsc_stress.cpp   -o spsc_stress
g++ -std=c++20 -O3 -march=native -DNDEBUG -Wall -Wextra -pthread -I include src/replay_main.cpp   -o lob_replay
g++ -std=c++20 -O3 -march=native -DNDEBUG -Wall -Wextra -pthread -I include src/perf_main.cpp     -o lob_perf
g++ -std=c++20 -O3 -march=native -DNDEBUG -Wall -Wextra -pthread -DLOB_PERF_LATENCY -I include src/perf_main.cpp -o lob_perf_latency
./lob_bench && ./lob_parallel && ./spsc_stress && ./lob_replay --fixture && ./lob_replay --selftest
```

`lob_bench` takes `--quick` (the mode CI runs: every check still runs, only
the benchmark sizes shrink, and the numbers it prints are not measurements)
and `--cpu N` (pin to logical CPU N on Windows; default 2, which on the 265H
is an E-core). `lob_parallel` takes `--quick`.

`lob_replay FILE` takes the decompressed BinaryFILE. Options:
`--differential` (run the reference alongside: compare what every order
message touched, and every book in full at every checkpoint),
`--checkpoint N` (default 5,000,000 messages), `--at HH:MM:SS,...` (extra
checkpoints that also print the named books; default 12:00:00,16:00:00 ET),
`--symbols A,B,...`, `--depth N`, `--chunk-mb N` (default 256),
`--ladder-mb N` (default 1024), `--cpu N`. It exits non-zero unless every
check passes. `--fixture` and `--selftest` are the two built-in tests above.

`lob_perf FILE` takes the same file. `--mode single` (default) times the
single-core loop; `--mode demux` and `--mode presplit` run the sharded
configurations for each W in `--workers 1,2,4` (default); `--cpu N` pins the
single-core thread (default: the first fast core after CPU 0), `--cpus a,b,...`
gives the multi-core CPU order (main thread first; default: fast cores
first, from the OS's CPU sets); `--chunk-mb N` and `--ladder-mb N` as for
`lob_replay`; `--placement first-add` places every ladder at its symbol's
first add instead of where the pre-scan found the day's adds (default
`prescan`). `lob_perf_latency FILE` takes `--cpu`, `--chunk-mb`,
`--ladder-mb` and `--placement`. Both print the machine, power state, TSC
calibration and a book digest per chunk, and exit non-zero unless every
check passes.

## Reproducibility

[`data/MANIFEST.md`](data/MANIFEST.md) identifies the input (source URL,
sizes, SHA-256, message counts by type) and gives the exact command behind
every file in `results/`, and which commit each binary was built from. Every
number in this README is in one of those files. The data itself is NASDAQ's
and is not redistributed here.

## Notes & limits

- One instrument per book (the standard sharding unit). The multi-symbol
  front is a `stock_locate`-indexed book table, as in `lob_replay` and the
  parallel engine.
- `LimitOrderBook`, the benchmark book, keeps integer cents inside a
  configurable band (default $0.01-$1,310.72, 3 MB of ladder per side) and
  drops and counts out-of-band adds. `ExactOrderBook`, the replay's book,
  holds any u32 wire price exactly, routing what its ladder does not cover to
  the overflow.
- `lob_replay` sizes its books from a pre-scan of the same file, which suits
  a historical replay. A live feed handler would size from the previous day
  or a reserve, since it cannot read the day ahead.
- The book does not match crossing orders; it is a *reconstructor*, and
  crossings are resolved by the venue and arrive as Execute messages, per ITCH
  semantics.
- The latency maxima (milliseconds in the recorded-day runs) are not book
  work (see *Recorded-day performance*); the p99.9 sits three orders of
  magnitude below them. On a tuned host you would pin to an isolated core
  (`isolcpus`), keep SMT off on that core, and use huge pages for the slab
  and the id map.
