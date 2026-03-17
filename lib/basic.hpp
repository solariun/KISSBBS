// =============================================================================
// basic.hpp — QBASIC-style interpreter for BBS scripts  (C++11, POSIX)
//
// Language:
//   Variables:  name$ (string), name (double/integer), user-defined TYPEs
//   Procedures: FUNCTION name(params) ... END FUNCTION  (returns value)
//               SUB name(params)      ... END SUB
//               CALL name(args)  or  name args
//   Types:      TYPE name / field AS type / END TYPE
//               DIM x AS typename  (or DIM x AS INTEGER/STRING/DOUBLE)
//   Constants:  CONST name = value
//   Statements: PRINT, INPUT, LET, GOTO label/linenum, GOSUB/RETURN,
//               FOR/NEXT/EXIT FOR, WHILE/WEND, DO/LOOP, END, STOP
//               IF/THEN/ELSEIF/ELSE/END IF  (block or single-line)
//               SELECT CASE / CASE / CASE ELSE / END SELECT
//               DIM, CONST, EXIT SUB, EXIT FUNCTION, EXIT DO
//               REM / '  (comments)
//               FOR var$ IN src$ MATCH pat$  (regex match iterator)
//   BBS I/O:    SEND, RECV
//   Database:   DBOPEN, DBCLOSE, DBEXEC, DBQUERY, DBFETCHALL
//   Network:    SOCKOPEN, SOCKCLOSE, SOCKSEND, SOCKRECV
//   Web:        HTTPGET url$ var$
//   System:     EXEC cmd$ var$ [timeout_ms]
//   APRS:       SEND_APRS info$
//               SEND_UI dest$, text$
//   Functions:  LEN(), VAL(), STR$(), LEFT$(), RIGHT$(), MID$(),
//               UPPER$(), LOWER$(), TRIM$(), CHR$(), ASC(), INSTR(),
//               INT(), ABS(), SQR(), RND(), LOG(), EXP(),
//               SIN(), COS(), TAN(), SGN(), MAX(), MIN()
//               REMATCH(), REFIND$(), REALL$(), RESUB$(), RESUBALL$(), REGROUP$(), RECOUNT()
//   Arrays:     DIM arr(n)           declare numeric array with n+1 slots (0..n)
//               DIM arr$(n)          declare string array
//               arr(i) = value       write element by numeric or string key
//               x = arr(i)          read element
//               FOR x IN arr         iterate all array values (numeric keys ascending)
//               ARRAY_SIZE(arr$)     number of elements in named array
//   Collections: MAP_SET, MAP_GET, MAP_HAS(), MAP_DEL, MAP_KEYS, MAP_SIZE(), MAP_CLEAR
//               QUEUE_PUSH, QUEUE_POP, QUEUE_PEEK, QUEUE_SIZE(), QUEUE_EMPTY(), QUEUE_CLEAR
//
// Usage:
//   Basic interp;
//   interp.on_send = [&](const std::string& s){ conn->send(s + "\r\n"); };
//   interp.on_recv = [&](int ms) -> std::string { ... };
//   interp.load_file("welcome.bas");
//   interp.run();
// =============================================================================
#pragma once

#include <deque>
#include <functional>
#include <map>
#include <set>
#include <string>
#include <vector>

class Basic {
public:
    // ── Host callbacks (set before run()) ─────────────────────────────────
    std::function<void(const std::string&)>              on_send;      // PRINT / SEND output
    std::function<std::string(int timeout_ms)>           on_recv;      // INPUT / RECV input
    std::function<void(const std::string&)>              on_log;       // error / debug log
    std::function<void(const std::string&)>              on_send_aprs; // SEND_APRS info$
    std::function<void(const std::string&,               // SEND_UI dest$, text$
                       const std::string&)>              on_send_ui;
    std::function<void(int linenum,                      // trace: called before each line
                       const std::string& src)>          on_trace;

