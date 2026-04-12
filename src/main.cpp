#include <ESP8266WiFi.h>
#include <ESP8266WebServer.h>
#include <time.h>
#include <LittleFS.h>
#include <ArduinoJson.h>
#include <Wire.h>
#include <Adafruit_BMP280.h>

// ========== Configuration ==========
ADC_MODE(ADC_VCC); // for monitoring VCC

const char *ntpServer = "pool.ntp.org";
const int gmtOffset_sec = 3600 * 4;
const int daylightOffset_sec = 0;

const int webServerPort = 80;
bool authEnabled = false;
bool leakSensorEnabled = false;
bool bmpEnabled = false;

int relayMaxWorkTimeSec = 10;
const int LEAK_SENSOR_PIN = D8;

// --- Network Settings ---
IPAddress local_IP(IP_LOCAL);
IPAddress gateway(IP_GATEWAY);
IPAddress subnet(IP_SUBNET);
IPAddress primaryDNS(IP_DNS_PRIMARY);
IPAddress secondaryDNS(IP_DNS_SECONDARY);

Adafruit_BMP280 bmp;
bool bmpAvailable = false;

const unsigned long LEAK_CHECK_INTERVAL = 1000;
const unsigned long BMP_INTERVAL = 30000;

unsigned long lastSensorRead = 0;
unsigned long lastLeakCheck = 0;

float bmpTemperature = 0;
float bmpPressure = 0;
bool leakDetected = false;

// --- Data structures ---
struct Schedule {
    String startCron;
    String endCron;
    int duration = 0;
    bool useDuration = false;
};

const int MAX_SCHEDULES = 10;

struct Relay {
    int id;
    int pin;
    String name;
    bool isLimited;
    bool state;
    bool manualMode;
    bool prevScheduledState;
    time_t lastOnTime;
    Schedule schedules[MAX_SCHEDULES];
    int scheduleCount;
};

Relay r1 = {0, D5, "Light 1", false};
Relay r2 = {1, D6, "Light 2", false};
Relay r3 = {2, D7, "Pump", true};
Relay *relays[3] = {&r1, &r2, &r3};
constexpr int RELAY_COUNT = 3;

ESP8266WebServer server(webServerPort);

// Datetime
struct tm timeinfo;
time_t lastSyncTime = 0;

bool updateTime() {
    auto now = time(nullptr);
    if (now < 946684800) return false; // NTP not ready

    // seconds weren't changed
    if (now != lastSyncTime) {
        localtime_r(&now, &timeinfo);
        lastSyncTime = now;
    }
    return true;
}

String getTimestamp() {
    if (!updateTime()) return {};

    char buf[25];
    strftime(buf, sizeof(buf), "[%d.%m.%Y %H:%M:%S] ", &timeinfo);
    return {buf};
}

// --- Logging ---
const char *LOG_FILENAME = "/log.txt";

void logEvent(const String &message) {
    const String timestamp = getTimestamp();
    if (timestamp == "") {
        Serial.println("[Time not set] " + message);
        return;
    }

    const String logLine = timestamp + message;
    Serial.println(logLine);

    if (File f = LittleFS.open(LOG_FILENAME, "a")) {
        f.println(logLine);
        f.close();
    }
}

