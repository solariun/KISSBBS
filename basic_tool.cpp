// =============================================================================
// basic_tool.cpp — Standalone BASIC interpreter / interactive REPL
//                  for offline testing and debugging of BBS .bas scripts
//                  C++11, POSIX
//
// Build:
//   make basic_tool
//
// Modes:
//   basic_tool [options] <file.bas>   Load and run a BASIC script, then exit
//   basic_tool [options]              Interactive QBASIC-style REPL
//
// Options:
//   -t, --trace          Print each executed line to stderr before running it
//                        Format:  "  >> <linenum>: <source>"  (dim/cyan)
//   -v, --var NAME=VAL   Pre-set a variable before running
//                          Name ends in $ → string variable
//                          Otherwise      → numeric variable (parsed as double)
//                        Example: -v callsign$=W1ABC  -v window=3
//   -T, --timeout MS     Default INPUT/RECV timeout in ms (0 = block; default 0)
//   -r, --repl           Open the interactive REPL after running a file
//   --no-color           Disable ANSI colour output
//   -h, --help           Print this help and exit
//
// REPL line-editor:
//   <linenum> <stmt>     Add or replace a stored line
//   <linenum>            Delete that line (empty body)
//   RUN                  Execute the stored program
//   LIST [from[-to]]     List all stored lines (or a range)
//   NEW                  Clear the stored program
//   LOAD <file>          Load a .bas file into the editor
//   SAVE <file>          Save the stored program to a file
//   HELP / ?             Show this command reference
//   QUIT / EXIT / BYE    Exit the tool
//
// Direct execution (no line number):
//   Any line without a leading line number that is not a REPL command is
//   executed immediately as a one-liner BASIC program.  Examples:
//     PRINT 2 + 2
//     PRINT LEFT$("hello", 3)
//
// Useful pre-set variables for BBS scripts:
//   -v callsign$=W1ABC -v bbs_name$=MyBBS -v db_path$=bbs.db
//
// Examples:
//   basic_tool welcome.bas
//   basic_tool --trace -v callsign\$=W1ABC -v bbs_name\$=MyBBS welcome.bas
//   basic_tool                          # interactive REPL
//   basic_tool --repl welcome.bas       # run then drop into REPL
// =============================================================================

#include <algorithm>
#include <cctype>
#include <climits>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iostream>
#include <map>
#include <sstream>
#include <string>
#include <unistd.h>
#include <vector>

#include "basic.hpp"

// ── ANSI colour helpers ────────────────────────────────────────────────────────
static bool g_color = true;

static std::string BOLD()   { return g_color ? "\033[1m"  : ""; }
static std::string DIM()    { return g_color ? "\033[2m"  : ""; }
static std::string CYAN()   { return g_color ? "\033[36m" : ""; }
static std::string YELLOW() { return g_color ? "\033[33m" : ""; }
static std::string RED()    { return g_color ? "\033[31m" : ""; }
static std::string GREEN()  { return g_color ? "\033[32m" : ""; }
static std::string RESET()  { return g_color ? "\033[0m"  : ""; }

// ── Global interpreter pointer (for Ctrl-C signal) ────────────────────────────
static Basic* g_interp = nullptr;

static void on_signal(int) {
    if (g_interp) g_interp->interrupt();
}

// =============================================================================
// Variable spec:  "name$=hello"  or  "count=42"
// =============================================================================
struct VarSpec {
    std::string name;
    std::string value;
    bool        is_str;
};

static VarSpec parse_var_spec(const std::string& s) {
    VarSpec v;
    auto eq = s.find('=');
    if (eq == std::string::npos) {
        std::cerr << "bad --var spec (missing '='): " << s << "\n";
        std::exit(1);
    }
    v.name  = s.substr(0, eq);
    v.value = s.substr(eq + 1);
    v.is_str = (!v.name.empty() && v.name.back() == '$');
    return v;
}

