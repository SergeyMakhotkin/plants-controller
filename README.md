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

## ️ Installation & Deployment

### 1. Secret Configuration

Create a `secrets.ini` file in the project root based on `secrets.ini.tmpl` and specify your WiFi and authentication
credentials.

### 2. Uploading Firmware

To upload the main firmware:

```bash

pio run --target upload
```

### 3. Uploading Data

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

---

##  Schedule Format (Cron)

`MINUTE HOUR DAY MONTH DAY_OF_WEEK`
Example: `0 12 * * *` — every day at 12:00.