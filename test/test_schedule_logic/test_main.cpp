// On-device unit tests for schedule_logic.h (cron parsing + time matching).
// Runs ON the ESP8266 (`pio test -e nodemcuv2 --upload-port ...`), using the
// real Arduino String class - this is exactly what makes these tests
// possible that connectivity_state.h's native tests couldn't cover, since
// String isn't available without the Arduino framework.
//
// isTimeInSchedule takes struct tm explicitly (see schedule_logic.h), so
// every case here builds a fixed, deterministic point in time by hand -
// no dependency on the board's actual WiFi/NTP state.

#include <Arduino.h>
#include <unity.h>
#include "schedule_logic.h"

void setUp() {}
void tearDown() {}

static struct tm makeTime(int mday, int mon0, int wday, int hour, int minute, int sec) {
    struct tm t = {};
    t.tm_mday = mday;
    t.tm_mon = mon0; // 0-based, like struct tm
    t.tm_wday = wday; // 0 = Sunday
    t.tm_hour = hour;
    t.tm_min = minute;
    t.tm_sec = sec;
    return t;
}

// ---------- isValidPart ----------

void test_isValidPart_wildcard_always_valid() {
    TEST_ASSERT_TRUE(isValidPart("*", 0, 59));
}

void test_isValidPart_step_within_max_valid() {
    TEST_ASSERT_TRUE(isValidPart("*/15", 0, 59));
}

void test_isValidPart_step_zero_invalid() {
    TEST_ASSERT_FALSE(isValidPart("*/0", 0, 59));
}

void test_isValidPart_step_exceeds_max_invalid() {
    TEST_ASSERT_FALSE(isValidPart("*/100", 0, 59));
}

void test_isValidPart_exact_number_in_range_valid() {
    TEST_ASSERT_TRUE(isValidPart("30", 0, 59));
}

void test_isValidPart_exact_number_below_min_invalid() {
    TEST_ASSERT_FALSE(isValidPart("0", 1, 31));
}

void test_isValidPart_exact_number_above_max_invalid() {
    TEST_ASSERT_FALSE(isValidPart("60", 0, 59));
}

void test_isValidPart_non_numeric_invalid() {
    TEST_ASSERT_FALSE(isValidPart("abc", 0, 59));
}

void test_isValidPart_trims_surrounding_whitespace() {
    TEST_ASSERT_TRUE(isValidPart(" 5 ", 0, 59));
}

// ---------- isValidCron ----------

void test_isValidCron_all_wildcards_valid() {
    TEST_ASSERT_TRUE(isValidCron("* * * * *"));
}

void test_isValidCron_specific_time_valid() {
    TEST_ASSERT_TRUE(isValidCron("30 14 * * *"));
}

void test_isValidCron_step_field_valid() {
    TEST_ASSERT_TRUE(isValidCron("*/15 * * * *"));
}

void test_isValidCron_rejects_too_few_fields() {
    TEST_ASSERT_FALSE(isValidCron("30 14 * *"));
}

void test_isValidCron_rejects_trailing_garbage() {
    // Extra 6th token bleeds into the day-of-week field and fails validation there.
    TEST_ASSERT_FALSE(isValidCron("30 14 * * * *"));
}

void test_isValidCron_rejects_minute_out_of_range() {
    TEST_ASSERT_FALSE(isValidCron("60 * * * *"));
}

void test_isValidCron_rejects_hour_out_of_range() {
    TEST_ASSERT_FALSE(isValidCron("* 24 * * *"));
}

void test_isValidCron_rejects_day_zero() {
    TEST_ASSERT_FALSE(isValidCron("* * 0 * *"));
}

void test_isValidCron_accepts_day_of_week_7_as_sunday_alias() {
    TEST_ASSERT_TRUE(isValidCron("* * * * 7"));
}

void test_isValidCron_rejects_non_numeric_field() {
    TEST_ASSERT_FALSE(isValidCron("abc * * * *"));
}

// ---------- isTimeInSchedule: start/end range ----------

void test_range_schedule_matches_inside_window() {
    Schedule s;
    s.startCron = "0 8 * * *";
    s.endCron = "30 8 * * *";
    s.useDuration = false;

    struct tm t = makeTime(15, 0, 3, 8, 15, 0); // 08:15
    TEST_ASSERT_TRUE(isTimeInSchedule(s, t));
}

