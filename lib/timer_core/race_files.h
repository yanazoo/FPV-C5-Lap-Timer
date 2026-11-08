// Race files on the SD card: race_NNN.csv, or race_NNN_YYYYMMDD_HHMM.csv
// (number, then the race's start date and time). Pure helpers, no SD access,
// so they run in the PC tests.
#pragma once
#include <string>
#include <vector>

namespace racefiles {

// Numbers run 1..kMaxNum. When the next one would pass it, the files on the
// card are renumbered 1..N (oldest first) and numbering goes on from N+1.
constexpr int kMaxNum = 999;

// NNN of a race file name ("race_012_20261108_1432.csv" -> 12), -1 for any
// other name. Up to 4 digits are read.
int number(const char* name);

// The same name with another number ("race_012_x.csv", 3 -> "race_003_x.csv").
std::string withNumber(const std::string& name, int n);

struct Rename {
    std::string from, to;
};
// Renames that give the race files (names without a folder) the numbers
// 1..N in their current order (by number, then name). In the returned
// order each target number is at most the file's own and the lower ones
// are already done, so no rename meets an existing name (two files with
// one number, possible only from older firmware, may move one up; a rename
// that would overwrite fails on FAT and is skipped).
std::vector<Rename> renumberPlan(std::vector<std::string> names);

}  // namespace racefiles
