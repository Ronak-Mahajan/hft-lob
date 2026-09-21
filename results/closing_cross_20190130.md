# Closing cross vs official close, 2019-01-30

Data: NASDAQ's public TotalView-ITCH 5.0 sample file `01302019.NASDAQ_ITCH50`
(sizes and hashes in `data/MANIFEST.md`). Machine: Intel Core Ultra 7
265H, Windows 11, g++ 16.1.0. Checked on 2026-09-21.

Raw output behind every number on this page:

- `itch_count_20190130.log`: `tools/itch_count.cpp`, a decoder that shares
  no code with `include/lob` or `src`. Source of the cross prices, the NOII
  lines, the order messages after 16:00 and the book when each cross was
  published.
- `closing_cross_replay_20190130.log`: `lob_replay` (built from commit
  eaeb960) on the same file, with the ten symbols below named. Source of the
  replayed books at 16:00:00.000 ET.
- `official_close_nasdaq_20190130.log`: the official closes from Nasdaq's
  own historical quotes, fetched by `tools/official_close_nasdaq.sh`, with
  the request URLs, the rows as served and the split arithmetic.
- `official_close_yahoo_20190130.log`: the same closes from Yahoo Finance,
  fetched by `tools/official_close_yahoo.sh`, with the request URLs and the
  split arithmetic.

## 1. Closing cross price vs official close

The Cross Trade message ('Q') with cross type 'C' is NASDAQ's closing
cross. For a NASDAQ-listed stock the closing cross price is the NASDAQ
Official Closing Price, so the two should be equal to the cent. All ten
symbols below are NASDAQ-listed.

Official close sources, fetched separately:

- Nasdaq's historical quotes (`api.nasdaq.com/api/quote/SYMBOL/historical`,
  the data behind nasdaq.com's historical-quotes pages), the 2019-01-30 row.
- Yahoo Finance daily bars (`query1.finance.yahoo.com/v8/finance/chart/SYMBOL`,
  interval 1d, 2019-01-30).

Both serve closes adjusted for later splits (not dividends), so each is
multiplied back by the product of the split ratios after 2019-01-30 and
rounded to the cent. Nasdaq's adjusted figures carry at most four decimals,
so for NVDA and TSLA the product is rounded (3.4347 x 40 = 137.388,
20.5847 x 15 = 308.7705). FB has traded as META since 2022 and is fetched
under that symbol.

| Symbol | Closing cross in the file | Shares | Published (ET) | Nasdaq close (split-adjusted) | Yahoo close (split-adjusted) | Splits since | Official close, both sources | Difference |
|---|---:|---:|---|---:|---:|---|---:|---:|
| AAPL  | 165.25  | 2,340,937 | 16:00:00.563 | 41.3125 | 41.3125 | 4:1 | 165.25 | 0.00 |
| MSFT  | 106.38  | 2,606,642 | 16:00:00.531 | 106.38 | 106.38 | none | 106.38 | 0.00 |
| AMZN  | 1670.43 | 183,741   | 16:00:00.764 | 83.5215 | 83.5215 | 20:1 | 1670.43 | 0.00 |
| GOOGL | 1097.99 | 116,867   | 16:00:00.273 | 54.8995 | 54.89950 | 20:1 | 1097.99 | 0.00 |
| FB    | 150.42  | 1,017,765 | 16:00:00.357 | 150.42 (META) | 150.42 (META) | none | 150.42 | 0.00 |
| INTC  | 47.54   | 2,204,287 | 16:00:00.003 | 47.54 | 47.54 | none | 47.54 | 0.00 |
| CSCO  | 46.71   | 1,937,262 | 16:00:00.414 | 46.71 | 46.71 | none | 46.71 | 0.00 |
| NVDA  | 137.39  | 335,421   | 16:00:00.591 | 3.4347 | 3.43475 | 4:1, 10:1 | 137.39 | 0.00 |
| TSLA  | 308.77  | 74,672    | 16:00:00.366 | 20.5847 | 20.584667 | 5:1, 3:1 | 308.77 | 0.00 |
| NFLX  | 340.66  | 298,927   | 16:00:00.617 | 34.066 | 34.066 | 10:1 | 340.66 | 0.00 |

