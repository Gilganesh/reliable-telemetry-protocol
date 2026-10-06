#!/usr/bin/env bash
set -u
ROOT="$(cd "$(dirname "$0")" && pwd)"
LOGS="$ROOT/logs"
PORT="${WEB_PORT:-8080}"
PIDS=()

mkdir -p "$LOGS"

stop_all() {
  trap - EXIT INT TERM HUP
  echo
  echo "Stopping..."
  for pid in "${PIDS[@]}"; do kill "$pid" 2>/dev/null; done
  wait 2>/dev/null
}
trap stop_all EXIT INT TERM HUP

if [ ! -x "$ROOT/.venv/bin/python" ]; then
  echo "Creating Python environment..."
  python3 -m venv "$ROOT/.venv" && "$ROOT/.venv/bin/pip" install -q -r "$ROOT/web/requirements.txt" || exit 1
fi

echo "Building gateway..."
make -s -C "$ROOT/gateway" || exit 1

if command -v systemctl >/dev/null 2>&1 && systemctl is-active --quiet mosquitto 2>/dev/null; then
  echo "Stopping the system mosquitto service (it owns port 1883)..."
  sudo systemctl stop mosquitto
fi

echo "Starting services..."
mosquitto -c "$ROOT/gateway/mosquitto.conf" >"$LOGS/mosquitto.log" 2>&1 &
PIDS+=("$!")
echo "  started mosquitto (pid $!, log: logs/mosquitto.log)"
sleep 1
(cd "$ROOT/web" && WEB_HOST=0.0.0.0 WEB_PORT="$PORT" exec "$ROOT/.venv/bin/python" app.py 127.0.0.1) >"$LOGS/web.log" 2>&1 &
PIDS+=("$!")
echo "  started web (pid $!, log: logs/web.log)"

sleep 2
for pid in "${PIDS[@]}"; do
  if ! kill -0 "$pid" 2>/dev/null; then
    echo "A service exited right after start, check the logs in $LOGS"
    exit 1
  fi
done

IPS="$(hostname -I 2>/dev/null)"
[ -z "$IPS" ] && IPS="$(ipconfig getifaddr en0 2>/dev/null)"
echo
echo "Dashboard:"
echo "  http://localhost:$PORT"
for ip in $IPS; do
  case "$ip" in
    *:*) ;;
    *) echo "  http://$ip:$PORT" ;;
  esac
done
echo "  http://$(hostname -s).local:$PORT"
echo
echo "Gateway output follows (events are also saved to gateway/gateway_log.txt). Press Ctrl+C to stop everything."
echo

if [ "${1:-}" = "--open" ]; then
  (xdg-open "http://localhost:$PORT" || open "http://localhost:$PORT") >/dev/null 2>&1 &
fi

(cd "$ROOT/gateway" && exec ./gateway) &
PIDS+=("$!")

while true; do
  for pid in "${PIDS[@]}"; do
    if ! kill -0 "$pid" 2>/dev/null; then
      echo
      echo "A service stopped unexpectedly (pid $pid). See logs/ and gateway/gateway_log.txt."
      exit 1
    fi
  done
  sleep 1
done
