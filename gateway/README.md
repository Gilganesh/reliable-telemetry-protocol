# Gateway

Single-process C daemon that receives frames from every node, validates and tracks them, acknowledges critical
messages and publishes its state over MQTT.

## Build and run

```bash
make            # needs libmosquitto and libcjson
./gateway
```

Run it from this directory: it reads `gateway.conf`, appends events to `gateway_log.txt` and keeps the MAC → node id
registry in `node_registry.txt` in the working directory. A terminal dashboard is printed every 2 seconds.

## Inputs

| Link | Details |
|---|---|
| UDP | port 5005, one frame per datagram |
| TCP | port 5006, up to 4 clients, connections idle for 20 s are closed |
| UART | scans `/dev/ttyUSB*` and `/dev/ttyACM*` every 2 s (override with `GATEWAY_UART`), up to 4 ports, 115200 8N1. A port becomes active once a valid frame arrives; unplugged ports are closed automatically |
| MQTT | subscribes to `telemetry/uplink` on `127.0.0.1:1883`, reconnects automatically; the gateway keeps working without a broker |

Every link feeds the same `handle_packet()` path, and replies (ACKs, commands, pings) go back over the link the
node was last heard on.

## Node tracking

- **Sequence window**: the last 64 sequence numbers per node distinguish duplicates from late packets. Gaps count as
  loss and are credited back if the packet arrives later.
- **Critical messages**: ALARM/CONFIG sequence numbers are remembered separately, so a retry that arrives long after
  newer traffic is still deduplicated rather than mistaken for a node restart.
- **Restarts**: a HELLO from a known MAC (or a sharp drop in the node's uptime timestamp) resets sequence tracking.
- **Online state**: a node goes offline after 15 s without traffic.
- **Latency**: every 5 s each online node receives `CONFIG {"cmd":"ping"}`; the ACK gives a round-trip time measured on
  the gateway clock (exponential moving average, α = 0.3).
- **Sample time**: node timestamps are uptime in milliseconds, not wall-clock. For every node the gateway tracks the minimum
  of (arrival time − node timestamp) since the node booted (with a slow 2 ms/packet relaxation to follow clock drift).
  Fresh packets define that minimum, so a packet that spent time in the node's buffer shows up with a larger difference;
  the excess is published as `age_ms` and the dashboard places the sample at `now − age_ms`. The estimate is reset when the
  node restarts.
- **Backlog**: nodes append `backlog` and `dropped` to their telemetry; the gateway reports the values from the newest
  packet.

## Node id assignment and provisioning

A booting node sends a HELLO (`HEARTBEAT` with `node_id = 0` and `{"mac": ..., "id": ...}`). The gateway answers with
`CONFIG {"cmd":"id","mac":...,"id":N}`, assigning the lowest free id and persisting it per MAC address.

When a node is seen on UART, the gateway sends two `CONFIG` frames until both are acknowledged:

```json
{"cmd":"wifi","s":"<ssid>","p":"<password>"}
{"cmd":"gw","ip":"<gateway ip>","udp":5005,"tcp":5006}
```

The IP is taken from the active network interface. The SSID and password come from the active NetworkManager
connection (`nmcli`, also tried via `sudo -n`) or from `gateway.conf`, which may list several networks
(see `gateway.conf.example`). Network detection runs in a background thread every 10 s, and nodes are re-provisioned
when the host changes networks.

## Published state

`telemetry/gateway/state` (every 2 s):

```json
{
  "nodes": [{"node_id": 1, "online": true, "transport": "UART", "received_count": 120, "lost_count": 2,
             "duplicate_count": 1, "max_seq_seen": 121, "loss_rate": 1.6, "last_seen_ms_ago": 830,
             "latency_ms": 14.2, "backlog": 0, "buffer_dropped": 0}],
  "corrupted_count": 0,
  "impairment": {"profile": "good", "active": false, "loss": 0, "dup": 0, "corrupt": 0, "delay_ms": 0,
                 "jitter_ms": 0, "node": 0, "blackout_left_s": 0, "dropped_up": 0, "dropped_down": 0,
                 "duplicated": 0, "corrupted": 0, "delayed": 0}
}
```

`telemetry/gateway/telemetry` (every accepted, non-duplicate packet):

```json
{"node_id": 1, "sequence": 121, "ts_ms": 605123, "type": 1, "transport": "UART", "age_ms": 0,
 "payload": {"temperature": 23.4, "humidity": 41.2, "backlog": 0, "dropped": 0}}
```

## Impairment control

Publish to `telemetry/gateway/control`:

```json
{"cmd": "impair", "profile": "lossy20", "node": 0, "seed": 42}
{"cmd": "impair", "loss": 5, "delay_ms": 50, "jitter_ms": 20}
{"cmd": "impair", "blackout_s": 30}
```

`profile` resets all parameters first; individual fields then adjust the current state; `blackout_s` leaves the
profile untouched (`0` cancels a blackout). `node = 0` applies to all nodes. Counters reset when the profile changes.
