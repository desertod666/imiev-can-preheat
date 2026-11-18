# i-MiEV / iOn CAN preheat example

A small ESP32 example that reproduces the CAN requests used by the factory MiEV Remote ECU to start and stop cabin heating. It provides HEAT and DEFROST commands through a USB serial terminal and prints heater-current feedback.

The command bytes and timing were used successfully on a **2012 Peugeot iOn with EV-ECU software 9499B22901**, with the key in **OFF** and an **AC charging cable connected to a powered charging station**. The owner observed roughly 2.4 kW of electric heating in those tests. This standalone extraction has been compile-checked and its timing logic has host tests. It still needs its own vehicle validation.

## Scope and current limits

This repository contains the CAN heating example, its build settings and protocol tests. It runs locally through USB. All vehicle transmissions use standard CAN ID `0x38D`, DLC 8, at 500 kbit/s.

**Heating with the cable unplugged remains unproven.** Sending a request or seeing some CAN traffic return does not demonstrate that the EV-ECU, charger or heater is fully awake. This example does not generate EVWO, CP or PP signals. Their physical behavior is still being investigated.

Use a separate ESP32 for this example. Flashing it over a vehicle's existing controller replaces that controller's firmware. Test on a parked vehicle in P, with the parking brake applied, key OFF, and the cable connected to a powered AC station. Monitor the vehicle during testing. Other firmware versions and vehicle years can behave differently.

## Hardware

- ESP32 or ESP32-S3 development board with a USB serial connection.
- A high-speed CAN transceiver with ESP32-compatible 3.3 V logic, such as an appropriately powered SN65HVD230 module.
- An OBD connection to the vehicle CAN bus.

The TWAI peripheral needs an external CAN transceiver. Use the transceiver's specified supply voltage. Keep its enable/standby pins in the normal operating state according to its datasheet.

| Connection | ESP32 `esp32dev` default | ESP32-S3 `esp32s3` default |
| --- | --- | --- |
| ESP TX to transceiver TXD | GPIO 5 | GPIO 17 |
| ESP RX from transceiver RXD | GPIO 4 | GPIO 18 |
| Transceiver CAN H | OBD pin 6 | OBD pin 6 |
| Transceiver CAN L | OBD pin 14 | OBD pin 14 |
| Logic ground | Vehicle signal ground, OBD pin 5 | Vehicle signal ground, OBD pin 5 |

These GPIOs are example wiring choices. Set `PREHEAT_CAN_TX` and `PREHEAT_CAN_RX` in `platformio.ini` for your board and wiring. Power the development board through a suitable USB source. OBD pin 16 is the vehicle's 12 V supply and must not be connected directly to a GPIO or the board's 3.3 V input. The vehicle bus already has termination. Disable an extra 120 ohm terminator on the transceiver module when joining that bus.

## Build and upload

Install PlatformIO, then run the commands from this directory. PlatformIO detects the serial port. If several boards are connected, select the intended port with `--upload-port`.

For a classic ESP32 board:

```sh
pio run -e esp32dev
pio run -e esp32dev --target upload
pio device monitor -b 115200
```

For ESP32-S3, use `-e esp32s3` for both build and upload. That environment uses native USB CDC. Choose the USB port matching your board's native USB connection.

The board boots in **listen-only mode**. Send commands with a newline or carriage return. An explicit HEAT, DEFROST or STOP request enables CAN transmission for that sequence. After cancellation and observation, it returns to listen-only mode.

## Serial commands

```text
STATUS
HEAT 60
STOP
DEFROST 60
STOP
```

Duration is measured in **seconds**, starting from the first heating-request frame accepted by the local CAN driver. The default is 60 seconds. The allowed range is 1 to 3600 seconds. Wait for the previous sequence to finish and return to `IDLE` before starting another one.

Allow about 15 seconds of preparation for HEAT and 20 seconds for DEFROST before the requested heating duration starts. STOP cancels the selected mode immediately and repeats its stop byte for 1.5 seconds, followed by 6 seconds of observation. Repeating STOP during that window does not extend it. Immediately after a board restart, STOP uses the HEAT stop byte. Select DEFROST first if you need that mode's stop byte.

