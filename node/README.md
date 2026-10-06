# Node

A node reads a sensor and sends telemetry to the gateway over the best available link (UART > TCP > UDP), buffering while
every link is down. Pick the variant that matches your hardware; each folder is a complete, self-contained Arduino sketch.

| Folder | Sensor | Display | Telemetry |
|---|---|---|---|
| [`sht41/`](sht41/) | SHT41 | none | `temperature`, `humidity` |
| [`sht41_lcd/`](sht41_lcd/) | SHT41 | 1602 LCD | `temperature`, `humidity`; raises an ALARM at 35 °C |
| [`accelerometer/`](accelerometer/) | MPU9250 | none | `roll`, `pitch`, `yaw` |
| [`accelerometer_lcd/`](accelerometer_lcd/) | MPU9250 | 1602 LCD | `roll`, `pitch`, `yaw` |
| [`no_sensor/`](no_sensor/) | none | none | `uptime_s`, `free_heap_kb`, `rssi` |
| [`no_sensor_lcd/`](no_sensor_lcd/) | none | 1602 LCD | `uptime_s`, `free_heap_kb`, `rssi` |
| [`simulated/`](simulated/) | none (software node) | none | `temperature`, `humidity` over MQTT |

## Flash a board

1. Install the ESP32 board package in the Arduino IDE. The `accelerometer` variants also need the **MPU9250** library by
   *hideakitai*; nothing else needs extra libraries.
2. Open the `.ino` file inside the folder you picked, select your ESP32 board and upload.
3. Start the gateway (`./run.sh` in the repository root) and connect the board to the gateway host with a USB cable. The
   gateway assigns a node id, sends the Wi-Fi credentials and its own address, and the board can then run without the cable.

Wiring and display notes are in [`../docs/hardware.md`](../docs/hardware.md).

## Try it without a board

```bash
make -C simulated
simulated/sim_node --node-id 101
```

See [`simulated/README.md`](simulated/README.md) for loss, duplication and alarm options.

## How the folders fit together

`_shared/` is not a variant. It holds the firmware core (`node_common.h`, `packet_queue.h`, `node_lcd.h`, `threshold.h`)
that every sketch is built from. Arduino only compiles files that sit next to the sketch, so `sync.sh` copies the core and
`../protocol/` into each variant folder. If you change `_shared/` or `../protocol/`, run `_shared/sync.sh`; `make test` in
the repository root verifies that the copies match. To add a variant, copy the nearest existing folder, rename the `.ino`
to match the folder name and implement the sensor hooks described in [`_shared/README.md`](_shared/README.md).
