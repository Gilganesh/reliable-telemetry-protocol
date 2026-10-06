# Hardware

## Boards and sensors

Developed and tested on ESP32-WROOM DevKit boards. The LCD wiring below is also chosen to work on an ESP32-S3 DevKitC-1. The
gateway runs on a Raspberry Pi or any Linux or macOS host.

Firmware: Arduino IDE with the ESP32 board package. The MPU9250 sketch also needs the **MPU9250** library by
*hideakitai*; the SHT41 sketch has no extra dependencies.

| Sketch | Sensor | Telemetry |
|---|---|---|
| `accelerometer` | MPU9250 | `roll`, `pitch`, `yaw` |
| `sht41` | SHT41 | `temperature`, `humidity` |
| `no_sensor` | none | `uptime_s`, `free_heap_kb`, `rssi` |

`no_sensor` needs nothing but the board: use it to check the link, the gateway and the dashboard before wiring a sensor.

## Flashing a node

1. Open `node/accelerometer/accelerometer.ino` or `node/sht41/sht41.ino` in the Arduino IDE and upload it.
2. With the gateway running, connect the board to the gateway host with a USB cable. The gateway detects the port,
   assigns a node id (lowest free id, stable per MAC address) and sends the Wi-Fi credentials and its own address.
3. The board stores the settings in NVS. From then on it keeps Wi-Fi as a backup link and can run without the cable.

If you edit `node/_shared/` or `protocol/`, run `node/_shared/sync.sh` to refresh the copies inside the sketch folders
(Arduino IDE only compiles files that live next to the sketch).

## Nodes with an LCD (1602 + I2C backpack)

Three sketches add a 16x2 character display that shows the active link and live node state. Pick one per board:

| Sketch | Sensor | Telemetry sent | Second LCD line |
|---|---|---|---|
| `no_sensor_lcd` | none | `uptime_s`, `free_heap_kb`, `rssi` | `Buf:0 Drop:0` |
| `accelerometer_lcd` | MPU9250 | `roll`, `pitch`, `yaw` | `R-1 P-24 Y-11` |
| `sht41_lcd` | SHT41 | `temperature`, `humidity`; raises an ALARM at 35 °C | `T21.3C H49.8%`, or `T36.2C ALARM!` while the alarm is active |

`sht41_lcd` raises a real, sensor-driven critical event: when the temperature reaches 35 °C it sends
`{"alarm":"overheat","temperature":36.2,"threshold":35}` as an acknowledged `ALARM` (retried, and queued while there is no
link), once per crossing. It re-arms after the temperature falls below 33 °C. The limit and the 2 °C hysteresis are
`TEMP_ALARM_C` and `TEMP_ALARM_HYSTERESIS_C` at the top of the sketch.

The first line is always `<link> Node <id>`, where the link is `UART`, `TCP`, `UDP` or `NO LINK`. While telemetry or alarms
are waiting in the node's buffer the second line alternates with `BUF 6 ALM 1`, and for three seconds after an alarm event it
shows `ALARM delivered`, `ALARM FAILED` or `WEB ALARM ON`.

### Wiring

The display shares the sensor's I2C bus, no extra pins: `GND` to GND, `VCC` to the 5 V pin, `SDA` to GPIO21, `SCL` to GPIO18.
These two GPIOs exist and are free on both an ESP32-WROOM DevKit and an ESP32-S3 DevKitC-1 (the S3 has no GPIO22), so the
same wiring works on either board. Connect the sensor to the same two pins.

The exception is `sht41_lcd`, which is meant for an ESP32-WROOM and uses the classic hardware pair SDA GPIO21 /
SCL GPIO22. To use other pins, define `NODE_I2C_SDA` / `NODE_I2C_SCL` before including `node_common.h`.

On an S3 board use the USB port wired to the UART bridge (labelled UART/COM) and keep *USB CDC On Boot* disabled in the
Arduino IDE, because the gateway talks to the board over that serial port.

The LCD driver is built in (no extra library), finds the backpack at `0x27` or `0x3F` and keeps running without a display.
If the screen lights up but shows only blocks or nothing, turn the contrast trimmer on the backpack.

### 5 V pull-ups

Before sharing the bus with a 3.3 V sensor such as the SHT41, check the backpack: many boards pull SDA/SCL up to their own
supply, which is 5 V. Measure SDA with the node powered; if it reads about 5 V, remove the two pull-up resistors on the
backpack or power it from 3.3 V (the display is dimmer, adjust the contrast trimmer). The sketches without a display
(`accelerometer`, `sht41`) are unaffected.

## Firmware options

Compile-time options at the top of `node/_shared/node_common.h`:

| Option | Default | Meaning |
|---|---|---|
| `LINK_VIA_USB_CABLE` | `1` | Use the board's USB serial as the wired link. Set to `0` to use UART2 on GPIO16 (RX) / GPIO17 (TX) and keep USB serial for logs and console commands (`alarm`, `status`, `forget`) |
| `SERVO_ENABLED` | `false` | Handle the `servo` command by driving a servo on `SERVO_PIN` (GPIO13, requires the ESP32Servo library); there is no dashboard control for it |
| `BUFFER_CAPACITY` | `50` | Telemetry packets buffered while offline (oldest dropped on overflow) |
| `ALARM_QUEUE_CAP` | `8` | Critical messages waiting for a link |