void test_range_schedule_does_not_match_before_window() {
    Schedule s;
    s.startCron = "0 8 * * *";
    s.endCron = "30 8 * * *";
    s.useDuration = false;

    struct tm t = makeTime(15, 0, 3, 7, 59, 59);
    TEST_ASSERT_FALSE(isTimeInSchedule(s, t));
}

void test_range_schedule_does_not_match_after_window() {
    Schedule s;
    s.startCron = "0 8 * * *";
    s.endCron = "30 8 * * *";
    s.useDuration = false;

    struct tm t = makeTime(15, 0, 3, 8, 31, 0);
    TEST_ASSERT_FALSE(isTimeInSchedule(s, t));
}

void test_range_schedule_spanning_midnight_matches_late_night() {
    Schedule s;
    s.startCron = "0 22 * * *";
    s.endCron = "0 2 * * *";
    s.useDuration = false;

    struct tm t = makeTime(15, 0, 3, 23, 30, 0);
    TEST_ASSERT_TRUE(isTimeInSchedule(s, t));
}

void test_range_schedule_spanning_midnight_matches_early_morning() {
    Schedule s;
    s.startCron = "0 22 * * *";
    s.endCron = "0 2 * * *";
    s.useDuration = false;

    struct tm t = makeTime(16, 0, 4, 1, 0, 0);
    TEST_ASSERT_TRUE(isTimeInSchedule(s, t));
}

void test_range_schedule_spanning_midnight_excludes_midday() {
    Schedule s;
    s.startCron = "0 22 * * *";
    s.endCron = "0 2 * * *";
    s.useDuration = false;

    struct tm t = makeTime(15, 0, 3, 12, 0, 0);
    TEST_ASSERT_FALSE(isTimeInSchedule(s, t));
}

// ---------- isTimeInSchedule: duration-based ----------

void test_duration_schedule_matches_at_start_minute_within_duration() {
    Schedule s;
    s.startCron = "0 8 * * *";
    s.useDuration = true;
    s.duration = 10; // seconds

    struct tm t = makeTime(15, 0, 3, 8, 0, 5);
    TEST_ASSERT_TRUE(isTimeInSchedule(s, t));
}

void test_duration_schedule_does_not_match_after_duration_elapsed() {
    Schedule s;
    s.startCron = "0 8 * * *";
    s.useDuration = true;
    s.duration = 10;

    struct tm t = makeTime(15, 0, 3, 8, 0, 10); // sec == duration, not < duration
    TEST_ASSERT_FALSE(isTimeInSchedule(s, t));
}

void test_duration_schedule_does_not_match_wrong_minute() {
    Schedule s;
    s.startCron = "0 8 * * *";
    s.useDuration = true;
    s.duration = 10;

    struct tm t = makeTime(15, 0, 3, 8, 1, 5);
    TEST_ASSERT_FALSE(isTimeInSchedule(s, t));
}

// ---------- isTimeInSchedule: date fields ----------

void test_day_of_month_filter_matches_exact_day() {
    Schedule s;
    s.startCron = "0 8 15 * *";
    s.endCron = "0 9 * * *";
    s.useDuration = false;

    struct tm match = makeTime(15, 0, 3, 8, 30, 0);
    struct tm noMatch = makeTime(16, 0, 4, 8, 30, 0);
    TEST_ASSERT_TRUE(isTimeInSchedule(s, match));
    TEST_ASSERT_FALSE(isTimeInSchedule(s, noMatch));
}

void test_day_of_month_step_filter() {
    Schedule s;
    s.startCron = "0 8 */5 * *"; // every 5th day of month
    s.endCron = "0 9 * * *";
    s.useDuration = false;

    struct tm onStep = makeTime(10, 0, 3, 8, 30, 0);   // 10 % 5 == 0
    struct tm offStep = makeTime(11, 0, 4, 8, 30, 0);  // 11 % 5 != 0
    TEST_ASSERT_TRUE(isTimeInSchedule(s, onStep));
    TEST_ASSERT_FALSE(isTimeInSchedule(s, offStep));
}

void test_month_filter_matches_exact_month() {
    Schedule s;
    s.startCron = "0 8 * 6 *"; // June (tm_mon == 5)
    s.endCron = "0 9 * * *";
    s.useDuration = false;

    struct tm june = makeTime(15, 5, 3, 8, 30, 0);
    struct tm july = makeTime(15, 6, 5, 8, 30, 0);
    TEST_ASSERT_TRUE(isTimeInSchedule(s, june));
    TEST_ASSERT_FALSE(isTimeInSchedule(s, july));
}

