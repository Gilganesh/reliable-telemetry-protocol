# Shared node firmware core

This folder is the source of truth for the ESP32 firmware core. It is not a node variant: the variants are the sibling
folders (see [`../README.md`](../README.md)), and `sync.sh` copies the files below into each of them. The firmware is split
into a sensor-independent core and one sketch per sensor.

| File | Role |
|---|---|
| `node_common.h` | Links, buffering, ALARM delivery, provisioning, console |
| `packet_queue.h` | Fixed-capacity ring buffer of packets (unit-tested on the host) |
| `../accelerometer/` | MPU9250 on I2C (SDA 21, SCL 22, address 0x68): `roll`, `pitch`, `yaw` |
| `../sht41/` | SHT41 on I2C (default pins, address 0x44): `temperature`, `humidity` |
| `threshold.h` | Threshold with hysteresis for sensor-driven alarms (`sht41_lcd` uses it; unit-tested on the host) |
| `node_lcd.h` | Dependency-free driver for a 1602 LCD behind a PCF8574 I2C backpack (unit-tested against an HD44780 emulator) |
| `../no_sensor_lcd/` | Sensorless node with an LCD: telemetry is `uptime_s`, `free_heap_kb`, `rssi` |
| `../accelerometer_lcd/`, `../sht41_lcd/` | The two sensor sketches with an LCD |
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

## LCD

A sketch enables the display with `#define NODE_LCD 1` before including `node_common.h`; without it none of the LCD code is compiled.
The sketch calls `Wire.begin(NODE_I2C_SDA, NODE_I2C_SCL)` (default SDA 21, SCL 18: valid on both ESP32-WROOM and ESP32-S3; `sht41_lcd` overrides SCL to 22 for a WROOM),
`lcd.setBusClocks(lcd_hz, sensor_hz)` and `lcd_init()`, and implements one extra hook:

```cpp
bool sensor_display(char *line, size_t n);   // 16-character line for row 2; return false to show buffer statistics
```

The display is refreshed every 250 ms and at most one changed line is written per pass, so the main loop is never blocked for
more than a few milliseconds. An MPU9250 on a 400 kHz bus drops to 100 kHz only for the duration of a display write. If the
display is unplugged the node keeps working and probes for it every 5 s. `lcd_message(line1, line2)` shows a message at
once (used for the IMU calibration prompt) and `LCD_FLASH(text)` shows a short event for three seconds.

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
| `SERVO_ENABLED` | `false` | Handle the `servo` command with a servo on GPIO13 (ESP32Servo library) |
| `WIRE_TIMEOUT_MS` | `3000` | Wired link timeout |
| `WIFI_PROBE_MS` / `WIFI_LINK_TIMEOUT_MS` | `2000` / `7000` | Wi-Fi probe interval and timeout |
| `BUFFER_CAPACITY` / `ALARM_QUEUE_CAP` | `50` / `8` | Offline buffer sizes |

Telemetry is sent every 5 s once the node has an id.

## Hardware notes

An I2C transaction can hang forever if the bus glitches, for example when the supply sags during Wi-Fi transmission,
so both sketches set `Wire.setTimeOut(1000)`. If a board still stops responding, check the sensor wiring and power the
boards from a powered USB hub.
