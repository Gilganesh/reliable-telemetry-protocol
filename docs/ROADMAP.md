# Roadmap

Status as of 2026-10-03. Background documents: [`case-brief.md`](case-brief.md) (requirements and acceptance
criteria), [`case-2.4-requirements.md`](case-2.4-requirements.md), [`team-blocks/`](team-blocks/).

## Where the project stands

The system is functionally complete: sensor nodes, gateway, impairment simulator, dashboard and a one-command launcher
all work together. What remains is evidence (recorded test runs on hardware), two documents and a few improvements.

| Area | State |
|---|---|
| Protocol (frame, CRC-32, ACK/retry, sequence tracking) | Done, unit-tested; specified with state diagrams in [`protocol.md`](protocol.md) |
| Node firmware: UART > TCP > UDP failover, store-and-forward, alarm queue, zero-touch provisioning | Done, verified on 4 boards |
| Sensors: MPU9250, SHT41, sensorless node | Done |
| LCD 1602 variants of all three sketches | Done, reported working on boards |
| Gateway: tracking, dedup, latency, backlog, impairment layer, MQTT state | Done |
| Dashboard: KPIs, node cards, charts on a true time axis, alarm log, impairment panel, light/dark theme | Done |
| Launcher: `run.sh`, desktop shortcut for the Raspberry Pi | Done |

## Acceptance criteria

| # | Criterion | State | Evidence / what is missing |
|---|---|---|---|
| 1 | At least 3 nodes at once | Done | 4 real ESP32 boards on a Raspberry Pi, ids assigned by MAC; more can be added with `sim_node --node-id` |
| 2 | 20% loss: critical events delivered or reported as "retries exhausted" | Emulated only | `lossy20` profile + node ALARM; an emulated node delivered 146 of 150 alarms, reported 4 exhausted, no duplicate events. Not yet recorded on a real board |
| 3 | A duplicate does not create a second event | Done | Dedup by node id and sequence; repeated ALARM is acknowledged but logged once |
| 4 | Outage, buffering, flush | Done | Verified on a board after the Wi-Fi/gateway outage fix. Not tested: buffer overflow (outage longer than about 250 s); a recorded 30 s run |
| 5 | A corrupted frame is rejected and the gateway keeps running | Emulated only | CRC rejection is counted and shown on the dashboard; the `flaky` profile was run against an emulated node, not against a board |

## Verification so far

- Unit tests (`make test`): protocol 19, reliability 30, packet queue 17, LCD driver 21 (against an HD44780 emulator), plus a
  Python/C codec cross-check and a firmware copy check.
- Emulated node against the real gateway: `lossy20` (150 alarms), `flaky` (corrupted frames rejected, counters match),
  `delay` (RTT 400 to 600 ms), a 3 s blackout, a late retry arriving 89 packets behind (still deduplicated).
- Buffered samples are placed at their true measurement time: with a simulated outage the stored times matched the real
  creation times within 40 ms. Not yet observed on a board.
- On hardware: simultaneous nodes on UART and TCP, UART hot-unplug and replug, failover to Wi-Fi, store-and-forward after a
  Wi-Fi outage, LCD sketches.

## Next steps, by priority

1. **Record the missing runs on boards**: `lossy20` with the node ALARM button, `flaky`, a 30 s blackout, and a long outage to
   check buffer overflow (`dropped` on the node must equal the loss the gateway reports). Save the numbers; they become the
   test report.
2. **Baseline comparison for the report**: add a naive mode to `sim_node` (send once, no ACK, no buffer) and a script that
   runs both modes through the same impairment profiles with a fixed seed, producing a table of delivered, lost and duplicate
   alarms. MQTT or a naive sender is explicitly allowed as a baseline in the brief.
3. **Sensor-driven alarms** in the firmware: raise ALARM on a threshold (temperature above a limit, tilt or shock) with
   hysteresis and a cool-down, so the critical event comes from the node instead of a button.
4. **Documents**: test report (profile, delivered, lost, retries, recovery time) and a high-level threat model (spoofed node,
   replay, plain-text Wi-Fi password on the serial link, open broker). The protocol specification is already in
   [`protocol.md`](protocol.md).
5. **Open questions for the mentor**: CRC-32 versus HMAC, what counts as a critical message, node buffer size.

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
- Two boards report zero roll/pitch/yaw because the MPU9250 is not detected on their I2C bus (wiring or power); the LCD
  sketch shows `IMU not found` in that case.
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