    // ── Pre-defined variables ─────────────────────────────────────────────
    // name is uppercased internally to match the BASIC lexer.
    void set_str(const std::string& name, const std::string& val);
    void set_num(const std::string& name, double val);

    // ── Host-side MAP pre-population (call after load_file / load_string) ─
    // Injects entries into a named MAP the BASIC script can read with
    // MAP_GET / MAP_HAS / MAP_KEYS / MAP_SIZE.
    // map_name and key are stored as-is (string literals in scripts are not
    // uppercased, only BASIC identifiers are).
    void map_set(const std::string& map_name, const std::string& key,
                 const std::string& val);
    void map_set(const std::string& map_name, const std::string& key,
                 double val);
    void map_clear(const std::string& map_name);   // remove all entries

    // ── Host-side QUEUE pre-population (call after load_file / load_string) ─
    // Pushes values into a named FIFO the BASIC script can read with
    // QUEUE_POP / QUEUE_PEEK / QUEUE_SIZE / QUEUE_EMPTY.
    void queue_push(const std::string& queue_name, const std::string& val);
    void queue_push(const std::string& queue_name, double val);
    void queue_clear(const std::string& queue_name); // remove all elements

    // ── Program loading ───────────────────────────────────────────────────
    bool load_file(const std::string& path);       // load from file
    void load_string(const std::string& source);   // load from string
    bool include_file(const std::string& path);    // merge another .bas into current program
    void include_string(const std::string& src);   // merge source string into current program
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
    struct ForFrame {
        std::string var;
        double      to, step;
        int         body_line;
        // ── Regex-match iterator variant (FOR x$ IN src$ MATCH pat$) ──────────
        bool                     is_match  = false; // true when using IN/MATCH form
        std::vector<std::string> matches;            // all regex matches collected at start
        std::size_t              match_idx = 0;      // current position in matches
    };

    // ── WHILE loop frame ─────────────────────────────────────────────────
    struct WhileFrame { int cond_line; };

    // ── Block-IF frame ────────────────────────────────────────────────────
    // tracks which branch is active and whether we've already taken a branch
    struct IfFrame {
        bool taken;       // a true branch was already executed
        bool in_true;     // currently executing the true branch
        int  if_line;     // line of the IF statement (for error messages)
    };

    // ── DO / LOOP frame ───────────────────────────────────────────────────
    struct DoFrame {
        int  do_line;        // line of the DO statement
        bool has_pre_cond;   // DO WHILE / DO UNTIL
        bool pre_is_while;   // true=WHILE, false=UNTIL
        std::string pre_expr;// pre-condition source (re-evaluated at LOOP)
    };

    // ── SELECT CASE frame ─────────────────────────────────────────────────
    struct SelectFrame {
        Value  selector;    // expression from SELECT CASE expr
        bool   matched;     // a CASE branch already executed
        bool   in_case;     // currently inside a matching CASE
        int    select_line;
    };

    // ── FUNCTION / SUB definition ─────────────────────────────────────────
    struct ProcDef {
        std::string              name;
        std::vector<std::string> params;  // parameter names
        int                      start_line; // line of FUNCTION/SUB header
        int                      end_line;   // line of END FUNCTION/END SUB
        bool                     is_function;// true=FUNCTION, false=SUB
    };

    // ── Call frame (FUNCTION/SUB call) ────────────────────────────────────
    struct CallFrame {
        std::string              proc_name;    // for FUNCTION: also holds return value var
        std::map<std::string, Value> locals;   // local variables (including params)
        int                      return_line;  // line to return to
        bool                     is_function;
        Value                    return_value; // FUNCTION return value
        bool                     exiting;      // EXIT SUB / EXIT FUNCTION seen
    };

    // ── TYPE (struct) definition ──────────────────────────────────────────
    struct TypeDef {
        std::string name;
        std::vector<std::string> fields; // ordered field names
        std::map<std::string, std::string> field_types; // field -> type
    };