static void apply_vars(Basic& b, const std::vector<VarSpec>& vars) {
    for (const auto& v : vars) {
        // The BASIC lexer uppercases all identifiers, so we must too.
        std::string uname = v.name;
        for (auto& c : uname) c = (char)std::toupper((unsigned char)c);

        if (v.is_str) {
            b.set_str(uname, v.value);
        } else {
            try { b.set_num(uname, std::stod(v.value)); }
            catch (...) { b.set_str(uname, v.value); }
        }
    }
}

// =============================================================================
// MAP / QUEUE injection specs
//
// --map  "mapname:key=value"   inject a string entry into a named MAP
// --mapn "mapname:key=3.14"    inject a numeric entry into a named MAP
// --queue  "queuename:value"   push a string onto a named QUEUE
// --queuen "queuename:3.14"    push a numeric value onto a named QUEUE
//
// Format rules:
//   MAP:   everything before the first ':' is map_name,
//          everything between ':' and first '=' is key,
//          everything after '=' is the value.
//   QUEUE: everything before the first ':' is queue_name,
//          everything after ':' is the value.
// =============================================================================
struct MapSpec {
    std::string map_name;
    std::string key;
    std::string value;
    bool        numeric;
};

struct QueueSpec {
    std::string queue_name;
    std::string value;
    bool        numeric;
};

static MapSpec parse_map_spec(const std::string& s, bool numeric) {
    MapSpec m;
    m.numeric = numeric;
    auto colon = s.find(':');
    if (colon == std::string::npos) {
        std::cerr << "bad --map spec (missing ':'): " << s
                  << "  expected:  mapname:key=value\n";
        std::exit(1);
    }
    m.map_name = s.substr(0, colon);
    std::string rest = s.substr(colon + 1);
    auto eq = rest.find('=');
    if (eq == std::string::npos) {
        std::cerr << "bad --map spec (missing '=' after key): " << s
                  << "  expected:  mapname:key=value\n";
        std::exit(1);
    }
    m.key   = rest.substr(0, eq);
    m.value = rest.substr(eq + 1);
    return m;
}

static QueueSpec parse_queue_spec(const std::string& s, bool numeric) {
    QueueSpec q;
    q.numeric = numeric;
    auto colon = s.find(':');
    if (colon == std::string::npos) {
        std::cerr << "bad --queue spec (missing ':'): " << s
                  << "  expected:  queuename:value\n";
        std::exit(1);
    }
    q.queue_name = s.substr(0, colon);
    q.value      = s.substr(colon + 1);
    return q;
}

static void apply_maps(Basic& b, const std::vector<MapSpec>& maps) {
    for (const auto& m : maps) {
        if (m.numeric) {
            try { b.map_set(m.map_name, m.key, std::stod(m.value)); }
            catch (...) { b.map_set(m.map_name, m.key, m.value); }
        } else {
            b.map_set(m.map_name, m.key, m.value);
        }
    }
}

static void apply_queues(Basic& b, const std::vector<QueueSpec>& queues) {
    for (const auto& q : queues) {
        if (q.numeric) {
            try { b.queue_push(q.queue_name, std::stod(q.value)); }
            catch (...) { b.queue_push(q.queue_name, q.value); }
        } else {
            b.queue_push(q.queue_name, q.value);
        }
    }
}

// =============================================================================
// Default variable values
//
// BBS scripts expect certain variables to be pre-set by the host.  When the
// user does not supply -v overrides for these, sensible defaults are used so
// scripts run without extra arguments during offline testing.
//
// Defaults (can all be overridden with -v NAME=VALUE):
//   CALLSIGN$  = "N0CALL"   — the connecting user's callsign
//   LOCAL$     = "N0CALL"   — alias for the local station callsign
//   REMOTE$    = ""         — remote station (empty in offline mode)
//   BBS_NAME$  = "MyBBS"    — BBS name shown in banners
//   DB_PATH$   = "bbs.db"   — SQLite database path
// =============================================================================
static void apply_default_vars(Basic& b, const std::vector<VarSpec>& user_vars) {
    // Build set of names already supplied by the user (uppercased)
    std::set<std::string> user_names;
    for (const auto& v : user_vars) {
        std::string n = v.name;
        for (auto& c : n) c = (char)std::toupper((unsigned char)c);
        user_names.insert(n);
    }

    auto set_default = [&](const std::string& uname, const std::string& val) {
        if (!user_names.count(uname)) b.set_str(uname, val);
    };

    set_default("CALLSIGN$", "N0CALL");
    set_default("LOCAL$",    "N0CALL");
    set_default("REMOTE$",   "");
    set_default("BBS_NAME$", "MyBBS");
    set_default("DB_PATH$",  "bbs.db");
}

