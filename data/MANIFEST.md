# Data manifest: NASDAQ TotalView-ITCH 5.0, 2019-01-30

The recorded-day results in `results/` come from one file: NASDAQ's public
TotalView-ITCH 5.0 sample capture for Wednesday 2019-01-30. The data is
NASDAQ's and is not in this repository, in whole or in part. This manifest
identifies the file exactly and lists the commands that produce every file in
`results/` from a fresh download.

## The file

| | |
|---|---|
| Source | NASDAQ's public ITCH sample directory, https://emi.nasdaq.com/ITCH/Nasdaq%20ITCH/ |
| File | `01302019.NASDAQ_ITCH50.gz` |
| Compressed size | 4,764,426,091 bytes |
| Compressed SHA-256 | `8c97b5b13bc451c012c2466fb7e258da134dab29aa47b67fe7b0088c78e870be` |
| Decompressed file | `01302019.NASDAQ_ITCH50` |
| Decompressed size | 11,245,883,092 bytes |
| Decompressed SHA-256 | `1d0972ffc25b35902ccc3f9069aae517da56903d5795f872902b8697315f30c3` |
| Format | ITCH 5.0 BinaryFILE: each message is a 2-byte big-endian length, then the message |
| Messages | 368,366,634 (the file ends exactly on a message boundary) |
| Message timestamps | 03:03:59.687 to 20:05:00.000 ET |

Sizes and hashes: [`results/manifest_20190130.log`](../results/manifest_20190130.log),
the output of `tools/data_manifest.sh`. The SHA-256 of the decompressed file
equals that of the gzip stream decompressed straight into `sha256sum`, so the
file on disk is a complete decompression of the download.

Messages by type, as counted by `lob_replay`'s pre-scan
([`results/replay_20190130_differential.log`](../results/replay_20190130_differential.log))
and, separately, by `tools/itch_count.cpp`, which shares no code with the book
([`results/itch_count_20190130.log`](../results/itch_count_20190130.log)).
The two counts are identical.

| Type | Message | Count |
|---|---|---:|
| `A` | Add Order | 162,970,455 |
| `D` | Order Delete | 158,273,361 |
| `U` | Order Replace | 27,222,746 |
| `E` | Order Executed | 8,096,995 |
| `X` | Order Cancel | 4,669,874 |
| `I` | Net Order Imbalance Indicator | 3,684,511 |
| `F` | Add Order with MPID | 1,725,898 |
| `P` | Trade (non-cross) | 1,326,184 |
| `L` | Market Participant Position | 193,769 |
| `C` | Order Executed With Price | 158,886 |
| `Q` | Cross Trade | 17,430 |
| `Y` | Reg SHO Restriction | 8,821 |
| `H` | Stock Trading Action | 8,805 |
| `R` | Stock Directory | 8,714 |
| `B` | Broken Trade | 116 |
| `J` | LULD Auction Collar | 62 |
| `S` | System Event | 6 |
| `V` | MWCB Decline Level | 1 |
| | **total** | **368,366,634** |

## Getting it

From the repository root (`data/` is ignored by git except for this file):

```bash
mkdir -p data && cd data
curl -O "https://emi.nasdaq.com/ITCH/Nasdaq%20ITCH/01302019.NASDAQ_ITCH50.gz"
gzip -dc 01302019.NASDAQ_ITCH50.gz > 01302019.NASDAQ_ITCH50
sh ../tools/data_manifest.sh          # sizes and hashes; compare with the table above
cd ..
```

The decompressed day takes 11,245,883,092 bytes of disk. The tools stream it
in 256 MB chunks and never load it whole.

## Building the tools

The committed runs used Windows 11, Git Bash and MinGW-w64 g++ 16.1.0
(WinLibs UCRT), with the README's build line. On Linux, drop `-static` and
`.exe`. `LOB_GIT_COMMIT` is the commit a tool prints in its log header.

