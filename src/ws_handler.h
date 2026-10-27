#pragma once
#include <Arduino.h>

void wsText(const String& msg);
void wsText(const char* msg);
void initWsHandler();
// active_update to every client (after slot / roster changes).
void wsActiveUpdate();
