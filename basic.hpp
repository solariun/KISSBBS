// =============================================================================
// basic.hpp — Tiny BASIC interpreter for BBS scripts  (C++11, POSIX)
//
// Language:
//   Variables:  name$ (string), name (double/integer)
//   Statements: PRINT, INPUT, LET, IF/THEN/ELSE, GOTO, GOSUB/RETURN,
//               FOR/NEXT, END, REM, SLEEP
//   BBS I/O:    SEND (-> AX.25 conn), RECV (<- AX.25 conn)
//   Database:   DBOPEN, DBCLOSE, DBEXEC, DBQUERY
//   Network:    SOCKOPEN, SOCKCLOSE, SOCKSEND, SOCKRECV
//   Web:        HTTPGET url$ var$
//   System:     EXEC cmd$ var$ [timeout_ms]
//   Functions:  LEN(), VAL(), STR$(), LEFT$(), RIGHT$(), MID$(),
//               UPPER$(), LOWER$(), TRIM$(), CHR$(), ASC(), INSTR()
//
// Usage:
//   Basic interp;
//   interp.on_send = [&](const std::string& s){ conn->send(s + "\r\n"); };
//   interp.on_recv = [&](int ms) -> std::string { ... };
//   interp.load_file("welcome.bas");
//   interp.run();
// =============================================================================
#pragma once

#include <functional>
#include <map>
#include <string>
#include <vector>

class Basic {
public:
    // ── Host callbacks (set before run()) ─────────────────────────────────
    std::function<void(const std::string&)>    on_send;  // PRINT / SEND output
    std::function<std::string(int timeout_ms)> on_recv;  // INPUT / RECV input
    std::function<void(const std::string&)>    on_log;   // error / debug log

    // ── Pre-defined variables ─────────────────────────────────────────────
    void set_str(const std::string& name, const std::string& val);
    void set_num(const std::string& name, double val);

    // ── Program loading ───────────────────────────────────────────────────
    bool load_file(const std::string& path);       // load from file
    void load_string(const std::string& source);   // load from string
    void add_line(int linenum, const std::string& text);
    void clear();

    // ── Execute.  Returns true on END/STOP, false on runtime error. ───────
    bool run();

    // ── Interrupt (call from another thread or signal) ────────────────────
    void interrupt() { interrupted_ = true; }

private:
    // ── Value type ────────────────────────────────────────────────────────
    struct Value {
        enum Kind { NUM, STR } kind;
        double      num;
        std::string str;
        Value() : kind(NUM), num(0.0) {}
        explicit Value(double d)      : kind(NUM), num(d) {}
        explicit Value(std::string s) : kind(STR), num(0.0), str(std::move(s)) {}
        double      to_num() const;
        std::string to_str() const;
        bool        to_bool()const { return kind==STR ? !str.empty() : num!=0.0; }
    };

    // ── FOR loop frame ────────────────────────────────────────────────────
    struct ForFrame { std::string var; double to, step; int body_line; };

    // ── Lexer ─────────────────────────────────────────────────────────────
    struct Lexer {
        const std::string& src;
        std::size_t pos;
        explicit Lexer(const std::string& s, std::size_t p = 0)
            : src(s), pos(p) {}

        void skip_ws();
        bool at_end() const { return pos >= src.size(); }
        char peek_ch() const { return pos < src.size() ? src[pos] : '\0'; }

        // Read one token (advances pos)
        enum TokKind { IDENT, NUM_LIT, STR_LIT, PUNCT, EOL };
        struct Token { TokKind kind; std::string text; double num; Token() : kind(EOL), num(0.0) {} };
        Token next_tok();
        Token peek_tok();
        bool  eat_kw(const char* kw);  // case-insensitive keyword match+consume
        bool  peek_kw(const char* kw) const;
        bool  eat_ch(char c);          // consume exact char
        bool  eat_str(const char* s);  // consume exact string
        std::string rest_trimmed();    // rest of input, trimmed
    };

    // ── Program storage ───────────────────────────────────────────────────
    std::map<int, std::string>   program_;     // line# -> source text
    std::map<std::string, Value> vars_;
    std::vector<int>             call_stack_;  // GOSUB return addresses
    std::vector<ForFrame>        for_stack_;
    volatile bool                interrupted_;

    // SQLite handle (void* to avoid sqlite3.h in this header)
    void* db_;

    // Open sockets: user handle (1,2,3...) -> POSIX fd
    std::map<int,int> sockets_;
    int next_sock_;

    Basic(const Basic&);
    Basic& operator=(const Basic&);

public:
    Basic() : interrupted_(false), db_(nullptr), next_sock_(1) {}
    ~Basic();

private:
    // ── Execution ─────────────────────────────────────────────────────────
    // Returns next line to jump to (0=END, -1=advance, >0=jump)
    int exec_line(int linenum, const std::string& src);
    int exec_stmt(Lexer& lx, int linenum);

    // ── Expression evaluator (recursive descent) ──────────────────────────
    Value eval_expr   (Lexer& lx);
    Value eval_or     (Lexer& lx);
    Value eval_and    (Lexer& lx);
    Value eval_not    (Lexer& lx);
    Value eval_cmp    (Lexer& lx);
    Value eval_add    (Lexer& lx);
    Value eval_mul    (Lexer& lx);
    Value eval_unary  (Lexer& lx);
    Value eval_primary(Lexer& lx);
    Value eval_func   (const std::string& fname, Lexer& lx);

    // ── Variable helpers ──────────────────────────────────────────────────
    Value  get_var(const std::string& name) const;
    void   set_var(const std::string& name, Value v);
    bool   is_str_var(const std::string& name) const; // ends with $

    // ── Command implementations ───────────────────────────────────────────
    int cmd_print  (Lexer& lx, int ln);
    int cmd_input  (Lexer& lx, int ln);
    int cmd_send   (Lexer& lx, int ln);
    int cmd_recv   (Lexer& lx, int ln);
    int cmd_let    (Lexer& lx, const std::string& varname, int ln);
    int cmd_if     (Lexer& lx, int ln);
    int cmd_goto   (Lexer& lx, int ln);
    int cmd_gosub  (Lexer& lx, int ln);
    int cmd_return (int ln);
    int cmd_for    (Lexer& lx, int ln);
    int cmd_next   (Lexer& lx, int ln);
    int cmd_sleep  (Lexer& lx, int ln);
    int cmd_dbopen (Lexer& lx, int ln);
    int cmd_dbclose(Lexer& lx, int ln);
    int cmd_dbexec (Lexer& lx, int ln);
    int cmd_dbquery(Lexer& lx, int ln);
    int cmd_httpget(Lexer& lx, int ln);
    int cmd_sockopen  (Lexer& lx, int ln);
    int cmd_sockclose (Lexer& lx, int ln);
    int cmd_socksend  (Lexer& lx, int ln);
    int cmd_sockrecv  (Lexer& lx, int ln);
    int cmd_exec   (Lexer& lx, int ln);

    // ── System utilities ──────────────────────────────────────────────────
    static std::string http_get(const std::string& url);
    static std::string exec_cmd(const std::string& cmd, int timeout_ms,
                                 bool capture_stderr);
    static int  tcp_connect(const std::string& host, int port);
    static bool sock_recv_line(int fd, std::string& out, int timeout_ms);

    void log(const std::string& msg) const {
        if (on_log) on_log(msg);
    }
    void send_line(const std::string& s) const {
        if (on_send) on_send(s);
    }
};
