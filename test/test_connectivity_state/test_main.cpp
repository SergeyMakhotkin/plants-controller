// Host-side unit tests for the WiFi/NTP connectivity state machine
// (src/connectivity_state.h). Runs on the dev machine (`pio test -e native`),
// no ESP8266 hardware involved - decideConnectivityAction() takes every
// input (WiFi status, time validity, timestamps) as a plain argument, so
// each scenario below just constructs the situation directly.

#include <unity.h>
#include "connectivity_state.h"

void setUp() {}
void tearDown() {}

// ---------- BOOT_WIFI: waiting for the router (scenario 1) ----------

void test_boot_wifi_idles_when_nothing_is_due() {
    // No WiFi yet, retry interval not reached, reboot budget not reached.
    ConnectivityDecision d = decideConnectivityAction(
        SystemState::BOOT_WIFI, /*wifiConnected=*/false, /*timeValid=*/false,
        /*nowMs=*/1000, /*bootStartMillis=*/0,
        /*lastWifiBeginAttempt=*/1000, /*lastNtpAttempt=*/0, /*lastWifiReconnect=*/0);

    TEST_ASSERT_TRUE(d.nextState == SystemState::BOOT_WIFI);
    TEST_ASSERT_TRUE(d.wifiBegin == WifiBeginReason::NONE);
    TEST_ASSERT_FALSE(d.doReboot);
}

void test_boot_wifi_reissues_begin_after_retry_interval() {
    ConnectivityDecision d = decideConnectivityAction(
        SystemState::BOOT_WIFI, false, false,
        /*nowMs=*/WIFI_BEGIN_RETRY_MS, /*bootStartMillis=*/0,
        /*lastWifiBeginAttempt=*/0, 0, 0);

    TEST_ASSERT_TRUE(d.nextState == SystemState::BOOT_WIFI);
    TEST_ASSERT_TRUE(d.wifiBegin == WifiBeginReason::RETRY);
    TEST_ASSERT_FALSE(d.doReboot);
}

void test_boot_wifi_does_not_reissue_begin_before_interval() {
    ConnectivityDecision d = decideConnectivityAction(
        SystemState::BOOT_WIFI, false, false,
        /*nowMs=*/WIFI_BEGIN_RETRY_MS - 1, /*bootStartMillis=*/0,
        /*lastWifiBeginAttempt=*/0, 0, 0);

    TEST_ASSERT_TRUE(d.wifiBegin == WifiBeginReason::NONE);
}

void test_boot_wifi_reboots_once_budget_exhausted() {
    // Router still not found after the full boot budget: reboot rather than hang forever.
    ConnectivityDecision d = decideConnectivityAction(
        SystemState::BOOT_WIFI, false, false,
        /*nowMs=*/BOOT_TIMEOUT_MS, /*bootStartMillis=*/0,
        /*lastWifiBeginAttempt=*/0, 0, 0);

    TEST_ASSERT_TRUE(d.doReboot);
}

void test_boot_wifi_no_reboot_just_before_budget() {
    ConnectivityDecision d = decideConnectivityAction(
        SystemState::BOOT_WIFI, false, false,
        /*nowMs=*/BOOT_TIMEOUT_MS - 1, /*bootStartMillis=*/0,
        /*lastWifiBeginAttempt=*/0, 0, 0);

    TEST_ASSERT_FALSE(d.doReboot);
}

void test_boot_wifi_moves_to_boot_ntp_once_connected() {
    ConnectivityDecision d = decideConnectivityAction(
        SystemState::BOOT_WIFI, /*wifiConnected=*/true, false,
        /*nowMs=*/5000, /*bootStartMillis=*/0,
        /*lastWifiBeginAttempt=*/0, 0, 0);

    TEST_ASSERT_TRUE(d.nextState == SystemState::BOOT_NTP);
    TEST_ASSERT_TRUE(d.enteredBootNtp);
    TEST_ASSERT_TRUE(d.ntpRequest == NtpRequestReason::INITIAL);
    TEST_ASSERT_FALSE(d.doReboot);
}