// --- Configuration processing ---
void saveRelayState() {
    JsonDocument doc;

    // 1. Пытаемся прочитать текущий файл
    File configFile = LittleFS.open("/config.json", "r");
    if (configFile) {
        DeserializationError error = deserializeJson(doc, configFile);
        configFile.close();
        if (error) {
            Serial.println("Error parsing config for relay state update");
            return;
        }
    }

    // 2. Получаем ссылку на массив "relays"
    // Если его нет, он создастся. Если есть — мы будем работать с ним.
    JsonArray relayArray = doc["relays"].as<JsonArray>();

    if (relayArray.isNull()) {
        // Если массива вообще нет в файле (первый запуск), создаем его
        relayArray = doc["relays"].to<JsonArray>();
    }

    // 3. Обновляем данные для каждого реле
    for (int i = 0; i < RELAY_COUNT; i++) {
        Relay *r = relays[i];
        bool found = false;

        // Ищем объект с соответствующим ID в существующем массиве
        for (JsonObject rObj: relayArray) {
            if (rObj["id"] == r->id) {
                rObj["manualMode"] = r->manualMode;
                rObj["state"] = r->state;
                rObj["lastOnTime"] = (uint32_t) r->lastOnTime;
                found = true;
                break;
            }
        }

        // Если реле с таким ID почему-то нет в файле, добавляем его
        if (!found) {
            JsonObject newRelay = relayArray.add<JsonObject>();
            newRelay["id"] = r->id;
            newRelay["manualMode"] = r->manualMode;
            newRelay["name"] = r->name;
            newRelay["isLimited"] = r->isLimited;
        }
    }

    // 4. Записываем обновленный документ обратно
    configFile = LittleFS.open("/config.json", "w");
    if (configFile) {
        serializeJson(doc, configFile);
        configFile.close();
        Serial.println("Relay states updated in config.json without data loss");
    }
}

void saveConfig() {
    JsonDocument doc;
    doc["pumpMax"] = relayMaxWorkTimeSec;
    doc["leakEnabled"] = leakSensorEnabled;
    doc["bmpEnabled"] = bmpEnabled;
    doc["authEnabled"] = authEnabled;

    JsonArray rArr = doc["relays"].to<JsonArray>();
    for (int i = 0; i < RELAY_COUNT; i++) {
        JsonObject rObj = rArr.add<JsonObject>();
        rObj["id"] = relays[i]->id;
        rObj["name"] = relays[i]->name;
        rObj["manualMode"] = relays[i]->manualMode;
        rObj["state"] = relays[i]->state;
        rObj["isLimited"] = relays[i]->isLimited;

        JsonArray sArr = rObj["schedules"].to<JsonArray>();
        for (int j = 0; j < relays[i]->scheduleCount; j++) {
            JsonObject sObj = sArr.add<JsonObject>();
            sObj["start"] = relays[i]->schedules[j].startCron;
            sObj["end"] = relays[i]->schedules[j].endCron;
            sObj["dur"] = relays[i]->schedules[j].duration;
            sObj["uDur"] = relays[i]->schedules[j].useDuration;
        }
    }
    File f = LittleFS.open("/config.json", "w");
    if (f) {
        serializeJson(doc, f);
        f.close();
    }
}

void loadConfig() {
    if (!LittleFS.exists("/config.json")) {
        Serial.println("Config file not found, using defaults");
        return;
    }
    File f = LittleFS.open("/config.json", "r");
    if (!f) {
        Serial.println("ERROR: Failed to open config file");
        return;
    }

    JsonDocument doc;
    DeserializationError error = deserializeJson(doc, f);
    if (!error) {
        relayMaxWorkTimeSec = doc["pumpMax"] | 30;
        leakSensorEnabled = doc["leakEnabled"] | false;
        bmpEnabled = doc["bmpEnabled"] | false;
        authEnabled = doc["authEnabled"] | false;

        JsonArray rArr = doc["relays"];
        for (int i = 0; i < RELAY_COUNT; i++) {
            JsonObject rObj = rArr[i]; // Получаем объект реле
            relays[i]->manualMode = rObj["manualMode"] | false;
            relays[i]->state = rObj["state"] | false;
            relays[i]->lastOnTime = rObj["lastOnTime"] | 0;
            relays[i]->isLimited = rObj["isLimited"] | (i == 2); // по умолчанию true только для насоса
            JsonArray sArr = rObj["schedules"];

            relays[i]->scheduleCount = 0;
            for (JsonObject sObj: sArr) {
                if (relays[i]->scheduleCount < MAX_SCHEDULES) {
                    Schedule &s = relays[i]->schedules[relays[i]->scheduleCount++];
                    s.startCron = sObj["start"] | "";
                    s.endCron = sObj["end"] | "";
                    s.duration = sObj["dur"];
                    s.useDuration = sObj["uDur"];
                }
            }
        }
        Serial.println("Config loaded successfully");
    } else {
        Serial.println("ERROR: Failed to parse config JSON");
    }
    f.close();
}