```bash
FLAGS="-std=c++20 -O3 -march=native -DNDEBUG -Wall -Wextra -pthread -static -I include"
COMMIT=$(git rev-parse HEAD)
g++ $FLAGS "-DLOB_GIT_COMMIT=\"$COMMIT\"" src/replay_main.cpp -o lob_replay.exe
g++ $FLAGS src/main.cpp -o lob_bench.exe
g++ $FLAGS src/parallel_main.cpp -o lob_parallel.exe
g++ $FLAGS "-DLOB_GIT_COMMIT=\"$COMMIT\"" "-DLOB_BUILD_FLAGS=\"$FLAGS\"" src/perf_main.cpp -o lob_perf.exe
g++ $FLAGS -DLOB_PERF_LATENCY "-DLOB_GIT_COMMIT=\"$COMMIT\"" "-DLOB_BUILD_FLAGS=\"$FLAGS -DLOB_PERF_LATENCY\"" src/perf_main.cpp -o lob_perf_latency.exe
g++ -std=c++20 -O3 -march=native -DNDEBUG -Wall -Wextra -pthread -static tools/itch_count.cpp -o itch_count.exe
```

Which commit each committed log was built from:

| Logs | Built from |
|---|---|
| `replay_20190130_differential.log`, `closing_cross_replay_20190130.log`, `replay_fixture.log`, `replay_selftest.log` | `d191efd` (printed in each log; build lines in `replay_run_env.log`) |
| `synthetic_*.log` | `fc9b010` (build lines in `replay_synthetic_run_env.log`) |
| `perf_20190130_run_*.log` | `380902a` (printed in every log; build lines in `perf_20190130_run_env.log`) |
| `perf_20190130_repro_*.log` | `1ed03a6` (printed in each log; build lines in `perf_20190130_repro_env.log`) |
| `perf_20190130_placement_*_throughput.log` | `09da5d0` (printed in each log; build lines in `perf_20190130_placement_env.log`) |
| `perf_20190130_placement_*_latency*.log` | `62f8be9` (printed in each log; build lines in `perf_20190130_placement_latency_env.log`) |
| `itch_count_20190130.log` | `2f2082f` (build line at the top of the log) |

At the commit that last updated this manifest: `lob_replay` compiles exactly
the sources of `d191efd`. `lob_bench` differs from `fc9b010` only in the
reference comparison code of its checks 6 and 7 (`src/book_diff.hpp`,
`src/replay_run.hpp`), not in the code its benchmarks time. `lob_parallel`
compiles exactly the sources of `fc9b010`. `lob_perf` differs from `380902a`
in `clock_of()` in `src/replay_day.hpp`, which formats timestamps in log
lines, and in the `--placement` option, whose default is the placement of
every earlier run; its timed loops are unchanged. `lob_perf_latency` also
measures the empty timer pair throughout the pass and subtracts it when the
percentiles are read (`62f8be9`). Both compile exactly the sources of
`62f8be9`. `tools/itch_count.cpp` is unchanged since `2f2082f`. To build
exactly the source a log names, `git checkout` that commit first.

## Reproducing every file in `results/`

Every run's console output was captured with the same wrapper, which appends
the exit status:

```bash
run() { out=$1; shift; "$@" > "$out" 2>&1; echo "exit $?" >> "$out"; }
R=..   # the repository root, seen from data/
```

Run from `data/`, with the decompressed file there:

