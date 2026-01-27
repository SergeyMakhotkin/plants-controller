#include <ESP8266WiFi.h>
#include <ESP8266WebServer.h>
#include <WiFiUdp.h>
#include <NTPClient.h>
#include <LittleFS.h>
#include <ArduinoJson.h>
#include <Wire.h>
#include <Adafruit_BMP280.h>

// ========== Configuration ==========
ADC_MODE(ADC_VCC); // for monitoring VCC
const char* ntpServer = "pool.ntp.org";
const int ntpInterval = 3600 * 4;
const int webServerPort = 80;
bool authEnabled = false;
bool leakSensorEnabled = false;
bool bmpEnabled = false;

int pumpMaxOnTimeSec = 30;
const int LEAK_SENSOR_PIN = D8;

// --- Network Settings ---
IPAddress local_IP(IP_LOCAL);
IPAddress gateway(IP_GATEWAY);
IPAddress subnet(IP_SUBNET);
IPAddress primaryDNS(IP_DNS_PRIMARY);
IPAddress secondaryDNS(IP_DNS_SECONDARY);

Adafruit_BMP280 bmp;
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
    bool state;
    bool manualMode;
    bool safetyLock;
    Schedule schedules[MAX_SCHEDULES];
    int currentSchedules = 0;
    unsigned long lastOnTime = 0;
};

Relay r1 = {0, D5, "Lamp 1", false, false, false};
Relay r2 = {1, D6, "Lamp 2", false, false, false};
Relay r3 = {2, D7, "Pump", false, false, false};
Relay* relays[3] = {&r1, &r2, &r3};

WiFiUDP ntpUDP;
NTPClient timeClient(ntpUDP, ntpServer, ntpInterval);
ESP8266WebServer server(webServerPort);

// --- Logging ---
const char* LOG_FILENAME = "/log.txt";

void logEvent(const String& message) {
    if (!timeClient.isTimeSet()) return;
    String logLine = "[" + timeClient.getFormattedTime() + "] " + message;
    Serial.println(logLine);

    File f = LittleFS.open(LOG_FILENAME, "a");
    if (f) { f.println(logLine); f.close(); }
}

// --- Configuration processing ---
void saveConfig() {
    StaticJsonDocument<2048> doc;
    doc["pumpMax"] = pumpMaxOnTimeSec;
    doc["leakEnabled"] = leakSensorEnabled;
    doc["bmpEnabled"] = bmpEnabled;
    doc["authEnabled"] = authEnabled;

    JsonArray rArr = doc.createNestedArray("relays");
    for (int i = 0; i < 3; i++) {
        JsonObject rObj = rArr.createNestedObject();
        JsonArray sArr = rObj.createNestedArray("schedules");
        for (int j = 0; j < relays[i]->currentSchedules; j++) {
            JsonObject sObj = sArr.createNestedObject();
            sObj["start"] = relays[i]->schedules[j].startCron;
            sObj["end"] = relays[i]->schedules[j].endCron;
            sObj["dur"] = relays[i]->schedules[j].duration;
            sObj["uDur"] = relays[i]->schedules[j].useDuration;
        }
    }
    File f = LittleFS.open("/config.json", "w");
    if (f) { serializeJson(doc, f); f.close(); }
}

