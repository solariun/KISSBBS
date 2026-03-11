// =============================================================================
// basic.cpp — Tiny BASIC interpreter implementation  (C++11, POSIX)
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
    // Format: no trailing .0 for integer values
    if (num == (long long)num) return std::to_string((long long)num);
    std::ostringstream os;
    os << num;
    return os.str();
}

// ─────────────────────────────────────────────────────────────────────────────
// Lexer
// ─────────────────────────────────────────────────────────────────────────────
static bool ci_eq(char a, char b) {
    return std::tolower((unsigned char)a) == std::tolower((unsigned char)b);
}
static bool ci_starts(const std::string& src, std::size_t pos, const char* kw) {
    std::size_t n = std::strlen(kw);
    if (pos + n > src.size()) return false;
    for (std::size_t i = 0; i < n; ++i)
        if (!ci_eq(src[pos + i], kw[i])) return false;
    // kw must not be a prefix of a longer identifier
    if (pos + n < src.size()) {
        char next = src[pos + n];
        if (std::isalnum((unsigned char)next) || next == '_' || next == '$' || next == '%')
            return false;
    }
    return true;
}

void Basic::Lexer::skip_ws() {
    while (pos < src.size() && src[pos] == ' ') ++pos;
}

bool Basic::Lexer::eat_kw(const char* kw) {
    skip_ws();
    if (!ci_starts(src, pos, kw)) return false;
    pos += std::strlen(kw);
    return true;
}

bool Basic::Lexer::peek_kw(const char* kw) const {
    std::size_t p = pos;
    while (p < src.size() && src[p] == ' ') ++p;
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
        if (pos < src.size()) ++pos; // eat closing "
        t.kind = STR_LIT; t.text = std::move(s);
        return t;
    }

    // Number
    if (std::isdigit((unsigned char)c) || (c == '.' && pos+1 < src.size() && std::isdigit((unsigned char)src[pos+1]))) {
        std::string s;
        while (pos < src.size() && (std::isdigit((unsigned char)src[pos]) || src[pos]=='.'))
            s += src[pos++];
        // optional exponent
        if (pos < src.size() && (src[pos]=='e'||src[pos]=='E')) {
            s += src[pos++];
            if (pos < src.size() && (src[pos]=='+'||src[pos]=='-')) s += src[pos++];
            while (pos < src.size() && std::isdigit((unsigned char)src[pos])) s += src[pos++];
        }
        t.kind = NUM_LIT; t.text = s;
        try { t.num = std::stod(s); } catch(...) { t.num = 0; }
        return t;
    }

    // Identifier or keyword
    if (std::isalpha((unsigned char)c) || c == '_') {
        std::string s;
        while (pos < src.size() && (std::isalnum((unsigned char)src[pos]) || src[pos]=='_'))
            s += src[pos++];
        // String variable suffix
        if (pos < src.size() && src[pos] == '$') s += src[pos++];
        // Integer variable suffix
        else if (pos < src.size() && src[pos] == '%') s += src[pos++];
        // Upper-case for keywords
        for (auto& ch : s) ch = static_cast<char>(std::toupper((unsigned char)ch));
        t.kind = IDENT; t.text = std::move(s);
        return t;
    }

    // Two-char operators
    if (pos+1 < src.size()) {
        std::string two; two += c; two += src[pos+1];
        if (two=="<>" || two=="<=" || two>=">=") {
            // Check exact matches
            if (two == "<>" || two == "<=" || two == ">=") {
                pos += 2;
                t.kind = PUNCT; t.text = std::move(two);
                return t;
            }
        }
    }

    // Single-char punctuation
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
// Variable helpers
// ─────────────────────────────────────────────────────────────────────────────
bool Basic::is_str_var(const std::string& name) const {
    return !name.empty() && name.back() == '$';
}

Basic::Value Basic::get_var(const std::string& name) const {
    auto it = vars_.find(name);
    if (it == vars_.end()) {
        return is_str_var(name) ? Value(std::string("")) : Value(0.0);
    }
    return it->second;
}