// =============================================================================
// Configure and run a Basic instance — shared between run-mode and REPL RUN
// =============================================================================
static void configure_interp(Basic& b, bool trace) {
    // PRINT / SEND → stdout (script supplies its own newlines)
    b.on_send = [](const std::string& s) {
        std::cout << s << std::flush;
    };

    // INPUT / RECV → blocking stdin readline
    // (The timeout_ms parameter from the script is ignored for stdin; the tool
    //  always blocks.  A future --timeout flag could add select() here.)
    b.on_recv = [](int /*timeout_ms*/) -> std::string {
        std::string line;
        if (!std::getline(std::cin, line)) return "";
        return line;
    };

    // Errors / info → stderr
    b.on_log = [](const std::string& msg) {
        std::cerr << RED() << "[BASIC] " << msg << RESET() << "\n";
    };

    // Trace each line before execution
    if (trace) {
        b.on_trace = [](int linenum, const std::string& src) {
            std::cerr << DIM() << CYAN()
                      << "  >> " << linenum << ": " << src
                      << RESET() << "\n";
        };
    }
}

// =============================================================================
// String utilities
// =============================================================================
static std::string str_trim(const std::string& s) {
    size_t a = s.find_first_not_of(" \t\r\n");
    if (a == std::string::npos) return "";
    size_t b = s.find_last_not_of(" \t\r\n");
    return s.substr(a, b - a + 1);
}

static std::string str_upper(std::string s) {
    for (auto& c : s) c = (char)std::toupper((unsigned char)c);
    return s;
}

// Check whether a trimmed string starts with an explicit BASIC line number.
// Returns true and sets linenum + body if it does.
static bool parse_linenum(const std::string& s, int& linenum, std::string& body) {
    if (s.empty() || !std::isdigit((unsigned char)s[0])) return false;
    size_t i = 0;
    while (i < s.size() && std::isdigit((unsigned char)s[i])) ++i;
    if (i == s.size() || std::isspace((unsigned char)s[i])) {
        linenum = std::stoi(s.substr(0, i));
        body    = (i < s.size()) ? str_trim(s.substr(i)) : "";
        return true;
    }
    return false;
}

// =============================================================================
// REPL program store
// =============================================================================
struct ReplProgram {
    std::map<int, std::string> lines;   // linenum → source text
    int next_auto = 10;                 // next auto-assigned line number

    void add(int n, const std::string& src) {
        if (src.empty()) lines.erase(n);
        else             lines[n] = src;
    }

    void list_lines(std::ostream& out, int from_ln = 0, int to_ln = INT_MAX) const {
        for (const auto& kv : lines) {
            if (kv.first >= from_ln && kv.first <= to_ln)
                out << BOLD() << kv.first << RESET() << "  " << kv.second << "\n";
        }
    }

    bool save(const std::string& path) const {
        std::ofstream f(path);
        if (!f) return false;
        for (const auto& kv : lines)
            f << kv.first << " " << kv.second << "\n";
        return true;
    }

    void load_into(Basic& b) const {
        b.clear();
        for (const auto& kv : lines)
            b.add_line(kv.first, kv.second);
    }

    void clear() { lines.clear(); next_auto = 10; }
};

