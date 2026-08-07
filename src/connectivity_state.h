#pragma once

#include <cstdint>

// Pure WiFi/NTP connectivity state machine.
//
// Deliberately free of Arduino/ESP8266 dependencies (no WiFi/LittleFS/String)
// so it can be unit tested on the host (`pio test -e native`) as well as
// compiled into the firmware. All hardware calls (WiFi.begin(), configTime(),
// ESP.restart(), logging) stay out of this file - decideConnectivityAction()
// only decides what should happen; the caller in main.cpp performs it.
//
// State flow: BOOT_WIFI -> BOOT_NTP -> RUNNING <-> DEGRADED
// "time obtained at least once" is not tracked as a separate flag - it is
// equivalent to time() being valid (see updateTime() in main.cpp): time()
// never reverts to invalid once synced, it just keeps ticking off millis().

enum class SystemState { BOOT_WIFI, BOOT_NTP, RUNNING, DEGRADED };

enum class WifiBeginReason { NONE, RETRY, RELINK_AFTER_NTP_BOOT_DROP };
enum class NtpRequestReason { NONE, INITIAL, BOOT_RETRY, PERIODIC, AFTER_RECONNECT };

// Single budget for BOOT_WIFI+BOOT_NTP combined, then reboot. Once time has
// been obtained once (RUNNING/DEGRADED) this safety net is permanently
// retired - relays must keep following the schedule off the local clock
// through long outages, not restart every few minutes.
const uint32_t BOOT_TIMEOUT_MS = 10UL * 60 * 1000;
const uint32_t WIFI_BEGIN_RETRY_MS = 2UL * 60 * 1000;   // reissue WiFi.begin() while in BOOT_WIFI
const uint32_t NTP_BOOT_RETRY_MS = 20UL * 1000;         // retry configTime() while in BOOT_NTP
const uint32_t NTP_STEADY_RESYNC_MS = 60UL * 60 * 1000; // forced resync cadence once RUNNING/DEGRADED
const uint32_t WIFI_RECONNECT_INTERVAL = 30UL * 1000;   // WiFi.reconnect() cadence while DEGRADED

struct ConnectivityDecision {
    SystemState nextState;
    WifiBeginReason wifiBegin = WifiBeginReason::NONE;
    NtpRequestReason ntpRequest = NtpRequestReason::NONE;
    bool doReboot = false;
    bool doWifiReconnect = false;
    bool enteredBootNtp = false;
    bool enteredRunning = false;
    bool enteredDegraded = false;
    bool exitedDegraded = false;
};

// All time arguments are millis()-style monotonic counters. uint32_t is used
// explicitly (not `unsigned long`, which is 32-bit on the ESP8266 target but
// 64-bit on a typical host) so the wraparound-safe unsigned-subtraction trick
// behaves identically whether this runs on the device or in a native test.
inline ConnectivityDecision decideConnectivityAction(
    SystemState state,
    bool wifiConnected,
    bool timeValid,
    uint32_t nowMs,
    uint32_t bootStartMillis,
    uint32_t lastWifiBeginAttempt,
    uint32_t lastNtpAttempt,
    uint32_t lastWifiReconnect
) {
    ConnectivityDecision d;
    d.nextState = state;

    switch (state) {
        case SystemState::BOOT_WIFI: {
            if (wifiConnected) {
                d.nextState = SystemState::BOOT_NTP;
                d.enteredBootNtp = true;
                d.ntpRequest = NtpRequestReason::INITIAL;
                break;
            }
            if (nowMs - bootStartMillis >= BOOT_TIMEOUT_MS) {
                d.doReboot = true;
                break;
            }
            if (nowMs - lastWifiBeginAttempt >= WIFI_BEGIN_RETRY_MS) {
                d.wifiBegin = WifiBeginReason::RETRY;
            }
            break;
        }

        case SystemState::BOOT_NTP: {
            if (!wifiConnected) {
                d.nextState = SystemState::BOOT_WIFI;
                d.wifiBegin = WifiBeginReason::RELINK_AFTER_NTP_BOOT_DROP;
                break;
            }
            if (timeValid) {
                d.nextState = SystemState::RUNNING;
                d.enteredRunning = true;
                break;
            }
            if (nowMs - bootStartMillis >= BOOT_TIMEOUT_MS) {
                d.doReboot = true;
                break;
            }
            if (nowMs - lastNtpAttempt >= NTP_BOOT_RETRY_MS) {
                d.ntpRequest = NtpRequestReason::BOOT_RETRY;
            }
            break;
        }

        case SystemState::RUNNING:
        case SystemState::DEGRADED: {
            if (!wifiConnected) {
                if (state == SystemState::RUNNING) {
                    d.nextState = SystemState::DEGRADED;
                    d.enteredDegraded = true;
                }
                if (nowMs - lastWifiReconnect >= WIFI_RECONNECT_INTERVAL) {
                    d.doWifiReconnect = true;
                }
                break;
            }

            if (state == SystemState::DEGRADED) {
                d.nextState = SystemState::RUNNING;
                d.exitedDegraded = true;
                d.ntpRequest = NtpRequestReason::AFTER_RECONNECT;
                break;
            }

            // Link up and steady: WiFi.begin()/reconnect() are never triggered here -
            // only a periodic NTP resync as insurance against "link up, no internet".
            if (nowMs - lastNtpAttempt >= NTP_STEADY_RESYNC_MS) {
                d.ntpRequest = NtpRequestReason::PERIODIC;
            }
            break;
        }
    }

    return d;
}
