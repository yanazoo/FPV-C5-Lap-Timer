// microSD on the S3 (the old gate node held the card; now the S3 does).
// Race CSVs and the roster backup, same formats as ESP-NOW-Lap-Timer except
// the pilot's MAC column becomes the VTX frequency.
#pragma once
#include <Arduino.h>
#include <vector>

void   sdInit();
void   sdCheckHotplug(uint32_t now);   // only while sdPollEnabled
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
bool   sdBackupRoster();
int    sdRestoreRoster();              // pilots restored, -1 on error
String sdListJson();
bool   sdReadFile(const String& path, String& out);
// Any file (a PNG too) up to maxLen bytes, and a whole file written at once.
bool   sdReadBytes(const String& path, std::vector<uint8_t>& out, size_t maxLen);
bool   sdWriteFile(const String& path, const uint8_t* data, size_t len);
bool   sdDeleteFile(const String& path);
