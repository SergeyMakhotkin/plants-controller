## Plants Controller 

An automated system for managing plant lighting and irrigation based on NodeMCU (ESP8266). 

##  Key Features 

-  **3-Channel Control**: 2 channels for lighting, 1 for the pump. 
-  **Intelligent Scheduling**: Support for Cron-like masks with second-level precision and duration-based timers. 
-  **Safety Engine**: Hard-coded pump runtime limit to prevent over-watering or flooding. 
-  **Sensor Support**: Integration with BMP280 (Temperature/Pressure) and Digital Water Leak Sensor. 
-  **Web Interface**: Full control via local network with authentication support. 

##  Hardware Pinout 

| Component     | NodeMCU Pin | Function                   |
|---------------|-------------|----------------------------|
| Relay 1       | D5          | Light 1                    |
| Relay 2       | D6          | Light 2                    |
| Relay 3       | D7          | Pump                       |
| I2C SDA       | D2          | BMP280 Data                |
| I2C SCL       | D1          | BMP280 Clock               |
| Leak Sensor   | D8          | Digital Input (Active LOW) |

##  Installation & Deployment (PlatformIO) 

###  1. Configuration (Secrets) 
 For security reasons, sensitive information (WiFi credentials, IP settings, etc.) is stored in a separate file. 

1.  Locate the `secrets.ini.tmpl` file in the project root. 
2.  Create a copy of it named `secrets.ini`. 
3.  Edit `secrets.ini` and replace the placeholder values with your actual network and authorization settings. 

>  **Note:** The `secrets.ini` file is ignored by Git to keep your credentials private. 

###  2. Uploading Data & Firmware 
 To upload the web interface files (HTML/CSS/Config) to the LittleFS partition: 
```bash
pio run --target uploadfs
```
To upload the main firmware:
```bash

pio run --target upload
```
Terminal Logs & Debugging

Use the Serial Monitor at 115200 baud to monitor system status:

    WiFi Connected. IP: 192.168.1.10: Successfully joined the network.

    [HH:MM:SS] Light 1 ON (Auto): Automated schedule trigger.

    [HH:MM:SS] Pump STARTED: Pump activated via schedule or web UI.

    SAFETY: Pump Timeout!: Critical alert. The pump was shut down for exceeding the safety limit.

    ALARM: Leak detected!: Water detected by the leak sensor on D8. 

Operational Logic

Manual vs. Auto Mode

    Auto Mode: Relays follow the schedules defined in the web interface.

    Manual Mode: Triggered via the Web UI.

        Lights: Automation is ignored until the next manual toggle.

        Pump: Automation is ignored, but the Safety Timeout remains active to prevent flooding. 

Safety Lock

If the pump exceeds pumpMaxOnTimeSec, it enters a Safety Lock state. The pump will remain disabled until it receives a manual "OFF" command from the UI or the current active schedule window expires.