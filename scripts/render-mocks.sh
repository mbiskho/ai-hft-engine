#!/usr/bin/env bash
# Render the report mock-ups to PNG with headless Chrome. Static HTML only.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
CHROME="${CHROME:-/Applications/Google Chrome.app/Contents/MacOS/Google Chrome}"
[[ -x "$CHROME" ]] || CHROME="$(command -v google-chrome || command -v chromium)"

render() { # src out width,height
  "$CHROME" --headless=new --disable-gpu --hide-scrollbars --force-device-scale-factor=2 \
    --window-size="$3" --virtual-time-budget=2000 --screenshot="$ROOT/reports/$2" "file://$ROOT/$1" >/dev/null 2>&1
  echo "reports/$2"
}

render reports/src/system-flow.html hft-system-flow.png 1600,1000
render index.html                   hft-dashboard.png   1288,1500