// A router that appears right as the boot budget expires must still win over rebooting.
void test_boot_wifi_connect_wins_over_simultaneous_reboot_deadline() {
    ConnectivityDecision d = decideConnectivityAction(
        SystemState::BOOT_WIFI, /*wifiConnected=*/true, false,
        /*nowMs=*/BOOT_TIMEOUT_MS, /*bootStartMillis=*/0,
        /*lastWifiBeginAttempt=*/0, 0, 0);

    TEST_ASSERT_TRUE(d.nextState == SystemState::BOOT_NTP);
    TEST_ASSERT_FALSE(d.doReboot);
}

// ---------- BOOT_NTP: WiFi up, waiting for time (scenario 2) ----------

void test_boot_ntp_idles_when_nothing_is_due() {
    ConnectivityDecision d = decideConnectivityAction(
        SystemState::BOOT_NTP, /*wifiConnected=*/true, /*timeValid=*/false,
        /*nowMs=*/1000, /*bootStartMillis=*/0,
        0, /*lastNtpAttempt=*/1000, 0);

    TEST_ASSERT_TRUE(d.nextState == SystemState::BOOT_NTP);
    TEST_ASSERT_TRUE(d.ntpRequest == NtpRequestReason::NONE);
}

void test_boot_ntp_retries_after_interval() {
    ConnectivityDecision d = decideConnectivityAction(
        SystemState::BOOT_NTP, true, false,
        /*nowMs=*/NTP_BOOT_RETRY_MS, /*bootStartMillis=*/0,
        0, /*lastNtpAttempt=*/0, 0);

    TEST_ASSERT_TRUE(d.ntpRequest == NtpRequestReason::BOOT_RETRY);
    TEST_ASSERT_TRUE(d.nextState == SystemState::BOOT_NTP);
}

void test_boot_ntp_transitions_to_running_when_time_becomes_valid() {
    ConnectivityDecision d = decideConnectivityAction(
        SystemState::BOOT_NTP, true, /*timeValid=*/true,
        /*nowMs=*/12345, /*bootStartMillis=*/0,
        0, 0, 0);

    TEST_ASSERT_TRUE(d.nextState == SystemState::RUNNING);
    TEST_ASSERT_TRUE(d.enteredRunning);
    TEST_ASSERT_FALSE(d.doReboot);
}

// Time syncing right as the boot budget expires must still win over rebooting -
// otherwise a slow-but-successful NTP reply could be discarded by a reboot mid-flight.
void test_boot_ntp_sync_wins_over_simultaneous_reboot_deadline() {
    ConnectivityDecision d = decideConnectivityAction(
        SystemState::BOOT_NTP, true, /*timeValid=*/true,
        /*nowMs=*/BOOT_TIMEOUT_MS, /*bootStartMillis=*/0,
        0, 0, 0);

    TEST_ASSERT_TRUE(d.nextState == SystemState::RUNNING);
    TEST_ASSERT_FALSE(d.doReboot);
}

void test_boot_ntp_reboots_once_budget_exhausted_without_sync() {
    ConnectivityDecision d = decideConnectivityAction(
        SystemState::BOOT_NTP, true, false,
        /*nowMs=*/BOOT_TIMEOUT_MS, /*bootStartMillis=*/0,
        0, 0, 0);

    TEST_ASSERT_TRUE(d.doReboot);
}

