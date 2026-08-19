#!/usr/bin/env bash
# Fetch one day of IEX DEEP+ (order-by-order) historical market data.
#
#   ./scripts/fetch_iex.sh [YYYYMMDD] [outdir]
#
# Defaults to 2024-12-23, the smallest full trading day in the DEEP+ archive
# (1.69 GB compressed, roughly 49 million order messages).
#
# Data provided for free by IEX. By accessing or using IEX Historical Data,
# you agree to the IEX Historical Data Terms of Use.
# https://iextrading.com/iex-historical-data-terms/
#
# The data file itself is NOT committed to this repository. IEX's terms do
# permit redistribution with attribution, but a 1.7 GB binary has no business
# in a git history. Run this instead; it takes about 90 seconds.
set -euo pipefail

DAY="${1:-20241223}"
OUT="${2:-data/iex}"
IDX="https://iextrading.com/api/1.0/hist"

mkdir -p "$OUT"
DEST="$OUT/${DAY}_DPLS.pcap.gz"

if [[ -f "$DEST" ]]; then
  echo "already have $DEST"
  exit 0
fi

echo "resolving DEEP+ link for $DAY ..."
# The download URL carries a required GCS 'generation' query parameter, so it
# must be resolved from the index at run time. A hand-copied URL returns 401
# once the generation changes.
LINK=$(curl -sS --fail --max-time 120 "$IDX" | python3 -c "
import json,sys
day=sys.argv[1]
d=json.load(sys.stdin)
if day not in d:
    sys.exit(f'no such day: {day}')
for f in d[day]:
    if f.get('feed') in ('DEEP+','DPLS'):
        print(f['link']); break
else:
    sys.exit(f'{day} has no DEEP+ feed (it starts 2024-10-01)')
" "$DAY")

echo "downloading -> $DEST"
curl -sS --fail --max-time 1800 -o "$DEST" "$LINK"
echo "done: $(du -h "$DEST" | cut -f1)"
echo
echo "next:"
echo "  make && ./build/pricetime_iex $DEST          # list symbols"
echo "  ./build/pricetime_iex $DEST SPY              # replay one"