All ten agree to the cent, in both sources. BKNG, which the replay log
also names, closes at 1818.70 in both (72.748 and 72.74800 before its
25:1 split), equal to its closing cross in the file. As a third,
unadjusted source for one of them, The Washington Post's report of
2019-01-30 ("Facebook reports robust profit, revenue gains; stock rises")
gives FB's close that day as $150.42.

## 2. Reconstructed book vs the closing cross

Books at 16:00:00.000 ET are the state after message 365,323,584, the last
message timestamped before 16:00. `lob_replay` and the independent decoder
print identical books there for all ten symbols (resting orders, level
counts, and the top five levels with shares and order counts).

The closing cross also executes orders that are never in the displayed
book (market-on-close, limit-on-close and imbalance-only orders), so the
continuous inside market just before 16:00 is not required to contain the
cross price. What the cross does require is that, once it has run, no
displayed bid rests above the cross price and no displayed ask rests below
it. The last column checks that, on the independent decoder's book at the
moment the 'Q' message is published.

| Symbol | Cross | Inside at 16:00:00.000 (bid / ask) | Brackets the cross | Inside when the cross is published | Brackets the cross |
|---|---:|---|---|---|---|
| AAPL  | 165.25  | 165.24 / 165.27   | yes | 165.24 / 165.27   | yes |
| MSFT  | 106.38  | 106.30 / 106.38   | yes (at the ask) | 106.30 / 106.38 | yes (at the ask) |
| AMZN  | 1670.43 | 1670.99 / 1671.10 | no, bid above the cross | 1670.42 / 1671.10 | yes |
| GOOGL | 1097.99 | 1097.59 / 1098.00 | yes | 1097.59 / 1098.00 | yes |
| FB    | 150.42  | 150.32 / 150.42   | yes (at the ask) | 150.32 / 150.42 | yes (at the ask) |
| INTC  | 47.54   | 47.53 / 47.55     | yes | 47.53 / 47.55     | yes |
| CSCO  | 46.71   | 46.70 / 46.71     | yes (at the ask) | 46.70 / 46.71 | yes (at the ask) |
| NVDA  | 137.39  | 137.41 / 137.46   | no, bid above the cross | 137.39 / 137.46 | yes (at the bid) |
| TSLA  | 308.77  | 308.76 / 308.77   | yes (at the ask) | 308.76 / 308.77 | yes (at the ask) |
| NFLX  | 340.66  | 340.53 / 340.66   | yes (at the ask) | 340.52 / 340.66 | yes (at the ask) |

The two that do not bracket at 16:00:00.000:

- AMZN. At 16:00:00.000 the best bids are 1670.99 x 291 (one order) and
  1670.43 x 5. The last closing NOII before the cross (16:00:00.265) shows
  183,440 shares paired, a sell imbalance of 3,115 shares, and near and far
  prices of 1670.43: sell interest in the closing book set the price below
  the continuous bid. Between 16:00:00.000 and the cross, the file has three
  non-printable Order Executed With Price messages for AMZN, all at 1670.43,
  for 301 shares. When the cross is published both bids at or above 1670.43
  are gone and the best bid is 1670.42.
- NVDA. At 16:00:00.000 the best bid is 137.41 x 100. Between 16:00:00.000
  and the cross the file has no NVDA executions, only adds, deletes and one
  replace, and when the cross is published the best bid is 137.39 x 500:
  the 137.41 order was withdrawn before the cross ran. The cross price
  equals that bid.

For every one of the ten, the book at the moment the closing cross is
published satisfies bid <= cross price <= ask.

## Scope

- Checked: the ten symbols above, their closing crosses from the file,
  their official closes from two public sources, Nasdaq and Yahoo Finance
  (plus one press report for FB), and the inside market at 16:00:00.000 and
  at each cross.
- Not checked: opening crosses against an external source, symbols outside
  this list, and full depth at the cross beyond the inside market.
- The official closes are Nasdaq's and Yahoo Finance's figures as served on
  2026-09-21, multiplied back through the splits listed in each log.
- The cross prices are data in the file, so this page checks the input and
  the books around the cross; the replay's correctness is checked against
  the reference model (`replay_20190130_differential.log`).