void loadConfig() {
    if (!LittleFS.exists("/config.json")) return;
    File f = LittleFS.open("/config.json", "r");
    StaticJsonDocument<2048> doc;
    DeserializationError error = deserializeJson(doc, f);
    if (!error) {
        pumpMaxOnTimeSec = doc["pumpMax"] | 30;
        leakSensorEnabled = doc["leakEnabled"] | false;
        bmpEnabled = doc["bmpEnabled"] | false;
        authEnabled = doc["authEnabled"] | false;

        JsonArray rArr = doc["relays"];
        for (int i = 0; i < 3; i++) {
            JsonArray sArr = rArr[i]["schedules"];
            relays[i]->currentSchedules = 0;
            for (JsonObject sObj : sArr) {
                if (relays[i]->currentSchedules < MAX_SCHEDULES) {
                    Schedule& s = relays[i]->schedules[relays[i]->currentSchedules++];
                    s.startCron = sObj["start"].as<String>();
                    s.endCron = sObj["end"].as<String>();
                    s.duration = sObj["dur"];
                    s.useDuration = sObj["uDur"];
                }
            }
        }
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
bool isCurrentTimeInSchedule(const Schedule& s) {
    if (!timeClient.isTimeSet()) return false;

    long curH = timeClient.getHours();
    long curM = timeClient.getMinutes();
    long curS = timeClient.getSeconds();
    long curTotalSec = curH * 3600 + curM * 60 + curS;

    auto parsePart = [](String str, int partIdx) {
        int start = 0, end = str.indexOf(' ');
        for (int i = 0; i < partIdx; i++) { start = end + 1; end = str.indexOf(' ', start); }
        String res = (end == -1) ? str.substring(start) : str.substring(start, end);
        res.trim();
        return res;
    };

    String sS = parsePart(s.startCron, 0);
    String sM = parsePart(s.startCron, 1);
    String sH = parsePart(s.startCron, 2);

    long startH = (sH == "*" || sH == "") ? curH : sH.toInt();
    long startM = (sM == "*" || sM == "") ? curM : sM.toInt();
    long startS = (sS == "*" || sS == "") ? 0 : sS.toInt();

    long startTotalSec = startH * 3600 + startM * 60 + startS;
    long endTotalSec = 0;

    if (s.useDuration) {
        endTotalSec = startTotalSec + s.duration;
    } else {
        String eS = parsePart(s.endCron, 0);
        String eM = parsePart(s.endCron, 1);
        String eH = parsePart(s.endCron, 2);
        long endH = (eH == "*" || eH == "") ? curH : eH.toInt();
        long endM = (eM == "*" || eM == "") ? curM : eM.toInt();
        long endS = (eS == "*" || eS == "") ? 0 : eS.toInt();
        endTotalSec = endH * 3600 + endM * 60 + endS;
    }

    if (endTotalSec < startTotalSec) { // go through midnight
        return (curTotalSec >= startTotalSec || curTotalSec < endTotalSec);
    }
    return (curTotalSec >= startTotalSec && curTotalSec < endTotalSec);
}

void updateRelaysLogic() {
    unsigned long now = millis();
    for (int i = 0; i < 3; i++) {
        bool shouldBeOn = false;
        for (int j = 0; j < relays[i]->currentSchedules; j++) {
            if (isCurrentTimeInSchedule(relays[i]->schedules[j])) {
                shouldBeOn = true; break;
            }
        }

        if (i < 2) { // СВЕТ
            if (relays[i]->manualMode) continue;
            if (relays[i]->state != shouldBeOn) {
                relays[i]->state = shouldBeOn;
                digitalWrite(relays[i]->pin, relays[i]->state ? LOW : HIGH);
                logEvent(relays[i]->name + (shouldBeOn ? " ON (Auto)" : " OFF (Auto)"));
            }
        } else { // НАСОС
            if (!shouldBeOn && !relays[i]->manualMode) relays[i]->safetyLock = false;
            bool target = (shouldBeOn || relays[i]->manualMode) && !relays[i]->safetyLock;

            if (relays[i]->state && (now - relays[i]->lastOnTime > (unsigned long)pumpMaxOnTimeSec * 1000)) {
                target = false;
                relays[i]->safetyLock = true;
                relays[i]->manualMode = false;
                logEvent("SAFETY: Pump Timeout!");
            }

            if (relays[i]->state != target) {
                relays[i]->state = target;
                if (target) relays[i]->lastOnTime = now;
                digitalWrite(relays[i]->pin, target ? LOW : HIGH);
                logEvent(relays[i]->name + (target ? " STARTED" : " STOPPED"));
            }
        }
    }
}

// --- WEB Handlers ---
String getSchedulesTable(int rid) {
    String html = "";
    for (int i = 0; i < relays[rid]->currentSchedules; i++) {
        Schedule& s = relays[rid]->schedules[i];
        html += "<tr><td>" + String(i + 1) + "</td><td><code>" + s.startCron + "</code></td><td>";
        html += s.useDuration ? (String(s.duration) + "s") : ("<code>" + s.endCron + "</code>");
        html += "</td><td><a href='/del?rid=" + String(rid) + "&id=" + String(i) + "' style='color:red;'>[X]</a></td></tr>";
    }
    return html.length() > 0 ? html : "<tr><td colspan='4'>No schedules</td></tr>";
}

void handleRelayAJAX(bool state) {
    int rid = server.arg("rid").toInt() - 1;
    if (rid >= 0 && rid < 3) {
        relays[rid]->manualMode = true;
        relays[rid]->state = state;
        if (state) relays[rid]->lastOnTime = millis();
        digitalWrite(relays[rid]->pin, state ? LOW : HIGH);
        server.send(200, "application/json", "{\"status\":\"OK\"}");
        logEvent(relays[rid]->name + " MANUAL " + (state ? "ON" : "OFF"));
    }
}

bool checkAuth() {
    if (!authEnabled) return true;
    if (!server.authenticate(WEB_USER, WEB_PASS)) {
        server.requestAuthentication(); return false;
    }
    return true;
}

// --- Setup и Loop ---
void setup() {
    Serial.begin(115200);
    Wire.begin();
    LittleFS.begin();
    loadConfig();

    for(int i=0; i<3; i++) {
        pinMode(relays[i]->pin, OUTPUT);
        digitalWrite(relays[i]->pin, HIGH);
    }
    pinMode(LEAK_SENSOR_PIN, INPUT_PULLUP);

    WiFi.config(local_IP, gateway, subnet, primaryDNS, secondaryDNS);
    WiFi.begin(WIFI_SSID, WIFI_PASS);
    while (WiFi.status() != WL_CONNECTED) delay(500);

    timeClient.begin();
    bmp.begin(0x76);

    server.on("/", [](){
        if (!checkAuth()) return;
        File f = LittleFS.open("/index.html", "r");
        String s = f.readString(); f.close();
        s.replace("%TIME%", timeClient.getFormattedTime());
        s.replace("%IP%", WiFi.localIP().toString());
        s.replace("%VERSION%", VERSION);

        for(int i=0; i<3; i++) {
            s.replace("%RELAY_NAME_"+String(i+1)+"%", relays[i]->name);
            s.replace("%RELAY_STATE_"+String(i+1)+"%", relays[i]->state ? "On" : "Off");
            s.replace("%SCHEDULES_"+String(i+1)+"%", getSchedulesTable(i));
        }
        server.send(200, "text/html", s);
    });

    server.on("/on", [](){ if(checkAuth()) handleRelayAJAX(true); });
    server.on("/off", [](){ if(checkAuth()) handleRelayAJAX(false); });

    server.on("/addsave", [](){
        if (!checkAuth()) return;
        int rid = server.arg("rid").toInt();
        if (rid >= 0 && rid < 3 && relays[rid]->currentSchedules < MAX_SCHEDULES) {
            Schedule& s = relays[rid]->schedules[relays[rid]->currentSchedules++];
            s.startCron = server.arg("start");
            if (server.arg("duration").length() > 0) {
                s.duration = server.arg("duration").toInt(); s.useDuration = true;
            } else {
                s.endCron = server.arg("end"); s.useDuration = false;
            }
            saveConfig();
            logEvent("New schedule added for R" + String(rid+1));
        }
        server.sendHeader("Location", "/"); server.send(303);
    });

    server.on("/del", [](){
        if (!checkAuth()) return;
        int rid = server.arg("rid").toInt();
        int id = server.arg("id").toInt();
        if (rid >= 0 && rid < 3 && id < relays[rid]->currentSchedules) {
            for (int i = id; i < relays[rid]->currentSchedules - 1; i++)
                relays[rid]->schedules[i] = relays[rid]->schedules[i+1];
            relays[rid]->currentSchedules--;
            saveConfig();
        }
        server.sendHeader("Location", "/"); server.send(303);
    });

    server.on("/settings", [](){
        if (!checkAuth()) return;
        if (server.hasArg("pumpMax")) {
            pumpMaxOnTimeSec = server.arg("pumpMax").toInt();
            leakSensorEnabled = server.hasArg("leakEn");
            authEnabled = server.hasArg("auth");
            saveConfig();
            server.sendHeader("Location", "/"); server.send(303); return;
        }
        File f = LittleFS.open("/settings.html", "r");
        String h = f.readString(); f.close();
        h.replace("%PUMP_MAX%", String(pumpMaxOnTimeSec));
        h.replace("%LEAK_CHECKED%", leakSensorEnabled ? "checked" : "");
        h.replace("%AUTH_CHECKED%", authEnabled ? "checked" : "");
        server.send(200, "text/html", h);
    });

    server.on("/style.css", [](){
    if (!checkAuth()) return;
    if (LittleFS.exists("/style.css")) {
        File f = LittleFS.open("/style.css", "r");
        server.streamFile(f, "text/css; charset=utf-8");
        f.close();
    } else {
        server.send(404, "text/plain", "CSS Not Found");
    }
});

    server.begin();
}

void loop() {
    server.handleClient();
    timeClient.update();
    updateRelaysLogic();

    unsigned long currentMillis = millis();
    if (currentMillis - lastLeakCheck >= LEAK_CHECK_INTERVAL) {
        lastLeakCheck = currentMillis;
        checkLeakSensor();
    }
    if (currentMillis - lastSensorRead >= BMP_INTERVAL) {
        lastSensorRead = currentMillis;
        readBMPData();
    }
}