void Basic::set_var(const std::string& name, Value v) {
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
    program_.clear(); vars_.clear();
    call_stack_.clear(); for_stack_.clear();
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

void Basic::load_string(const std::string& source) {
    clear();
    std::istringstream ss(source);
    std::string line;
    while (std::getline(ss, line)) {
        // trim CR
        while (!line.empty() && (line.back()=='\r'||line.back()=='\n')) line.pop_back();
        if (line.empty()) continue;
        // skip leading spaces
        std::size_t p = 0;
        while (p < line.size() && line[p] == ' ') ++p;
        // parse line number
        if (p < line.size() && std::isdigit((unsigned char)line[p])) {
            int linenum = 0;
            while (p < line.size() && std::isdigit((unsigned char)line[p]))
                linenum = linenum*10 + (line[p++]-'0');
            while (p < line.size() && line[p]==' ') ++p;
            program_[linenum] = line.substr(p);
        }
    }
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
// Expression evaluator (recursive descent)
// ─────────────────────────────────────────────────────────────────────────────
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
        int len = (int)n.to_num();
        if (len < 0) len = 0;
        if ((std::size_t)len > str.size()) len = (int)str.size();
        return Value(str.substr(0, len));
    }
    if (fname == "RIGHT$") {
        Value s = eval_expr(lx); lx.eat_ch(',');
        Value n = eval_expr(lx); lx.eat_ch(')');
        std::string str = s.to_str();
        int len = (int)n.to_num();
        if (len < 0) len = 0;
        if ((std::size_t)len > str.size()) len = (int)str.size();
        return Value(str.substr(str.size() - len));
    }
    if (fname == "MID$") {
        Value s = eval_expr(lx); lx.eat_ch(',');
        Value p = eval_expr(lx);
        int mpos = (int)p.to_num() - 1; // BASIC is 1-based
        int len = -1;
        if (lx.eat_ch(',')) { Value l = eval_expr(lx); len = (int)l.to_num(); }
        lx.eat_ch(')');
        std::string str = s.to_str();
        if (mpos < 0) mpos = 0;
        if ((std::size_t)mpos >= str.size()) return Value(std::string(""));
        if (len < 0) return Value(str.substr(mpos));
        return Value(str.substr(mpos, len));
    }
    if (fname == "CHR$") {
        Value v = eval_expr(lx); lx.eat_ch(')');
        char c = (char)(int)v.to_num();
        return Value(std::string(1, c));
    }
    if (fname == "ASC") {
        Value v = eval_expr(lx); lx.eat_ch(')');
        std::string s = v.to_str();
        return Value(s.empty() ? 0.0 : (double)(unsigned char)s[0]);
    }
    if (fname == "INSTR") {
        Value s = eval_expr(lx); lx.eat_ch(',');
        Value f2 = eval_expr(lx); lx.eat_ch(')');
        std::string::size_type p = s.to_str().find(f2.to_str());
        return Value(p == std::string::npos ? -1.0 : (double)(p+1)); // 1-based
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
    // Unknown function -- skip args
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
    // Parenthesised expression
    if (lx.eat_ch('(')) {
        Value v = eval_expr(lx);
        lx.eat_ch(')');
        return v;
    }
    auto tok = lx.peek_tok();
    if (tok.kind == Lexer::NUM_LIT) {
        lx.next_tok();
        return Value(tok.num);
    }
    if (tok.kind == Lexer::STR_LIT) {
        lx.next_tok();
        return Value(tok.text);
    }
    if (tok.kind == Lexer::IDENT) {
        lx.next_tok();
        std::string name = tok.text;
        // Function call?
        if (lx.peek_ch() == '(') {
            return eval_func(name, lx);
        }
        return get_var(name);
    }
    return Value(0.0);
}

Basic::Value Basic::eval_unary(Lexer& lx) {
    lx.skip_ws();
    if (lx.eat_ch('-')) {
        Value v = eval_primary(lx);
        return Value(-v.to_num());
    }
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
            // String concatenation if either side is a string
            if (left.kind == Value::STR || r.kind == Value::STR)
                left = Value(left.to_str() + r.to_str());
            else
                left = Value(left.to_num() + r.to_num());
        } else if (c == '-') {
            lx.eat_ch('-');
            Value r = eval_mul(lx);
            left = Value(left.to_num() - r.to_num());
        } else break;
    }
    return left;
}

