// Race files on the SD card: race_NNN.csv, or race_NNN_YYYYMMDD_HHMM.csv
// (number, then the race's start date and time). Pure helpers, no SD access,
// so they run in the PC tests.
#pragma once
#include <string>

namespace racefiles {

// Numbers run 000..999 and then start again at 000; the date in the name,
// not the number, tells which race is older.
constexpr int kNumbers = 1000;

// NNN of a race file name ("race_012_20261108_1432.csv" -> 12), -1 for any
// other name.
int number(const char* name);

// "YYYYMMDD_HHMM" from a race file name, "" when it has no date.
std::string dateOf(const char* name);

// True if race file a is older than b: files with a date by the date, those
// without (saved before the timer had a clock) count as older than any with
// one; then by number.
bool older(const char* a, const char* b);

}  // namespace racefiles
