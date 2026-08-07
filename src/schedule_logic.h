#pragma once

// Cron-style schedule parsing/matching logic: the Schedule struct, cron field
// validation (isValidPart/isValidCron), and time-in-schedule matching
// (isTimeInSchedule).
//
// Unlike connectivity_state.h, this uses Arduino's String and struct tm, so
// it is NOT native-testable (String needs the Arduino framework to exist at
// all) - it's covered by on-device unit tests instead (`pio test -e
// nodemcuv2`, see test/test_schedule_logic/). isTimeInSchedule takes the
// point in time as an explicit struct tm parameter rather than reading the
// wall clock itself, precisely so tests can supply arbitrary deterministic
// dates/times without waiting on real NTP sync.

#include <Arduino.h>
#include <time.h>

struct Schedule {
    String startCron;
    String endCron;
    int duration = 0;
    bool useDuration = false;
};

inline bool isValidPart(String part, int minVal, int maxVal) {
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

inline bool isValidCron(String str) {
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

    // Валидация каждого поля согласно логике isTimeInSchedule
    if (!isValidPart(parts[0], 0, 59)) return false; // Минуты
    if (!isValidPart(parts[1], 0, 23)) return false; // Часы
    if (!isValidPart(parts[2], 1, 31)) return false; // Дни
    if (!isValidPart(parts[3], 1, 12)) return false; // Месяцы
    if (!isValidPart(parts[4], 0, 7)) return false; // День недели (0-7)

    return true;
}

inline bool isTimeInSchedule(const Schedule &s, const struct tm &timeinfo) {
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