Basic::Value Basic::eval_cmp(Lexer& lx) {
    Value left = eval_add(lx);
    lx.skip_ws();
    std::string op;
    if (lx.eat_str("<>")) op = "<>";
    else if (lx.eat_str("<=")) op = "<=";
    else if (lx.eat_str(">=")) op = ">=";
    else if (lx.eat_ch('<'))   op = "<";
    else if (lx.eat_ch('>'))   op = ">";
    else if (lx.eat_ch('='))   op = "=";
    else return left;

    Value right = eval_add(lx);
    bool result = false;
    if (left.kind == Value::STR || right.kind == Value::STR) {
        std::string ls = left.to_str(), rs = right.to_str();
        if      (op == "=")  result = ls == rs;
        else if (op == "<>") result = ls != rs;
        else if (op == "<")  result = ls <  rs;
        else if (op == ">")  result = ls >  rs;
        else if (op == "<=") result = ls <= rs;
        else                 result = ls >= rs;
    } else {
        double ln = left.to_num(), rn = right.to_num();
        if      (op == "=")  result = ln == rn;
        else if (op == "<>") result = ln != rn;
        else if (op == "<")  result = ln <  rn;
        else if (op == ">")  result = ln >  rn;
        else if (op == "<=") result = ln <= rn;
        else                 result = ln >= rn;
    }
    return Value(result ? 1.0 : 0.0);
}

Basic::Value Basic::eval_not(Lexer& lx) {
    if (lx.eat_kw("NOT")) {
        Value v = eval_not(lx);
        return Value(v.to_bool() ? 0.0 : 1.0);
    }
    return eval_cmp(lx);
}

Basic::Value Basic::eval_and(Lexer& lx) {
    Value left = eval_not(lx);
    while (lx.eat_kw("AND")) {
        Value right = eval_not(lx);
        left = Value((left.to_bool() && right.to_bool()) ? 1.0 : 0.0);
    }
    return left;
}

