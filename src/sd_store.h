// microSD on the S3 (the old gate node held the card; now the S3 does).
// Race CSVs and the roster backup, same formats as ESP-NOW-Lap-Timer except
// the pilot's MAC column becomes the VTX frequency.
#pragma once
#include <Arduino.h>

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
bool   sdDeleteFile(const String& path);
