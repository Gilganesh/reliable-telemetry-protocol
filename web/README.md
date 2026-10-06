# Web dashboard

A Flask app that renders the gateway's state and stores sensor samples in SQLite. It never recomputes delivery statistics:
everything it shows comes from the gateway over MQTT.

## Run

The easiest way is `./run.sh` in the repository root, which starts the broker, the gateway and this dashboard together.
To run it alone (a broker must be running):

```bash
python3 -m venv ../.venv && ../.venv/bin/pip install -r requirements.txt
../.venv/bin/python app.py 127.0.0.1        # broker host, optional broker port as the second argument
```

Open <http://localhost:8080>.

## What it shows

- Node cards with online/offline state, loss rate, round-trip latency and node-side buffer backlog.
- Time-series charts on the true measurement time, so data buffered during an outage fills the gap after the link returns.
- An alarm log, remote commands (raise an alarm on a node, simulate loss on the node itself).
- The channel impairment panel: profiles `good`, `lossy20`, `delay`, `flaky`, custom parameters and timed blackouts.
- A light and a dark theme.

## Settings

| Variable | Default | Meaning |
|---|---|---|
| `WEB_HOST` | `127.0.0.1` | Bind address; use `0.0.0.0` to expose the dashboard on the LAN |
| `WEB_PORT` | `8080` | HTTP port |
| `TELEMETRY_DB` | `web/telemetry.db` | SQLite database for samples and alarms |
| `RETENTION_HOURS` | `24` | How long samples are kept |

The REST API and the MQTT topics it uses are listed in [`../docs/reference.md`](../docs/reference.md).

## Files

| File | Role |
|---|---|
| `app.py` | Flask app, MQTT client, SQLite storage, REST API |
| `static/index.html` | The single-page dashboard (Chart.js is vendored in `static/`) |
| `protocol_codec.py` | Python implementation of the frame codec, cross-checked against the C code by `make test` |