Basic::Value Basic::eval_expr(Lexer& lx) {
    Value left = eval_and(lx);
    while (lx.eat_kw("OR")) {
        Value right = eval_and(lx);
        left = Value((left.to_bool() || right.to_bool()) ? 1.0 : 0.0);
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
    while (!lx.at_end() && lx.peek_ch() != ':') {
        if (!first) {
            if (lx.eat_ch(';')) { /* no separator */ }
            else if (lx.eat_ch(',')) { out += '\t'; }
            else break;
        }
        lx.skip_ws();
        if (lx.at_end() || lx.peek_ch() == ':') break;
        Value v = eval_expr(lx);
        out += v.to_str();
        first = false;
    }
    send_line(out + "\r\n");
    return -1;
}

int Basic::cmd_input(Lexer& lx, int) {
    // Optional prompt string
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
    // trim CR/LF
    while (!line.empty() && (line.back()=='\r'||line.back()=='\n')) line.pop_back();
    if (is_str_var(varname)) set_var(varname, Value(line));
    else {
        try { set_var(varname, Value(std::stod(line))); }
        catch (...) { set_var(varname, Value(0.0)); }
    }
    return -1;
}

int Basic::cmd_send(Lexer& lx, int ln) { return cmd_print(lx, ln); }

int Basic::cmd_recv(Lexer& lx, int) {
    lx.skip_ws();
    auto tok = lx.next_tok();
    std::string varname = tok.text;
    int timeout_ms = 10000;
    lx.skip_ws();
    if (lx.eat_ch(',')) {
        lx.skip_ws();
        if (std::isdigit((unsigned char)lx.peek_ch())) {
            Value t = eval_expr(lx);
            timeout_ms = (int)t.to_num();
        }
    }
    std::string line;
    if (on_recv) line = on_recv(timeout_ms);
    while (!line.empty() && (line.back()=='\r'||line.back()=='\n')) line.pop_back();
    if (is_str_var(varname)) set_var(varname, Value(line));
    else {
        try { set_var(varname, Value(std::stod(line))); }
        catch (...) { set_var(varname, Value(0.0)); }
    }
    return -1;
}

int Basic::cmd_let(Lexer& lx, const std::string& varname, int) {
    lx.eat_ch('=');
    Value v = eval_expr(lx);
    set_var(varname, std::move(v));
    return -1;
}

int Basic::cmd_if(Lexer& lx, int ln) {
    Value cond = eval_expr(lx);
    if (!lx.eat_kw("THEN")) {
        log("IF without THEN on line " + std::to_string(ln));
        return -1;
    }
    lx.skip_ws();
    if (cond.to_bool()) {
        // THEN branch: if next token is a number, GOTO that line
        if (std::isdigit((unsigned char)lx.peek_ch())) {
            Value v = eval_expr(lx);
            return (int)v.to_num();
        }
        // Otherwise execute inline statement
        int jmp = exec_stmt(lx, ln);
        return jmp;
    } else {
        // Skip THEN branch, look for ELSE
        int depth = 0;
        while (!lx.at_end()) {
            lx.skip_ws();
            if (depth == 0 && lx.peek_kw("ELSE")) { lx.eat_kw("ELSE"); break; }
            char c = lx.peek_ch();
            if (c == '(') { ++depth; lx.next_tok(); }
            else if (c == ')') { if(depth>0) --depth; lx.next_tok(); }
            else if (c == '"') { lx.next_tok(); } // skip string literal
            else { lx.next_tok(); }
        }
        if (lx.at_end()) return -1;  // no ELSE
        lx.skip_ws();
        if (std::isdigit((unsigned char)lx.peek_ch())) {
            Value v = eval_expr(lx);
            return (int)v.to_num();
        }
        return exec_stmt(lx, ln);
    }
}

int Basic::cmd_goto(Lexer& lx, int) {
    Value v = eval_expr(lx);
    return (int)v.to_num();
}

int Basic::cmd_gosub(Lexer& lx, int ln) {
    Value v = eval_expr(lx);
    // Find the next line after current (return address)
    auto it = program_.find(ln);
    if (it != program_.end()) {
        ++it;
        call_stack_.push_back(it != program_.end() ? it->first : 0);
    } else {
        call_stack_.push_back(0);
    }
    return (int)v.to_num();
}

int Basic::cmd_return(int) {
    if (call_stack_.empty()) {
        log("RETURN without GOSUB");
        return 0;
    }
    int ret = call_stack_.back();
    call_stack_.pop_back();
    return ret == 0 ? 0 : ret;
}

int Basic::cmd_for(Lexer& lx, int ln) {
    auto tok = lx.next_tok();
    std::string var = tok.text;
    lx.eat_ch('=');
    Value from_v = eval_expr(lx);
    lx.eat_kw("TO");
    Value to_v = eval_expr(lx);
    double step = 1.0;
    if (lx.eat_kw("STEP")) step = eval_expr(lx).to_num();

    set_var(var, from_v);

    // body_line = line immediately after this FOR line
    auto it = program_.find(ln);
    int body = 0;
    if (it != program_.end()) {
        ++it;
        if (it != program_.end()) body = it->first;
    }
    // Push or update FOR frame
    for (auto& f : for_stack_) {
        if (f.var == var) {
            f.to = to_v.to_num(); f.step = step; f.body_line = body;
            return -1;
        }
    }
    ForFrame fr;
    fr.var = var; fr.to = to_v.to_num(); fr.step = step; fr.body_line = body;
    for_stack_.push_back(fr);
    return -1;
}

int Basic::cmd_next(Lexer& lx, int) {
    // Optional variable name
    std::string var;
    lx.skip_ws();
    if (!lx.at_end() && lx.peek_ch() != ':') {
        auto tok = lx.peek_tok();
        if (tok.kind == Lexer::IDENT) {
            lx.next_tok();
            var = tok.text;
        }
    }
    // Find matching FOR frame
    if (for_stack_.empty()) { log("NEXT without FOR"); return -1; }
    ForFrame& fr = for_stack_.back();
    if (!var.empty() && fr.var != var) { log("NEXT var mismatch"); return -1; }

    Value cur = get_var(fr.var);
    double newval = cur.to_num() + fr.step;
    set_var(fr.var, Value(newval));

    bool continue_loop = (fr.step >= 0) ? (newval <= fr.to) : (newval >= fr.to);
    if (continue_loop) return fr.body_line;
    for_stack_.pop_back();
    return -1; // fall through to next line
}

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
    (void)lx;
    log("DBOPEN: SQLite not compiled in (build with -DHAVE_SQLITE3 -lsqlite3)");
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
    if (rc != SQLITE_OK) {
        log(std::string("DBEXEC error: ") + (errmsg ? errmsg : "?"));
        sqlite3_free(errmsg);
    }
#else
    (void)lx;
    log("DBEXEC: SQLite not compiled in");
#endif
    return -1;
}

