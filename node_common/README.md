# Node firmware

ESP32 firmware is split into a sensor-independent core and one sketch per sensor.

| File | Role |
|---|---|
| `node_common.h` | Links, buffering, ALARM delivery, provisioning, console |
| `packet_queue.h` | Fixed-capacity ring buffer of packets (unit-tested on the host) |
| `../node_accelerometer/` | MPU9250 on I2C (SDA 21, SCL 22, address 0x68): `roll`, `pitch`, `yaw` |
| `../node_sht41/` | SHT41 on I2C (default pins, address 0x44): `temperature`, `humidity` |
| `raw_tests/` | Minimal sketches to bring up each sensor on its own |
| `sync.sh` | Copies the core and `../protocol/` into each sketch folder; `--check` verifies the copies |

A sensor sketch implements three hooks and forwards `setup()`/`loop()` to the core:

```cpp
void sensor_setup();                        // once, after Serial is up
void sensor_update();                       // every loop iteration (e.g. IMU filter)
bool sensor_payload(char *buf, size_t n);   // JSON object for the next telemetry packet; false skips it
```

Arduino IDE only compiles files located next to the sketch, so run `sync.sh` after editing this folder or
`../protocol/`.

## Links

The node keeps all links open and sends over the best live one: **UART > TCP > UDP**.

- **UART** is alive while frames from the gateway keep arriving (the gateway sends a keep-alive every second;
  timeout 3 s).
- **TCP/UDP** are alive only while the gateway answers on them. Every 2 s the node sends a HELLO probe over both; a
  link without a reply for 7 s is considered dead. An open socket alone is not trusted: a lost gateway would
  otherwise swallow packets instead of letting them be buffered.
- With no live link, telemetry goes into a 50-packet RAM buffer (oldest dropped on overflow) and is flushed in order,
  one packet every 50 ms, once a link returns. New telemetry is appended to the buffer while it drains.
- ALARMs go through a separate 8-entry queue and are delivered one at a time with ACK/retry. If retries are
  exhausted the ALARM returns to the head of the queue and is retried after 5 s.

## Provisioning

No credentials are compiled in. On first boot, connect the board to the gateway over UART: it receives a node id,
Wi-Fi credentials and the gateway address, acknowledges them and stores them in NVS. After that it can run on Wi-Fi
alone. The serial command `forget` erases the stored settings.

## Build options

| Option | Default | Meaning |
|---|---|---|
| `LINK_VIA_USB_CABLE` | `1` | Wired link over the board's USB serial (UART0). Logs and console commands are disabled because the port carries protocol frames |
| | `0` | Wired link over UART2: GPIO17 (TX) → adapter RX, GPIO16 (RX) → adapter TX, common GND. USB serial stays free for logs and the `alarm`, `status`, `forget` commands |
| `SERVO_ENABLED` | `false` | Drive a servo on GPIO18 (ESP32Servo library) |
| `WIRE_TIMEOUT_MS` | `3000` | Wired link timeout |
| `WIFI_PROBE_MS` / `WIFI_LINK_TIMEOUT_MS` | `2000` / `7000` | Wi-Fi probe interval and timeout |
| `BUFFER_CAPACITY` / `ALARM_QUEUE_CAP` | `50` / `8` | Offline buffer sizes |

Telemetry is sent every 5 s once the node has an id.

## Hardware notes

An I2C transaction can hang forever if the bus glitches, for example when the supply sags during Wi-Fi transmission,
so both sketches set `Wire.setTimeOut(1000)`. If a board still stops responding, check the sensor wiring and power the
boards from a powered USB hub.
