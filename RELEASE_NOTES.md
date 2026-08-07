# Release Notes

## 1.1.1

Reliability rework of WiFi/NTP acquisition after a power outage, plus a
non-blocking connectivity watchdog for the running system. Not a bugfix
release on its own (previous behavior "worked" in the common case) and not
a major version bump (no config/API changes), hence `1.1.1`.

### Why

Three failure scenarios were identified for what happens when the
controller loses power along with its router/ISP:

1. Router not yet up when the controller boots — WiFi never found.
2. Router up but the ISP link isn't yet — WiFi connects, NTP never syncs.
3. WiFi or internet drops while already running on schedule.

The previous firmware handled (1) and (2) with a blocking retry loop in
`setup()`: relays stayed safely OFF, but the HTTP server never started and
schedules never ran until WiFi/NTP succeeded, with no upper bound on the
wait and no automatic recovery if the WiFi stack ever wedged. Scenario (3)
was only partially handled (link loss was retried, but "link up, no
internet" was invisible to the code).

### What changed

- WiFi/NTP acquisition moved out of blocking `setup()` into a non-blocking
  state machine (`BOOT_WIFI` → `BOOT_NTP` → `RUNNING` ⇄ `DEGRADED`) driven
  from `loop()`. The controller can no longer hang indefinitely at boot.
- `WiFi.begin()` is reissued every 2 minutes while waiting for the router
  (previously called once, relying on the WiFi SDK's own retry behavior).
- A single 10-minute reboot safety net covers the combined "no WiFi" +
  "no NTP" boot phases — if time still hasn't been obtained after 10
  minutes, the controller reboots itself rather than risk staying wedged.
  This safety net is permanently disabled the moment time is obtained once;
  it never fires again for the rest of that power cycle, so a later outage
  can't cause a reboot loop that interrupts a running schedule.
- Once running, an hourly forced NTP resync guards against "WiFi link up,
  no internet" — previously undetectable, since the existing watchdog only
  checked link status. A confirmed resync (a real clock jump) is logged.
- Boot-time events are logged to a side file (`/log_early.txt`, uptime
  timestamps) before the first NTP sync, then replayed into the real log
  with reconstructed wall-clock timestamps once time is available — so
  early boot activity is no longer lost from the persistent log.
- No `WiFi.begin()`/`WiFi.reconnect()` calls happen while already connected
  and running — confirmed by test, since needlessly cycling a stable
  connection was explicitly called out as undesirable during design.

### Testing

Two separate test suites, split by what they need — `test_filter` in
`platformio.ini` keeps each scoped to its own environment so `pio test`
without `-e` doesn't try (and fail) to build both under both:

- `pio test -e native` — 21 unit tests for the connectivity state machine
  (all three scenarios above, plus boundary cases: simultaneous
  success/reboot-deadline, steady-state in `RUNNING` never re-triggering
  WiFi actions, `millis()` wraparound). Hardware-free by design, runs on
  the host.
- `pio test -e nodemcuv2` — 34 on-device unit tests for cron/schedule
  parsing and time matching (`isValidPart`, `isValidCron`,
  `isTimeInSchedule`, extracted into `src/schedule_logic.h`). Needs the
  real Arduino `String`, so it builds, uploads, and runs on the actual
  controller, reporting results back over serial. Note: this temporarily
  overwrites the flashed firmware with the test binary (empty `loop()` —
  no WiFi/relays/schedules while it's on) — `pio run --target upload`
  puts the real firmware back afterwards.

Both suites pass in full (21/21, 34/34).

**Flashed and verified live** on the controller (`192.168.100.110`): boot
log confirms `BOOT_WIFI` → `BOOT_NTP` → `RUNNING` completing in ~4s, and
the early-log-to-real-log replay (`/log_early.txt` → `/log.txt` with
reconstructed timestamps) working as designed.

### Files touched

- `src/main.cpp` — state machine driver, early-boot logging, minor
  readability fixes (`RELAY_COUNT`, `NTP_VALID_EPOCH`); schedule
  parsing/matching delegated to `schedule_logic.h`.
- `src/connectivity_state.h` — new; pure WiFi/NTP decision logic, no
  Arduino/ESP8266 dependency (host-testable).
- `src/schedule_logic.h` — new; `Schedule` struct + cron parsing/matching
  (`isValidPart`, `isValidCron`, `isTimeInSchedule`), uses Arduino `String`
  (on-device-testable, not host-testable).
- `test/test_connectivity_state/test_main.cpp` — new; 21 host-side tests.
- `test/test_schedule_logic/test_main.cpp` — new; 34 on-device tests.
- `platformio.ini` — `VERSION` bump, `default_envs` pinned to `nodemcuv2`,
  new `[env:native]` test environment, `test_build_src`/`test_filter` to
  keep the two test suites from colliding.
- `README.md` — documented the connectivity/resilience behavior, device
  detection (`pio device list`), and both test suites.