// Load a .bas file into a ReplProgram.  Lines with explicit line numbers
// are stored as-is; lines without numbers are auto-numbered.
static bool load_bas_file(ReplProgram& prog, const std::string& path,
                          std::string& errmsg) {
    std::ifstream f(path);
    if (!f) { errmsg = "cannot open: " + path; return false; }

    prog.clear();
    std::string raw;
    int auto_ln = 10;
    while (std::getline(f, raw)) {
        std::string line = str_trim(raw);
        if (line.empty()) { auto_ln += 10; continue; }

        int n; std::string body;
        if (parse_linenum(line, n, body)) {
            prog.add(n, body);
            auto_ln = n + 10;
        } else {
            prog.add(auto_ln, line);
            auto_ln += 10;
        }
    }
    prog.next_auto = auto_ln;
    return true;
}

// Parse "LIST [from[-to]]" argument string
static void parse_list_range(const std::string& args, int& from_ln, int& to_ln) {
    from_ln = 0; to_ln = INT_MAX;
    std::string a = str_trim(args);
    if (a.empty()) return;
    auto dash = a.find('-');
    if (dash == std::string::npos) {
        try { from_ln = to_ln = std::stoi(a); } catch (...) {}
    } else {
        try { from_ln = std::stoi(a.substr(0, dash)); } catch (...) { from_ln = 0; }
        try { to_ln   = std::stoi(a.substr(dash + 1)); } catch (...) { to_ln = INT_MAX; }
    }
}

// =============================================================================
// Print REPL help
// =============================================================================
static void repl_help() {
    std::cout << "\n"
              << "  " << BOLD() << "Line editing:" << RESET() << "\n"
              << "    <linenum> <stmt>     Add/replace a stored line  (e.g.  10 PRINT \"hi\")\n"
              << "    <linenum>            Delete that line\n"
              << "\n"
              << "  " << BOLD() << "Program commands:" << RESET() << "\n"
              << "    RUN                  Execute the stored program\n"
              << "    LIST [from[-to]]     List stored lines (e.g.  LIST 10-50)\n"
              << "    NEW                  Clear the stored program\n"
              << "    LOAD <file>          Load a .bas file\n"
              << "    SAVE <file>          Save stored program to file\n"
              << "\n"
              << "  " << BOLD() << "Direct execution:" << RESET() << "\n"
              << "    Any line without a line number that is not a command is run\n"
              << "    immediately.  Example:  PRINT 1 + SQR(4)\n"
              << "\n"
              << "  " << BOLD() << "Other:" << RESET() << "\n"
              << "    HELP / ?             This help\n"
              << "    QUIT / EXIT / BYE    Exit\n"
              << "\n";
}