void test_day_of_week_filter_matches_exact_weekday() {
    Schedule s;
    s.startCron = "0 8 * * 3"; // Wednesday
    s.endCron = "0 9 * * *";
    s.useDuration = false;

    struct tm wednesday = makeTime(15, 0, 3, 8, 30, 0);
    struct tm thursday = makeTime(16, 0, 4, 8, 30, 0);
    TEST_ASSERT_TRUE(isTimeInSchedule(s, wednesday));
    TEST_ASSERT_FALSE(isTimeInSchedule(s, thursday));
}

void test_day_of_week_7_matches_sunday() {
    Schedule s;
    s.startCron = "0 8 * * 7"; // 7 == Sunday alias, same as wday 0
    s.endCron = "0 9 * * *";
    s.useDuration = false;

    struct tm sunday = makeTime(19, 0, 0, 8, 30, 0); // tm_wday = 0
    TEST_ASSERT_TRUE(isTimeInSchedule(s, sunday));
}

void test_all_wildcards_only_time_gates() {
    Schedule s;
    s.startCron = "0 8 * * *";
    s.endCron = "0 9 * * *";
    s.useDuration = false;

    // Any date should match as long as the time-of-day is inside the window.
    struct tm t1 = makeTime(1, 0, 1, 8, 30, 0);
    struct tm t2 = makeTime(31, 11, 6, 8, 30, 0);
    TEST_ASSERT_TRUE(isTimeInSchedule(s, t1));
    TEST_ASSERT_TRUE(isTimeInSchedule(s, t2));
}

void setup() {
    delay(2000); // let the board/serial settle before Unity starts printing

    UNITY_BEGIN();

    RUN_TEST(test_isValidPart_wildcard_always_valid);
    RUN_TEST(test_isValidPart_step_within_max_valid);
    RUN_TEST(test_isValidPart_step_zero_invalid);
    RUN_TEST(test_isValidPart_step_exceeds_max_invalid);
    RUN_TEST(test_isValidPart_exact_number_in_range_valid);
    RUN_TEST(test_isValidPart_exact_number_below_min_invalid);
    RUN_TEST(test_isValidPart_exact_number_above_max_invalid);
    RUN_TEST(test_isValidPart_non_numeric_invalid);
    RUN_TEST(test_isValidPart_trims_surrounding_whitespace);

    RUN_TEST(test_isValidCron_all_wildcards_valid);
    RUN_TEST(test_isValidCron_specific_time_valid);
    RUN_TEST(test_isValidCron_step_field_valid);
    RUN_TEST(test_isValidCron_rejects_too_few_fields);
    RUN_TEST(test_isValidCron_rejects_trailing_garbage);
    RUN_TEST(test_isValidCron_rejects_minute_out_of_range);
    RUN_TEST(test_isValidCron_rejects_hour_out_of_range);
    RUN_TEST(test_isValidCron_rejects_day_zero);
    RUN_TEST(test_isValidCron_accepts_day_of_week_7_as_sunday_alias);
    RUN_TEST(test_isValidCron_rejects_non_numeric_field);

    RUN_TEST(test_range_schedule_matches_inside_window);
    RUN_TEST(test_range_schedule_does_not_match_before_window);
    RUN_TEST(test_range_schedule_does_not_match_after_window);
    RUN_TEST(test_range_schedule_spanning_midnight_matches_late_night);
    RUN_TEST(test_range_schedule_spanning_midnight_matches_early_morning);
    RUN_TEST(test_range_schedule_spanning_midnight_excludes_midday);

    RUN_TEST(test_duration_schedule_matches_at_start_minute_within_duration);
    RUN_TEST(test_duration_schedule_does_not_match_after_duration_elapsed);
    RUN_TEST(test_duration_schedule_does_not_match_wrong_minute);

    RUN_TEST(test_day_of_month_filter_matches_exact_day);
    RUN_TEST(test_day_of_month_step_filter);
    RUN_TEST(test_month_filter_matches_exact_month);
    RUN_TEST(test_day_of_week_filter_matches_exact_weekday);
    RUN_TEST(test_day_of_week_7_matches_sunday);
    RUN_TEST(test_all_wildcards_only_time_gates);

    UNITY_END();
}

void loop() {}
