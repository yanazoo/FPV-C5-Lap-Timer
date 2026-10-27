// microSD on the S3 (the old gate node held the card; now the S3 does).
// Race CSVs and the roster backup, same formats as ESP-NOW-Lap-Timer except
// the pilot's MAC column becomes the VTX frequency.
#pragma once
#include <Arduino.h>

void   sdInit();
void   sdCheckHotplug(uint32_t now);   // only while sdPollEnabled
bool   sdSaveRace();                   // caller holds gLock
bool   sdBackupRoster();
int    sdRestoreRoster();              // pilots restored, -1 on error
String sdListJson();
bool   sdReadFile(const String& path, String& out);
bool   sdDeleteFile(const String& path);