// --- Sensors ---
void readBMPData() {
    if (!bmpEnabled) return;
    float t = bmp.readTemperature();
    float p_pa = bmp.readPressure();
    if (!isnan(t) && !isnan(p_pa)) {
        bmpTemperature = t;
        bmpPressure = p_pa / 133.322;
    }
}

void checkLeakSensor() {
    if (!leakSensorEnabled) return;
    bool currentStatus = (digitalRead(LEAK_SENSOR_PIN) == LOW);
    if (currentStatus != leakDetected) {
        leakDetected = currentStatus;
        logEvent(leakDetected ? "ALARM: Leak detected!" : "SYSTEM: Leak cleared.");
    }
}

// --- Schedule logic ---
bool isCurrentTimeInSchedule(const Schedule &s) {
    if (!updateTime()) return false;

    // 1. Сначала проверяем дату (день, месяц, день недели)
    // Разбор Cron строки (мин час день мес день_нед)
    int firstSpace = s.startCron.indexOf(' ');
    int secondSpace = s.startCron.indexOf(' ', firstSpace + 1);
    int thirdSpace = s.startCron.indexOf(' ', secondSpace + 1);
    int fourthSpace = s.startCron.indexOf(' ', thirdSpace + 1);

    String minStr = s.startCron.substring(0, firstSpace);
    String hourStr = s.startCron.substring(firstSpace + 1, secondSpace);
    String dayStr = s.startCron.substring(secondSpace + 1, thirdSpace);
    String monthStr = s.startCron.substring(thirdSpace + 1, fourthSpace);
    String dowStr = s.startCron.substring(fourthSpace + 1);

    // Проверка дня месяца
    if (dayStr != "*") {
        if (dayStr.startsWith("*/")) {
            int interval = dayStr.substring(2).toInt();
            if (interval > 0 && (timeinfo.tm_mday % interval) != 0) return false;
        } else if (dayStr.toInt() != timeinfo.tm_mday) return false;
    }
    // Проверка месяца
    if (monthStr != "*") {
        if (monthStr.startsWith("*/")) {
            int interval = monthStr.substring(2).toInt();
            if (interval > 0 && ((timeinfo.tm_mon + 1) % interval) != 0) return false;
        } else if (monthStr.toInt() != (timeinfo.tm_mon + 1)) return false;
    }
    // Проверка дня недели
    if (dowStr != "*") {
        int targetDow = (dowStr.toInt() == 7) ? 0 : dowStr.toInt();
        if (targetDow != timeinfo.tm_wday) return false;
    }

    // 2. Проверка времени (Часы и Минуты)
    long curTotalSec = (long)timeinfo.tm_hour * 3600 + (long)timeinfo.tm_min * 60 + timeinfo.tm_sec;
    int startM = (minStr == "*") ? 0 : minStr.toInt();
    int startH = (hourStr == "*") ? 0 : hourStr.toInt();
    long startTotalSec = (long)startH * 3600 + (long)startM * 60;

    if (s.useDuration) {
        // Если используем длительность: должна совпадать минута начала
        bool timeMatch = (hourStr == "*" || startH == timeinfo.tm_hour) &&
                         (minStr == "*" || startM == timeinfo.tm_min);
        return timeMatch && (timeinfo.tm_sec < s.duration);
    } else {
        // Если используем время конца: проверяем вхождение в диапазон
        int eFirstSpace = s.endCron.indexOf(' ');
        int eSecondSpace = s.endCron.indexOf(' ', eFirstSpace + 1);
        int endM = s.endCron.substring(0, eFirstSpace).toInt();
        int endH = s.endCron.substring(eFirstSpace + 1, eSecondSpace).toInt();
        long endTotalSec = (long)endH * 3600 + (long)endM * 60 + 59;

        if (endTotalSec < startTotalSec) { // Переход через полночь
            return (curTotalSec >= startTotalSec || curTotalSec <= endTotalSec);
        }
        return (curTotalSec >= startTotalSec && curTotalSec <= endTotalSec);
    }
}

