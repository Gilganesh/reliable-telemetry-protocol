#!/usr/bin/env bash
set -eu
root="$(cd "$(dirname "$0")/.." && pwd)"
sketches="node_accelerometer node_sht41"
files="node_common/node_common.h node_common/packet_queue.h protocol/protocol.h protocol/protocol.c protocol/reliability.h protocol/reliability.c"

bad=0
for sk in $sketches; do
  for f in $files; do
    src="$root/$f"; dst="$root/$sk/$(basename "$f")"
    if [ "${1:-}" = "--check" ]; then
      cmp -s "$src" "$dst" || { echo "Out of sync: $sk/$(basename "$f")"; bad=1; }
    else
      cp "$src" "$dst"
    fi
  done
done
[ "${1:-}" = "--check" ] && [ "$bad" = 0 ] && echo "All sketch copies are in sync."
exit $bad