// =============================================================================
// Interactive REPL
// =============================================================================
static void repl(const std::vector<VarSpec>&   vars,
                 const std::vector<MapSpec>&    maps,
                 const std::vector<QueueSpec>&  queues,
                 bool trace,
                 ReplProgram* preloaded = nullptr) {
    ReplProgram prog;
    if (preloaded) prog = *preloaded;

    bool is_tty = isatty(fileno(stdin));

    auto prompt = [&]() {
        if (is_tty)
            std::cout << GREEN() << "BASIC" << RESET() << "> " << std::flush;
    };

    if (is_tty) {
        std::cout << BOLD() << "\nBASIC Offline Tool" << RESET()
                  << "  (HELP for commands, QUIT to exit)\n\n";
    }

    std::string input;
    prompt();

    while (std::getline(std::cin, input)) {
        std::string line = str_trim(input);
        if (line.empty()) { prompt(); continue; }

        // ── Explicit line number → store/delete line ──────────────────────
        int linenum; std::string body;
        if (parse_linenum(line, linenum, body)) {
            prog.add(linenum, body);
            if (body.empty())
                std::cout << DIM() << "(line " << linenum << " deleted)"
                          << RESET() << "\n";
            prompt();
            continue;
        }

        // ── REPL commands (case-insensitive) ──────────────────────────────
        std::string cmd1, rest_args;
        {
            auto sp = line.find_first_of(" \t");
            if (sp == std::string::npos) {
                cmd1 = str_upper(line);
                rest_args = "";
            } else {
                cmd1 = str_upper(line.substr(0, sp));
                rest_args = str_trim(line.substr(sp));
            }
        }

        if (cmd1 == "QUIT" || cmd1 == "EXIT" || cmd1 == "BYE") {
            std::cout << "73!\n";
            break;
        }

        if (cmd1 == "HELP" || cmd1 == "?") {
            repl_help();
            prompt(); continue;
        }

        if (cmd1 == "NEW") {
            prog.clear();
            std::cout << GREEN() << "OK" << RESET() << "\n";
            prompt(); continue;
        }

        if (cmd1 == "LIST") {
            if (prog.lines.empty()) {
                std::cout << DIM() << "(no program stored)" << RESET() << "\n";
            } else {
                int from_ln, to_ln;
                parse_list_range(rest_args, from_ln, to_ln);
                prog.list_lines(std::cout, from_ln, to_ln);
            }
            prompt(); continue;
        }

        if (cmd1 == "LOAD") {
            if (rest_args.empty()) {
                std::cerr << YELLOW() << "Usage: LOAD <file>" << RESET() << "\n";
                prompt(); continue;
            }
            std::string errmsg;
            if (load_bas_file(prog, rest_args, errmsg)) {
                std::cout << GREEN() << "Loaded " << prog.lines.size()
                          << " lines from " << rest_args << RESET() << "\n";
            } else {
                std::cerr << RED() << "Error: " << errmsg << RESET() << "\n";
            }
            prompt(); continue;
        }

        if (cmd1 == "SAVE") {
            if (rest_args.empty()) {
                std::cerr << YELLOW() << "Usage: SAVE <file>" << RESET() << "\n";
                prompt(); continue;
            }
            if (prog.save(rest_args))
                std::cout << GREEN() << "Saved " << prog.lines.size()
                          << " lines to " << rest_args << RESET() << "\n";
            else
                std::cerr << RED() << "Error: cannot write " << rest_args
                          << RESET() << "\n";
            prompt(); continue;
        }

        if (cmd1 == "RUN") {
            if (prog.lines.empty()) {
                std::cerr << YELLOW()
                          << "No program stored.  Use LOAD <file> or enter numbered lines."
                          << RESET() << "\n";
                prompt(); continue;
            }

            Basic b;
            g_interp = &b;
            prog.load_into(b);           // calls b.clear() internally
            apply_default_vars(b, vars); // defaults first
            apply_vars(b, vars);         // user overrides
            apply_maps(b, maps);
            apply_queues(b, queues);
            configure_interp(b, trace);

            bool ok = b.run();
            g_interp = nullptr;

            std::cout << "\n"
                      << (ok ? GREEN() : RED())
                      << "--- program " << (ok ? "ended OK" : "ended with error")
                      << " ---" << RESET() << "\n";
            prompt(); continue;
        }

        // ── Immediate execution: single BASIC statement ───────────────────
        // Wrap in a one-line program and run instantly.  Useful for quick
        // expressions: PRINT 2+2  /  PRINT LEFT$("hello",3)  / etc.
        {
            Basic b;
            g_interp = &b;
            b.add_line(10, line);
            apply_vars(b, vars);     // add_line does not call clear(), so
            apply_maps(b, maps);     // order doesn't matter here — but keep
            apply_queues(b, queues); // consistent: inject after loading.
            configure_interp(b, trace);
            b.run();
            g_interp = nullptr;
        }

        prompt();
    }
}