void updateRelaysLogic() {
    if (!updateTime()) return;
    time_t now = time(nullptr);

    for (int i = 0; i < RELAY_COUNT; i++) {
        Relay *r = relays[i];
        bool anyScheduleActive = false;

        // check all schedule slots
        for (int j = 0; j < r->scheduleCount; j++) {
            if (isCurrentTimeInSchedule(r->schedules[j])) {
                anyScheduleActive = true;
                break;
            }
        }

        // resetting manualMode at the START of a new slot
        if (anyScheduleActive && !r->prevScheduledState) {
            if (r->manualMode) {
                r->manualMode = false;
                logEvent("AUTO: " + r->name + " manual mode reset by schedule start");
                saveRelayState(); // Сохраняем изменение
            }
        }
        r->prevScheduledState = anyScheduleActive;

        // determining relay state
        bool targetState = r->manualMode ? r->state : anyScheduleActive;

        // check the protective time interval (isLimited)
        if (targetState && r->isLimited && r->lastOnTime > 0 && now > 946684800) {
            if ((now - r->lastOnTime) > relayMaxWorkTimeSec) {
                targetState = false;
                r->manualMode = false; // Возврат в авто
                saveRelayState();
                logEvent("SAFETY: " + r->name + " timeout. Manual mode reset.");
            }
        }

        // 4. ЕДИНСТВЕННАЯ ТОЧКА УПРАВЛЕНИЯ И ЗАПИСИ ВРЕМЕНИ
        // Проверяем, отличается ли желаемое (target) от текущего (digitalRead)
        bool currentPhysicalState = (digitalRead(r->pin) == LOW); // LOW = ON

        if (targetState != currentPhysicalState) {
            if (targetState && !currentPhysicalState) {
                if (r->lastOnTime == 0) {
                    r->lastOnTime = now;
                    saveRelayState();
                }
            }

            if (!targetState && currentPhysicalState) {
                r->lastOnTime = 0;
                saveRelayState();
            }

            // apply the state if it has changed
            digitalWrite(r->pin, targetState ? LOW : HIGH);
            r->state = targetState;
            logEvent(r->name + (r->state ? " ON" : " OFF"));
        }
    }
}

// --- WEB Handlers ---
bool isValidPart(String part, int minVal, int maxVal) {
    part.trim();
    if (part == "*") return true;

    // Проверка формата */N
    if (part.startsWith("*/")) {
        int v = part.substring(2).toInt();
        return (v > 0 && v <= maxVal);
    }

    // Проверка конкретного числа N
    // toInt() вернет 0, если в строке буквы. Проверим, что это реально число.
    for (char c: part) if (!isDigit(c)) return false;

    int v = part.toInt();
    return (v >= minVal && v <= maxVal);
}

bool isValidCron(String str) {
    str.trim();
    // Разбиваем строку по пробелам
    int partsFound = 0;
    String parts[5];

    int lastSpace = -1;
    for (int i = 0; i < 5; i++) {
        int nextSpace = str.indexOf(' ', lastSpace + 1);
        if (i < 4 && nextSpace == -1) return false; // Нужно 5 частей

        if (i == 4) parts[i] = str.substring(lastSpace + 1);
        else parts[i] = str.substring(lastSpace + 1, nextSpace);

        parts[i].trim();
        if (parts[i].length() == 0) return false;

        lastSpace = nextSpace;
        partsFound++;
    }

    if (partsFound != 5) return false;

    // Валидация каждого поля согласно логике isCurrentTimeInSchedule
    if (!isValidPart(parts[0], 0, 59)) return false; // Минуты
    if (!isValidPart(parts[1], 0, 23)) return false; // Часы
    if (!isValidPart(parts[2], 1, 31)) return false; // Дни
    if (!isValidPart(parts[3], 1, 12)) return false; // Месяцы
    if (!isValidPart(parts[4], 0, 7)) return false; // День недели (0-7)

    return true;
}

