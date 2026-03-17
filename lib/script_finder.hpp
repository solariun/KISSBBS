// =============================================================================
// script_finder.hpp — Reusable BASIC script discovery with regex matching
//
// Resolves script paths using a priority search order:
//   1. --bas-path CLI option (highest priority)
//   2. KISSBBS_BASIC_PATH environment variable
//   3. Application-specific default directory (e.g. "." or from config)
//
// Name resolution (resolve_script):
//   1. Exact path exists → use it directly
//   2. Try name + ".bas" extension
//   3. Search configured directories for exact filename match
//   4. Treat name as regex pattern → list matches for user selection
//
// Usage:
//   ScriptFinder finder;
//   finder.add_search_path("/home/user/.kissbbs/scripts");  // from --bas-path
//   finder.add_search_path(getenv("KISSBBS_BASIC_PATH"));   // from env
//   finder.add_search_path(".");                             // fallback default
//
//   auto scripts = finder.find("sim.*");         // regex match
//   auto path    = finder.resolve("welcome");    // full resolution chain
// =============================================================================
#pragma once

#include <algorithm>
#include <cstdlib>
#include <functional>
#include <regex>
#include <string>
#include <vector>

#include <dirent.h>
#include <unistd.h>

class ScriptFinder {
public:
    // ── Construction ─────────────────────────────────────────────────────
    ScriptFinder() {
        // Auto-load KISSBBS_BASIC_PATH from environment
        const char* env = std::getenv("KISSBBS_BASIC_PATH");
        if (env && env[0]) env_path_ = env;
    }

    // ── Search path management ───────────────────────────────────────────
    // Paths added via add_search_path() have highest priority (--bas-path).
    void add_search_path(const std::string& dir) {
        if (!dir.empty()) cli_paths_.push_back(dir);
    }

    // Set the application default directory (lowest priority).
    void set_default_dir(const std::string& dir) { default_dir_ = dir; }

    // ── Script discovery ─────────────────────────────────────────────────
    // Find .bas files matching a regex pattern in all search paths.
    // Empty pattern returns all .bas files found.
    std::vector<std::string> find(const std::string& pattern = "") const {
        std::vector<std::string> results;
        auto dirs = search_dirs();
        for (const auto& dir : dirs) {
            auto found = scan_dir(dir, pattern);
            for (auto& f : found) {
                std::string full = dir + "/" + f;
                // Avoid duplicates (same basename from different dirs)
                bool dup = false;
                for (const auto& r : results)
                    if (r == full) { dup = true; break; }
                if (!dup) results.push_back(full);
            }
        }
        return results;
    }

    // ── Full resolution chain ────────────────────────────────────────────
    // Given a name (or path), try to resolve to a single .bas file:
    //   1. Exact path exists → return it
    //   2. name + ".bas" exists → return it
    //   3. Search dirs for exact filename match
    //   4. Treat as regex → if exactly 1 match, return it
    //   5. Multiple matches → return empty (caller should use find() + picker)
    // Returns empty string if nothing found.
    std::string resolve(const std::string& name) const {
        if (name.empty()) return "";

        // 1. Exact path
        if (access(name.c_str(), R_OK) == 0) return name;

        // 2. Try adding .bas extension
        if (name.size() < 4 || lower4(name) != ".bas") {
            std::string with_ext = name + ".bas";
            if (access(with_ext.c_str(), R_OK) == 0) return with_ext;
        }

        // 3. Search configured directories
        auto dirs = search_dirs();
        for (const auto& dir : dirs) {
            std::string path = dir + "/" + name;
            if (access(path.c_str(), R_OK) == 0) return path;
            if (name.size() < 4 || lower4(name) != ".bas") {
                path = dir + "/" + name + ".bas";
                if (access(path.c_str(), R_OK) == 0) return path;
            }
        }

        // 4. Treat as regex pattern
        auto matches = find(name);
        if (matches.size() == 1) return matches[0];

        return "";  // 0 or multiple matches
    }

    // ── Interactive picker (for terminal apps) ───────────────────────────
    // Callback type: display a prompt and read user input with timeout.
    // Returns the input string, or empty on timeout/cancel.
    using ReadlineFn = std::function<std::string(const std::string& prompt, int timeout_ms)>;

    // Resolve with interactive fallback: if multiple matches, show numbered
    // list and let user pick.  Returns full path or empty on cancel/error.
    // out_fn: called to display each line to the user.
    std::string resolve_interactive(
        const std::string& name,
        const std::function<void(const std::string&)>& out_fn,
        const ReadlineFn& readline_fn) const
    {
        // Try direct resolution first
        std::string path = resolve(name);
        if (!path.empty()) return path;

        // Get matches (regex or all if name is empty)
        auto matches = find(name.empty() ? "" : name);
        if (matches.empty()) {
            if (name.empty())
                out_fn("No .bas scripts found in search paths.");
            else
                out_fn("No .bas scripts matching '" + name + "' found.");
            return "";
        }

        // Display numbered list
        for (std::size_t i = 0; i < matches.size(); ++i) {
            out_fn("  [" + std::to_string(i + 1) + "] " + basename_of(matches[i]));
        }

        std::string choice = readline_fn(
            "Select [1-" + std::to_string(matches.size()) + "]: ", 30000);
        if (choice.empty()) return "";

        int n = std::atoi(choice.c_str());
        if (n >= 1 && n <= static_cast<int>(matches.size()))
            return matches[static_cast<std::size_t>(n - 1)];

        out_fn("Invalid selection.");
        return "";
    }

    // ── Accessors ────────────────────────────────────────────────────────
    std::vector<std::string> search_dirs() const {
        std::vector<std::string> dirs;
        // Priority order: CLI paths > env path > default dir
        for (const auto& p : cli_paths_) dirs.push_back(p);
        if (!env_path_.empty()) dirs.push_back(env_path_);
        if (!default_dir_.empty()) dirs.push_back(default_dir_);
        if (dirs.empty()) dirs.push_back(".");
        return dirs;
    }

private:
    std::vector<std::string> cli_paths_;   // from --bas-path (highest priority)
    std::string              env_path_;    // from KISSBBS_BASIC_PATH
    std::string              default_dir_; // app-specific default

    // Scan a directory for .bas files optionally matching a regex pattern.
    static std::vector<std::string> scan_dir(const std::string& dir,
                                              const std::string& pattern) {
        std::vector<std::string> results;
        DIR* d = opendir(dir.c_str());
        if (!d) return results;

        std::regex re;
        bool use_regex = !pattern.empty();
        if (use_regex) {
            try { re = std::regex(pattern, std::regex::icase | std::regex::ECMAScript); }
            catch (...) { use_regex = false; }
        }

        struct dirent* entry;
        while ((entry = readdir(d)) != nullptr) {
            std::string name(entry->d_name);
            if (name.size() < 5) continue;
            std::string ext = name.substr(name.size() - 4);
            for (auto& c : ext)
                c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            if (ext != ".bas") continue;
            if (use_regex && !std::regex_search(name, re)) continue;
            results.push_back(name);
        }
        closedir(d);
        std::sort(results.begin(), results.end());
        return results;
    }

    static std::string lower4(const std::string& s) {
        if (s.size() < 4) return "";
        std::string ext = s.substr(s.size() - 4);
        for (auto& c : ext)
            c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        return ext;
    }

    static std::string basename_of(const std::string& path) {
        auto pos = path.find_last_of('/');
        return (pos == std::string::npos) ? path : path.substr(pos + 1);
    }
};
