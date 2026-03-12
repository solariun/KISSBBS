// =============================================================================
// basic.cpp — QBASIC-style interpreter implementation  (C++11, POSIX)
// =============================================================================
#include "basic.hpp"

#include <algorithm>
#include <cassert>
#include <cctype>
#include <cerrno>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <regex>
#include <sstream>
#include <stdexcept>

#include <fcntl.h>
#include <netdb.h>
#include <signal.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>

#ifdef HAVE_SQLITE3
#include <sqlite3.h>
#endif

// ─────────────────────────────────────────────────────────────────────────────
// Destructor
// ─────────────────────────────────────────────────────────────────────────────
Basic::~Basic() {
#ifdef HAVE_SQLITE3
    if (db_) { sqlite3_close((sqlite3*)db_); db_ = nullptr; }
#endif
    for (auto& kv : sockets_) ::close(kv.second);
    sockets_.clear();
}

// ─────────────────────────────────────────────────────────────────────────────
// Value helpers
// ─────────────────────────────────────────────────────────────────────────────
double Basic::Value::to_num() const {
    if (kind == NUM) return num;
    if (str.empty()) return 0.0;
    try { return std::stod(str); } catch (...) { return 0.0; }
}

std::string Basic::Value::to_str() const {
    if (kind == STR) return str;
    if (num == (long long)num) return std::to_string((long long)num);
    std::ostringstream os;
    os << num;
    return os.str();
}

// ─────────────────────────────────────────────────────────────────────────────
// Lexer helpers
// ─────────────────────────────────────────────────────────────────────────────
static bool ci_eq(char a, char b) {
    return std::tolower((unsigned char)a) == std::tolower((unsigned char)b);
}
static bool ci_starts(const std::string& src, std::size_t pos, const char* kw) {
    std::size_t n = std::strlen(kw);
    if (pos + n > src.size()) return false;
    for (std::size_t i = 0; i < n; ++i)
        if (!ci_eq(src[pos + i], kw[i])) return false;
    if (pos + n < src.size()) {
        char next = src[pos + n];
        if (std::isalnum((unsigned char)next) || next == '_' || next == '$' || next == '%')
            return false;
    }
    return true;
}

void Basic::Lexer::skip_ws() {
    while (pos < src.size() && (src[pos] == ' ' || src[pos] == '\t')) ++pos;
}

bool Basic::Lexer::eat_kw(const char* kw) {
    skip_ws();
    if (!ci_starts(src, pos, kw)) return false;
    pos += std::strlen(kw);
    return true;
}

bool Basic::Lexer::peek_kw(const char* kw) const {
    std::size_t p = pos;
    while (p < src.size() && (src[p] == ' ' || src[p] == '\t')) ++p;
    return ci_starts(src, p, kw);
}

bool Basic::Lexer::eat_ch(char c) {
    skip_ws();
    if (pos < src.size() && src[pos] == c) { ++pos; return true; }
    return false;
}

bool Basic::Lexer::eat_str(const char* s) {
    skip_ws();
    std::size_t n = std::strlen(s);
    if (pos + n > src.size()) return false;
    for (std::size_t i = 0; i < n; ++i)
        if (src[pos+i] != s[i]) return false;
    pos += n;
    return true;
}

std::string Basic::Lexer::rest_trimmed() {
    skip_ws();
    std::string r = pos < src.size() ? src.substr(pos) : "";
    while (!r.empty() && (r.back()==' '||r.back()=='\t')) r.pop_back();
    pos = src.size();
    return r;
}

Basic::Lexer::Token Basic::Lexer::next_tok() {
    skip_ws();
    Token t;
    if (pos >= src.size()) { t.kind = EOL; t.text = ""; return t; }

    char c = src[pos];

    // Comment: ' or REM (at start only — handled in exec_stmt)
    if (c == '\'') { pos = src.size(); t.kind = EOL; return t; }

    // String literal
    if (c == '"') {
        ++pos;
        std::string s;
        while (pos < src.size() && src[pos] != '"') {
            if (src[pos] == '\\' && pos+1 < src.size()) {
                ++pos;
                switch(src[pos]) {
                    case 'n': s += '\n'; break;
                    case 'r': s += '\r'; break;
                    case 't': s += '\t'; break;
                    default:  s += src[pos]; break;
                }
            } else {
                s += src[pos];
            }
            ++pos;
        }
        if (pos < src.size()) ++pos;
        t.kind = STR_LIT; t.text = std::move(s);
        return t;
    }

    // Number
    if (std::isdigit((unsigned char)c) || (c == '.' && pos+1 < src.size() && std::isdigit((unsigned char)src[pos+1]))) {
        std::string s;
        while (pos < src.size() && (std::isdigit((unsigned char)src[pos]) || src[pos]=='.'))
            s += src[pos++];
        if (pos < src.size() && (src[pos]=='e'||src[pos]=='E')) {
            s += src[pos++];
            if (pos < src.size() && (src[pos]=='+'||src[pos]=='-')) s += src[pos++];
            while (pos < src.size() && std::isdigit((unsigned char)src[pos])) s += src[pos++];
        }
        t.kind = NUM_LIT; t.text = s;
        try { t.num = std::stod(s); } catch(...) { t.num = 0; }
        return t;
    }

    // Identifier / keyword
    if (std::isalpha((unsigned char)c) || c == '_') {
        std::string s;
        while (pos < src.size() && (std::isalnum((unsigned char)src[pos]) || src[pos]=='_'))
            s += src[pos++];
        if (pos < src.size() && src[pos] == '$') s += src[pos++];
        else if (pos < src.size() && src[pos] == '%') s += src[pos++];
        for (auto& ch : s) ch = static_cast<char>(std::toupper((unsigned char)ch));
        t.kind = IDENT; t.text = std::move(s);
        return t;
    }

    // Two-char operators
    if (pos+1 < src.size()) {
        std::string two; two += c; two += src[pos+1];
        if (two == "<>" || two == "<=" || two == ">=") {
            pos += 2;
            t.kind = PUNCT; t.text = std::move(two);
            return t;
        }
    }

    ++pos;
    t.kind = PUNCT; t.text = std::string(1, c);
    return t;
}

Basic::Lexer::Token Basic::Lexer::peek_tok() {
    std::size_t save = pos;
    Token t = next_tok();
    pos = save;
    return t;
}

// ─────────────────────────────────────────────────────────────────────────────
// Variable helpers (scope-aware)
// ─────────────────────────────────────────────────────────────────────────────
bool Basic::is_str_var(const std::string& name) const {
    return !name.empty() && name.back() == '$';
}

Basic::Value Basic::get_var(const std::string& name) const {
    // Check CONST first
    {
        auto it = consts_.find(name);
        if (it != consts_.end()) return it->second;
    }
    // Check local frame
    if (!frame_stack_.empty()) {
        auto& locals = frame_stack_.back().locals;
        auto it = locals.find(name);
        if (it != locals.end()) return it->second;
        // For function name itself, return return_value
        if (name == frame_stack_.back().proc_name && frame_stack_.back().is_function)
            return frame_stack_.back().return_value;
    }
    // Check type-variable fields (name contains '.')
    {
        auto it = type_vars_.find(name);
        if (it != type_vars_.end()) return it->second;
    }
    // Global
    auto it = vars_.find(name);
    if (it == vars_.end())
        return is_str_var(name) ? Value(std::string("")) : Value(0.0);
    return it->second;
}

void Basic::set_var(const std::string& name, Value v) {
    // Setting function name inside a function = setting return value
    if (!frame_stack_.empty() && frame_stack_.back().is_function
        && name == frame_stack_.back().proc_name) {
        frame_stack_.back().return_value = std::move(v);
        return;
    }
    // Local frame
    if (!frame_stack_.empty()) {
        auto& locals = frame_stack_.back().locals;
        if (locals.find(name) != locals.end()) {
            locals[name] = std::move(v);
            return;
        }
    }
    // Type variable field
    if (name.find('.') != std::string::npos) {
        type_vars_[name] = std::move(v);
        return;
    }
    // Global
    vars_[name] = std::move(v);
}

void Basic::set_str(const std::string& name, const std::string& val) {
    vars_[name] = Value(val);
}

void Basic::set_num(const std::string& name, double val) {
    vars_[name] = Value(val);
}

// ─────────────────────────────────────────────────────────────────────────────
// Program loading
// ─────────────────────────────────────────────────────────────────────────────
void Basic::clear() {
    program_.clear(); labels_.clear(); procs_.clear();
    types_.clear(); consts_.clear();
    vars_.clear(); type_vars_.clear();
    call_stack_.clear(); for_stack_.clear(); while_stack_.clear();
    if_stack_.clear(); do_stack_.clear(); select_stack_.clear();
    frame_stack_.clear();
    next_auto_line_ = 1;
    interrupted_ = false;
#ifdef HAVE_SQLITE3
    if (db_) { sqlite3_close((sqlite3*)db_); db_ = nullptr; }
#endif
    for (auto& kv : sockets_) ::close(kv.second);
    sockets_.clear(); next_sock_ = 1;
}

void Basic::add_line(int linenum, const std::string& text) {
    program_[linenum] = text;
}

// Strip inline comment (after ' that is not inside a string)
static std::string strip_comment(const std::string& line) {
    bool in_str = false;
    for (std::size_t i = 0; i < line.size(); ++i) {
        if (line[i] == '"') { in_str = !in_str; continue; }
        if (!in_str && line[i] == '\'') return line.substr(0, i);
    }
    return line;
}

void Basic::load_string(const std::string& source) {
    clear();
    std::istringstream ss(source);
    std::string line;
    int auto_ln = 1;
    while (std::getline(ss, line)) {
        while (!line.empty() && (line.back()=='\r'||line.back()=='\n')) line.pop_back();
        if (line.empty()) continue;
        std::size_t p = 0;
        while (p < line.size() && line[p] == ' ') ++p;
        if (p >= line.size()) continue;

        // Skip pure comment lines
        if (line[p] == '\'') {
            // Still need a slot so labels/procs can reference it
            program_[auto_ln++] = "";
            continue;
        }
        // Check for REM at start
        std::string upper_start;
        for (std::size_t i = p; i < line.size() && i < p+4; ++i)
            upper_start += static_cast<char>(std::toupper((unsigned char)line[i]));
        if (upper_start.substr(0, 3) == "REM") {
            program_[auto_ln++] = "";
            continue;
        }

        // Does line start with a line number?
        if (std::isdigit((unsigned char)line[p])) {
            int linenum = 0;
            std::size_t q = p;
            while (q < line.size() && std::isdigit((unsigned char)line[q]))
                linenum = linenum*10 + (line[q++]-'0');
            // Must be followed by space or end
            if (q == line.size() || line[q] == ' ' || line[q] == '\t') {
                while (q < line.size() && (line[q]==' '||line[q]=='\t')) ++q;
                program_[linenum] = strip_comment(line.substr(q));
                // Keep auto_ln ahead of explicit line numbers
                if (linenum >= auto_ln) auto_ln = linenum + 1;
                continue;
            }
        }

        // Check for label: starts with alpha, ends with ':'
        // e.g. "MyLabel:" or "MyLabel:  ' comment"
        {
            std::string word;
            std::size_t q = p;
            while (q < line.size() && (std::isalnum((unsigned char)line[q]) || line[q]=='_'))
                word += line[q++];
            while (q < line.size() && (line[q]==' '||line[q]=='\t')) ++q;
            if (!word.empty() && q < line.size() && line[q] == ':') {
                // It's a label line — store label pointing to this slot
                // Convert to upper for case-insensitive lookup
                for (auto& ch : word) ch = static_cast<char>(std::toupper((unsigned char)ch));
                labels_[word] = auto_ln;
                program_[auto_ln] = ""; // empty body
                // If there's code after the colon on same line, store it too
                ++q; // skip ':'
                while (q < line.size() && (line[q]==' '||line[q]=='\t')) ++q;
                if (q < line.size() && line[q] != '\'' && line[q] != '\0') {
                    // code after label on same line: place on same line number
                    program_[auto_ln] = strip_comment(line.substr(q));
                }
                auto_ln++;
                continue;
            }
        }

        // Normal line without line number
        program_[auto_ln] = strip_comment(line.substr(p));
        auto_ln++;
    }
    first_pass();
}