String getSchedulesTable(int rid) {
    String html = "";
    for (int i = 0; i < relays[rid]->scheduleCount; i++) {
        Schedule &s = relays[rid]->schedules[i];
        // Делаем время начала ссылкой для редактирования
        html += "<tr><td>" + String(i + 1) + "</td>";
        html += "<td><a href='/edit?rid=" + String(rid) + "&id=" + String(i) + "'><code>" + s.startCron +
                "</code></a></td>";
        html += "<td>";
        html += s.useDuration ? (String(s.duration) + "s") : ("<code>" + s.endCron + "</code>");
        html += "</td><td><a href='/del?rid=" + String(rid) + "&id=" + String(i) +
                "' style='color:red;'>[X]</a></td></tr>";
    }
    return html.length() > 0 ? html : "<tr><td colspan='4'>No schedules</td></tr>";
}

enum class RelayActionType {
    SCHEDULED,
    WEB,
    BUTTON,
};

String getRelayActionTypeName(RelayActionType type) {
    switch (type) {
        case RelayActionType::SCHEDULED: return "scheduled";
        case RelayActionType::WEB: return "web ui";
        // case RelayActionType::BUTTON:        return "button";
        default: return "unknown";
    }
}

void handleRelayAJAX(bool targetState) {
    if (!server.hasArg("rid")) {
        server.send(400, "application/json", "{\"status\":\"ERROR\",\"message\":\"Missing rid\"}");
        return;
    }

    int rid = server.arg("rid").toInt();
    if (rid < 0 || rid >= 3) {
        server.send(400, "application/json", "{\"status\":\"ERROR\",\"message\":\"Invalid rid\"}");
        return;
    }
    Relay *r = relays[rid];
    r->manualMode = !r->manualMode;

    if (r->manualMode) {
        r->state = targetState;
        logEvent("MANUAL: " + r->name + " set to " + String(targetState ? "ON" : "OFF"));
    } else {
        logEvent("Relay " + r->name + " switched to AUTO mode");
    }

    saveRelayState();

    String json = "{";
    json += "\"id\":" + String(r->id) + ",";
    json += "\"state\":" + String(r->state ? "true" : "false") + ",";
    json += "\"manual\":" + String(r->manualMode ? "true" : "false");
    json += "}";
    server.send(200, "application/json", json);
}

bool checkAuth() {
    if (!authEnabled) return true;
    if (!server.authenticate(WEB_USER, WEB_PASS)) {
        server.requestAuthentication();
        return false;
    }
    return true;
}

//web server handlers
void webHandleRoot() {
    if (!checkAuth()) return;
    File f = LittleFS.open("/index.html", "r");
    String htmlStr = f.readString();
    f.close();

    htmlStr.replace("%TIME%", getTimestamp());
    htmlStr.replace("%IP%", WiFi.localIP().toString());
    htmlStr.replace("%VERSION%", VERSION);

    unsigned long sec = millis() / 1000;
    char uptimeBuf[20];
    snprintf(uptimeBuf, sizeof(uptimeBuf), "%lud %02lu:%02lu:%02lu", sec / 86400, (sec % 86400) / 3600,
             (sec % 3600) / 60, sec % 60);
    htmlStr.replace("%UPTIME%", String(uptimeBuf));
    htmlStr.replace("%VCC%", String(ESP.getVcc() / 1024.0, 2) + "V");
    htmlStr.replace("%HEAP%", String(ESP.getFreeHeap()));
    htmlStr.replace("%RESET_REASON%", ESP.getResetReason());

    // 2. Информация с датчиков
    String bmpStr = "Sensor disabled";
    if (bmpEnabled) {
        if (bmpAvailable) {
            bmpStr = "🌡️ Temp: " + String(bmpTemperature, 1) + "°C | 🎈 Pres: " + String(bmpPressure, 1) + " mmHg";
        } else {
            bmpStr = "<span style='color:red;'>Sensor error</span>";
        }
    }
    htmlStr.replace("%BMP_INFO%", bmpStr);

    String leakStr = leakSensorEnabled
                         ? (leakDetected
                                ? "<span class='status-alarm'>ALARM: LEAK!</span>"
                                : "<span class='status-ok'>Dry</span>")
                         : "Disabled";
    htmlStr.replace("%LEAK_INFO%", "💧 Состояние: " + leakStr);

    // 3. Управление реле
    for (int i = 0; i < RELAY_COUNT; i++) {
        String rid = String(i);
        htmlStr.replace("%RELAY_NAME_" + rid + "%", relays[i]->name);
        htmlStr.replace("%RELAY_STATE_" + rid + "%", relays[i]->state ? "On" : "Off");

        // Режим (Manual/Auto)
        String modeHtml = relays[i]->manualMode
                              ? "<span class='mode-label mode-manual'>Manual</span>"
                              : "<span class='mode-label mode-auto'>Auto</span>";
        htmlStr.replace("%RELAY_MODE_" + rid + "%", modeHtml);

        // Настройка кнопки (динамическое состояние при загрузке)
        htmlStr.replace("%BTN_CLASS_" + rid + "%", relays[i]->state ? "off" : "on");
        htmlStr.replace("%BTN_TEXT_" + rid + "%", relays[i]->state ? "ВЫКЛЮЧИТЬ" : "ВКЛЮЧИТЬ");
        htmlStr.replace("%BTN_ACTION_" + rid + "%", relays[i]->state ? "off" : "on");

        htmlStr.replace("%SCHEDULES_" + rid + "%", getSchedulesTable(i));
    }
    server.send(200, "text/html", htmlStr);
}

