# Simulated node

A software node that speaks the same protocol as the ESP32 firmware over MQTT. Use it to run the stack without
hardware or to add more nodes to a test bench.

```bash
make
./sim_node [--node-id N] [--loss-percent P] [--duplicate-percent P] [--send-alarm] [broker_host]
```

| Option | Default | Meaning |
|---|---|---|
| `--node-id` | `99` | Node id; run several instances with different ids to simulate a larger network |
| `--loss-percent` | `0` | Drop this share of outgoing frames, including ALARM retries and ping replies |
| `--duplicate-percent` | `0` | Send this share of outgoing frames twice |
| `--send-alarm` | off | Send one ALARM with ACK/retry shortly after start |
| `broker_host` | `127.0.0.1` | MQTT broker (port 1883) |

The node publishes `{"temperature", "humidity"}` telemetry every 3 s to `telemetry/uplink`, listens on
`telemetry/downlink/<id>` for ACKs and latency pings, and reconnects to the broker automatically.