    // ── Lexer ─────────────────────────────────────────────────────────────
    struct Lexer {
        const std::string& src;
        std::size_t pos;
        explicit Lexer(const std::string& s, std::size_t p = 0)
            : src(s), pos(p) {}

        void skip_ws();
        bool at_end() const { return pos >= src.size(); }
        char peek_ch() const { return pos < src.size() ? src[pos] : '\0'; }

        enum TokKind { IDENT, NUM_LIT, STR_LIT, PUNCT, EOL };
        struct Token { TokKind kind; std::string text; double num; Token() : kind(EOL), num(0.0) {} };
        Token next_tok();
        Token peek_tok();
        bool  eat_kw(const char* kw);  // case-insensitive keyword match+consume
        bool  peek_kw(const char* kw) const;
        bool  eat_ch(char c);
        bool  eat_str(const char* s);
        std::string rest_trimmed();
    };

    // ── Program storage ───────────────────────────────────────────────────
    std::map<int, std::string>           program_;      // line# -> source text
    std::map<std::string, int>           labels_;       // label -> line#
    std::map<std::string, ProcDef>       procs_;        // name -> definition
    std::map<std::string, TypeDef>       types_;        // typename -> definition
    std::map<std::string, Value>         consts_;       // CONST name -> value
    std::map<std::string, Value>         vars_;         // global variables
    std::map<std::string, Value>         type_vars_;    // "varname.field" -> value

    // ── Execution stacks ──────────────────────────────────────────────────
    std::vector<int>          call_stack_;   // GOSUB return addresses
    std::vector<ForFrame>     for_stack_;
    std::vector<WhileFrame>   while_stack_;
    std::vector<IfFrame>      if_stack_;
    std::vector<DoFrame>      do_stack_;
    std::vector<SelectFrame>  select_stack_;
    std::vector<CallFrame>    frame_stack_;  // FUNCTION/SUB call frames

    // ── Auto line-number counter ───────────────────────────────────────────
    int  next_auto_line_;

    volatile bool interrupted_;

    // SQLite handle (void* to avoid sqlite3.h in this header)
    void* db_;

    // Open sockets: user handle (1,2,3...) -> POSIX fd
    std::map<int,int> sockets_;
    int next_sock_;

    // Named collections: MAP (string→Value) and QUEUE (FIFO of Values)
    std::map<std::string, std::map<std::string, Value>> maps_;
    std::map<std::string, std::deque<Value>>             queues_;

    // Arrays: names of variables declared as DIM arr(n); backed by maps_
    std::set<std::string> arrays_;

    Basic(const Basic&);
    Basic& operator=(const Basic&);

public:
    Basic() : next_auto_line_(1), interrupted_(false), db_(nullptr), next_sock_(1) {}
    ~Basic();

private:
    // ── Variable access (scope-aware) ────────────────────────────────────
    Value  get_var(const std::string& name) const;
    void   set_var(const std::string& name, Value v);
    bool   is_str_var(const std::string& name) const;

    // ── Execution ─────────────────────────────────────────────────────────
    // Returns next line to jump to (0=END, -1=advance, >0=jump)
    int exec_line(int linenum, const std::string& src);
    int exec_stmt(Lexer& lx, int linenum);

    // ── Program loading internals ─────────────────────────────────────────
    void parse_lines(const std::string& source,
                     std::set<std::string>& included, int depth);

    // ── Program scanning ──────────────────────────────────────────────────
    void first_pass();            // collect labels, procs, types, consts
    int  resolve_target(Lexer& lx, int linenum); // parse line# or label
    int  find_end_for_block(const std::string& kw, int from_line); // find END IF, WEND, etc.

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
    Value call_proc   (const std::string& name, std::vector<Value> args, int call_line);

