# Reference

The frame format, message types and state machines are specified in [`protocol.md`](protocol.md).

## Configuration

### Gateway

| Setting | Where | Default |
|---|---|---|
| UDP / TCP ports | `UDP_PORT`, `TCP_PORT` in `gateway.c` | 5005 / 5006 |
| MQTT broker | `MQTT_HOST`, `MQTT_PORT` in `gateway.c` | 127.0.0.1:1883 |
| Serial ports to scan | `GATEWAY_UART` environment variable (glob) | `/dev/ttyUSB*`, `/dev/ttyACM*` |
| Wi-Fi credentials for provisioning | `gateway/gateway.conf` (see `gateway.conf.example`) | detected via `nmcli` |

`gateway.conf` and the node registry (`node_registry.txt`) are ignored by git. Delete the registry to restart node
numbering. On macOS there is no `nmcli`, so list your networks in `gateway.conf`.

### Web dashboard

`app.py [broker_host] [broker_port]` plus environment variables:

| Variable | Default | Meaning |
|---|---|---|
| `WEB_HOST` | `127.0.0.1` | Bind address; use `0.0.0.0` to expose the dashboard on the LAN |
| `WEB_PORT` | `8080` | HTTP port |
| `TELEMETRY_DB` | `web/telemetry.db` | SQLite database for samples and alarms |
| `RETENTION_HOURS` | `24` | How long samples are kept |

### Starting the services by hand

`./run.sh` does all of this for you.

```bash
# 1. MQTT broker (listens on all interfaces, anonymous access)
mosquitto -c gateway/mosquitto.conf

# 2. Gateway (run from gateway/ so it finds gateway.conf and writes its log and node registry there)
cd gateway && ./gateway

# 3. Web dashboard
python3 -m venv .venv && .venv/bin/pip install -r web/requirements.txt
cd web && WEB_HOST=0.0.0.0 ../.venv/bin/python app.py 127.0.0.1
```

On a desktop session (for example Raspberry Pi OS) `scripts/install-shortcut.sh` puts a **Telemetry Dashboard** icon on the
desktop: double-click it to run everything and open the dashboard in the browser.

## Protocol summary

All multi-byte fields are little-endian. A frame is the header, the payload, then a CRC-32 (IEEE 802.3, same as
`zlib.crc32`) computed over header and payload.

| Offset | Size | Field | Notes |
|---|---|---|---|
| 0 | 1 | `version` | currently `1` |
| 1 | 1 | `msg_type` | see below |
| 2 | 2 | `node_id` | `0` is reserved for HELLO and broadcast |
| 4 | 4 | `sequence` | one counter per node, shared by all message types |
| 8 | 8 | `timestamp_ms` | node uptime in milliseconds |
| 16 | 2 | `payload_len` | 0..128 |
| 18 | n | `payload` | UTF-8 JSON |
| 18 + n | 4 | `crc32` | |

| Type | Value | Delivery |
|---|---|---|
| `HEARTBEAT` | 0 | best effort; with `node_id = 0` it is a HELLO carrying `{"mac": ..., "id": ...}` |
| `TELEMETRY` | 1 | best effort, buffered on the node while offline |
| `ALARM` | 2 | acknowledged, retried |
| `CONFIG` | 3 | acknowledged, retried |
| `ACK` | 4 | echoes the sequence number of the acknowledged frame |

Stream transports (UART, TCP) are framed by the header itself: the receiver resynchronises byte by byte on a plausible
header (`version`, `msg_type`, `payload_len`) and validates the CRC. After a failed check it drops one byte and rescans, so
a damaged or truncated frame costs only itself. UDP and MQTT carry one frame per datagram/message.

### Gateway to node commands

`CONFIG` frames with a JSON payload: `{"cmd":"id"}` (node id assignment), `wifi`, `gw` (provisioning), `ping` (latency
probe), `servo` (firmware only, not exposed in the dashboard), `fire_alarm` (ask the node to raise its own ALARM),
`simulate_loss`. An `ALARM` frame with `{"cmd":"alarm","active":true|false}` toggles the remote alarm indicator on the node.

## MQTT topics

| Topic | Direction | Format |
|---|---|---|
| `telemetry/uplink` | MQTT nodes → gateway | binary frame |
| `telemetry/downlink/<node_id>` | gateway / web → node | binary frame |
| `telemetry/gateway/state` | gateway → web, every 2 s | JSON: per-node counters, latency, backlog, impairment state |
| `telemetry/gateway/telemetry` | gateway → web, per accepted packet | JSON: header fields, decoded payload and `age_ms` (how long ago the sample was measured) |
| `telemetry/gateway/control` | web → gateway | JSON: `{"cmd":"impair", "profile", "loss", "dup", "corrupt", "delay_ms", "jitter_ms", "node", "seed", "blackout_s"}` |

## Web API

| Method | Endpoint | Purpose |
|---|---|---|
| GET | `/api/state` | Node list, counters, impairment and broker status |
| GET | `/api/metrics` | Metric names available per node |
| GET | `/api/history?node_id=&metric=&since_s=` | Time series for a chart |
| GET | `/api/events` | Recent alarms |
| POST | `/api/impairment` | Apply a profile, custom parameters or a blackout |
| POST | `/api/node-alarm/<node_id>` | Ask a node to raise an ALARM |
| POST | `/api/alarm/<node_id>`, `/api/alarm/all` | Toggle the remote alarm on nodes |
| POST | `/api/simulate-loss/<node_id>` | Enable outbound packet loss on the node itself |
| POST | `/api/config/broker` | Switch the MQTT broker the dashboard listens to |

## Channel impairment

The impairment layer sits at the gateway's single entry and exit points, so it affects every transport in both
directions.

| Profile | Loss | Duplicates | Corruption | Delay |
|---|---|---|---|---|
| `good` | 0 | 0 | 0 | 0 |
| `lossy20` | 20% | 0 | 0 | 0 |
| `delay` | 0 | 0 | 0 | 200 ms + 0..100 ms jitter |
| `flaky` | 10% | 10% | 5% | 100 ms + 0..300 ms jitter (causes reordering) |

A blackout drops all traffic for N seconds. When it targets all nodes it also stops the gateway's UART keep-alive, so
wired nodes switch to buffering exactly as they would after a real disconnect. Other profiles act on the gateway side
only: a node on a healthy wire does not notice the loss and does not buffer.