int Basic::cmd_dbquery(Lexer& lx, int) {
#ifdef HAVE_SQLITE3
    if (!db_) { log("DBQUERY: no open database"); return -1; }
    Value sql = eval_expr(lx);
    lx.skip_ws(); lx.eat_ch(',');
    auto tok = lx.next_tok();
    std::string varname = tok.text;

    sqlite3_stmt* stmt = nullptr;
    int rc = sqlite3_prepare_v2((sqlite3*)db_, sql.to_str().c_str(), -1, &stmt, nullptr);
    std::string result;
    if (rc == SQLITE_OK && stmt) {
        if (sqlite3_step(stmt) == SQLITE_ROW) {
            const char* txt = (const char*)sqlite3_column_text(stmt, 0);
            if (txt) result = txt;
        }
        sqlite3_finalize(stmt);
    } else {
        log(std::string("DBQUERY error: ") + sqlite3_errmsg((sqlite3*)db_));
    }
    if (is_str_var(varname)) set_var(varname, Value(result));
    else { try { set_var(varname, Value(std::stod(result))); } catch(...) { set_var(varname, Value(0.0)); } }
#else
    (void)lx;
    log("DBQUERY: SQLite not compiled in");
#endif
    return -1;
}

int Basic::cmd_httpget(Lexer& lx, int) {
    Value url = eval_expr(lx);
    lx.skip_ws(); lx.eat_ch(',');
    auto tok = lx.next_tok();
    std::string varname = tok.text;
    std::string body = http_get(url.to_str());
    if (is_str_var(varname)) set_var(varname, Value(body));
    else { try { set_var(varname, Value(std::stod(body))); } catch(...) { set_var(varname, Value(0.0)); } }
    return -1;
}

int Basic::cmd_sockopen(Lexer& lx, int) {
    Value host = eval_expr(lx); lx.eat_ch(',');
    Value port = eval_expr(lx); lx.eat_ch(',');
    auto tok = lx.next_tok(); std::string varname = tok.text;
    int fd = tcp_connect(host.to_str(), (int)port.to_num());
    double handle = -1;
    if (fd >= 0) {
        handle = next_sock_++;
        sockets_[(int)handle] = fd;
    }
    set_var(varname, Value(handle));
    return -1;
}

int Basic::cmd_sockclose(Lexer& lx, int) {
    Value h = eval_expr(lx);
    auto it = sockets_.find((int)h.to_num());
    if (it != sockets_.end()) { ::close(it->second); sockets_.erase(it); }
    return -1;
}

int Basic::cmd_socksend(Lexer& lx, int) {
    Value h = eval_expr(lx); lx.eat_ch(',');
    Value data = eval_expr(lx);
    auto it = sockets_.find((int)h.to_num());
    if (it != sockets_.end()) {
        std::string s = data.to_str();
        ::write(it->second, s.data(), s.size());
    } else {
        log("SOCKSEND: invalid socket handle");
    }
    return -1;
}

int Basic::cmd_sockrecv(Lexer& lx, int) {
    Value h = eval_expr(lx); lx.eat_ch(',');
    auto tok = lx.next_tok(); std::string varname = tok.text;
    int timeout_ms = 5000;
    if (lx.eat_ch(',')) { Value t = eval_expr(lx); timeout_ms = (int)t.to_num(); }
    std::string line;
    auto it = sockets_.find((int)h.to_num());
    if (it != sockets_.end()) sock_recv_line(it->second, line, timeout_ms);
    else log("SOCKRECV: invalid socket handle");
    if (is_str_var(varname)) set_var(varname, Value(line));
    else { try { set_var(varname, Value(std::stod(line))); } catch(...) { set_var(varname, Value(0.0)); } }
    return -1;
}

int Basic::cmd_exec(Lexer& lx, int) {
    Value cmd = eval_expr(lx);
    lx.skip_ws(); lx.eat_ch(',');
    auto tok = lx.next_tok(); std::string varname = tok.text;
    int timeout_ms = 10000;
    bool capture_stderr = false;
    if (lx.eat_ch(',')) {
        Value t = eval_expr(lx); timeout_ms = (int)t.to_num();
        if (lx.eat_ch(',')) { Value s = eval_expr(lx); capture_stderr = s.to_bool(); }
    }
    std::string out = exec_cmd(cmd.to_str(), timeout_ms, capture_stderr);
    if (is_str_var(varname)) set_var(varname, Value(out));
    else { try { set_var(varname, Value(std::stod(out))); } catch(...) { set_var(varname, Value(0.0)); } }
    return -1;
}

