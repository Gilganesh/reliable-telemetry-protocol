#!/usr/bin/env bash
set -eu
root="$(cd "$(dirname "$0")/.." && pwd)"
sketches="node_accelerometer node_sht41 node_default_lcd node_accelerometer_lcd node_sht41_lcd"
lcd_sketches="node_default_lcd node_accelerometer_lcd node_sht41_lcd"
files="node_common/node_common.h node_common/packet_queue.h protocol/protocol.h protocol/protocol.c protocol/reliability.h protocol/reliability.c"
lcd_files="node_common/node_lcd.h"

bad=0
sync_file() {
  local sk="$1" f="$2"
  local src="$root/$f" dst="$root/$sk/$(basename "$f")"
  if [ "${MODE:-}" = "--check" ]; then
    cmp -s "$src" "$dst" || { echo "Out of sync: $sk/$(basename "$f")"; bad=1; }
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
[ "$MODE" = "--check" ] && [ "$bad" = 0 ] && echo "All sketch copies are in sync."
exit $bad