// =============================================================================
// Usage
// =============================================================================
static void usage(const char* prog) {
    std::cout <<
        "Usage:\n"
        "  " << prog << " [options] <file.bas>   load & run a BASIC script\n"
        "  " << prog << " [options]              interactive QBASIC REPL\n"
        "\n"
        "Options:\n"
        "  -t, --trace            Print each executed line to stderr\n"
        "  -v, --var NAME=VAL     Pre-set a variable ($ suffix → string; else numeric)\n"
        "  --map  NAME:KEY=VAL    Inject a string entry into a MAP\n"
        "  --mapn NAME:KEY=NUM    Inject a numeric entry into a MAP\n"
        "  --queue  NAME:VAL      Push a string onto a QUEUE\n"
        "  --queuen NAME:NUM      Push a numeric value onto a QUEUE\n"
        "  -r, --repl             Open REPL after running a file\n"
        "  --no-color             Disable ANSI colour output\n"
        "  -h, --help             Show this help\n"
        "\n"
        "REPL commands:\n"
        "  <linenum> <stmt>     Add/replace stored line\n"
        "  RUN / LIST / NEW     Execute / list / clear program\n"
        "  LOAD <f> / SAVE <f>  Load / save .bas file\n"
        "  QUIT / EXIT          Exit\n"
        "\n"
        "Examples:\n"
        "  " << prog << " welcome.bas\n"
        "  " << prog << " --trace -v callsign\\$=W1ABC -v bbs_name\\$=MyBBS welcome.bas\n"
        "  " << prog << " --map cfg:host=localhost --map cfg:port=8080 script.bas\n"
        "  " << prog << " --mapn scores:W1ABC=100 --queue msgs:hello script.bas\n"
        "  " << prog << "\n";
}

// =============================================================================
// main
// =============================================================================
int main(int argc, char** argv) {
    bool        trace      = false;
    bool        open_repl  = false;
    std::string file;
    std::vector<VarSpec>   vars;
    std::vector<MapSpec>   maps;
    std::vector<QueueSpec> queues;

    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];

        if (a == "-t" || a == "--trace") {
            trace = true;
        } else if (a == "-r" || a == "--repl") {
            open_repl = true;
        } else if (a == "--no-color") {
            g_color = false;
        } else if (a == "-h" || a == "--help") {
            usage(argv[0]);
            return 0;
        } else if ((a == "-v" || a == "--var") && i + 1 < argc) {
            vars.push_back(parse_var_spec(argv[++i]));
        } else if (a == "--map" && i + 1 < argc) {
            maps.push_back(parse_map_spec(argv[++i], /*numeric=*/false));
        } else if (a == "--mapn" && i + 1 < argc) {
            maps.push_back(parse_map_spec(argv[++i], /*numeric=*/true));
        } else if (a == "--queue" && i + 1 < argc) {
            queues.push_back(parse_queue_spec(argv[++i], /*numeric=*/false));
        } else if (a == "--queuen" && i + 1 < argc) {
            queues.push_back(parse_queue_spec(argv[++i], /*numeric=*/true));
        } else if (a[0] != '-') {
            if (file.empty()) {
                file = a;
            } else {
                std::cerr << "Unexpected argument: " << a << "\n";
                usage(argv[0]);
                return 1;
            }
        } else {
            std::cerr << "Unknown option: " << a << "\n";
            usage(argv[0]);
            return 1;
        }
    }

    // Disable colour when stdout is not a terminal
    if (!isatty(fileno(stdout))) g_color = false;

    // Register Ctrl-C handler so interrupted_ is set cleanly
    signal(SIGINT, on_signal);

    if (!file.empty()) {
        // ── Run mode: load file and execute ───────────────────────────────
        Basic b;
        g_interp = &b;

        if (!b.load_file(file)) {
            std::cerr << RED() << "Error: cannot load '" << file << "'"
                      << RESET() << "\n";
            return 1;
        }

        // Apply data after load_file: load_file calls clear() internally,
        // which would wipe any variables / maps / queues set before loading.
        apply_default_vars(b, vars); // defaults first, user -v flags override
        apply_vars(b, vars);
        apply_maps(b, maps);
        apply_queues(b, queues);
        configure_interp(b, trace);
        bool ok = b.run();
        g_interp = nullptr;

        if (!ok) return 1;

        if (open_repl) {
            // Reload file into REPL editor so the user can inspect/modify it
            ReplProgram prog;
            std::string errmsg;
            load_bas_file(prog, file, errmsg);
            repl(vars, maps, queues, trace, &prog);
        }
    } else {
        // ── Interactive REPL mode ─────────────────────────────────────────
        repl(vars, maps, queues, trace);
    }

    return 0;
}