    // ── Command implementations ───────────────────────────────────────────
    int cmd_print      (Lexer& lx, int ln);
    int cmd_input      (Lexer& lx, int ln);
    int cmd_send       (Lexer& lx, int ln);
    int cmd_recv       (Lexer& lx, int ln);
    int cmd_let        (Lexer& lx, const std::string& varname, int ln);
    int cmd_if         (Lexer& lx, int ln);          // single-line IF
    int cmd_block_if   (Lexer& lx, int ln);          // block IF (THEN at EOL)
    int cmd_block_if_cond(bool cond, int ln);        // shared block-IF entry
    int cmd_elseif     (Lexer& lx, int ln);
    int cmd_else       (int ln);
    int cmd_end_if     (int ln);
    int cmd_goto       (Lexer& lx, int ln);
    int cmd_gosub      (Lexer& lx, int ln);
    int cmd_return     (int ln);
    int cmd_for        (Lexer& lx, int ln);
    int cmd_next       (Lexer& lx, int ln);
    int cmd_exit_for   (int ln);
    int cmd_while      (Lexer& lx, int ln);
    int cmd_wend       (Lexer& lx, int ln);
    int cmd_do         (Lexer& lx, int ln);
    int cmd_loop       (Lexer& lx, int ln);
    int cmd_exit_do    (int ln);
    int cmd_select     (Lexer& lx, int ln);
    int cmd_case       (Lexer& lx, int ln);
    int cmd_end_select (int ln);
    int cmd_sub        (Lexer& lx, int ln);
    int cmd_function   (Lexer& lx, int ln);
    int cmd_end_sub    (int ln);
    int cmd_end_func   (int ln);
    int cmd_call       (Lexer& lx, int ln);
    int cmd_exit_sub   (int ln);
    int cmd_exit_func  (int ln);
    int cmd_dim        (Lexer& lx, int ln);
    int cmd_const      (Lexer& lx, int ln);
    int cmd_type       (Lexer& lx, int ln);
    int cmd_end_type   (int ln);
    int cmd_sleep      (Lexer& lx, int ln);
    int cmd_dbopen     (Lexer& lx, int ln);
    int cmd_dbclose    (Lexer& lx, int ln);
    int cmd_dbexec     (Lexer& lx, int ln);
    int cmd_dbquery    (Lexer& lx, int ln);
    int cmd_dbfetchall (Lexer& lx, int ln);
    int cmd_httpget    (Lexer& lx, int ln);
    int cmd_sockopen   (Lexer& lx, int ln);
    int cmd_sockclose  (Lexer& lx, int ln);
    int cmd_socksend   (Lexer& lx, int ln);
    int cmd_sockrecv   (Lexer& lx, int ln);
    int cmd_exec       (Lexer& lx, int ln);
    int cmd_send_aprs  (Lexer& lx, int ln);
    int cmd_send_ui    (Lexer& lx, int ln);
    // ── MAP commands ─────────────────────────────────────────────────────────
    int cmd_map_set   (Lexer& lx, int ln);
    int cmd_map_get   (Lexer& lx, int ln);
    int cmd_map_del   (Lexer& lx, int ln);
    int cmd_map_keys  (Lexer& lx, int ln);
    int cmd_map_clear (Lexer& lx, int ln);
    // ── QUEUE commands ───────────────────────────────────────────────────────
    int cmd_queue_push (Lexer& lx, int ln);
    int cmd_queue_pop  (Lexer& lx, int ln);
    int cmd_queue_peek (Lexer& lx, int ln);
    int cmd_queue_clear(Lexer& lx, int ln);

    // ── Block-scan helpers ────────────────────────────────────────────────
    int find_wend_line      (int from_line);
    int find_loop_line      (int from_line);
    int find_next_line      (const std::string& var, int from_line);
    int find_end_if_line    (int from_line);
    int find_end_select_line(int from_line);
    int find_end_sub_line   (int from_line);
    int find_end_func_line  (int from_line);
    // Find next ELSEIF/ELSE/END IF (returns line#, kind set to 0=ELSEIF,1=ELSE,2=ENDIF)
    int find_next_if_branch (int from_line, int& kind);

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
