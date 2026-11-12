// microSD on the S3 (the old gate node held the card; now the S3 does).
// Race CSVs and the roster backup, same formats as ESP-NOW-Lap-Timer except
// the pilot's MAC column becomes the VTX frequency.
#pragma once
#include <Arduino.h>

void   sdInit();
void   sdShutdown();                   // unmount before a restart
bool   sdSaveRace();                   // caller holds gLock
// Wall clock from a browser (the S3 has no RTC): epoch in ms (UTC) and the
// browser's UTC offset in minutes. Sets the system time, so race files get
// real timestamps too.
void   sdSetClock(int64_t epochMs, int tzMin);
// A fresh race started (caller holds gLock): remember its wall-clock start
// and rules for the "#FPVC5" line sdSaveRace writes.
void   sdNoteRaceStart();
// Delete every file and folder on the card. Returns the number of entries
// removed, -1 without a card.
int    sdEraseAll();
// Backups are named with the date and time they were saved
// (pilots_20261110_1432.csv), or plain (pilots.csv) before the timer knows
// the date. sdStampedName gives that name; sdBackupName accepts both forms.
String sdStampedName(const char* base, const char* ext);
bool   sdBackupName(const String& path, const char* base, const char* ext);
bool   sdBackupRoster(String& path);   // the file written
int    sdRestoreRoster(const String& path);   // pilots restored, -1 on error
String sdListJson();
bool   sdReadFile(const String& path, String& out);
// A whole file written at once.
bool   sdWriteFile(const String& path, const uint8_t* data, size_t len);
// Pieces of a file: append (first = start a new file), read at an offset,
// size (-1: none), and put a finished file in place of another.
bool    sdAppendFile(const String& path, const uint8_t* data, size_t len, bool first);
size_t  sdReadAt(const String& path, size_t offset, uint8_t* out, size_t len);
int32_t sdFileSize(const String& path);
bool    sdReplaceFile(const String& from, const String& to);
bool   sdDeleteFile(const String& path);
