#include "race_files.h"

#include <stdio.h>
#include <string.h>

#include <algorithm>

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

std::string withNumber(const std::string& name, int n) {
    size_t end = 5;
    while (end < name.size() && name[end] >= '0' && name[end] <= '9') end++;
    char num[8];
    snprintf(num, sizeof num, "%03d", n);
    return "race_" + std::string(num) + name.substr(end);
}

std::vector<Rename> renumberPlan(std::vector<std::string> names) {
    names.erase(std::remove_if(names.begin(), names.end(), [](const std::string& s) { return number(s.c_str()) < 0; }),
                names.end());
    std::sort(names.begin(), names.end(), [](const std::string& a, const std::string& b) {
        int na = number(a.c_str()), nb = number(b.c_str());
        return na != nb ? na < nb : a < b;
    });
    std::vector<Rename> plan;
    for (size_t i = 0; i < names.size(); i++) {
        std::string to = withNumber(names[i], (int)i + 1);
        if (to != names[i]) plan.push_back({names[i], to});
    }
    return plan;
}

}  // namespace racefiles
