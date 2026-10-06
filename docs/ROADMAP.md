# Roadmap

Current state and planned work. The protocol is specified in [`protocol.md`](protocol.md).

## Where the project stands

| Area | State |
|---|---|
| Protocol (frame, CRC-32, ACK/retry, sequence tracking) | Done, unit-tested, specified with state diagrams |
| Node firmware: UART > TCP > UDP failover, store-and-forward, alarm queue, zero-touch provisioning | Done, verified on 4 boards |
| Sensors: MPU9250, SHT41, sensorless node | Done |
| LCD 1602 variants of all three sketches | Done |
| Gateway: tracking, dedup, latency, backlog, impairment layer, MQTT state | Done |
| Dashboard: KPIs, node cards, charts on a true time axis, alarm log, impairment panel, light/dark theme | Done |
| Launcher: `run.sh`, desktop shortcut for the Raspberry Pi | Done |

## Verification so far

- Unit tests (`make test`): protocol 21, reliability 30, packet queue 17, stream frame reader 9, alarm threshold 13, LCD driver 21 (against an
  HD44780 emulator), plus a Python/C codec cross-check and a firmware copy check.
- Stream resynchronisation, measured on a real TCP stream: a frame with a damaged length field used to cost three frames, a
  truncated frame two; now each costs exactly one. A frame with another protocol version is rejected on every link.
- Emulated node against the real gateway: `lossy20` (150 alarms: 146 delivered, 4 reported as retries exhausted, no duplicate
  events), `flaky` (corrupted frames rejected, counters match), `delay` (RTT 400 to 600 ms), a 3 s blackout, a late retry
  arriving 89 packets behind (still deduplicated).
- Buffered samples are placed at their true measurement time: with a simulated outage the stored times matched the real
  creation times within 40 ms.
- On hardware: simultaneous nodes on UART and TCP, UART hot-unplug and replug, failover to Wi-Fi, store-and-forward after a
  Wi-Fi outage, LCD sketches.

## Next steps

1. **Recorded hardware runs**: `lossy20` with a real board raising ALARMs, `flaky`, a 30 s blackout, and a long outage to
   check buffer overflow (`dropped` on the node must equal the loss the gateway reports).
2. **Baseline comparison**: a naive mode in `sim_node` (send once, no ACK, no buffer) and a script that runs both modes
   through the same impairment profiles with a fixed seed, producing a table of delivered, lost and duplicate alarms.
3. **More sensor-driven alarms**: `node_sht41_lcd` raises an ALARM at 35 °C (once per crossing, 2 °C hysteresis,
   `send_alarm_json()` in the core). The same for the IMU (tilt or shock) is not done yet.
4. **Threat model**: spoofed node, replay, plain-text Wi-Fi password on the serial link, open broker.

## Known limitations and technical debt

- CRC-32 only: no authentication and no replay protection. HMAC-SHA256 (mbedTLS is on the ESP32) is the natural next step.
- Buffered telemetry is fire-and-forget once a link accepts it; if a link dies in the middle of a flush those packets are
  lost. Fix: acknowledge the last packet of a batch.
- The node buffer is in RAM (50 telemetry packets, 8 alarms) and does not survive a reboot. NVS persistence is an option.
- Only one critical message is in flight per node.
- Loss rate on the dashboard is cumulative since the gateway started; a windowed value (last N packets) would be clearer.
- Latency is round-trip only; one-way delay and jitter are not measured. After a link switch the smoothed RTT takes about
  25 s to settle.
- Impairment acts inside the gateway, so a node on a healthy wire does not notice the loss and does not buffer. Only a
  blackout reaches the node (it stops the keep-alive).
- With `LINK_VIA_USB_CABLE 1` the board's serial port carries protocol frames, so logs and console commands are unavailable
  (use `0` for debugging).
- The mosquitto config allows anonymous access on all interfaces; Wi-Fi credentials travel to nodes in plain text over the
  serial link and are stored in NVS in plain text.
- The MPU9250 may not be detected on a board's I2C bus because of wiring or power; the LCD sketch shows `IMU not found` in
  that case.
- The LCD backpack may pull SDA/SCL up to 5 V; check this before sharing the bus with a 3.3 V sensor such as the SHT41.

## Ideas

- Prometheus `/metrics` endpoint and a Grafana dashboard.
- Adaptive retry: exponential backoff with jitter, timeout from measured RTT.
- Message priorities (ALARM over CONFIG over TELEMETRY) in the queues; thinning old telemetry on overflow instead of
  dropping the oldest.
- End-to-end confirmation of dashboard commands (the dashboard currently reports "sent" once the MQTT publish succeeds).
- Docker Compose bench (broker, gateway, web, several `sim_node`), a fuzz test for `protocol_unpack`, OTA updates,
  reset reason in the HELLO frame, systemd units for start on boot.

## Operating notes

- Start everything: `./run.sh` (add `--open` for the browser, or use the desktop shortcut from `./install-shortcut.sh`).
- After pulling changes on the Raspberry Pi: `make -B -C gateway` when `gateway.c` or `protocol/` changed, then restart the
  gateway and the dashboard. Boards only need reflashing when `node_common/` or the sketches changed.
- Run `node_common/sync.sh` after editing `node_common/` or `protocol/`; `make test` checks that the sketch copies match.
- macOS: Firefox needs the Local Network permission; AddressSanitizer hangs on this system, so `make test` uses UBSan.
