#!/bin/bash
# Official 2019-01-30 closes for the closing-cross check
# (results/closing_cross_20190130.md), from Nasdaq's own historical quotes,
# the source independent of Yahoo Finance (tools/official_close_yahoo.sh).
#
# All eleven symbols are Nasdaq-listed, so their official close is the Nasdaq
# Official Closing Price. Nasdaq's historical daily rows are adjusted for
# later splits, like Yahoo's; each close is multiplied back by the product of
# the split ratios listed below and rounded to the cent. MSFT, INTC, CSCO and
# FB (META since 2022) have not split since 2019-01-30, so their closes are
# compared as served.
#
# Usage: tools/official_close_nasdaq.sh > results/official_close_nasdaq_20190130.log
# Needs bash, curl and perl.
set -eu
UA='Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/124.0 Safari/537.36'
API=https://api.nasdaq.com/api/quote
TODAY=$(date '+%Y-%m-%d')
# The API returns no rows when both dates are in 2019, so the query runs from
# 2019-01-29 to today and the 01/30/2019 row is picked out of the result.
Q="assetclass=stocks&fromdate=2019-01-29&todate=$TODAY&limit=9999"

# Splits after 2019-01-30, as ratio:effective date.
declare -A SPLITS=(
  [AAPL]="4:2020-08-31"
  [AMZN]="20:2022-06-06"
  [GOOGL]="20:2022-07-18"
  [NVDA]="4:2021-07-20 10:2024-06-10"
  [TSLA]="5:2020-08-31 3:2022-08-25"
  [NFLX]="10:2025-11-17"
  [BKNG]="25:2026-04-06"
)

echo "=== official 2019-01-30 closes from Nasdaq's historical quotes, fetched $(date '+%Y-%m-%d %H:%M %Z') ==="
echo "Rows are adjusted for later splits (not dividends); each close is multiplied back by the split ratios listed"
echo "and rounded to the cent. FB is listed as META since 2022."
echo
for sym in AAPL MSFT AMZN GOOGL META INTC CSCO NVDA TSLA NFLX BKNG; do
  body=$(curl -s --max-time 60 -A "$UA" -H 'Accept: application/json, text/plain, */*' \
         -H 'Origin: https://www.nasdaq.com' -H 'Referer: https://www.nasdaq.com/' "$API/$sym/historical?$Q")
  echo "$sym  GET $API/$sym/historical?$Q"
  BODY="$body" SPL="${SPLITS[$sym]:-}" perl -e '
    my $b = $ENV{BODY};
    my ($row) = $b =~ /(\{"date":"01\/30\/2019"[^}]*\})/ or die "no 01/30/2019 row\n";
    my ($close) = $row =~ /"close":"\$([0-9.]+)"/ or die "no close\n";
    print "      row $row\n";
    my ($f, @s) = (1);
    for my $x (split " ", $ENV{SPL}) { my ($r, $d) = split /:/, $x; $f *= $r; push @s, "$r:1 on $d"; }
    printf "      splits after 2019-01-30: %s\n", @s ? join(", ", @s) : "none";
    printf "      factor %g | unadjusted close %.2f\n", $f, $close * $f;
  '
done