| File | Command |
|---|---|
| `manifest_20190130.log` | `sh $R/tools/data_manifest.sh > $R/results/manifest_20190130.log` |
| `replay_20190130_differential.log` | `run $R/results/replay_20190130_differential.log $R/lob_replay.exe 01302019.NASDAQ_ITCH50 --differential` |
| `closing_cross_replay_20190130.log` | `run $R/results/closing_cross_replay_20190130.log $R/lob_replay.exe 01302019.NASDAQ_ITCH50 --symbols AAPL,MSFT,AMZN,GOOGL,FB,INTC,CSCO,NVDA,TSLA,NFLX --at 12:00:00,16:00:00 --cpu 0` |
| `itch_count_20190130.log` | `$R/itch_count.exe 01302019.NASDAQ_ITCH50`; the log carries the build and run commands above the output |
| `perf_20190130_run_throughput_1.log` .. `_5.log` | `run $R/results/perf_20190130_run_throughput_1.log $R/lob_perf.exe 01302019.NASDAQ_ITCH50`, five times |
| `perf_20190130_run_throughput_ecore.log` | `run ... $R/lob_perf.exe 01302019.NASDAQ_ITCH50 --cpu 2` |
| `perf_20190130_run_latency_1.log` .. `_3.log` | `run ... $R/lob_perf_latency.exe 01302019.NASDAQ_ITCH50`, three times |
| `perf_20190130_run_multicore_demux.log` | `run ... $R/lob_perf.exe 01302019.NASDAQ_ITCH50 --mode demux --workers 1,2,3,4,5,8,13` |
| `perf_20190130_run_multicore_presplit.log` | `run ... $R/lob_perf.exe 01302019.NASDAQ_ITCH50 --mode presplit --workers 1,2,3,4,5,8,13` |
| `perf_20190130_repro_latency.log` | `run ... $R/lob_perf_latency.exe 01302019.NASDAQ_ITCH50`, built from `1ed03a6` |
| `perf_20190130_repro_throughput.log` | `run ... $R/lob_perf.exe 01302019.NASDAQ_ITCH50`, built from `1ed03a6` |
| `perf_20190130_placement_prescan_throughput.log`, `perf_20190130_placement_firstadd_throughput.log` | `run ... $R/lob_perf.exe 01302019.NASDAQ_ITCH50`, then the same with `--placement first-add`, built from `09da5d0` |
| `perf_20190130_placement_prescan_latency_1.log`, `perf_20190130_placement_firstadd_latency.log`, `perf_20190130_placement_prescan_latency_2.log` | `run ... $R/lob_perf_latency.exe 01302019.NASDAQ_ITCH50`, the same with `--placement first-add`, then the first again, built from `62f8be9` |

No market data needed, from any directory:

| File | Command |
|---|---|
| `synthetic_lob_bench_run_1.log` .. `_3.log` | `run synthetic_lob_bench_run_1.log ./lob_bench.exe --cpu 1`, three times |
| `synthetic_lob_parallel_run_1.log` | `run synthetic_lob_parallel_run_1.log ./lob_parallel.exe` |
| `replay_fixture.log` | `run replay_fixture.log ./lob_replay.exe --fixture` |
| `replay_selftest.log` | `run replay_selftest.log ./lob_replay.exe --selftest` |
| `official_close_yahoo_20190130.log` | `bash tools/official_close_yahoo.sh > results/official_close_yahoo_20190130.log` (network; rerun on 2026-09-21, its output differed from the committed log only in the fetch time on the first line) |
| `official_close_nasdaq_20190130.log` | `bash tools/official_close_nasdaq.sh > results/official_close_nasdaq_20190130.log` (network) |
| `perf_20190130.json` | `perl tools/perf_json.pl results > results/perf_20190130.json` (built from the committed logs; refuses if any run failed or printed a book digest different from the differential run's) |

Recorded by the scripts that ran the batches:

- `perf_20190130_run_env.log`, `perf_20190130_repro_env.log`,
  `perf_20190130_placement_env.log`, `perf_20190130_placement_latency_env.log`,
  `replay_run_env.log` and `replay_synthetic_run_env.log`: the build lines,
  the run order, and before each run the power state, the power plan and the
  processes that used the most CPU in the preceding 5 s. Each batch ran back
  to back in the order recorded there.

Written by hand, from the logs it cites:

- `closing_cross_20190130.md`: the closing cross prices in the file against
  the official closes, from `itch_count_20190130.log`,
  `closing_cross_replay_20190130.log`, `official_close_nasdaq_20190130.log`
  and `official_close_yahoo_20190130.log`.

The run order matters for the performance logs only in that they share a
machine: timings vary with the host, its power plan and whatever else is
running, and a repeat on another machine will measure different numbers. The
reconstructed books do not vary: every run, in any mode, prints the same book
digest after each 256 MB chunk as the differential run.
