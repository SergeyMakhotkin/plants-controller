#  Plants Controller

An intelligent irrigation and lighting control system based on NodeMCU (ESP8266).

## Key Features

- **3-Channel Control**: 2 channels for lighting, 1 for the pump (with protection).
- **Advanced Scheduling**: Support for Cron-masks with second-level precision.
- **Smart Manual Mode**: Manual control that works harmoniously with automation.
- **Safety Engine**: Automatic shutdown when the work time limit is exceeded.
- **Sensors**: Water leak monitoring (D8) and climate tracking (BMP280).

---

##  Operational Logic

The system operates in two modes: **Auto** (scheduled) and **Manual** (user-driven).

### 1. Manual Control (Web UI)

- **Turn On/Off**: When a button is pressed in the UI, the relay enters **Manual** mode. In this state, the automated
  schedule is ignored.
- **Return to Auto**: If you press "Turn On" while the relay is already manually "ON", it will **return to Auto mode**.
  It won't turn off, but it will now wait for the next command from the schedule.

### 2. Automatic Reset

- **Slot Start**: When a scheduled "ON" time arrives, the relay **always** resets from Manual to **Auto** mode. This
  ensures that if you forget to turn off the lights manually, the system regains control.

### 3. Safety Timeout

- For relays with the `isLimited` flag (e.g., the Pump), the `relayMaxWorkTimeSec` timer is active.
- If the relay runs longer than the defined limit, it is **forcibly turned off**, and the Manual mode is reset. This
  protects against flooding in case of a sensor failure or scheduling error.

---

## Connectivity & Resilience

Designed to recover cleanly from a power outage, where the router/ISP may take a while to come back:

- **Booting**: relays stay OFF until a schedule can actually be evaluated. `WiFi.begin()` is retried every 2 minutes
  while waiting for the router; once linked, NTP sync is retried every 20 seconds. A single 10-minute budget covers
  both — if no time has been obtained by then, the controller reboots itself rather than risk staying stuck.
- **Once running**: that reboot safety net is retired for good. If WiFi drops, reconnecting is retried every 30
  seconds in the background, but relays keep following the schedule off the local clock the whole time — no reboot,
  no interruption. If the link stays up but the internet/NTP server doesn't, the clock is force-resynced every hour.

**Status LED** (onboard LED, D4) shows the current state at a glance, no serial monitor needed. Each state blinks
its own count of short pulses once a second (e.g. 2 short blinks, pause, repeat), so it doubles as a status code:

| What you see                              | Meaning                                  |
|---------------------------------------------|-------------------------------------------|
| 1 short blink/sec                            | Booting, waiting for WiFi                  |
| 2 short blinks/sec                           | WiFi connected, waiting for time sync      |
| Solid on                                     | Running normally                           |
| 3 short blinks/sec                           | WiFi/internet lost — still running schedules off the local clock |
| Slow, even blink (~0.5 Hz, no pulse-counting)| WiFi setup AP mode                         |

---

## WiFi Setup Without Reflashing

WiFi credentials and static IP normally come from `secrets.ini` at compile time, but they can also be changed live,
without touching a USB cable:

1. Power on the controller (or reset it), then within the first ~3 seconds, press and hold the onboard **FLASH**
   button for about a second. (Don't hold it *while* powering on — GPIO0 has to be free at the exact moment of
   reset, or the chip enters its own bootloader instead of your firmware.)
2. The controller raises its own open WiFi network, `PlantsController-Setup`. Connect to it from a phone or laptop.
3. Open `http://192.168.4.1` in a browser — a form to set SSID, password, and static IP (address/gateway/subnet/DNS)
   appears, pre-filled with whatever is currently active.
4. Submit it. The controller saves the settings to LittleFS and reboots into normal mode, connecting to the new
   network.

If it's never been configured this way, the compiled `secrets.ini` values are used exactly as before — this is
purely additive. This feature needs the new `data/wifi_setup.html` template on the device, so deploying it requires
both `pio run --target upload` **and** `pio run --target uploadfs`.

---

## ️ Installation & Deployment

### 1. Secret Configuration

Create a `secrets.ini` file in the project root based on `secrets.ini.tmpl` and specify your WiFi and authentication
credentials.

### 2. Finding the Device

`upload_port` in `platformio.ini` is hardcoded (`/dev/cu.usbserial-A5069RR4`). To confirm the controller is connected
and check its port, or to find the right value after swapping cables/boards:

```bash
pio device list
```

### 3. Uploading Firmware

To upload the main firmware:

```bash

pio run --target upload
```

### 4. Uploading Data

To make the web interface functional, you must upload the files from the `data` folder to LittleFS:

```bash
pio run --target uploadfs
```

---

##  Hardware Pinout

| Component   | NodeMCU Pin | Function                   |
|-------------|-------------|----------------------------|
| Relay 1     | D5          | Light 1                    |
| Relay 2     | D6          | Light 2                    |
| Relay 3     | D7          | Pump (`isLimited: true`)   |
| I2C SDA     | D2          | BMP280 Data                |
| I2C SCL     | D1          | BMP280 Clock               |
| Leak Sensor | D8          | Digital Input (Active LOW) |
| Setup Button | D3 (GPIO0) | Onboard FLASH button — hold to enter WiFi setup mode |

---

##  Schedule Format (Cron)

`MINUTE HOUR DAY MONTH DAY_OF_WEEK`
Example: `0 12 * * *` — every day at 12:00.

---

## Testing

```bash
pio run                 # compile the firmware (nodemcuv2)
pio test -e native      # host-side unit tests for the WiFi/NTP connectivity logic, no hardware needed
pio test -e nodemcuv2   # on-device unit tests for cron/schedule parsing, needs the controller connected
```

There are two separate test suites, split by what they need:

- **`test/test_connectivity_state`** (`-e native`) — the WiFi/NTP state machine (`src/connectivity_state.h`) is
  deliberately hardware-free, so this runs on your dev machine, no controller required.
- **`test/test_schedule_logic`** (`-e nodemcuv2`) — cron parsing and schedule matching (`src/schedule_logic.h`) use
  Arduino's `String`, which only exists with the real framework, so this builds, uploads, and runs on the actual
  board, reporting results back over serial.

`test_filter` in `platformio.ini` keeps each suite scoped to its own environment — `pio test` without `-e` would
otherwise try (and fail) to build both under both environments.

**`pio test -e nodemcuv2` overwrites the firmware on the controller with the test binary** (its `loop()` does
nothing — no WiFi, no relays, no schedules while it's flashed). Run `pio run --target upload` afterwards to put the
real firmware back.