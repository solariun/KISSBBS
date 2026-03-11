// =============================================================================
// ini.hpp — Header-only INI file parser  (C++11, zero dependencies)
//
// Format supported:
//   [section]
//   key = value    ; inline comment (also # supported)
//
// Example:
//   IniConfig cfg;
//   if (!cfg.load("bbs.ini")) { /* file not found */ }
//   std::string call = cfg.get("ax25", "callsign", "N0CALL");
//   int baud         = cfg.get_int("kiss", "baud", 9600);
// =============================================================================
#pragma once

#include <cctype>
#include <fstream>
#include <map>
#include <sstream>
#include <string>

class IniConfig {
public:
    // Load file.  Returns false on open error (does not throw).
    bool load(const std::string& path) {
        std::ifstream f(path);
        if (!f.is_open()) return false;
        std::string line, section;
        while (std::getline(f, line)) {
            // strip inline comments
            std::string::size_type c = line.find_first_of(";#");
            if (c != std::string::npos) line.resize(c);
            line = trim_(line);
            if (line.empty()) continue;
            if (line.front() == '[' && line.back() == ']') {
                section = trim_(line.substr(1, line.size() - 2));
            } else {
                auto eq = line.find('=');
                if (eq == std::string::npos || section.empty()) continue;
                std::string key = trim_(line.substr(0, eq));
                std::string val = trim_(line.substr(eq + 1));
                if (!key.empty()) data_[section][key] = val;
            }
        }
        return true;
    }

    std::string get(const std::string& sec, const std::string& key,
                    const std::string& def = "") const {
        auto si = data_.find(sec);
        if (si == data_.end()) return def;
        auto ki = si->second.find(key);
        return (ki == si->second.end()) ? def : ki->second;
    }

    int get_int(const std::string& sec, const std::string& key, int def = 0) const {
        std::string v = get(sec, key);
        if (v.empty()) return def;
        try { return std::stoi(v); } catch (...) { return def; }
    }

    double get_double(const std::string& sec, const std::string& key,
                      double def = 0.0) const {
        std::string v = get(sec, key);
        if (v.empty()) return def;
        try { return std::stod(v); } catch (...) { return def; }
    }

    bool get_bool(const std::string& sec, const std::string& key,
                  bool def = false) const {
        std::string v = get(sec, key);
        if (v.empty()) return def;
        for (auto& ch : v) ch = static_cast<char>(
            std::tolower(static_cast<unsigned char>(ch)));
        return v == "1" || v == "true" || v == "yes" || v == "on";
    }

    bool has(const std::string& sec, const std::string& key) const {
        auto si = data_.find(sec);
        if (si == data_.end()) return false;
        return si->second.count(key) > 0;
    }

    bool has_section(const std::string& sec) const {
        return data_.count(sec) > 0;
    }

    // Returns all key=value pairs in a section (empty map if missing)
    const std::map<std::string, std::string>& section(const std::string& sec) const {
        static const std::map<std::string, std::string> empty;
        auto it = data_.find(sec);
        return (it == data_.end()) ? empty : it->second;
    }

private:
    std::map<std::string, std::map<std::string, std::string>> data_;

    static std::string trim_(const std::string& s) {
        auto b = s.find_first_not_of(" \t\r\n");
        auto e = s.find_last_not_of(" \t\r\n");
        return (b == std::string::npos) ? "" : s.substr(b, e - b + 1);
    }
};