// ─────────────────────────────────────────────────────────────────────────────
// Statement dispatcher
// ─────────────────────────────────────────────────────────────────────────────
int Basic::exec_stmt(Lexer& lx, int linenum) {
    lx.skip_ws();
    if (lx.at_end() || lx.peek_ch() == ':') return -1;

    auto tok = lx.peek_tok();
    if (tok.kind != Lexer::IDENT) return -1;

    std::string kw = tok.text;

    // Assignments: varname = expr  or  LET varname = expr
    if (kw == "LET") { lx.next_tok(); auto v = lx.next_tok(); return cmd_let(lx, v.text, linenum); }
    if (kw == "REM" || kw == "'") { lx.pos = lx.src.size(); return -1; }
    if (kw == "PRINT")  { lx.next_tok(); return cmd_print(lx, linenum); }
    if (kw == "SEND")   { lx.next_tok(); return cmd_send(lx, linenum); }
    if (kw == "INPUT")  { lx.next_tok(); return cmd_input(lx, linenum); }
    if (kw == "RECV")   { lx.next_tok(); return cmd_recv(lx, linenum); }
    if (kw == "IF")     { lx.next_tok(); return cmd_if(lx, linenum); }
    if (kw == "GOTO")   { lx.next_tok(); return cmd_goto(lx, linenum); }
    if (kw == "GOSUB")  { lx.next_tok(); return cmd_gosub(lx, linenum); }
    if (kw == "RETURN") { lx.next_tok(); return cmd_return(linenum); }
    if (kw == "FOR")    { lx.next_tok(); return cmd_for(lx, linenum); }
    if (kw == "NEXT")   { lx.next_tok(); return cmd_next(lx, linenum); }
    if (kw == "SLEEP")  { lx.next_tok(); return cmd_sleep(lx, linenum); }
    if (kw == "END" || kw == "STOP") { return 0; }
    if (kw == "DBOPEN")   { lx.next_tok(); return cmd_dbopen(lx, linenum); }
    if (kw == "DBCLOSE")  { lx.next_tok(); return cmd_dbclose(lx, linenum); }
    if (kw == "DBEXEC")   { lx.next_tok(); return cmd_dbexec(lx, linenum); }
    if (kw == "DBQUERY")  { lx.next_tok(); return cmd_dbquery(lx, linenum); }
    if (kw == "HTTPGET")  { lx.next_tok(); return cmd_httpget(lx, linenum); }
    if (kw == "SOCKOPEN") { lx.next_tok(); return cmd_sockopen(lx, linenum); }
    if (kw == "SOCKCLOSE"){ lx.next_tok(); return cmd_sockclose(lx, linenum); }
    if (kw == "SOCKSEND") { lx.next_tok(); return cmd_socksend(lx, linenum); }
    if (kw == "SOCKRECV") { lx.next_tok(); return cmd_sockrecv(lx, linenum); }
    if (kw == "EXEC")     { lx.next_tok(); return cmd_exec(lx, linenum); }

    // Assignment without LET: VARNAME = expr
    if (tok.kind == Lexer::IDENT) {
        std::string varname = tok.text;
        lx.next_tok();
        lx.skip_ws();
        if (lx.peek_ch() == '=') {
            return cmd_let(lx, varname, linenum);
        }
    }

    log("Unknown statement: " + kw + " on line " + std::to_string(linenum));
    return -1;
}

int Basic::exec_line(int linenum, const std::string& src) {
    Lexer lx(src);
    int jmp = -1;
    for (;;) {
        lx.skip_ws();
        if (lx.at_end()) break;
        jmp = exec_stmt(lx, linenum);
        if (jmp != -1) return jmp;   // jump or END
        lx.skip_ws();
        if (!lx.eat_ch(':')) break;  // colon separates statements
    }
    return -1;
}

