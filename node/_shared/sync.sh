#!/usr/bin/env bash
set -eu
root="$(cd "$(dirname "$0")/../.." && pwd)"
sketches="accelerometer sht41 no_sensor no_sensor_lcd accelerometer_lcd sht41_lcd"
lcd_sketches="no_sensor_lcd accelerometer_lcd sht41_lcd"
files="node/_shared/node_common.h node/_shared/packet_queue.h protocol/protocol.h protocol/protocol.c protocol/reliability.h protocol/reliability.c"
lcd_files="node/_shared/node_lcd.h"
threshold_sketches="sht41_lcd"
threshold_files="node/_shared/threshold.h"

bad=0
sync_file() {
  local sk="$1" f="$2"
  local src="$root/$f" dst="$root/node/$sk/$(basename "$f")"
  if [ "${MODE:-}" = "--check" ]; then
    cmp -s "$src" "$dst" || { echo "Out of sync: node/$sk/$(basename "$f")"; bad=1; }
  else
    cp "$src" "$dst"
  fi
}

MODE="${1:-}"
for sk in $sketches; do
  for f in $files; do sync_file "$sk" "$f"; done
done
for sk in $lcd_sketches; do
  for f in $lcd_files; do sync_file "$sk" "$f"; done
done
for sk in $threshold_sketches; do
  for f in $threshold_files; do sync_file "$sk" "$f"; done
done
[ "$MODE" = "--check" ] && [ "$bad" = 0 ] && echo "All sketch copies are in sync."
exit $bad