void webHandleRelayOn() {
    if (checkAuth()) {
        handleRelayAJAX(true);
    } else {
        logEvent("WARNING: unauthorized ajax request for 'on' action");
    }
}

void webHandleRelayOff() {
    if (checkAuth()) {
        handleRelayAJAX(false);
    } else {
        logEvent("WARNING: unauthorized ajax request for 'on' action");
    }
}

void webHandleLogs() {
    if (!checkAuth()) return;

    File tmpl = LittleFS.open("/logs.html", "r");
    if (!tmpl) {
        server.send(404, "text/plain", "Template not found");
        return;
    }
    String html = tmpl.readString();
    tmpl.close();

    File logFile = LittleFS.open(LOG_FILENAME, "r");
    String logData = "";
    if (logFile) {
        while (logFile.available()) {
            String line = logFile.readStringUntil('\n');
            line.trim();
            if (line.length() > 0) {
                // Reverse log messages
                logData = line + "\n" + logData;
            }
        }
        logFile.close();
    } else {
        logData = "Log file is empty or not found.";
    }

    html.replace("%LOG_CONTENT%", logData);
    server.send(200, "text/html", html);
}

void webHandleSettings() {
    if (!checkAuth()) return;

    // Если запрос содержит параметры — сохраняем их
    if (server.hasArg("pumpMax") || server.hasArg("leakEn") || server.hasArg("bmpEn") || server.hasArg("auth")) {
        int oldPumpMax = relayMaxWorkTimeSec;
        bool oldLeakEnabled = leakSensorEnabled;
        bool oldBmpEnabled = bmpEnabled;
        bool oldAuthEnabled = authEnabled;

        relayMaxWorkTimeSec = server.arg("pumpMax").toInt();
        leakSensorEnabled = server.hasArg("leakEn");
        bmpEnabled = server.hasArg("bmpEn");
        authEnabled = server.hasArg("auth");
        saveConfig();

        String logMsg = "Settings updated: ";
        if (oldPumpMax != relayMaxWorkTimeSec) {
            logMsg += "pumpMax=" + String(relayMaxWorkTimeSec) + "s ";
        }
        if (oldLeakEnabled != leakSensorEnabled) {
            logMsg += "leak=" + String(leakSensorEnabled ? "ON" : "OFF") + " ";
        }
        if (oldBmpEnabled != bmpEnabled) {
            logMsg += "bmp=" + String(bmpEnabled ? "ON" : "OFF") + " ";
        }
        if (oldAuthEnabled != authEnabled) {
            logMsg += "auth=" + String(authEnabled ? "ON" : "OFF");
        }
        logEvent(logMsg);

        // Перенаправляем на главную после сохранения, чтобы не видеть "Settings Updated"
        server.sendHeader("Location", "/");
        server.send(303);
        return;
    }
    // Если параметров нет — просто показываем страницу настроек
    File f = LittleFS.open("/settings.html", "r");
    if (!f) {
        server.send(404, "text/plain", "Settings template not found");
        logEvent("ERROR: settings.html not found");
        return;
    }
    String html = f.readString();
    f.close();

    // Заполняем плейсхолдеры в шаблоне
    html.replace("%PUMP_MAX%", String(relayMaxWorkTimeSec));
    html.replace("%LEAK_CHECKED%", leakSensorEnabled ? "checked" : "");
    html.replace("%BMP_CHECKED%", bmpEnabled ? "checked" : "");
    html.replace("%AUTH_CHECKED%", authEnabled ? "checked" : "");

    server.send(200, "text/html", html);
}