void test_boot_ntp_falls_back_to_boot_wifi_when_link_drops() {
    // Scenario 3 variant during boot: WiFi disappears again before time was ever obtained.
    ConnectivityDecision d = decideConnectivityAction(
        SystemState::BOOT_NTP, /*wifiConnected=*/false, false,
        /*nowMs=*/9999, /*bootStartMillis=*/0,
        0, 0, 0);

    TEST_ASSERT_TRUE(d.nextState == SystemState::BOOT_WIFI);
    TEST_ASSERT_TRUE(d.wifiBegin == WifiBeginReason::RELINK_AFTER_NTP_BOOT_DROP);
    TEST_ASSERT_FALSE(d.doReboot); // the shared boot budget keeps counting, it does not reset here
}

// ---------- RUNNING / DEGRADED: already synced (scenario 3) ----------

void test_running_link_loss_enters_degraded_and_attempts_reconnect() {
    ConnectivityDecision d = decideConnectivityAction(
        SystemState::RUNNING, /*wifiConnected=*/false, /*timeValid=*/true,
        /*nowMs=*/WIFI_RECONNECT_INTERVAL, /*bootStartMillis=*/0,
        0, 0, /*lastWifiReconnect=*/0);

    TEST_ASSERT_TRUE(d.nextState == SystemState::DEGRADED);
    TEST_ASSERT_TRUE(d.enteredDegraded);
    TEST_ASSERT_TRUE(d.doWifiReconnect);
}

void test_running_never_reboots_no_matter_how_stale_the_boot_timer_is() {
    // The reboot safety net is retired for good once time has been obtained -
    // a multi-hour outage must not restart the controller and interrupt relays.
    ConnectivityDecision d = decideConnectivityAction(
        SystemState::RUNNING, /*wifiConnected=*/false, true,
        /*nowMs=*/BOOT_TIMEOUT_MS * 100, /*bootStartMillis=*/0,
        0, 0, /*lastWifiReconnect=*/BOOT_TIMEOUT_MS * 100);

    TEST_ASSERT_FALSE(d.doReboot);
}

void test_degraded_reconnect_waits_for_its_own_interval() {
    ConnectivityDecision d = decideConnectivityAction(
        SystemState::DEGRADED, false, true,
        /*nowMs=*/WIFI_RECONNECT_INTERVAL - 1, /*bootStartMillis=*/0,
        0, 0, /*lastWifiReconnect=*/0);

    TEST_ASSERT_FALSE(d.doWifiReconnect);
    TEST_ASSERT_TRUE(d.nextState == SystemState::DEGRADED);
}

void test_running_steady_link_never_triggers_wifi_begin_or_reconnect() {
    // Stable connection: no periodic resync due yet either -> no action at all.
    ConnectivityDecision d = decideConnectivityAction(
        SystemState::RUNNING, /*wifiConnected=*/true, true,
        /*nowMs=*/1000, /*bootStartMillis=*/0,
        0, /*lastNtpAttempt=*/1000, 0);

    TEST_ASSERT_TRUE(d.nextState == SystemState::RUNNING);
    TEST_ASSERT_TRUE(d.wifiBegin == WifiBeginReason::NONE);
    TEST_ASSERT_FALSE(d.doWifiReconnect);
    TEST_ASSERT_TRUE(d.ntpRequest == NtpRequestReason::NONE);
}

void test_running_requests_periodic_resync_after_steady_interval() {
    // Link up the whole time, no internet though: only the hourly forced resync
    // ever notices - this is the "link up, no internet" case (scenario 3b).
    ConnectivityDecision d = decideConnectivityAction(
        SystemState::RUNNING, true, true,
        /*nowMs=*/NTP_STEADY_RESYNC_MS, /*bootStartMillis=*/0,
        0, /*lastNtpAttempt=*/0, 0);

    TEST_ASSERT_TRUE(d.ntpRequest == NtpRequestReason::PERIODIC);
    TEST_ASSERT_TRUE(d.nextState == SystemState::RUNNING);
}