The example yields to an incoming `0x38D` from another controller. Avoid running it alongside an active factory Remote ECU or another sender using that ID. It also yields when fresh vehicle frames indicate ACC/ON, movement, or a selected gear other than P. Missing telemetry does not establish that the car is parked, so check the physical conditions before issuing a command. A handover or CAN failure can prevent a stop request from reaching the vehicle. In that case, verify the heater state at the car.

## CAN sequence

Each request is an 8-byte standard data frame. The first byte is listed below. The remaining seven bytes are zero. Requests repeat every **50 ms** while a transmission phase is active.

| Phase | HEAT | DEFROST | Duration |
| --- | --- | --- | --- |
| Baseline | Listen | Listen | 1.5 s |
| Neutral | `02` | `02` | 3.1 s |
| Preparation | `C2` | `C2` | HEAT 10.15 s, DEFROST 14.95 s |
| Heating request | `C5` | `C7`, then `CF` | User duration, DEFROST switches after 2.15 s |
| Stop request | `C4` | `C6` | 1.5 s |
| Observe stop | Receive | Receive | 6 s |

These are reproduced capture/test timings. They do not establish the minimum timing required by the car. In this example, `C2` is the preparation byte. It does not guarantee a full wake-up from sleep.

## Reading the output

- `TX_SUBMITTED` means the local CAN driver accepted a frame for transmission. It does not mean that the car accepted the command or started heating.
- `PTC_CURRENT_OBSERVED` means a fresh `0x384` reported nonzero electric-heater current after this sequence submitted a heating request. Verify airflow and warming at the vehicle as well.
- `RX384` reports PTC current from byte 4 in 0.1 A units and the observed temperature decoding for bytes 5 and 6. These are heater-circuit temperature readings. Cabin temperature is not measured by this example.
- `RX389` reports AC input current from byte 6 in 0.1 A units when a fresh charger frame is available.
- `zero_after_stop_observed=1` means a new `0x384` reported zero heater and compressor load after a stop frame was submitted. This is one feedback observation, so check it alongside the vehicle's actual behavior.
- `STALE_OR_MISSING` means there is no recent sample. Cached readings are not presented as current readings.
- `rx_missed`, `rx_overrun`, `tx_failed` and `bus_errors` help identify transport problems. Driver counters restart when switching CAN mode.

## Protocol tests

The pure C++ sequence can be tested without a board using a host C++ compiler:

```sh
g++ -std=c++11 -Wall -Wextra -Werror -Iinclude test/test_protocol.cpp -o /tmp/imiev-preheat-test
/tmp/imiev-preheat-test
```

The checks cover the complete 60-second HEAT request, DEFROST follow-up timing, cancellation, repeated STOP, rejected starts, failed local submission and 32-bit timer wraparound.

## Research and acknowledgements

Thanks to **Pascal / Colombo2b** for recording and sharing factory-remote CAN traffic, **iso14000** for helping with the capture, and **Tony, puffnfresh, MickeyS70** and the other forum contributors for the wiring and firmware discussion.

- [Factory-remote CAN captures shared by Colombo2b](https://myimiev.com/threads/retrofit-remote-control.6227/post-60048)
- [Owner's report of successful HEAT and DEFROST tests](https://myimiev.com/threads/retrofit-remote-control.6227/post-60074)
- [Later clarification of partial wake-up and unsuccessful heating without powered AC](https://myimiev.com/threads/retrofit-remote-control.6227/post-60096)
- [The ongoing retrofit discussion](https://myimiev.com/threads/retrofit-remote-control.6227/)
- [Related EV-ECU emulation work](https://github.com/puffnfresh/imiev-emulator/issues/1)
- [Espressif TWAI driver documentation](https://docs.espressif.com/projects/esp-idf/en/v4.4/esp32s3/api-reference/peripherals/twai.html)

When reporting results, include the vehicle year, EV-ECU software number, cable and station-power state, key position, selected mode, and timestamped serial output. Record the actual fan/heater response and the current feedback. Please remove VINs and other personal information from shared logs.