void webHandleStyle() {
    if (!checkAuth()) return;
    if (LittleFS.exists("/style.css")) {
        File f = LittleFS.open("/style.css", "r");
        server.streamFile(f, "text/css; charset=utf-8");
        f.close();
    } else {
        server.send(404, "text/plain", "CSS Not Found");
    }
}

void webHandleScheduleAdd() {
    if (!checkAuth()) return;

    String rid = server.arg("rid");
    File f = LittleFS.open("/add.html", "r");
    if (!f) {
        server.send(404, "text/plain", "Error: add.html not found in LittleFS");
        return;
    }

    String html = f.readString();
    f.close();

    html.replace("%RID%", rid);
    server.send(200, "text/html", html);
}

void webHandleScheduleEdit() {
    if (!checkAuth()) return;

    int rid = server.arg("rid").toInt();
    int id = server.arg("id").toInt();

    if (rid < 0 || rid >= RELAY_COUNT || id < 0 || id >= relays[rid]->scheduleCount) {
        server.send(404, "text/plain", "Schedule not found");
        return;
    }

    File f = LittleFS.open("/edit.html", "r");
    if (!f) {
        server.send(404, "text/plain", "edit.html not found");
        return;
    }

    String html = f.readString();
    f.close();

    Schedule &s = relays[rid]->schedules[id];

    html.replace("%RID%", String(rid));
    html.replace("%ID%", String(id));
    html.replace("%START%", s.startCron);
    html.replace("%END%", s.useDuration ? "" : s.endCron);
    html.replace("%DURATION%", s.useDuration ? String(s.duration) : "");

    server.send(200, "text/html", html);
}

void webHandleScheduleSave() {
    if (!checkAuth()) return;

    if (!server.hasArg("rid")) {
        server.send(400, "text/plain", "Missing rid");
        return;
    }

    int rid = server.arg("rid").toInt();
    if (rid < 0 || rid >= RELAY_COUNT) return;

    String start = server.arg("start");
    String end = server.arg("end");
    String durationRaw = server.arg("duration");

    if (!isValidCron(start)) {
        server.send(400, "text/plain", "Invalid Start Cron format. Need 5 parts.");
        return;
    }

    if (durationRaw.length() == 0 && !isValidCron(end)) {
        server.send(400, "text/plain", "Invalid End Cron format or Duration missing.");
        return;
    }

    int id = -1;
    if (server.hasArg("id") && server.arg("id") != "") {
        id = server.arg("id").toInt();
    }

    Relay *r = relays[rid];
    Schedule *s = nullptr;

    if (id >= 0 && id < r->scheduleCount) {
        // Schedule editing
        s = &r->schedules[id];
        logEvent("Schedule UPDATED for " + r->name + " [ID:" + String(id) + "]");
    } else if (r->scheduleCount < MAX_SCHEDULES) {
        // Schedule adding
        s = &r->schedules[r->scheduleCount++];
        logEvent("Schedule ADDED for " + r->name);
    } else {
        server.send(400, "text/plain", "Max schedules reached");
        return;
    }

    s->startCron = server.arg("start");

    if (durationRaw.length() > 0) {
        s->duration = durationRaw.toInt();
        s->useDuration = true;
        s->endCron = "";
    } else {
        s->endCron = end;
        s->useDuration = false;
        s->duration = 0;
    }

    saveConfig();

    server.sendHeader("Location", "/");
    server.send(303);
}

