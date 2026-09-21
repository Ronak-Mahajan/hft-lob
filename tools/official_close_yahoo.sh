#!/bin/bash
# Official 2019-01-30 closes for the closing-cross check
# (results/closing_cross_20190130.md), from Yahoo Finance's chart API.
#
# Yahoo's daily close is adjusted for later splits, so each close is
# multiplied back by the product of the split ratios Yahoo lists after
# 2019-01-30 and rounded to the cent. FB is fetched as META.
#
# Usage: tools/official_close_yahoo.sh > results/official_close_yahoo_20190130.log
# Needs bash, curl and perl. Output differs from the committed log only in
# the fetch time on its first line, as long as Yahoo serves the same data.
set -eu
UA='Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/120.0 Safari/537.36'
API=https://query1.finance.yahoo.com/v8/finance/chart
DAY='period1=1548806400&period2=1548892800&interval=1d'              # 2019-01-30 00:00 .. 2019-01-31 00:00 UTC
SPLITS='period1=1548806400&period2=1790000000&interval=3mo&events=split'

echo "=== official 2019-01-30 closes from Yahoo Finance (chart API), fetched $(date '+%Y-%m-%d %H:%M %Z') ==="
echo "Yahoo's daily 'close' is adjusted for splits (not dividends). Each close is multiplied back by the product of"
echo "the split ratios Yahoo lists after 2019-01-30 and rounded to the cent. FB is listed as META since 2022."
echo "Split events from $API/SYMBOL?$SPLITS"
echo
for sym in AAPL MSFT AMZN GOOGL META INTC CSCO NVDA TSLA NFLX BKNG; do
  day=$(curl -s -A "$UA" "$API/$sym?$DAY")
  splits=$(curl -s -A "$UA" "$API/$sym?$SPLITS")
  echo "$sym  GET $API/$sym?$DAY"
  DAYJSON="$day" SPLITJSON="$splits" perl -e '
    my ($d, $s) = ($ENV{DAYJSON}, $ENV{SPLITJSON});
    my ($ts) = $d =~ /"timestamp":\[(\d+)\]/ or die "no 2019-01-30 bar\n";
    my ($close) = $d =~ /"close":\[([0-9.]+)\]/ or die "no close\n";
    my @t = gmtime($ts);
    printf "      bar timestamp %d (%04d-%02d-%02d %02d:%02d UTC) | close %s\n",
           $ts, $t[5] + 1900, $t[4] + 1, $t[3], $t[2], $t[1], $close;
    my (@ratio, @raw);
    my $factor = 1;
    while ($s =~ /\{("date":(\d+),"numerator":([0-9.]+),"denominator":([0-9.]+),"splitRatio":"([^"]+)")\}/g) {
      next if $2 <= $ts;                                  # only splits after the bar
      push @raw, $1;
      push @ratio, $5;
      $factor *= $3 / $4;
    }
    printf "      splits after 2019-01-30 (events=split): %s\n",
           @ratio ? join(" ", @ratio) . "  " . join(" ", @raw) . " " : "none ";
    printf "      factor %g | unadjusted close %.2f\n", $factor, $close * $factor;
  '
done
