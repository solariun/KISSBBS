' =============================================================================
' welcome.bas — KISSBBS welcome script  (QBASIC dialect)
' Pre-filled vars: bbs_name$, callsign$, db_path$
' =============================================================================

CONST VERSION$ = "1.0"

' ── Banner ────────────────────────────────────────────────────────────────────
SUB PrintBanner
    PRINT "============================================"
    PRINT "  " + bbs_name$
    PRINT "  AX.25 Packet BBS  v" + VERSION$
    PRINT "============================================"
    PRINT ""
END SUB

' ── Unread message count for this user ───────────────────────────────────────
FUNCTION UnreadCount$
    DIM n$ AS STRING
    DBOPEN db_path$
    DBEXEC "CREATE TABLE IF NOT EXISTS msgs (id INTEGER PRIMARY KEY AUTOINCREMENT, from_call TEXT, to_call TEXT, subject TEXT DEFAULT '', body TEXT DEFAULT '', read INTEGER DEFAULT 0, ts DATETIME DEFAULT (datetime('now')))"
    DBQUERY "SELECT COUNT(*) FROM msgs WHERE to_call='" + callsign$ + "' AND read=0", n$
    DBCLOSE
    UnreadCount$ = n$
END FUNCTION

' ── Best-effort weather (skipped if host unreachable) ─────────────────────────
SUB ShowWeather
    DIM wx$ AS STRING
    HTTPGET "http://wttr.in/?format=3", wx$
    IF TRIM$(wx$) <> "" THEN
        PRINT "Weather: " + wx$
        PRINT ""
    END IF
END SUB

' ── Help screen ───────────────────────────────────────────────────────────────
SUB ShowHelp
    PRINT "Built-in commands:"
    PRINT "  H / ?            This help"
    PRINT "  U                Users online"
    PRINT "  I                BBS info"
    PRINT "  W                Who / uptime"
    PRINT "  PS               Process list"
    PRINT "  DIR              Directory listing"
    PRINT "  B                Send APRS beacon"
    PRINT "  UI <DEST> <msg>  Send UI frame"
    PRINT ""
    PRINT "Script commands:"
    PRINT "  EMAIL            BBS email system"
    PRINT ""
    PRINT "  BYE / Q          Disconnect"
    PRINT ""
END SUB

' =============================================================================
' Main
' =============================================================================
CALL PrintBanner

PRINT "Welcome, " + callsign$ + "!  de " + bbs_name$
PRINT ""

CALL ShowWeather

' Announce unread mail
DIM unread$ AS STRING
unread$ = UnreadCount$
IF unread$ <> "0" AND unread$ <> "" THEN
    PRINT "*** You have " + unread$ + " unread message(s). Type EMAIL to read. ***"
    PRINT ""
END IF

CALL ShowHelp

END
