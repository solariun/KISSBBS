' =============================================================================
' email.bas — BBS Email System (SQLite-backed, QBASIC dialect)
' Pre-filled vars: callsign$, bbs_name$, db_path$
' Commands: LIST  READ <n>  COMPOSE <to> <subj>  REPLY <n>  DELETE <n>  QUIT
' =============================================================================

' ── Database helpers ──────────────────────────────────────────────────────────

SUB DbInit
    DBOPEN db_path$
    DBEXEC "CREATE TABLE IF NOT EXISTS msgs (id INTEGER PRIMARY KEY AUTOINCREMENT, from_call TEXT NOT NULL, to_call TEXT NOT NULL, subject TEXT DEFAULT '', body TEXT DEFAULT '', read INTEGER DEFAULT 0, ts DATETIME DEFAULT (datetime('now')))"
END SUB

' ── String utilities ──────────────────────────────────────────────────────────

' Return first word of s$ (up to first space)
FUNCTION Word1$(s$)
    DIM p AS INTEGER
    p = INSTR(s$, " ")
    IF p > 0 THEN
        Word1$ = LEFT$(s$, p - 1)
    ELSE
        Word1$ = s$
    END IF
END FUNCTION

' Return everything after the first word (rest of string)
FUNCTION Rest$(s$)
    DIM p AS INTEGER
    p = INSTR(s$, " ")
    IF p > 0 THEN
        Rest$ = TRIM$(MID$(s$, p + 1))
    ELSE
        Rest$ = ""
    END IF
END FUNCTION

' Extract column n (1-based) from a pipe-delimited row
FUNCTION Col$(row$, n)
    DIM i AS INTEGER
    DIM cur$ AS STRING
    DIM p AS INTEGER
    cur$ = row$
    i = 1
    DO WHILE i < n
        p = INSTR(cur$, "|")
        IF p = 0 THEN
            Col$ = ""
            EXIT FUNCTION
        END IF
        cur$ = MID$(cur$, p + 1)
        i = i + 1
    LOOP
    p = INSTR(cur$, "|")
    IF p > 0 THEN
        Col$ = LEFT$(cur$, p - 1)
    ELSE
        Col$ = cur$
    END IF
END FUNCTION

' ── LIST — show messages addressed to this user ───────────────────────────────
SUB CmdList
    DIM tot$ AS STRING
    DIM unr$ AS STRING
    DIM rows$ AS STRING
    DIM pos AS INTEGER
    DIM tilde AS INTEGER
    DIM rowdata$ AS STRING

    DBQUERY "SELECT COUNT(*) FROM msgs WHERE to_call='" + callsign$ + "'", tot$
    DBQUERY "SELECT COUNT(*) FROM msgs WHERE to_call='" + callsign$ + "' AND read=0", unr$

    PRINT ""
    PRINT "Messages for " + callsign$ + ": " + tot$ + " total, " + unr$ + " unread"

    IF tot$ = "0" THEN
        PRINT " (no messages)"
        EXIT SUB
    END IF

    PRINT " ID  N  FROM         DATE              SUBJECT"
    PRINT "----+--+------------+-----------------+---------------------"

    DBFETCHALL "SELECT id, CASE WHEN read=0 THEN '*' ELSE ' ' END, from_call, substr(ts,1,16), subject FROM msgs WHERE to_call='" + callsign$ + "' ORDER BY id DESC LIMIT 20", rows$, "|", "~"

    IF rows$ = "" THEN EXIT SUB

    pos = 1
    DO WHILE pos <= LEN(rows$)
        tilde = INSTR(MID$(rows$, pos), "~")
        IF tilde = 0 THEN
            rowdata$ = MID$(rows$, pos)
            pos = LEN(rows$) + 1
        ELSE
            rowdata$ = MID$(rows$, pos, tilde - 1)
            pos = pos + tilde
        END IF

        ' Format: id|flag|from|date|subject
        DIM rid$ AS STRING
        DIM flag$ AS STRING
        DIM from$ AS STRING
        DIM ts$ AS STRING
        DIM subj$ AS STRING
        rid$  = Col$(rowdata$, 1)
        flag$ = Col$(rowdata$, 2)
        from$ = Col$(rowdata$, 3)
        ts$   = Col$(rowdata$, 4)
        subj$ = Col$(rowdata$, 5)

        PRINT " " + LEFT$(rid$ + "   ", 3) + " " + flag$ + "  " + LEFT$(from$ + "            ", 12) + " " + LEFT$(ts$ + "                 ", 17) + " " + LEFT$(subj$ + "                     ", 21)
    LOOP
    PRINT ""
END SUB

' ── READ <id> — display a message ─────────────────────────────────────────────
SUB CmdRead(args$)
    DIM rid$ AS STRING
    DIM chk$ AS STRING
    DIM mfrom$ AS STRING
    DIM mts$ AS STRING
    DIM msubj$ AS STRING
    DIM mbody$ AS STRING

    rid$ = TRIM$(args$)
    IF rid$ = "" THEN
        PRINT "Usage: READ <id>"
        EXIT SUB
    END IF

    DBQUERY "SELECT id FROM msgs WHERE id=" + rid$ + " AND to_call='" + callsign$ + "'", chk$
    IF chk$ = "" THEN
        PRINT "No message #" + rid$ + " for you."
        EXIT SUB
    END IF

    DBQUERY "SELECT from_call FROM msgs WHERE id=" + rid$, mfrom$
    DBQUERY "SELECT ts        FROM msgs WHERE id=" + rid$, mts$
    DBQUERY "SELECT subject   FROM msgs WHERE id=" + rid$, msubj$
    DBQUERY "SELECT body      FROM msgs WHERE id=" + rid$, mbody$

    PRINT ""
    PRINT "--- Message #" + rid$ + " ---"
    PRINT "From   : " + mfrom$
    PRINT "Date   : " + mts$
    PRINT "Subject: " + msubj$
    PRINT "---"
    PRINT mbody$
    PRINT "---"
    PRINT ""

    DBEXEC "UPDATE msgs SET read=1 WHERE id=" + rid$