void webHandleScheduleDel() {
    if (!checkAuth()) return;
    int rid = server.arg("rid").toInt();
    int id = server.arg("id").toInt();
    if (rid >= 0 && rid < 3 && id < relays[rid]->scheduleCount) {
        for (int i = id; i < relays[rid]->scheduleCount - 1; i++)
            relays[rid]->schedules[i] = relays[rid]->schedules[i + 1];
        relays[rid]->scheduleCount--;
        saveConfig();
    }
    server.sendHeader("Location", "/");
    server.send(303);
}

// --- Setup и Loop ---
void setup() {
    Serial.begin(115200);
    delay(100);
    Serial.println("\n=== System Booting ===");
    Serial.println("Firmware: " + String(VERSION));


    if (!LittleFS.begin()) {
        Serial.println("LittleFS mount failed!");
        return;
    } else {
        Serial.println("LittleFS mounted successfully.");
    }

    loadConfig();

    for (int i = 0; i < 3; i++) {
        pinMode(relays[i]->pin, OUTPUT);
        digitalWrite(relays[i]->pin, HIGH);
    }
    pinMode(LEAK_SENSOR_PIN, INPUT_PULLUP);

    // Init BMP180
    Wire.begin();
    if (bmp.begin()) {
        bmpAvailable = true;
        Serial.println("BMP180 initialized");
    } else {
        Serial.println("BMP180 not found");
    }

    WiFi.mode(WIFI_STA);
    WiFi.config(local_IP, gateway, subnet, primaryDNS, secondaryDNS);
    WiFi.begin(WIFI_SSID, WIFI_PASS);
    Serial.print("Connecting to WiFi");
    int attempts = 0;
    while (WiFi.status() != WL_CONNECTED && attempts < 30) {
        delay(500);
        Serial.print(".");
        attempts++;
        yield();
    }

    if (WiFi.status() == WL_CONNECTED) {
        Serial.println("\nWiFi Connected. IP: " + WiFi.localIP().toString());
        configTime(gmtOffset_sec, daylightOffset_sec, ntpServer);
        Serial.print("Waiting for NTP time sync");
        int timeRetry = 0;
        while (time(nullptr) < 946684800 && timeRetry < 20) {
            delay(500);
            Serial.print(".");
            timeRetry++;
        }
        Serial.println("\n\nNTP client initialized");
        logEvent("SYSTEM: Boot. Reason: " + ESP.getResetReason() + " | FW=" + String(VERSION));
    } else {
        Serial.println("\nWiFi connection failed!");
    }

    // web server configuration
    server.on("/", webHandleRoot);
    server.on("/on", webHandleRelayOn);
    server.on("/off", webHandleRelayOff);
    server.on("/logs", webHandleLogs);
    server.on("/settings", webHandleSettings);
    server.on("/add", webHandleScheduleAdd);
    server.on("/edit", webHandleScheduleEdit);
    server.on("/save", HTTP_POST, webHandleScheduleSave);
    server.on("/del", webHandleScheduleDel);
    server.on("/style.css", webHandleStyle);

    server.begin();
    Serial.println("HTTP server started");
    logEvent("SYSTEM: HTTP server started");

    logEvent("SYSTEM: Boot complete, FW=" + String(VERSION));
}

void loop() {
    server.handleClient();
    updateRelaysLogic();

    unsigned long currentMillis = millis();
    // TODO move to checkSensors func
    // MH-RD
    if (leakSensorEnabled && (currentMillis - lastLeakCheck >= LEAK_CHECK_INTERVAL)) {
        lastLeakCheck = currentMillis;
        checkLeakSensor();
    }
    // BMP280
    if (bmpEnabled && (currentMillis - lastSensorRead >= BMP_INTERVAL)) {
        lastSensorRead = currentMillis;
        readBMPData();
    }
}
