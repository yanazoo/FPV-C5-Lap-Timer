#include "race_files.h"

#include <string.h>

namespace racefiles {

int number(const char* name) {
    if (strncmp(name, "race_", 5) != 0) return -1;
    const char* p = name + 5;
    int n = 0, digits = 0;
    while (*p >= '0' && *p <= '9' && digits < 4) n = n * 10 + (*p++ - '0'), digits++;
    if (!digits) return -1;
    size_t len = strlen(p);
    if (len < 4 || strcmp(p + len - 4, ".csv") != 0) return -1;
    return (*p == '.' || *p == '_') ? n : -1;
}

std::string dateOf(const char* name) {
    if (number(name) < 0) return "";
    const char* p = strchr(name + 5, '_');
    // "_YYYYMMDD_HHMM.csv"
    if (!p || strlen(p) != 18) return "";
    for (int i = 1; i < 14; i++) {
        bool digit = p[i] >= '0' && p[i] <= '9';
        if (i == 9 ? p[i] != '_' : !digit) return "";
    }
    return std::string(p + 1, 13);
}

bool older(const char* a, const char* b) {
    std::string da = dateOf(a), db = dateOf(b);
    if (da != db) return da < db;   // "" (no date) sorts first
    return number(a) < number(b);
}

}  // namespace racefiles