bool Basic::load_file(const std::string& path) {
    std::ifstream f(path);
    if (!f.is_open()) return false;
    std::ostringstream ss;
    ss << f.rdbuf();
    load_string(ss.str());
    return true;
}

// ─────────────────────────────────────────────────────────────────────────────
// First pass: collect FUNCTION/SUB/TYPE/CONST definitions
// ─────────────────────────────────────────────────────────────────────────────
void Basic::first_pass() {
    // Local lambda-style helper to parse "(a AS type, b$, ...)"
    auto parse_params = [&](Lexer& lx) -> std::vector<std::string> {
        std::vector<std::string> params;
        if (!lx.eat_ch('(')) return params;
        while (!lx.at_end() && lx.peek_ch() != ')') {
            lx.skip_ws();
            auto tok = lx.next_tok();
            if (tok.kind == Lexer::IDENT) {
                std::string pname = tok.text;
                if (lx.eat_kw("AS")) { lx.next_tok(); }
                params.push_back(pname);
            }
            lx.eat_ch(',');
        }
        lx.eat_ch(')');
        return params;
    };
    // We do one scan of all lines to collect:
    //  - FUNCTION/SUB headers -> procs_
    //  - END FUNCTION / END SUB -> sets end_line on latest open proc
    //  - TYPE headers -> types_
    //  - END TYPE
    //  - CONST
    std::string cur_proc; // name of currently open FUNCTION/SUB
    std::string cur_type; // name of currently open TYPE

    for (auto& kv : program_) {
        int ln = kv.first;
        const std::string& src = kv.second;
        if (src.empty()) continue;

        Lexer lx(src);
        lx.skip_ws();
        if (lx.at_end()) continue;

        // FUNCTION name(params)
        if (lx.peek_kw("FUNCTION")) {
            lx.eat_kw("FUNCTION");
            lx.skip_ws();
            auto tok = lx.next_tok();
            if (tok.kind == Lexer::IDENT) {
                ProcDef pd;
                pd.name = tok.text;
                pd.params = parse_params(lx);
                pd.start_line = ln;
                pd.end_line   = -1;
                pd.is_function = true;
                procs_[pd.name] = pd;
                cur_proc = pd.name;
            }
            continue;
        }
        // SUB name(params)
        if (lx.peek_kw("SUB")) {
            lx.eat_kw("SUB");
            lx.skip_ws();
            auto tok = lx.next_tok();
            if (tok.kind == Lexer::IDENT) {
                ProcDef pd;
                pd.name = tok.text;
                pd.params = parse_params(lx);
                pd.start_line = ln;
                pd.end_line   = -1;
                pd.is_function = false;
                procs_[pd.name] = pd;
                cur_proc = pd.name;
            }
            continue;
        }
        // END FUNCTION / END SUB
        if (lx.peek_kw("END")) {
            Lexer lx2(src);
            lx2.eat_kw("END");
            if (lx2.eat_kw("FUNCTION") || lx2.eat_kw("SUB")) {
                if (!cur_proc.empty()) {
                    procs_[cur_proc].end_line = ln;
                    cur_proc.clear();
                }
                continue;
            }
        }
        // TYPE name
        if (lx.peek_kw("TYPE")) {
            lx.eat_kw("TYPE");
            lx.skip_ws();
            auto tok = lx.next_tok();
            if (tok.kind == Lexer::IDENT) {
                TypeDef td;
                td.name = tok.text;
                types_[td.name] = td;
                cur_type = td.name;
            }
            continue;
        }
        // END TYPE
        if (lx.peek_kw("END")) {
            Lexer lx2(src);
            lx2.eat_kw("END");
            if (lx2.eat_kw("TYPE")) {
                cur_type.clear();
                continue;
            }
        }
        // Field inside TYPE: fieldname AS type
        if (!cur_type.empty() && !lx.peek_kw("END")) {
            auto tok = lx.next_tok();
            if (tok.kind == Lexer::IDENT) {
                std::string fname = tok.text;
                std::string ftype = "DOUBLE";
                if (lx.eat_kw("AS")) {
                    auto ttok = lx.next_tok();
                    ftype = ttok.text;
                }
                types_[cur_type].fields.push_back(fname);
                types_[cur_type].field_types[fname] = ftype;
            }
            continue;
        }
        // CONST name = value
        if (lx.peek_kw("CONST")) {
            lx.eat_kw("CONST");
            lx.skip_ws();
            auto tok = lx.next_tok();
            if (tok.kind == Lexer::IDENT) {
                std::string cname = tok.text;
                lx.eat_ch('=');
                // evaluate simple literal
                lx.skip_ws();
                auto vtok = lx.next_tok();
                Value cv;
                if (vtok.kind == Lexer::NUM_LIT) cv = Value(vtok.num);
                else if (vtok.kind == Lexer::STR_LIT) cv = Value(vtok.text);
                else if (vtok.kind == Lexer::PUNCT && vtok.text == "-") {
                    auto vt2 = lx.next_tok();
                    cv = Value(-vt2.num);
                }
                consts_[cname] = cv;
            }
            continue;
        }
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// Resolve GOTO/GOSUB target: line number or label name
// ─────────────────────────────────────────────────────────────────────────────
int Basic::resolve_target(Lexer& lx, int linenum) {
    lx.skip_ws();
    // Numeric target
    if (std::isdigit((unsigned char)lx.peek_ch())) {
        Value v = eval_expr(lx);
        return (int)v.to_num();
    }
    // Label target
    auto tok = lx.next_tok();
    if (tok.kind == Lexer::IDENT) {
        auto it = labels_.find(tok.text);
        if (it != labels_.end()) return it->second;
        log("Undefined label: " + tok.text + " on line " + std::to_string(linenum));
    }
    return -1;
}

// ─────────────────────────────────────────────────────────────────────────────
// Block-scan helpers
// ─────────────────────────────────────────────────────────────────────────────
int Basic::find_wend_line(int from_line) {
    int depth = 1;
    for (auto it = program_.upper_bound(from_line); it != program_.end(); ++it) {
        Lexer lx(it->second);
        if (lx.eat_kw("WHILE")) ++depth;
        else if (lx.eat_kw("WEND")) { if (--depth == 0) { auto n=it;++n; return n!=program_.end()?n->first:0; } }
    }
    log("WHILE without matching WEND");
    return 0;
}

int Basic::find_loop_line(int from_line) {
    int depth = 1;
    for (auto it = program_.upper_bound(from_line); it != program_.end(); ++it) {
        Lexer lx(it->second);
        if (lx.eat_kw("DO")) ++depth;
        else if (lx.eat_kw("LOOP")) { if (--depth == 0) return it->first; }
    }
    log("DO without matching LOOP");
    return 0;
}

int Basic::find_next_line(const std::string& var, int from_line) {
    for (auto it = program_.upper_bound(from_line); it != program_.end(); ++it) {
        Lexer lx(it->second);
        if (lx.eat_kw("NEXT")) {
            lx.skip_ws();
            if (lx.at_end()) return it->first; // bare NEXT
            auto tok = lx.peek_tok();
            if (tok.kind == Lexer::IDENT && tok.text == var) return it->first;
            if (var.empty()) return it->first;
        }
    }
    return 0;
}

int Basic::find_end_if_line(int from_line) {
    int depth = 1;
    for (auto it = program_.upper_bound(from_line); it != program_.end(); ++it) {
        const std::string& src = it->second;
        Lexer lx(src);
        lx.skip_ws();
        if (lx.peek_kw("IF")) { ++depth; continue; }
        if (lx.peek_kw("END")) {
            Lexer lx2(src); lx2.eat_kw("END");
            if (lx2.eat_kw("IF")) { if (--depth == 0) { auto n=it;++n; return n!=program_.end()?n->first:0; } }
        }
    }
    log("IF without END IF");
    return 0;
}

// Find next ELSEIF / ELSE / END IF at same depth=1 (for IF false-skip)
// kind: 0=ELSEIF, 1=ELSE, 2=END IF
int Basic::find_next_if_branch(int from_line, int& kind) {
    int depth = 1;
    for (auto it = program_.upper_bound(from_line); it != program_.end(); ++it) {
        const std::string& src = it->second;
        if (src.empty()) continue;
        Lexer lx(src);
        lx.skip_ws();

        if (lx.peek_kw("IF")) {
            // Count nested IF only if it's a block IF (THEN at EOL)
            Lexer lx2(src);
            lx2.eat_kw("IF");
            // scan past condition to THEN
            int paren = 0;
            while (!lx2.at_end()) {
                lx2.skip_ws();
                if (lx2.peek_ch() == '(') { ++paren; lx2.next_tok(); }
                else if (lx2.peek_ch() == ')') { --paren; lx2.next_tok(); }
                else if (paren == 0 && lx2.eat_kw("THEN")) break;
                else lx2.next_tok();
            }
            lx2.skip_ws();
            if (lx2.at_end()) { ++depth; } // THEN at EOL => block IF
            continue;
        }
        if (depth == 1) {
            if (lx.peek_kw("ELSEIF") || lx.peek_kw("ELSE IF")) { kind=0; return it->first; }
            Lexer lxe(src);
            if (lxe.eat_kw("ELSE")) {
                lxe.skip_ws();
                if (lxe.at_end() || lxe.peek_ch()==':' || lxe.peek_ch()=='\'') { kind=1; return it->first; }
                if (lxe.peek_kw("IF")) { kind=0; return it->first; } // ELSE IF = ELSEIF
            }
        }
        if (lx.peek_kw("END")) {
            Lexer lx2(src); lx2.eat_kw("END");
            if (lx2.eat_kw("IF")) {
                --depth;
                if (depth == 0) { kind=2; return it->first; }
            }
        }
    }
    kind = 2;
    return 0;
}

int Basic::find_end_select_line(int from_line) {
    int depth = 1;
    for (auto it = program_.upper_bound(from_line); it != program_.end(); ++it) {
        Lexer lx(it->second);
        if (lx.eat_kw("SELECT")) ++depth;
        else {
            Lexer lx2(it->second); lx2.eat_kw("END");
            if (lx2.eat_kw("SELECT")) { if (--depth == 0) { auto n=it;++n; return n!=program_.end()?n->first:0; } }
        }
    }
    return 0;
}

int Basic::find_end_sub_line(int from_line) {
    for (auto it = program_.upper_bound(from_line); it != program_.end(); ++it) {
        Lexer lx(it->second); lx.eat_kw("END");
        if (lx.eat_kw("SUB")) return it->first;
    }
    return 0;
}

int Basic::find_end_func_line(int from_line) {
    for (auto it = program_.upper_bound(from_line); it != program_.end(); ++it) {
        Lexer lx(it->second); lx.eat_kw("END");
        if (lx.eat_kw("FUNCTION")) return it->first;
    }
    return 0;
}

// ─────────────────────────────────────────────────────────────────────────────
// Expression evaluator
// ─────────────────────────────────────────────────────────────────────────────
Basic::Value Basic::call_proc(const std::string& name, std::vector<Value> args, int call_line) {
    auto it = procs_.find(name);
    if (it == procs_.end()) {
        log("Undefined procedure: " + name + " on line " + std::to_string(call_line));
        return Value(0.0);
    }
    const ProcDef& pd = it->second;

    // Build call frame
    CallFrame cf;
    cf.proc_name   = pd.name;
    cf.is_function = pd.is_function;
    cf.exiting     = false;
    cf.return_value= Value(0.0);

    // Bind parameters
    for (std::size_t i = 0; i < pd.params.size(); ++i) {
        cf.locals[pd.params[i]] = (i < args.size()) ? args[i] : Value(0.0);
    }

    // Find line after header (first body line)
    auto prog_it = program_.upper_bound(pd.start_line);
    int body_start = (prog_it != program_.end()) ? prog_it->first : 0;
    int end_line   = pd.end_line;

    // Determine return address: line after CALL site
    auto caller_it = program_.find(call_line);
    int ret_line = 0;
    if (caller_it != program_.end()) {
        ++caller_it;
        ret_line = (caller_it != program_.end()) ? caller_it->first : 0;
    }
    cf.return_line = ret_line;

    frame_stack_.push_back(cf);

    // Run body
    auto run_it = program_.lower_bound(body_start);
    while (run_it != program_.end() && !interrupted_) {
        if (end_line > 0 && run_it->first >= end_line) break;

        int jmp = exec_line(run_it->first, run_it->second);

        // Check if EXIT SUB/FUNCTION was triggered
        if (!frame_stack_.empty() && frame_stack_.back().exiting) break;

        if (jmp == 0) break; // END inside proc
        if (jmp > 0) {
            // If jumping back to our end line or beyond, treat as return
            if (jmp >= end_line && end_line > 0) break;
            run_it = program_.lower_bound(jmp);
        } else {
            ++run_it;
        }
    }

    Value ret = frame_stack_.back().return_value;
    frame_stack_.pop_back();
    return ret;
}

Basic::Value Basic::eval_func(const std::string& fname, Lexer& lx) {
    lx.eat_ch('(');

    if (fname == "LEN") {
        Value v = eval_expr(lx); lx.eat_ch(')');
        return Value((double)v.to_str().size());
    }
    if (fname == "VAL") {
        Value v = eval_expr(lx); lx.eat_ch(')');
        return Value(v.to_num());
    }
    if (fname == "STR$") {
        Value v = eval_expr(lx); lx.eat_ch(')');
        return Value(v.to_str());
    }
    if (fname == "UPPER$") {
        Value v = eval_expr(lx); lx.eat_ch(')');
        std::string s = v.to_str();
        for (auto& c : s) c = static_cast<char>(std::toupper((unsigned char)c));
        return Value(s);
    }
    if (fname == "LOWER$") {
        Value v = eval_expr(lx); lx.eat_ch(')');
        std::string s = v.to_str();
        for (auto& c : s) c = static_cast<char>(std::tolower((unsigned char)c));
        return Value(s);
    }
    if (fname == "TRIM$") {
        Value v = eval_expr(lx); lx.eat_ch(')');
        std::string s = v.to_str();
        auto b = s.find_first_not_of(" \t\r\n");
        auto e = s.find_last_not_of(" \t\r\n");
        return Value(b==std::string::npos ? std::string("") : s.substr(b, e-b+1));
    }
    if (fname == "LEFT$") {
        Value s = eval_expr(lx); lx.eat_ch(',');
        Value n = eval_expr(lx); lx.eat_ch(')');
        std::string str = s.to_str();
        int len = (int)n.to_num(); if (len<0) len=0;
        if ((std::size_t)len > str.size()) len=(int)str.size();
        return Value(str.substr(0,len));
    }
    if (fname == "RIGHT$") {
        Value s = eval_expr(lx); lx.eat_ch(',');
        Value n = eval_expr(lx); lx.eat_ch(')');
        std::string str = s.to_str();
        int len = (int)n.to_num(); if (len<0) len=0;
        if ((std::size_t)len > str.size()) len=(int)str.size();
        return Value(str.substr(str.size()-len));
    }
    if (fname == "MID$") {
        Value s = eval_expr(lx); lx.eat_ch(',');
        Value p = eval_expr(lx);
        int mpos = (int)p.to_num() - 1;
        int len = -1;
        if (lx.eat_ch(',')) { Value l=eval_expr(lx); len=(int)l.to_num(); }
        lx.eat_ch(')');
        std::string str = s.to_str();
        if (mpos<0) mpos=0;
        if ((std::size_t)mpos >= str.size()) return Value(std::string(""));
        if (len<0) return Value(str.substr(mpos));
        return Value(str.substr(mpos,len));
    }
    if (fname == "CHR$") {
        Value v = eval_expr(lx); lx.eat_ch(')');
        char c = (char)(int)v.to_num();
        return Value(std::string(1,c));
    }
    if (fname == "ASC") {
        Value v = eval_expr(lx); lx.eat_ch(')');
        std::string s = v.to_str();
        return Value(s.empty() ? 0.0 : (double)(unsigned char)s[0]);
    }
    if (fname == "INSTR") {
        Value s = eval_expr(lx); lx.eat_ch(',');
        Value f2 = eval_expr(lx); lx.eat_ch(')');
        auto pos = s.to_str().find(f2.to_str());
        return Value(pos==std::string::npos ? 0.0 : (double)(pos+1));
    }
    if (fname == "INT") {
        Value v = eval_expr(lx); lx.eat_ch(')');
        return Value(std::floor(v.to_num()));
    }
    if (fname == "ABS") {
        Value v = eval_expr(lx); lx.eat_ch(')');
        return Value(std::fabs(v.to_num()));
    }
    if (fname == "SQR") {
        Value v = eval_expr(lx); lx.eat_ch(')');
        return Value(std::sqrt(v.to_num()));
    }
    if (fname == "RND") {
        lx.skip_ws();
        double n = 1.0;
        if (lx.peek_ch() != ')') { Value v=eval_expr(lx); n=v.to_num(); }
        lx.eat_ch(')');
        return Value(n*(std::rand()/(RAND_MAX+1.0)));
    }
    if (fname == "LOG") {
        Value v = eval_expr(lx); lx.eat_ch(')');
        double x=v.to_num(); return Value(x>0?std::log(x):0.0);
    }
    if (fname == "EXP") {
        Value v = eval_expr(lx); lx.eat_ch(')');
        return Value(std::exp(v.to_num()));
    }
    if (fname == "SIN") {
        Value v = eval_expr(lx); lx.eat_ch(')');
        return Value(std::sin(v.to_num()));
    }
    if (fname == "COS") {
        Value v = eval_expr(lx); lx.eat_ch(')');
        return Value(std::cos(v.to_num()));
    }
    if (fname == "TAN") {
        Value v = eval_expr(lx); lx.eat_ch(')');
        return Value(std::tan(v.to_num()));
    }
    if (fname == "SGN") {
        Value v = eval_expr(lx); lx.eat_ch(')');
        double x=v.to_num();
        return Value(x>0?1.0:(x<0?-1.0:0.0));
    }
    if (fname == "MAX") {
        Value a = eval_expr(lx); lx.eat_ch(',');
        Value b = eval_expr(lx); lx.eat_ch(')');
        return Value(a.to_num()>=b.to_num()?a.to_num():b.to_num());
    }
    if (fname == "MIN") {
        Value a = eval_expr(lx); lx.eat_ch(',');
        Value b = eval_expr(lx); lx.eat_ch(')');
        return Value(a.to_num()<=b.to_num()?a.to_num():b.to_num());
    }

    // ── REGEX functions ───────────────────────────────────────────────────────
    // REMATCH(pattern$, str$) → 1 if pattern matches anywhere, 0 otherwise
    if (fname == "REMATCH") {
        Value pat = eval_expr(lx); lx.eat_ch(',');
        Value str = eval_expr(lx); lx.eat_ch(')');
        try {
            std::regex re(pat.to_str());
            return Value(std::regex_search(str.to_str(), re) ? 1.0 : 0.0);
        } catch (...) { return Value(0.0); }
    }
    // REFIND$(pattern$, str$) → first full match, or ""
    if (fname == "REFIND$") {
        Value pat = eval_expr(lx); lx.eat_ch(',');
        Value str = eval_expr(lx); lx.eat_ch(')');
        try {
            std::regex  re(pat.to_str());
            std::smatch m;
            std::string s = str.to_str();
            if (std::regex_search(s, m, re)) return Value(m[0].str());
        } catch (...) {}
        return Value(std::string(""));
    }
    // REALL$(pattern$, str$ [, sep$]) → all matches joined by sep$ (default ",")
    if (fname == "REALL$") {
        Value pat = eval_expr(lx); lx.eat_ch(',');
        Value str = eval_expr(lx);
        std::string sep = ",";
        if (lx.eat_ch(',')) { Value sv = eval_expr(lx); sep = sv.to_str(); }
        lx.eat_ch(')');
        try {
            std::regex   re(pat.to_str());
            std::string  text = str.to_str();
            auto         beg  = std::sregex_iterator(text.begin(), text.end(), re);
            std::string  result;
            bool         first = true;
            for (auto it = beg; it != std::sregex_iterator(); ++it) {
                if (!first) result += sep;
                result += (*it)[0].str();
                first = false;
            }
            return Value(result);
        } catch (...) {}
        return Value(std::string(""));
    }
    // RESUB$(pattern$, repl$, str$) → replace first match (ECMAScript replacement syntax)
    if (fname == "RESUB$") {
        Value pat  = eval_expr(lx); lx.eat_ch(',');
        Value repl = eval_expr(lx); lx.eat_ch(',');
        Value str  = eval_expr(lx); lx.eat_ch(')');
        try {
            std::regex re(pat.to_str());
            return Value(std::regex_replace(str.to_str(), re, repl.to_str(),
                         std::regex_constants::format_first_only));
        } catch (...) { return str; }
    }
    // RESUBALL$(pattern$, repl$, str$) → replace all matches
    if (fname == "RESUBALL$") {
        Value pat  = eval_expr(lx); lx.eat_ch(',');
        Value repl = eval_expr(lx); lx.eat_ch(',');
        Value str  = eval_expr(lx); lx.eat_ch(')');
        try {
            std::regex re(pat.to_str());
            return Value(std::regex_replace(str.to_str(), re, repl.to_str()));
        } catch (...) { return str; }
    }
    // REGROUP$(pattern$, str$, n) → nth capture group (0=whole, 1=first group...)
    if (fname == "REGROUP$") {
        Value pat = eval_expr(lx); lx.eat_ch(',');
        Value str = eval_expr(lx); lx.eat_ch(',');
        Value n   = eval_expr(lx); lx.eat_ch(')');
        try {
            std::regex  re(pat.to_str());
            std::smatch m;
            std::string s = str.to_str();
            if (std::regex_search(s, m, re)) {
                int idx = (int)n.to_num();
                if (idx >= 0 && idx < (int)m.size()) return Value(m[idx].str());
            }
        } catch (...) {}
        return Value(std::string(""));
    }
    // RECOUNT(pattern$, str$) → number of non-overlapping matches
    if (fname == "RECOUNT") {
        Value pat = eval_expr(lx); lx.eat_ch(',');
        Value str = eval_expr(lx); lx.eat_ch(')');
        try {
            std::regex re(pat.to_str());
            std::string text = str.to_str();
            auto beg = std::sregex_iterator(text.begin(), text.end(), re);
            return Value((double)std::distance(beg, std::sregex_iterator()));
        } catch (...) {}
        return Value(0.0);
    }
    // ── MAP functions (MAP_HAS, MAP_SIZE) ─────────────────────────────────────
    if (fname == "MAP_HAS") {
        Value nm  = eval_expr(lx); lx.eat_ch(',');
        Value key = eval_expr(lx); lx.eat_ch(')');
        auto  it  = maps_.find(nm.to_str());
        return Value((it != maps_.end() && it->second.count(key.to_str())) ? 1.0 : 0.0);
    }
    if (fname == "MAP_SIZE") {
        Value nm = eval_expr(lx); lx.eat_ch(')');
        auto  it = maps_.find(nm.to_str());
        return Value(it != maps_.end() ? (double)it->second.size() : 0.0);
    }
    // ── QUEUE functions (QUEUE_SIZE, QUEUE_EMPTY) ─────────────────────────────
    if (fname == "QUEUE_SIZE") {
        Value nm = eval_expr(lx); lx.eat_ch(')');
        auto  it = queues_.find(nm.to_str());
        return Value(it != queues_.end() ? (double)it->second.size() : 0.0);
    }
    if (fname == "QUEUE_EMPTY") {
        Value nm = eval_expr(lx); lx.eat_ch(')');
        auto  it = queues_.find(nm.to_str());
        return Value((it == queues_.end() || it->second.empty()) ? 1.0 : 0.0);
    }

    // User-defined FUNCTION call
    if (procs_.count(fname)) {
        std::vector<Value> args;
        while (!lx.at_end() && lx.peek_ch() != ')') {
            args.push_back(eval_expr(lx));
            lx.eat_ch(',');
        }
        lx.eat_ch(')');
        return call_proc(fname, args, 0);
    }

    // Unknown: skip args
    int depth = 1;
    while (!lx.at_end() && depth > 0) {
        char c = lx.peek_ch();
        if (c == '(') ++depth;
        else if (c == ')') --depth;
        lx.next_tok();
    }
    log("Unknown function: " + fname);
    return Value(0.0);
}

Basic::Value Basic::eval_primary(Lexer& lx) {
    lx.skip_ws();
    if (lx.eat_ch('(')) {
        Value v = eval_expr(lx);
        lx.eat_ch(')');
        return v;
    }
    auto tok = lx.peek_tok();
    if (tok.kind == Lexer::NUM_LIT) { lx.next_tok(); return Value(tok.num); }
    if (tok.kind == Lexer::STR_LIT) { lx.next_tok(); return Value(tok.text); }
    if (tok.kind == Lexer::IDENT) {
        lx.next_tok();
        std::string name = tok.text;
        // Function call?
        if (lx.peek_ch() == '(') return eval_func(name, lx);
        // struct field: name.field
        lx.skip_ws();
        if (lx.peek_ch() == '.') {
            lx.eat_ch('.');
            auto ftok = lx.next_tok();
            std::string key = name + "." + ftok.text;
            return get_var(key);
        }
        return get_var(name);
    }
    return Value(0.0);
}

Basic::Value Basic::eval_unary(Lexer& lx) {
    lx.skip_ws();
    if (lx.eat_ch('-')) return Value(-eval_primary(lx).to_num());
    if (lx.eat_ch('+')) return eval_primary(lx);
    return eval_primary(lx);
}

Basic::Value Basic::eval_mul(Lexer& lx) {
    Value left = eval_unary(lx);
    for (;;) {
        lx.skip_ws();
        char c = lx.peek_ch();
        if (c == '*') { lx.eat_ch('*'); Value r=eval_unary(lx); left=Value(left.to_num()*r.to_num()); }
        else if (c == '/') { lx.eat_ch('/'); Value r=eval_unary(lx); double d=r.to_num(); left=Value(d!=0?left.to_num()/d:0.0); }
        else if (lx.peek_kw("MOD")) { lx.eat_kw("MOD"); Value r=eval_unary(lx); long long d=(long long)r.to_num(); left=Value(d!=0?(double)((long long)left.to_num()%d):0.0); }
        else break;
    }
    return left;
}

Basic::Value Basic::eval_add(Lexer& lx) {
    Value left = eval_mul(lx);
    for (;;) {
        lx.skip_ws();
        char c = lx.peek_ch();
        if (c == '+') {
            lx.eat_ch('+');
            Value r = eval_mul(lx);
            if (left.kind==Value::STR || r.kind==Value::STR)
                left = Value(left.to_str()+r.to_str());
            else
                left = Value(left.to_num()+r.to_num());
        } else if (c == '-') {
            lx.eat_ch('-');
            Value r = eval_mul(lx);
            left = Value(left.to_num()-r.to_num());
        } else break;
    }
    return left;
}

Basic::Value Basic::eval_cmp(Lexer& lx) {
    Value left = eval_add(lx);
    lx.skip_ws();
    std::string op;
    if (lx.eat_str("<>")) op="<>";
    else if (lx.eat_str("<=")) op="<=";
    else if (lx.eat_str(">=")) op=">=";
    else if (lx.eat_ch('<'))   op="<";
    else if (lx.eat_ch('>'))   op=">";
    else if (lx.eat_ch('='))   op="=";
    else return left;
    Value right = eval_add(lx);
    bool result = false;
    if (left.kind==Value::STR || right.kind==Value::STR) {
        std::string ls=left.to_str(), rs=right.to_str();
        if      (op=="=")  result=ls==rs;
        else if (op=="<>") result=ls!=rs;
        else if (op=="<")  result=ls<rs;
        else if (op==">")  result=ls>rs;
        else if (op=="<=") result=ls<=rs;
        else               result=ls>=rs;
    } else {
        double ln=left.to_num(), rn=right.to_num();
        if      (op=="=")  result=ln==rn;
        else if (op=="<>") result=ln!=rn;
        else if (op=="<")  result=ln<rn;
        else if (op==">")  result=ln>rn;
        else if (op=="<=") result=ln<=rn;
        else               result=ln>=rn;
    }
    return Value(result?1.0:0.0);
}

Basic::Value Basic::eval_not(Lexer& lx) {
    if (lx.eat_kw("NOT")) { Value v=eval_not(lx); return Value(v.to_bool()?0.0:1.0); }
    return eval_cmp(lx);
}

Basic::Value Basic::eval_and(Lexer& lx) {
    Value left = eval_not(lx);
    while (lx.eat_kw("AND")) {
        Value right = eval_not(lx);
        left = Value((left.to_bool()&&right.to_bool())?1.0:0.0);
    }
    return left;
}

Basic::Value Basic::eval_expr(Lexer& lx) {
    Value left = eval_and(lx);
    while (lx.eat_kw("OR")) {
        Value right = eval_and(lx);
        left = Value((left.to_bool()||right.to_bool())?1.0:0.0);
    }
    return left;
}

// ─────────────────────────────────────────────────────────────────────────────
// Commands
// ─────────────────────────────────────────────────────────────────────────────
int Basic::cmd_print(Lexer& lx, int) {
    std::string out;
    bool first = true;
    lx.skip_ws();
    while (!lx.at_end() && lx.peek_ch() != ':' && lx.peek_ch() != '\'') {
        if (!first) {
            if (lx.eat_ch(';')) { /* no separator */ }
            else if (lx.eat_ch(',')) { out += '\t'; }
            else break;
        }
        lx.skip_ws();
        if (lx.at_end() || lx.peek_ch()==':' || lx.peek_ch()=='\'') break;
        Value v = eval_expr(lx);
        out += v.to_str();
        first = false;
    }
    send_line(out + "\r\n");
    return -1;
}

int Basic::cmd_input(Lexer& lx, int) {
    lx.skip_ws();
    if (lx.peek_ch() == '"') {
        auto tok = lx.next_tok();
        send_line(tok.text);
        lx.eat_ch(';'); lx.eat_ch(',');
    }
    lx.skip_ws();
    auto tok = lx.next_tok();
    std::string varname = tok.text;
    std::string line;
    if (on_recv) line = on_recv(30000);
    while (!line.empty() && (line.back()=='\r'||line.back()=='\n')) line.pop_back();
    if (is_str_var(varname)) set_var(varname, Value(line));
    else { try { set_var(varname, Value(std::stod(line))); } catch(...) { set_var(varname, Value(0.0)); } }
    return -1;
}

int Basic::cmd_send(Lexer& lx, int ln) { return cmd_print(lx, ln); }

int Basic::cmd_recv(Lexer& lx, int) {
    lx.skip_ws();
    auto tok = lx.next_tok();
    std::string varname = tok.text;
    int timeout_ms = 10000;
    if (lx.eat_ch(',')) { Value t=eval_expr(lx); timeout_ms=(int)t.to_num(); }
    std::string line;
    if (on_recv) line = on_recv(timeout_ms);
    while (!line.empty() && (line.back()=='\r'||line.back()=='\n')) line.pop_back();
    if (is_str_var(varname)) set_var(varname, Value(line));
    else { try { set_var(varname, Value(std::stod(line))); } catch(...) { set_var(varname, Value(0.0)); } }
    return -1;
}

int Basic::cmd_let(Lexer& lx, const std::string& varname, int) {
    // Handle "var.field = expr"
    std::string fullname = varname;
    lx.skip_ws();
    if (lx.peek_ch() == '.') {
        lx.eat_ch('.');
        auto ftok = lx.next_tok();
        fullname = varname + "." + ftok.text;
    }
    lx.eat_ch('=');
    Value v = eval_expr(lx);
    set_var(fullname, std::move(v));
    return -1;
}

// ─────────────────────────────────────────────────────────────────────────────
// IF / ELSEIF / ELSE / END IF
// ─────────────────────────────────────────────────────────────────────────────
int Basic::cmd_if(Lexer& lx, int ln) {
    Value cond = eval_expr(lx);
    if (!lx.eat_kw("THEN")) {
        log("IF without THEN on line " + std::to_string(ln));
        return -1;
    }
    lx.skip_ws();

    // Block IF: nothing (or only comment) after THEN
    if (lx.at_end() || lx.peek_ch() == '\'') {
        return cmd_block_if_cond(cond.to_bool(), ln);
    }

    // Single-line IF THEN <stmts> [ELSE <stmts>]
    if (cond.to_bool()) {
        if (std::isdigit((unsigned char)lx.peek_ch())) {
            Value v = eval_expr(lx); return (int)v.to_num();
        }
        for (;;) {
            lx.skip_ws();
            if (lx.at_end() || lx.peek_kw("ELSE")) break;
            int jmp = exec_stmt(lx, ln);
            if (jmp != -1) return jmp;
            lx.skip_ws();
            if (lx.peek_kw("ELSE") || lx.at_end()) break;
            if (!lx.eat_ch(':')) break;
        }
        if (lx.eat_kw("ELSE")) lx.pos = lx.src.size();
        return -1;
    } else {
        // Skip to ELSE
        int depth = 0;
        while (!lx.at_end()) {
            lx.skip_ws();
            if (depth==0 && lx.peek_kw("ELSE")) { lx.eat_kw("ELSE"); break; }
            char c = lx.peek_ch();
            if      (c=='(') { ++depth; lx.next_tok(); }
            else if (c==')') { if(depth>0)--depth; lx.next_tok(); }
            else               { lx.next_tok(); }
        }
        if (lx.at_end()) return -1;
        lx.skip_ws();
        if (std::isdigit((unsigned char)lx.peek_ch())) { Value v=eval_expr(lx); return (int)v.to_num(); }
        for (;;) {
            lx.skip_ws();
            if (lx.at_end()) break;
            int jmp = exec_stmt(lx, ln);
            if (jmp != -1) return jmp;
            lx.skip_ws();
            if (lx.at_end() || !lx.eat_ch(':')) break;
        }
        return -1;
    }
}

// Shared helper for block IF start (used by both IF THEN<eol> and cmd_block_if)
int Basic::cmd_block_if_cond(bool cond, int ln) {
    if (cond) {
        IfFrame fr; fr.taken=true; fr.in_true=true; fr.if_line=ln;
        if_stack_.push_back(fr);
        return -1; // execute body
    } else {
        IfFrame fr; fr.taken=false; fr.in_true=false; fr.if_line=ln;
        if_stack_.push_back(fr);
        // Skip to first ELSEIF/ELSE/END IF
        int kind = 2;
        int target = find_next_if_branch(ln, kind);
        if (kind == 2) { if_stack_.pop_back(); return target; }
        return target; // jump to ELSEIF or ELSE line
    }
}

int Basic::cmd_block_if(Lexer& lx, int ln) {
    Value cond = eval_expr(lx);
    lx.eat_kw("THEN");
    return cmd_block_if_cond(cond.to_bool(), ln);
}

int Basic::cmd_elseif(Lexer& lx, int ln) {
    if (if_stack_.empty()) { log("ELSEIF without IF on line "+std::to_string(ln)); return -1; }
    IfFrame& fr = if_stack_.back();

    if (fr.taken) {
        // A prior branch already executed: skip to END IF
        if_stack_.pop_back();
        return find_end_if_line(ln);
    }

    // Evaluate new condition
    Value cond = eval_expr(lx);
    lx.eat_kw("THEN");
    if (cond.to_bool()) {
        fr.taken   = true;
        fr.in_true = true;
        return -1; // execute this branch
    } else {
        fr.in_true = false;
        int kind = 2;
        int target = find_next_if_branch(ln, kind);
        if (kind == 2) { if_stack_.pop_back(); return target; }
        return target;
    }
}

int Basic::cmd_else(int ln) {
    if (if_stack_.empty()) { log("ELSE without IF on line "+std::to_string(ln)); return -1; }
    IfFrame& fr = if_stack_.back();
    if (fr.taken) {
        // Prior branch ran: skip to END IF
        if_stack_.pop_back();
        return find_end_if_line(ln);
    }
    fr.taken   = true;
    fr.in_true = true;
    return -1; // execute ELSE body
}

int Basic::cmd_end_if(int ln) {
    if (if_stack_.empty()) { log("END IF without IF on line "+std::to_string(ln)); return -1; }
    if_stack_.pop_back();
    return -1;
}

// ─────────────────────────────────────────────────────────────────────────────
// GOTO / GOSUB / RETURN
// ─────────────────────────────────────────────────────────────────────────────
int Basic::cmd_goto(Lexer& lx, int ln) {
    int target = resolve_target(lx, ln);
    return target > 0 ? target : -1;
}

int Basic::cmd_gosub(Lexer& lx, int ln) {
    int target = resolve_target(lx, ln);
    auto it = program_.find(ln);
    if (it != program_.end()) {
        ++it;
        call_stack_.push_back(it != program_.end() ? it->first : 0);
    } else {
        call_stack_.push_back(0);
    }
    return target > 0 ? target : -1;
}

int Basic::cmd_return(int) {
    if (call_stack_.empty()) { log("RETURN without GOSUB"); return 0; }
    int ret = call_stack_.back(); call_stack_.pop_back();
    return ret == 0 ? 0 : ret;
}

// ─────────────────────────────────────────────────────────────────────────────
// FOR / NEXT / EXIT FOR
// ─────────────────────────────────────────────────────────────────────────────
int Basic::cmd_for(Lexer& lx, int ln) {
    auto tok = lx.next_tok();
    std::string var = tok.text;

    // ── FOR var$ IN src$ MATCH pat$ — regex match iterator ─────────────────
    if (lx.eat_kw("IN")) {
        Value src = eval_expr(lx);
        if (!lx.eat_kw("MATCH")) {
            log("FOR IN: expected MATCH on line " + std::to_string(ln));
            return -1;
        }
        Value pat = eval_expr(lx);

        // Collect all regex matches up front
        std::vector<std::string> matches;
        try {
            std::regex re(pat.to_str());
            std::string text = src.to_str();
            auto it  = std::sregex_iterator(text.begin(), text.end(), re);
            auto end = std::sregex_iterator();
            for (; it != end; ++it) matches.push_back((*it)[0].str());
        } catch (const std::regex_error& e) {
            log("FOR IN MATCH: regex error: " + std::string(e.what()) +
                " on line " + std::to_string(ln));
        }

        // Line immediately after this FOR statement = body start
        auto prog_it = program_.find(ln);
        int body = 0;
        if (prog_it != program_.end()) {
            ++prog_it;
            if (prog_it != program_.end()) body = prog_it->first;
        }

        if (matches.empty()) {
            // No matches: skip to line after NEXT
            int nxt = find_next_line(var, ln);
            if (nxt > 0) {
                auto it2 = program_.upper_bound(nxt);
                return it2 != program_.end() ? it2->first : 0;
            }
            return 0;
        }

        // Reuse existing frame if we somehow re-enter this FOR line
        for (auto& f : for_stack_) {
            if (f.var == var && f.is_match) {
                f.matches   = std::move(matches);
                f.match_idx = 0;
                f.body_line = body;
                set_var(var, Value(f.matches[0]));
                return -1;
            }
        }

        ForFrame fr;
        fr.var       = var;
        fr.to        = 0; fr.step = 0;
        fr.body_line = body;
        fr.is_match  = true;
        fr.matches   = std::move(matches);
        fr.match_idx = 0;
        set_var(var, Value(fr.matches[0]));
        for_stack_.push_back(std::move(fr));
        return -1;
    }

    // ── Numeric FOR var = from TO to [STEP step] ────────────────────────────
    lx.eat_ch('=');
    Value from_v = eval_expr(lx);
    lx.eat_kw("TO");
    Value to_v   = eval_expr(lx);
    double step  = 1.0;
    if (lx.eat_kw("STEP")) step = eval_expr(lx).to_num();

    set_var(var, from_v);

    auto it = program_.find(ln);
    int body = 0;
    if (it != program_.end()) { ++it; if (it != program_.end()) body = it->first; }

    // Reuse existing numeric frame (e.g. re-entry from nested scope)
    for (auto& f : for_stack_) {
        if (f.var == var && !f.is_match) {
            f.to = to_v.to_num(); f.step = step; f.body_line = body;
            return -1;
        }
    }

    ForFrame fr;
    fr.var       = var;
    fr.to        = to_v.to_num();
    fr.step      = step;
    fr.body_line = body;
    fr.is_match  = false;
    for_stack_.push_back(fr);
    return -1;
}

int Basic::cmd_next(Lexer& lx, int) {
    std::string var;
    lx.skip_ws();
    if (!lx.at_end() && lx.peek_ch() != ':') {
        auto tok = lx.peek_tok();
        if (tok.kind == Lexer::IDENT) { lx.next_tok(); var = tok.text; }
    }
    if (for_stack_.empty()) { log("NEXT without FOR"); return -1; }
    ForFrame& fr = for_stack_.back();
    if (!var.empty() && fr.var != var) { log("NEXT var mismatch"); return -1; }

    // ── Regex-match iterator NEXT ───────────────────────────────────────────
    if (fr.is_match) {
        ++fr.match_idx;
        if (fr.match_idx < fr.matches.size()) {
            set_var(fr.var, Value(fr.matches[fr.match_idx]));
            return fr.body_line; // jump back to loop body
        }
        for_stack_.pop_back();
        return -1; // fall through to line after NEXT
    }

    // ── Numeric FOR NEXT ────────────────────────────────────────────────────
    double newval = get_var(fr.var).to_num() + fr.step;
    set_var(fr.var, Value(newval));
    bool cont = (fr.step >= 0) ? (newval <= fr.to) : (newval >= fr.to);
    if (cont) return fr.body_line;
    for_stack_.pop_back();
    return -1;
}

int Basic::cmd_exit_for(int ln) {
    if (for_stack_.empty()) { log("EXIT FOR without FOR"); return -1; }
    std::string var = for_stack_.back().var;
    for_stack_.pop_back();
    // Jump to line after the matching NEXT
    int nxt = find_next_line(var, ln);
    if (nxt > 0) {
        auto it = program_.upper_bound(nxt);
        return it!=program_.end() ? it->first : 0;
    }
    return 0;
}

// ─────────────────────────────────────────────────────────────────────────────
// WHILE / WEND
// ─────────────────────────────────────────────────────────────────────────────
int Basic::cmd_while(Lexer& lx, int ln) {
    Value cond = eval_expr(lx);
    if (cond.to_bool()) {
        WhileFrame fr; fr.cond_line=ln;
        while_stack_.push_back(fr);
        return -1;
    }
    return find_wend_line(ln);
}

int Basic::cmd_wend(Lexer&, int) {
    if (while_stack_.empty()) { log("WEND without WHILE"); return -1; }
    int cond_line = while_stack_.back().cond_line;
    while_stack_.pop_back();
    return cond_line;
}

// ─────────────────────────────────────────────────────────────────────────────
// DO / LOOP / EXIT DO
// ─────────────────────────────────────────────────────────────────────────────
int Basic::cmd_do(Lexer& lx, int ln) {
    DoFrame fr;
    fr.do_line = ln;
    fr.has_pre_cond  = false;
    fr.pre_is_while  = false;

    lx.skip_ws();
    if (lx.eat_kw("WHILE")) {
        fr.has_pre_cond = true; fr.pre_is_while = true;
        fr.pre_expr = lx.rest_trimmed();
        // Evaluate now
        Lexer lx2(fr.pre_expr);
        Value cond = eval_expr(lx2);
        do_stack_.push_back(fr);
        if (!cond.to_bool()) {
            do_stack_.pop_back();
            int loop_ln = find_loop_line(ln);
            auto it = program_.upper_bound(loop_ln);
            return it!=program_.end() ? it->first : 0;
        }
        return -1;
    }
    if (lx.eat_kw("UNTIL")) {
        fr.has_pre_cond = true; fr.pre_is_while = false;
        fr.pre_expr = lx.rest_trimmed();
        Lexer lx2(fr.pre_expr);
        Value cond = eval_expr(lx2);
        do_stack_.push_back(fr);
        if (cond.to_bool()) {
            do_stack_.pop_back();
            int loop_ln = find_loop_line(ln);
            auto it = program_.upper_bound(loop_ln);
            return it!=program_.end() ? it->first : 0;
        }
        return -1;
    }
    // Bare DO
    do_stack_.push_back(fr);
    return -1;
}

int Basic::cmd_loop(Lexer& lx, int ln) {
    if (do_stack_.empty()) { log("LOOP without DO"); return -1; }
    DoFrame& fr = do_stack_.back();

    lx.skip_ws();
    // LOOP WHILE / LOOP UNTIL (post-condition)
    if (lx.eat_kw("WHILE")) {
        Value cond = eval_expr(lx);
        if (cond.to_bool()) return fr.do_line; // repeat
        do_stack_.pop_back(); return -1;
    }
    if (lx.eat_kw("UNTIL")) {
        Value cond = eval_expr(lx);
        if (!cond.to_bool()) return fr.do_line; // repeat
        do_stack_.pop_back(); return -1;
    }

    // Bare LOOP: if DO had pre-condition, re-evaluate
    if (fr.has_pre_cond) {
        Lexer lx2(fr.pre_expr);
        Value cond = eval_expr(lx2);
        bool repeat = fr.pre_is_while ? cond.to_bool() : !cond.to_bool();
        if (repeat) return fr.do_line;
        do_stack_.pop_back(); return -1;
    }

    // Infinite loop
    (void)ln;
    return fr.do_line;
}

int Basic::cmd_exit_do(int ln) {
    if (do_stack_.empty()) { log("EXIT DO without DO"); return -1; }
    do_stack_.pop_back();
    int loop_ln = find_loop_line(ln);
    auto it = program_.upper_bound(loop_ln);
    return it!=program_.end() ? it->first : 0;
}

// ─────────────────────────────────────────────────────────────────────────────
// SELECT CASE / CASE / END SELECT
// ─────────────────────────────────────────────────────────────────────────────
int Basic::cmd_select(Lexer& lx, int ln) {
    lx.eat_kw("CASE"); // consume CASE keyword after SELECT
    Value sel = eval_expr(lx);
    SelectFrame fr;
    fr.selector    = sel;
    fr.matched     = false;
    fr.in_case     = false;
    fr.select_line = ln;
    select_stack_.push_back(fr);
    // Jump to first CASE line
    for (auto it = program_.upper_bound(ln); it != program_.end(); ++it) {
        Lexer lx2(it->second);
        if (lx2.eat_kw("CASE")) return it->first;
        Lexer lx3(it->second); lx3.eat_kw("END");
        if (lx3.eat_kw("SELECT")) { select_stack_.pop_back(); auto n=it;++n; return n!=program_.end()?n->first:0; }
    }
    return -1;
}

int Basic::cmd_case(Lexer& lx, int ln) {
    if (select_stack_.empty()) { log("CASE without SELECT on line "+std::to_string(ln)); return -1; }
    SelectFrame& fr = select_stack_.back();

    // If we're coming from a matched case body, skip to END SELECT
    if (fr.matched) {
        int end_sel = find_end_select_line(fr.select_line);
        select_stack_.pop_back();
        return end_sel;
    }

    lx.skip_ws();

    // CASE ELSE
    if (lx.peek_kw("ELSE")) {
        fr.in_case = true; fr.matched = true;
        return -1;
    }

    // Evaluate CASE expressions
    bool matches = false;
    while (!lx.at_end() && lx.peek_ch()!=':' && lx.peek_ch()!='\'') {
        lx.skip_ws();
        // CASE IS <op> <expr>
        if (lx.eat_kw("IS")) {
            lx.skip_ws();
            std::string op;
            if (lx.eat_str("<>")) op="<>";
            else if (lx.eat_str("<=")) op="<=";
            else if (lx.eat_str(">=")) op=">=";
            else if (lx.eat_ch('<'))   op="<";
            else if (lx.eat_ch('>'))   op=">";
            else if (lx.eat_ch('='))   op="=";
            Value rhs = eval_expr(lx);
            double ls = fr.selector.to_num(), rs = rhs.to_num();
            bool r = false;
            if      (op=="=")  r=ls==rs; else if (op=="<>") r=ls!=rs;
            else if (op=="<")  r=ls<rs;  else if (op==">")  r=ls>rs;
            else if (op=="<=") r=ls<=rs; else if (op==">=") r=ls>=rs;
            if (r) matches=true;
        } else {
            Value v1 = eval_expr(lx);
            lx.skip_ws();
            // CASE a TO b
            if (lx.eat_kw("TO")) {
                Value v2 = eval_expr(lx);
                double sv=fr.selector.to_num();
                if (sv>=v1.to_num() && sv<=v2.to_num()) matches=true;
            } else {
                // Simple equality
                if (fr.selector.kind==Value::STR || v1.kind==Value::STR) {
                    if (fr.selector.to_str()==v1.to_str()) matches=true;
                } else {
                    if (fr.selector.to_num()==v1.to_num()) matches=true;
                }
            }
        }
        if (!lx.eat_ch(',')) break;
    }

    if (matches) {
        fr.in_case = true; fr.matched = true;
        return -1; // execute this CASE body
    }
    // Skip to next CASE (or END SELECT)
    int depth = 0;
    for (auto it = program_.upper_bound(ln); it != program_.end(); ++it) {
        Lexer lx2(it->second);
        if (lx2.eat_kw("SELECT")) { ++depth; continue; }
        if (depth == 0 && lx2.eat_kw("CASE")) return it->first;
        Lexer lx3(it->second); lx3.eat_kw("END");
        if (lx3.eat_kw("SELECT")) {
            if (depth==0) { select_stack_.pop_back(); auto n=it;++n; return n!=program_.end()?n->first:0; }
            --depth;
        }
    }
    return 0;
}

int Basic::cmd_end_select(int) {
    if (!select_stack_.empty()) select_stack_.pop_back();
    return -1;
}

// ─────────────────────────────────────────────────────────────────────────────
// SUB / FUNCTION / END SUB / END FUNCTION
// ─────────────────────────────────────────────────────────────────────────────
// At top level, SUB/FUNCTION lines are skipped (they are definitions)
int Basic::cmd_sub(Lexer& lx, int ln) {
    (void)lx;
    // Skip to END SUB
    return find_end_sub_line(ln) + 1; // line after END SUB
}

int Basic::cmd_function(Lexer& lx, int ln) {
    (void)lx;
    return find_end_func_line(ln) + 1;
}

int Basic::cmd_end_sub(int) {
    // If inside a CALL frame, signal exit
    if (!frame_stack_.empty()) { frame_stack_.back().exiting = true; }
    return -1;
}

int Basic::cmd_end_func(int) {
    if (!frame_stack_.empty()) { frame_stack_.back().exiting = true; }
    return -1;
}

int Basic::cmd_exit_sub(int) {
    if (!frame_stack_.empty()) { frame_stack_.back().exiting = true; }
    return -1;
}

int Basic::cmd_exit_func(int) {
    if (!frame_stack_.empty()) { frame_stack_.back().exiting = true; }
    return -1;
}

// ─────────────────────────────────────────────────────────────────────────────
// CALL name(args)
// ─────────────────────────────────────────────────────────────────────────────
int Basic::cmd_call(Lexer& lx, int ln) {
    lx.skip_ws();
    auto tok = lx.next_tok();
    std::string name = tok.text;
    std::vector<Value> args;
    if (lx.eat_ch('(')) {
        while (!lx.at_end() && lx.peek_ch()!=')') {
            args.push_back(eval_expr(lx));
            lx.eat_ch(',');
        }
        lx.eat_ch(')');
    } else {
        lx.skip_ws();
        while (!lx.at_end() && lx.peek_ch()!=':' && lx.peek_ch()!='\'') {
            args.push_back(eval_expr(lx));
            if (!lx.eat_ch(',')) break;
        }
    }
    call_proc(name, args, ln);
    return -1;
}

// ─────────────────────────────────────────────────────────────────────────────
// DIM / CONST / TYPE / END TYPE
// ─────────────────────────────────────────────────────────────────────────────
int Basic::cmd_dim(Lexer& lx, int) {
    // DIM varname [AS type] [, varname [AS type]]...
    while (!lx.at_end()) {
        lx.skip_ws();
        auto tok = lx.next_tok();
        if (tok.kind != Lexer::IDENT) break;
        std::string vname = tok.text;
        // Handle array notation: DIM a(n) — just register, no real arrays
        if (lx.eat_ch('(')) {
            while (!lx.at_end() && lx.peek_ch()!=')') lx.next_tok();
            lx.eat_ch(')');
        }
        // Optional AS type
        if (lx.eat_kw("AS")) {
            auto ttok = lx.next_tok();
            std::string tname = ttok.text;
            // Check if user-defined type
            if (types_.count(tname)) {
                // Initialize all fields
                for (auto& fname : types_[tname].fields) {
                    std::string key = vname + "." + fname;
                    std::string ftype = types_[tname].field_types[fname];
                    if (ftype=="STRING" || ftype=="STRING$")
                        type_vars_[key] = Value(std::string(""));
                    else
                        type_vars_[key] = Value(0.0);
                }
            }
            // For built-in types, just initialize with default
            else if (!frame_stack_.empty()) {
                auto& locs = frame_stack_.back().locals;
                if (locs.find(vname)==locs.end())
                    locs[vname] = is_str_var(vname) ? Value(std::string("")) : Value(0.0);
            }
        } else {
            // Initialize variable
            if (!frame_stack_.empty()) {
                auto& locs = frame_stack_.back().locals;
                if (locs.find(vname)==locs.end())
                    locs[vname] = is_str_var(vname) ? Value(std::string("")) : Value(0.0);
            } else {
                if (vars_.find(vname)==vars_.end())
                    vars_[vname] = is_str_var(vname) ? Value(std::string("")) : Value(0.0);
            }
        }
        if (!lx.eat_ch(',')) break;
    }
    return -1;
}

int Basic::cmd_const(Lexer& lx, int) {
    // CONST already handled in first_pass; skip at runtime
    lx.pos = lx.src.size();
    return -1;
}

int Basic::cmd_type(Lexer& lx, int ln) {
    // TYPE already collected in first_pass; skip to END TYPE
    (void)lx;
    for (auto it = program_.upper_bound(ln); it != program_.end(); ++it) {
        Lexer lx2(it->second); lx2.eat_kw("END");
        if (lx2.eat_kw("TYPE")) { auto n=it;++n; return n!=program_.end()?n->first:0; }
    }
    return -1;
}

int Basic::cmd_end_type(int) { return -1; }

// ─────────────────────────────────────────────────────────────────────────────
// SLEEP / DB / HTTP / SOCK / EXEC / SEND_APRS / SEND_UI (unchanged from v1)
// ─────────────────────────────────────────────────────────────────────────────
int Basic::cmd_sleep(Lexer& lx, int) {
    Value ms = eval_expr(lx);
    usleep((useconds_t)(ms.to_num() * 1000.0));
    return -1;
}

int Basic::cmd_dbopen(Lexer& lx, int) {
#ifdef HAVE_SQLITE3
    Value path = eval_expr(lx);
    if (db_) { sqlite3_close((sqlite3*)db_); db_ = nullptr; }
    int rc = sqlite3_open(path.to_str().c_str(), (sqlite3**)&db_);
    if (rc != SQLITE_OK) {
        log(std::string("DBOPEN error: ") + sqlite3_errmsg((sqlite3*)db_));
        sqlite3_close((sqlite3*)db_); db_ = nullptr;
    }
#else
    (void)lx; log("DBOPEN: SQLite not compiled in");
#endif
    return -1;
}

int Basic::cmd_dbclose(Lexer&, int) {
#ifdef HAVE_SQLITE3
    if (db_) { sqlite3_close((sqlite3*)db_); db_ = nullptr; }
#endif
    return -1;
}

int Basic::cmd_dbexec(Lexer& lx, int) {
#ifdef HAVE_SQLITE3
    if (!db_) { log("DBEXEC: no open database"); return -1; }
    Value sql = eval_expr(lx);
    char* errmsg = nullptr;
    int rc = sqlite3_exec((sqlite3*)db_, sql.to_str().c_str(), nullptr, nullptr, &errmsg);
    if (rc != SQLITE_OK) { log(std::string("DBEXEC error: ")+(errmsg?errmsg:"?")); sqlite3_free(errmsg); }
#else
    (void)lx; log("DBEXEC: SQLite not compiled in");
#endif
    return -1;
}

int Basic::cmd_dbquery(Lexer& lx, int) {
#ifdef HAVE_SQLITE3
    if (!db_) { log("DBQUERY: no open database"); return -1; }
    Value sql = eval_expr(lx); lx.skip_ws(); lx.eat_ch(',');
    auto tok = lx.next_tok(); std::string varname = tok.text;
    sqlite3_stmt* stmt = nullptr;
    int rc = sqlite3_prepare_v2((sqlite3*)db_, sql.to_str().c_str(), -1, &stmt, nullptr);
    std::string result;
    if (rc==SQLITE_OK && stmt) {
        if (sqlite3_step(stmt)==SQLITE_ROW) {
            const char* txt=(const char*)sqlite3_column_text(stmt,0);
            if (txt) result=txt;
        }
        sqlite3_finalize(stmt);
    } else { log(std::string("DBQUERY error: ")+sqlite3_errmsg((sqlite3*)db_)); }
    if (is_str_var(varname)) set_var(varname, Value(result));
    else { try { set_var(varname, Value(std::stod(result))); } catch(...) { set_var(varname, Value(0.0)); } }
#else
    (void)lx; log("DBQUERY: SQLite not compiled in");
#endif
    return -1;
}

int Basic::cmd_dbfetchall(Lexer& lx, int) {
#ifdef HAVE_SQLITE3
    if (!db_) { log("DBFETCHALL: no open database"); return -1; }
    Value sql = eval_expr(lx); lx.skip_ws(); lx.eat_ch(',');
    auto tok = lx.next_tok(); std::string varname = tok.text;
    std::string col_sep="\t", row_sep="\n";
    if (lx.eat_ch(',')) { Value cs=eval_expr(lx); col_sep=cs.to_str(); }
    if (lx.eat_ch(',')) { Value rs=eval_expr(lx); row_sep=rs.to_str(); }
    sqlite3_stmt* stmt = nullptr;
    int rc = sqlite3_prepare_v2((sqlite3*)db_, sql.to_str().c_str(), -1, &stmt, nullptr);
    std::string result;
    if (rc==SQLITE_OK && stmt) {
        bool first=true;
        while (sqlite3_step(stmt)==SQLITE_ROW) {
            if (!first) result+=row_sep; first=false;
            int nc=sqlite3_column_count(stmt);
            for (int c=0;c<nc;++c) {
                if (c>0) result+=col_sep;
                const char* txt=(const char*)sqlite3_column_text(stmt,c);
                if (txt) result+=txt;
            }
        }
        sqlite3_finalize(stmt);
    } else { log(std::string("DBFETCHALL error: ")+sqlite3_errmsg((sqlite3*)db_)); }
    if (is_str_var(varname)) set_var(varname, Value(result));
    else { try { set_var(varname, Value(std::stod(result))); } catch(...) { set_var(varname, Value(0.0)); } }
#else
    (void)lx; log("DBFETCHALL: SQLite not compiled in");
#endif
    return -1;
}

int Basic::cmd_httpget(Lexer& lx, int) {
    Value url=eval_expr(lx); lx.skip_ws(); lx.eat_ch(',');
    auto tok=lx.next_tok(); std::string varname=tok.text;
    std::string body=http_get(url.to_str());
    if (is_str_var(varname)) set_var(varname, Value(body));
    else { try { set_var(varname, Value(std::stod(body))); } catch(...) { set_var(varname, Value(0.0)); } }
    return -1;
}

int Basic::cmd_sockopen(Lexer& lx, int) {
    Value host=eval_expr(lx); lx.eat_ch(',');
    Value port=eval_expr(lx); lx.eat_ch(',');
    auto tok=lx.next_tok(); std::string varname=tok.text;
    int fd=tcp_connect(host.to_str(),(int)port.to_num());
    double handle=-1;
    if (fd>=0) { handle=next_sock_++; sockets_[(int)handle]=fd; }
    set_var(varname, Value(handle));
    return -1;
}

int Basic::cmd_sockclose(Lexer& lx, int) {
    Value h=eval_expr(lx);
    auto it=sockets_.find((int)h.to_num());
    if (it!=sockets_.end()) { ::close(it->second); sockets_.erase(it); }
    return -1;
}

int Basic::cmd_socksend(Lexer& lx, int) {
    Value h=eval_expr(lx); lx.eat_ch(',');
    Value data=eval_expr(lx);
    auto it=sockets_.find((int)h.to_num());
    if (it!=sockets_.end()) { std::string s=data.to_str(); ::write(it->second,s.data(),s.size()); }
    else log("SOCKSEND: invalid socket handle");
    return -1;
}

int Basic::cmd_sockrecv(Lexer& lx, int) {
    Value h=eval_expr(lx); lx.eat_ch(',');
    auto tok=lx.next_tok(); std::string varname=tok.text;
    int timeout_ms=5000;
    if (lx.eat_ch(',')) { Value t=eval_expr(lx); timeout_ms=(int)t.to_num(); }
    std::string line;
    auto it=sockets_.find((int)h.to_num());
    if (it!=sockets_.end()) sock_recv_line(it->second,line,timeout_ms);
    else log("SOCKRECV: invalid socket handle");
    if (is_str_var(varname)) set_var(varname, Value(line));
    else { try { set_var(varname, Value(std::stod(line))); } catch(...) { set_var(varname, Value(0.0)); } }
    return -1;
}

int Basic::cmd_exec(Lexer& lx, int) {
    Value cmd=eval_expr(lx); lx.skip_ws(); lx.eat_ch(',');
    auto tok=lx.next_tok(); std::string varname=tok.text;
    int timeout_ms=10000; bool capture_stderr=false;
    if (lx.eat_ch(',')) { Value t=eval_expr(lx); timeout_ms=(int)t.to_num();
        if (lx.eat_ch(',')) { Value s=eval_expr(lx); capture_stderr=s.to_bool(); } }
    std::string out=exec_cmd(cmd.to_str(),timeout_ms,capture_stderr);
    if (is_str_var(varname)) set_var(varname, Value(out));
    else { try { set_var(varname, Value(std::stod(out))); } catch(...) { set_var(varname, Value(0.0)); } }
    return -1;
}

int Basic::cmd_send_aprs(Lexer& lx, int) {
    Value info=eval_expr(lx);
    if (on_send_aprs) on_send_aprs(info.to_str());
    else log("SEND_APRS: no callback");
    return -1;
}

int Basic::cmd_send_ui(Lexer& lx, int) {
    Value dest=eval_expr(lx); lx.eat_ch(',');
    Value text=eval_expr(lx);
    if (on_send_ui) on_send_ui(dest.to_str(), text.to_str());
    else log("SEND_UI: no callback");
    return -1;
}

// =============================================================================
// MAP — named string-keyed associative arrays
// MAP_SET  mapname$, key$, value     — create or update entry
// MAP_GET  mapname$, key$, var$      — retrieve entry into var$ (or 0/"" if missing)
// MAP_DEL  mapname$, key$            — remove one entry
// MAP_KEYS mapname$, var$            — write comma-joined key list into var$
// MAP_CLEAR mapname$                 — remove all entries in named map
// MAP_HAS(mapname$, key$) → 1/0     — test membership (expression function)
// MAP_SIZE(mapname$)      → count   — number of entries (expression function)
// =============================================================================
int Basic::cmd_map_set(Lexer& lx, int) {
    Value nm  = eval_expr(lx); lx.eat_ch(',');
    Value key = eval_expr(lx); lx.eat_ch(',');
    Value val = eval_expr(lx);
    maps_[nm.to_str()][key.to_str()] = std::move(val);
    return -1;
}

int Basic::cmd_map_get(Lexer& lx, int) {
    Value nm  = eval_expr(lx); lx.eat_ch(',');
    Value key = eval_expr(lx); lx.eat_ch(',');
    auto  tok = lx.next_tok();
    const std::string& varname = tok.text;
    auto& m   = maps_[nm.to_str()];
    auto  it  = m.find(key.to_str());
    Value result = (it != m.end()) ? it->second
                 : (is_str_var(varname) ? Value(std::string("")) : Value(0.0));
    set_var(varname, std::move(result));
    return -1;
}

int Basic::cmd_map_del(Lexer& lx, int) {
    Value nm  = eval_expr(lx); lx.eat_ch(',');
    Value key = eval_expr(lx);
    auto  it  = maps_.find(nm.to_str());
    if (it != maps_.end()) it->second.erase(key.to_str());
    return -1;
}

int Basic::cmd_map_keys(Lexer& lx, int) {
    Value nm  = eval_expr(lx); lx.eat_ch(',');
    auto  tok = lx.next_tok();
    auto  it  = maps_.find(nm.to_str());
    std::string result;
    if (it != maps_.end()) {
        bool first = true;
        for (auto& kv : it->second) {
            if (!first) result += ",";
            result += kv.first;
            first = false;
        }
    }
    set_var(tok.text, Value(result));
    return -1;
}

int Basic::cmd_map_clear(Lexer& lx, int) {
    Value nm = eval_expr(lx);
    maps_.erase(nm.to_str());
    return -1;
}

// =============================================================================
// QUEUE — named FIFO queues
// QUEUE_PUSH  qname$, value    — enqueue at back
// QUEUE_POP   qname$, var$     — dequeue from front into var$ (or 0/"" if empty)
// QUEUE_PEEK  qname$, var$     — peek at front without removing
// QUEUE_CLEAR qname$           — discard all items
// QUEUE_SIZE(qname$)  → count  — number of items (expression function)
// QUEUE_EMPTY(qname$) → 1/0   — true when empty (expression function)
// =============================================================================
int Basic::cmd_queue_push(Lexer& lx, int) {
    Value nm  = eval_expr(lx); lx.eat_ch(',');
    Value val = eval_expr(lx);
    queues_[nm.to_str()].push_back(std::move(val));
    return -1;
}

int Basic::cmd_queue_pop(Lexer& lx, int) {
    Value nm  = eval_expr(lx); lx.eat_ch(',');
    auto  tok = lx.next_tok();
    const std::string& varname = tok.text;
    auto& q   = queues_[nm.to_str()];
    Value result = q.empty()
                 ? (is_str_var(varname) ? Value(std::string("")) : Value(0.0))
                 : q.front();
    if (!q.empty()) q.pop_front();
    set_var(varname, std::move(result));
    return -1;
}

int Basic::cmd_queue_peek(Lexer& lx, int) {
    Value nm  = eval_expr(lx); lx.eat_ch(',');
    auto  tok = lx.next_tok();
    const std::string& varname = tok.text;
    auto& q   = queues_[nm.to_str()];
    Value result = q.empty()
                 ? (is_str_var(varname) ? Value(std::string("")) : Value(0.0))
                 : q.front();
    set_var(varname, std::move(result));
    return -1;
}

int Basic::cmd_queue_clear(Lexer& lx, int) {
    Value nm = eval_expr(lx);
    queues_.erase(nm.to_str());
    return -1;
}

// ─────────────────────────────────────────────────────────────────────────────
// Statement dispatcher
// ─────────────────────────────────────────────────────────────────────────────
int Basic::exec_stmt(Lexer& lx, int linenum) {
    lx.skip_ws();
    if (lx.at_end() || lx.peek_ch()==':') return -1;
    // Inline comment
    if (lx.peek_ch()=='\'') { lx.pos=lx.src.size(); return -1; }

    auto tok = lx.peek_tok();
    if (tok.kind != Lexer::IDENT) return -1;
    std::string kw = tok.text;

    // ── Assignments ──────────────────────────────────────────────────────
    if (kw=="LET") { lx.next_tok(); auto v=lx.next_tok(); return cmd_let(lx, v.text, linenum); }

    // ── Comments ─────────────────────────────────────────────────────────
    if (kw=="REM") { lx.pos=lx.src.size(); return -1; }

    // ── I/O ──────────────────────────────────────────────────────────────
    if (kw=="PRINT")  { lx.next_tok(); return cmd_print(lx, linenum); }
    if (kw=="SEND")   { lx.next_tok(); return cmd_send(lx, linenum); }
    if (kw=="INPUT")  { lx.next_tok(); return cmd_input(lx, linenum); }
    if (kw=="RECV")   { lx.next_tok(); return cmd_recv(lx, linenum); }

    // ── Flow control ──────────────────────────────────────────────────────
    if (kw=="GOTO")   { lx.next_tok(); return cmd_goto(lx, linenum); }
    if (kw=="GOSUB")  { lx.next_tok(); return cmd_gosub(lx, linenum); }
    if (kw=="RETURN") { lx.next_tok(); return cmd_return(linenum); }

    // ── IF / ELSEIF / ELSE / END (various) ──────────────────────────────
    if (kw=="IF") {
        lx.next_tok();
        // Peek ahead: is THEN at EOL? => block IF
        // We do a two-pass: parse condition, check if THEN is followed by EOL
        std::size_t saved_pos = lx.pos;
        // Scan for THEN
        int paren=0; bool found_then=false;
        while (!lx.at_end()) {
            lx.skip_ws();
            if (lx.peek_ch()=='(') { ++paren; lx.next_tok(); }
            else if (lx.peek_ch()==')') { --paren; lx.next_tok(); }
            else if (paren==0 && lx.eat_kw("THEN")) { found_then=true; break; }
            else lx.next_tok();
        }
        bool block_if = found_then && lx.at_end();
        lx.pos = saved_pos; // restore

        if (block_if) return cmd_block_if(lx, linenum);
        return cmd_if(lx, linenum);
    }
    if (kw=="ELSEIF" || (kw=="ELSE" && lx.peek_kw("IF"))) {
        // Handle both "ELSEIF" and "ELSE IF"
        lx.next_tok();
        if (kw=="ELSE") lx.eat_kw("IF");
        return cmd_elseif(lx, linenum);
    }
    if (kw=="ELSE") { lx.next_tok(); return cmd_else(linenum); }
    if (kw=="END") {
        lx.next_tok();
        lx.skip_ws();
        if (lx.eat_kw("IF"))       return cmd_end_if(linenum);
        if (lx.eat_kw("SELECT"))   return cmd_end_select(linenum);
        if (lx.eat_kw("SUB"))      return cmd_end_sub(linenum);
        if (lx.eat_kw("FUNCTION")) return cmd_end_func(linenum);
        if (lx.eat_kw("TYPE"))     return cmd_end_type(linenum);
        // Bare END = stop
        return 0;
    }
    if (kw=="STOP") { lx.next_tok(); return 0; }

    // ── FOR / NEXT ────────────────────────────────────────────────────────
    if (kw=="FOR")  { lx.next_tok(); return cmd_for(lx, linenum); }
    if (kw=="NEXT") { lx.next_tok(); return cmd_next(lx, linenum); }

    // ── WHILE / WEND ──────────────────────────────────────────────────────
    if (kw=="WHILE") { lx.next_tok(); return cmd_while(lx, linenum); }
    if (kw=="WEND")  { lx.next_tok(); return cmd_wend(lx, linenum); }

    // ── DO / LOOP ─────────────────────────────────────────────────────────
    if (kw=="DO")   { lx.next_tok(); return cmd_do(lx, linenum); }
    if (kw=="LOOP") { lx.next_tok(); return cmd_loop(lx, linenum); }

    // ── SELECT CASE ───────────────────────────────────────────────────────
    if (kw=="SELECT") { lx.next_tok(); return cmd_select(lx, linenum); }
    if (kw=="CASE")   { lx.next_tok(); return cmd_case(lx, linenum); }

    // ── SUB / FUNCTION ────────────────────────────────────────────────────
    if (kw=="SUB")      { lx.next_tok(); return cmd_sub(lx, linenum); }
    if (kw=="FUNCTION") { lx.next_tok(); return cmd_function(lx, linenum); }
    if (kw=="CALL")     { lx.next_tok(); return cmd_call(lx, linenum); }

    // ── EXIT ─────────────────────────────────────────────────────────────
    if (kw=="EXIT") {
        lx.next_tok(); lx.skip_ws();
        if (lx.eat_kw("FOR"))      return cmd_exit_for(linenum);
        if (lx.eat_kw("DO"))       return cmd_exit_do(linenum);
        if (lx.eat_kw("SUB"))      return cmd_exit_sub(linenum);
        if (lx.eat_kw("FUNCTION")) return cmd_exit_func(linenum);
        return -1;
    }

    // ── DIM / CONST / TYPE ────────────────────────────────────────────────
    if (kw=="DIM")   { lx.next_tok(); return cmd_dim(lx, linenum); }
    if (kw=="CONST") { lx.next_tok(); return cmd_const(lx, linenum); }
    if (kw=="TYPE")  { lx.next_tok(); return cmd_type(lx, linenum); }

    // ── Sleep ─────────────────────────────────────────────────────────────
    if (kw=="SLEEP") { lx.next_tok(); return cmd_sleep(lx, linenum); }

    // ── Database ─────────────────────────────────────────────────────────
    if (kw=="DBOPEN")     { lx.next_tok(); return cmd_dbopen(lx, linenum); }
    if (kw=="DBCLOSE")    { lx.next_tok(); return cmd_dbclose(lx, linenum); }
    if (kw=="DBEXEC")     { lx.next_tok(); return cmd_dbexec(lx, linenum); }
    if (kw=="DBQUERY")    { lx.next_tok(); return cmd_dbquery(lx, linenum); }
    if (kw=="DBFETCHALL") { lx.next_tok(); return cmd_dbfetchall(lx, linenum); }

    // ── Net / Web / System ────────────────────────────────────────────────
    if (kw=="HTTPGET")   { lx.next_tok(); return cmd_httpget(lx, linenum); }
    if (kw=="SOCKOPEN")  { lx.next_tok(); return cmd_sockopen(lx, linenum); }
    if (kw=="SOCKCLOSE") { lx.next_tok(); return cmd_sockclose(lx, linenum); }
    if (kw=="SOCKSEND")  { lx.next_tok(); return cmd_socksend(lx, linenum); }
    if (kw=="SOCKRECV")  { lx.next_tok(); return cmd_sockrecv(lx, linenum); }
    if (kw=="EXEC")      { lx.next_tok(); return cmd_exec(lx, linenum); }

    // ── APRS / UI ─────────────────────────────────────────────────────────
    if (kw=="SEND_APRS") { lx.next_tok(); return cmd_send_aprs(lx, linenum); }
    if (kw=="SEND_UI")   { lx.next_tok(); return cmd_send_ui(lx, linenum); }

    // ── MAP ───────────────────────────────────────────────────────────────────
    if (kw=="MAP_SET")   { lx.next_tok(); return cmd_map_set  (lx, linenum); }
    if (kw=="MAP_GET")   { lx.next_tok(); return cmd_map_get  (lx, linenum); }
    if (kw=="MAP_DEL")   { lx.next_tok(); return cmd_map_del  (lx, linenum); }
    if (kw=="MAP_KEYS")  { lx.next_tok(); return cmd_map_keys (lx, linenum); }
    if (kw=="MAP_CLEAR") { lx.next_tok(); return cmd_map_clear(lx, linenum); }

    // ── QUEUE ─────────────────────────────────────────────────────────────────
    if (kw=="QUEUE_PUSH")  { lx.next_tok(); return cmd_queue_push (lx, linenum); }
    if (kw=="QUEUE_POP")   { lx.next_tok(); return cmd_queue_pop  (lx, linenum); }
    if (kw=="QUEUE_PEEK")  { lx.next_tok(); return cmd_queue_peek (lx, linenum); }
    if (kw=="QUEUE_CLEAR") { lx.next_tok(); return cmd_queue_clear(lx, linenum); }

    // ── Assignment without LET: VARNAME [.FIELD] = expr ──────────────────
    // (Must come before implicit SUB call so "FuncName = val" sets return value)
    if (tok.kind == Lexer::IDENT) {
        std::string varname = kw;
        std::size_t saved_pos = lx.pos;
        lx.next_tok(); // consume identifier
        lx.skip_ws();
        std::string fullname = varname;
        if (lx.peek_ch() == '.') {
            lx.eat_ch('.');
            auto ftok = lx.next_tok();
            fullname = varname + "." + ftok.text;
            lx.skip_ws();
        }
        if (lx.peek_ch() == '=') {
            lx.eat_ch('=');
            Value v = eval_expr(lx);
            set_var(fullname, std::move(v));
            return -1;
        }
        // Not an assignment — restore and try implicit SUB call
        lx.pos = saved_pos;
    }

    // ── Implicit call to SUB: name [arg, ...] ────────────────────────────
    if (procs_.count(kw)) {
        lx.next_tok();
        std::vector<Value> args;
        lx.skip_ws();
        while (!lx.at_end() && lx.peek_ch()!=':' && lx.peek_ch()!='\'') {
            args.push_back(eval_expr(lx));
            if (!lx.eat_ch(',')) break;
        }
        call_proc(kw, args, linenum);
        return -1;
    }

    log("Unknown statement: " + kw + " on line " + std::to_string(linenum));
    return -1;
}

int Basic::exec_line(int linenum, const std::string& src) {
    Lexer lx(src);
    int jmp = -1;
    for (;;) {
        lx.skip_ws();
        if (lx.at_end() || lx.peek_ch()=='\'') break;
        jmp = exec_stmt(lx, linenum);
        if (jmp != -1) return jmp;
        lx.skip_ws();
        if (!lx.eat_ch(':')) break;
    }
    return -1;
}

// ─────────────────────────────────────────────────────────────────────────────
// Missing declaration: cmd_block_if_cond is used above but declared in .cpp
// We need a forward declaration (since it is a private method).
// Actually it is declared as cmd_block_if in the header — let me adjust.
// cmd_block_if_cond is a helper, use it inline via a lambda-equivalent
// Actually I declared it inline in cmd_if as a shared implementation; but
// it is not in the header. Let me just make it a normal private method by
// adding it directly — since it is only called from cmd_if and cmd_block_if,
// and already defined above, this is fine in C++. The forward usage in
// exec_stmt and cmd_if works because the definition is in the same TU.
// ─────────────────────────────────────────────────────────────────────────────

// ─────────────────────────────────────────────────────────────────────────────
// Main run loop
// ─────────────────────────────────────────────────────────────────────────────
bool Basic::run() {
    interrupted_ = false;
    if (program_.empty()) return true;

    auto it = program_.begin();
    while (it != program_.end() && !interrupted_) {
        int jmp = exec_line(it->first, it->second);
        if (jmp == 0) return true;  // END
        if (jmp > 0) {
            it = program_.lower_bound(jmp);
            if (it == program_.end()) {
                log("Jump to undefined line " + std::to_string(jmp));
                return false;
            }
        } else {
            ++it;
        }
    }
    return true;
}

// ─────────────────────────────────────────────────────────────────────────────
// System utilities
// ─────────────────────────────────────────────────────────────────────────────
int Basic::tcp_connect(const std::string& host, int port) {
    char port_str[16];
    snprintf(port_str, sizeof(port_str), "%d", port);
    struct addrinfo hints, *res = nullptr;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family   = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    if (getaddrinfo(host.c_str(), port_str, &hints, &res) != 0) return -1;
    int fd = socket(res->ai_family, res->ai_socktype, res->ai_protocol);
    if (fd < 0) { freeaddrinfo(res); return -1; }
    if (connect(fd, res->ai_addr, res->ai_addrlen) < 0) {
        ::close(fd); freeaddrinfo(res); return -1;
    }
    freeaddrinfo(res);
    return fd;
}

bool Basic::sock_recv_line(int fd, std::string& out, int timeout_ms) {
    out.clear();
    struct timeval tv; tv.tv_sec=timeout_ms/1000; tv.tv_usec=(timeout_ms%1000)*1000;
    for (;;) {
        fd_set fds; FD_ZERO(&fds); FD_SET(fd, &fds);
        int n = select(fd+1, &fds, nullptr, nullptr, &tv);
        if (n<=0) return !out.empty();
        char c; ssize_t r=::read(fd, &c, 1);
        if (r<=0) return !out.empty();
        if (c=='\n') return true;
        if (c!='\r') out+=c;
    }
}

std::string Basic::http_get(const std::string& url) {
    std::string host, path; int port=80;
    std::string u=url;
    if (u.size()>=7 && u.substr(0,7)=="http://")  u=u.substr(7);
    else if (u.size()>=8 && u.substr(0,8)=="https://") u=u.substr(8);
    auto slash=u.find('/');
    if (slash==std::string::npos) { host=u; path="/"; }
    else { host=u.substr(0,slash); path=u.substr(slash); }
    auto colon=host.find(':');
    if (colon!=std::string::npos) {
        try { port=std::stoi(host.substr(colon+1)); } catch(...) {}
        host=host.substr(0,colon);
    }
    int fd=tcp_connect(host,port);
    if (fd<0) return "[ERROR: connect failed]";
    std::string req="GET "+path+" HTTP/1.0\r\nHost: "+host+"\r\nConnection: close\r\n\r\n";
    ::write(fd, req.data(), req.size());
    std::string resp; char buf[1024]; ssize_t n;
    while ((n=::read(fd, buf, sizeof(buf)))>0) resp.append(buf,n);
    ::close(fd);
    auto hend=resp.find("\r\n\r\n");
    if (hend!=std::string::npos) return resp.substr(hend+4);
    auto hend2=resp.find("\n\n");
    if (hend2!=std::string::npos) return resp.substr(hend2+2);
    return resp;
}

std::string Basic::exec_cmd(const std::string& cmd, int timeout_ms, bool capture_stderr) {
    int pipefd[2];
    if (pipe(pipefd)<0) return "[ERROR: pipe failed]";
    pid_t pid=fork();
    if (pid<0) { ::close(pipefd[0]); ::close(pipefd[1]); return "[ERROR: fork failed]"; }
    if (pid==0) {
        ::close(pipefd[0]);
        dup2(pipefd[1], STDOUT_FILENO);
        if (capture_stderr) dup2(pipefd[1], STDERR_FILENO);
        ::close(pipefd[1]);
        execl("/bin/sh","sh","-c",cmd.c_str(),nullptr);
        _exit(127);
    }
    ::close(pipefd[1]);
    std::string out; char buf[256];
    struct timeval tv; tv.tv_sec=timeout_ms/1000; tv.tv_usec=(timeout_ms%1000)*1000;
    for (;;) {
        fd_set fds; FD_ZERO(&fds); FD_SET(pipefd[0], &fds);
        int sel=select(pipefd[0]+1, &fds, nullptr, nullptr, &tv);
        if (sel<=0) { kill(pid,SIGKILL); ::close(pipefd[0]); waitpid(pid,nullptr,0);
            return out.empty() ? "[TIMEOUT]" : out+"\n[TIMEOUT]"; }
        ssize_t n=::read(pipefd[0], buf, sizeof(buf));
        if (n<=0) break;
        out.append(buf,n);
    }
    ::close(pipefd[0]); waitpid(pid,nullptr,0);
    while (!out.empty() && (out.back()=='\n'||out.back()=='\r')) out.pop_back();
    return out;
}