// ─────────────────────────────────────────────────────────────────────────────
// Main run loop
// ─────────────────────────────────────────────────────────────────────────────
bool Basic::run() {
    interrupted_ = false;
    if (program_.empty()) return true;

    auto it = program_.begin();
    while (it != program_.end() && !interrupted_) {
        int jmp = exec_line(it->first, it->second);
        if (jmp == 0) return true;   // END
        if (jmp > 0) {
            it = program_.lower_bound(jmp);
            if (it == program_.end() || it->first != jmp) {
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
    struct timeval tv;
    tv.tv_sec  = timeout_ms / 1000;
    tv.tv_usec = (timeout_ms % 1000) * 1000;
    for (;;) {
        fd_set fds; FD_ZERO(&fds); FD_SET(fd, &fds);
        int n = select(fd+1, &fds, nullptr, nullptr, &tv);
        if (n <= 0) return !out.empty();
        char c;
        ssize_t r = ::read(fd, &c, 1);
        if (r <= 0) return !out.empty();
        if (c == '\n') { return true; }
        if (c != '\r') out += c;
    }
}

std::string Basic::http_get(const std::string& url) {
    // Parse: http://host[:port]/path
    std::string host, path;
    int port = 80;
    std::string u = url;
    if (u.size() >= 7 && u.substr(0, 7) == "http://")  u = u.substr(7);
    else if (u.size() >= 8 && u.substr(0,8) == "https://") u = u.substr(8); // no TLS support
    auto slash = u.find('/');
    if (slash == std::string::npos) { host = u; path = "/"; }
    else { host = u.substr(0, slash); path = u.substr(slash); }
    auto colon = host.find(':');
    if (colon != std::string::npos) {
        try { port = std::stoi(host.substr(colon+1)); } catch(...) {}
        host = host.substr(0, colon);
    }
    int fd = tcp_connect(host, port);
    if (fd < 0) return "[ERROR: connect failed]";

    // Send HTTP/1.0 GET (simple, no keep-alive)
    std::string req = "GET " + path + " HTTP/1.0\r\nHost: " + host + "\r\nConnection: close\r\n\r\n";
    ::write(fd, req.data(), req.size());

    // Read all response
    std::string resp;
    char buf[1024];
    ssize_t n;
    while ((n = ::read(fd, buf, sizeof(buf))) > 0)
        resp.append(buf, n);
    ::close(fd);

    // Strip headers (find \r\n\r\n)
    auto hend = resp.find("\r\n\r\n");
    if (hend != std::string::npos) return resp.substr(hend + 4);
    auto hend2 = resp.find("\n\n");
    if (hend2 != std::string::npos) return resp.substr(hend2 + 2);
    return resp;
}

std::string Basic::exec_cmd(const std::string& cmd, int timeout_ms,
                             bool capture_stderr) {
    int pipefd[2];
    if (pipe(pipefd) < 0) return "[ERROR: pipe failed]";

    pid_t pid = fork();
    if (pid < 0) { ::close(pipefd[0]); ::close(pipefd[1]); return "[ERROR: fork failed]"; }

    if (pid == 0) {
        // Child
        ::close(pipefd[0]);
        dup2(pipefd[1], STDOUT_FILENO);
        if (capture_stderr) dup2(pipefd[1], STDERR_FILENO);
        ::close(pipefd[1]);
        execl("/bin/sh", "sh", "-c", cmd.c_str(), nullptr);
        _exit(127);
    }

    // Parent
    ::close(pipefd[1]);

    std::string out;
    char buf[256];
    struct timeval tv;
    tv.tv_sec  = timeout_ms / 1000;
    tv.tv_usec = (timeout_ms % 1000) * 1000;

    for (;;) {
        fd_set fds; FD_ZERO(&fds); FD_SET(pipefd[0], &fds);
        int sel = select(pipefd[0]+1, &fds, nullptr, nullptr, &tv);
        if (sel <= 0) {
            // Timeout or error: kill child
            kill(pid, SIGKILL);
            ::close(pipefd[0]);
            waitpid(pid, nullptr, 0);
            return out.empty() ? "[TIMEOUT]" : out + "\n[TIMEOUT]";
        }
        ssize_t n = ::read(pipefd[0], buf, sizeof(buf));
        if (n <= 0) break;
        out.append(buf, n);
    }

    ::close(pipefd[0]);
    waitpid(pid, nullptr, 0);
    // trim trailing newline
    while (!out.empty() && (out.back()=='\n'||out.back()=='\r')) out.pop_back();
    return out;
}
