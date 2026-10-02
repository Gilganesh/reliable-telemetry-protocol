#!/usr/bin/env bash
# Розкладає спільні файли по скетчах вузлів: Arduino IDE компілює лише те, що лежить
# у папці скетча, тому кожен скетч мусить мати власні копії.
#   node_common/{node_common.h,packet_queue.h} і protocol/{protocol,reliability}.{h,c}
#   -> node_accelerometer/ та node_sht41/
# Використання:  node_common/sync.sh          -- скопіювати
#                node_common/sync.sh --check  -- лише перевірити, що копії ідентичні (код виходу 1, якщо ні)
set -eu
root="$(cd "$(dirname "$0")/.." && pwd)"
sketches="node_accelerometer node_sht41"
files="node_common/node_common.h node_common/packet_queue.h protocol/protocol.h protocol/protocol.c protocol/reliability.h protocol/reliability.c"

bad=0
for sk in $sketches; do
  for f in $files; do
    src="$root/$f"; dst="$root/$sk/$(basename "$f")"
    if [ "${1:-}" = "--check" ]; then
      cmp -s "$src" "$dst" || { echo "РОЗБІЖНІСТЬ: $sk/$(basename "$f")"; bad=1; }
    else
      cp "$src" "$dst"
    fi
  done
done
[ "${1:-}" = "--check" ] && [ "$bad" = 0 ] && echo "Копії ідентичні."
exit $bad