END SUB

' ── COMPOSE <to> <subject> — write a new message ─────────────────────────────
SUB CmdCompose(args$)
    DIM cto$ AS STRING
    DIM csubj$ AS STRING
    DIM cbody$ AS STRING
    DIM cline$ AS STRING
    DIM cancelled AS INTEGER

    cto$   = UPPER$(Word1$(args$))
    csubj$ = Rest$(args$)

    IF cto$ = "" THEN
        PRINT "Usage: COMPOSE <to> <subject>"
        EXIT SUB
    END IF
    IF csubj$ = "" THEN csubj$ = "(no subject)"

    PRINT "Composing to " + cto$ + " / Subject: " + csubj$
    PRINT "Enter body (. alone to send, CANCEL to abort):"

    cbody$    = ""
    cancelled = 0

    DO
        SEND "> "
        RECV cline$, 300000
        SELECT CASE UPPER$(TRIM$(cline$))
            CASE "."
                EXIT DO
            CASE "CANCEL"
                cancelled = 1
                EXIT DO
            CASE ELSE
                cbody$ = cbody$ + cline$ + "\n"
        END SELECT
    LOOP

    IF cancelled THEN
        PRINT "Cancelled."
        EXIT SUB
    END IF

    DBEXEC "INSERT INTO msgs (from_call,to_call,subject,body) VALUES ('" + callsign$ + "','" + cto$ + "','" + csubj$ + "','" + cbody$ + "')"
    PRINT "Message sent to " + cto$ + "."
END SUB

' ── REPLY <id> — reply to a received message ─────────────────────────────────
SUB CmdReply(args$)
    DIM rrid$ AS STRING
    DIM rto$ AS STRING
    DIM rosubj$ AS STRING

    rrid$ = TRIM$(args$)
    IF rrid$ = "" THEN
        PRINT "Usage: REPLY <id>"
        EXIT SUB
    END IF

    DBQUERY "SELECT from_call FROM msgs WHERE id=" + rrid$ + " AND to_call='" + callsign$ + "'", rto$
    IF rto$ = "" THEN
        PRINT "No message #" + rrid$ + " for you."
        EXIT SUB
    END IF

    DBQUERY "SELECT subject FROM msgs WHERE id=" + rrid$, rosubj$
    CALL CmdCompose(rto$ + " Re: " + rosubj$)
END SUB

' ── DELETE <id> — delete a message you own ───────────────────────────────────
SUB CmdDelete(args$)
    DIM drid$ AS STRING
    DIM dchk$ AS STRING
    DIM dconf$ AS STRING

    drid$ = TRIM$(args$)
    IF drid$ = "" THEN
        PRINT "Usage: DELETE <id>"
        EXIT SUB
    END IF

    DBQUERY "SELECT id FROM msgs WHERE id=" + drid$ + " AND to_call='" + callsign$ + "'", dchk$
    IF dchk$ = "" THEN
        PRINT "No message #" + drid$ + " for you."
        EXIT SUB
    END IF

    PRINT "Delete message #" + drid$ + "? (Y/N)"
    RECV dconf$, 30000
    IF UPPER$(TRIM$(dconf$)) <> "Y" THEN
        PRINT "Cancelled."
        EXIT SUB
    END IF

    DBEXEC "DELETE FROM msgs WHERE id=" + drid$
    PRINT "Message #" + drid$ + " deleted."
END SUB

' ── HELP ─────────────────────────────────────────────────────────────────────
SUB CmdHelp
    PRINT ""
    PRINT "Email commands:"
    PRINT "  LIST               List your messages"
    PRINT "  READ <id>          Read a message"
    PRINT "  COMPOSE <to> <subj>  Write a new message"
    PRINT "  REPLY <id>         Reply to a message"
    PRINT "  DELETE <id>        Delete a message"
    PRINT "  HELP / ?           This help"
    PRINT "  QUIT / BYE / Q     Exit email"
    PRINT ""
END SUB

' =============================================================================
' Main
' =============================================================================
CALL DbInit

PRINT ""
PRINT "=== " + bbs_name$ + " Email System ==="
PRINT "User: " + callsign$

CALL CmdList
CALL CmdHelp

DO
    SEND "Email> "
    RECV cmd$, 120000

    IF cmd$ = "" THEN
        ' timeout — keep looping
    ELSE
        DIM kw$ AS STRING
        kw$ = UPPER$(TRIM$(Word1$(cmd$)))
        DIM args$ AS STRING
        args$ = Rest$(cmd$)

        SELECT CASE kw$
            CASE "LIST", "L"
                CALL CmdList
            CASE "READ", "R"
                CALL CmdRead(args$)
            CASE "COMPOSE", "COMP", "C"
                CALL CmdCompose(args$)
            CASE "REPLY"
                CALL CmdReply(args$)
            CASE "DELETE", "DEL", "D"
                CALL CmdDelete(args$)
            CASE "HELP", "H", "?"
                CALL CmdHelp
            CASE "QUIT", "Q", "BYE", "EXIT"
                EXIT DO
            CASE ELSE
                PRINT "Unknown command '" + kw$ + "'. Type HELP for commands."
        END SELECT
    END IF
LOOP

PRINT ""
PRINT "Goodbye from BBS Email!  73 de " + bbs_name$
DBCLOSE
END