void test_running_does_not_resync_before_steady_interval() {
    ConnectivityDecision d = decideConnectivityAction(
        SystemState::RUNNING, true, true,
        /*nowMs=*/NTP_STEADY_RESYNC_MS - 1, /*bootStartMillis=*/0,
        0, /*lastNtpAttempt=*/0, 0);

    TEST_ASSERT_TRUE(d.ntpRequest == NtpRequestReason::NONE);
}

void test_degraded_recovers_to_running_and_forces_a_resync() {
    ConnectivityDecision d = decideConnectivityAction(
        SystemState::DEGRADED, /*wifiConnected=*/true, true,
        /*nowMs=*/42, /*bootStartMillis=*/0,
        0, 0, 0);

    TEST_ASSERT_TRUE(d.nextState == SystemState::RUNNING);
    TEST_ASSERT_TRUE(d.exitedDegraded);
    TEST_ASSERT_TRUE(d.ntpRequest == NtpRequestReason::AFTER_RECONNECT);
}

// ---------- millis() wraparound ----------

void test_boot_wifi_retry_survives_millis_wraparound() {
    // bootStartMillis was taken just before millis() wrapped around 0 (a real
    // ESP8266 millis() is a 32-bit counter, hence uint32_t here rather than
    // `unsigned long`, which is 64-bit on a typical host). nowMs has wrapped to a
    // small value; unsigned subtraction must still read as a small elapsed time,
    // not a huge one - i.e. no reboot from a false-positive underflow.
    uint32_t bootStartMillis = 0xFFFFFFFFUL - 100; // 100ms before wraparound
    uint32_t nowMs = 50;                            // 150ms of real elapsed time since boot

    ConnectivityDecision d = decideConnectivityAction(
        SystemState::BOOT_WIFI, false, false,
        nowMs, bootStartMillis,
        /*lastWifiBeginAttempt=*/bootStartMillis, 0, 0);

    TEST_ASSERT_FALSE(d.doReboot);
    TEST_ASSERT_TRUE(d.wifiBegin == WifiBeginReason::NONE); // 150ms < WIFI_BEGIN_RETRY_MS
}

int main(int argc, char **argv) {
    UNITY_BEGIN();

    RUN_TEST(test_boot_wifi_idles_when_nothing_is_due);
    RUN_TEST(test_boot_wifi_reissues_begin_after_retry_interval);
    RUN_TEST(test_boot_wifi_does_not_reissue_begin_before_interval);
    RUN_TEST(test_boot_wifi_reboots_once_budget_exhausted);
    RUN_TEST(test_boot_wifi_no_reboot_just_before_budget);
    RUN_TEST(test_boot_wifi_moves_to_boot_ntp_once_connected);
    RUN_TEST(test_boot_wifi_connect_wins_over_simultaneous_reboot_deadline);

    RUN_TEST(test_boot_ntp_idles_when_nothing_is_due);
    RUN_TEST(test_boot_ntp_retries_after_interval);
    RUN_TEST(test_boot_ntp_transitions_to_running_when_time_becomes_valid);
    RUN_TEST(test_boot_ntp_sync_wins_over_simultaneous_reboot_deadline);
    RUN_TEST(test_boot_ntp_reboots_once_budget_exhausted_without_sync);
    RUN_TEST(test_boot_ntp_falls_back_to_boot_wifi_when_link_drops);

    RUN_TEST(test_running_link_loss_enters_degraded_and_attempts_reconnect);
    RUN_TEST(test_running_never_reboots_no_matter_how_stale_the_boot_timer_is);
    RUN_TEST(test_degraded_reconnect_waits_for_its_own_interval);
    RUN_TEST(test_running_steady_link_never_triggers_wifi_begin_or_reconnect);
    RUN_TEST(test_running_requests_periodic_resync_after_steady_interval);
    RUN_TEST(test_running_does_not_resync_before_steady_interval);
    RUN_TEST(test_degraded_recovers_to_running_and_forces_a_resync);

    RUN_TEST(test_boot_wifi_retry_survives_millis_wraparound);

    return UNITY_END();
}
