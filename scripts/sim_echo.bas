REM sim_echo.bas — Echo server for AX.25 simulator testing
REM Echoes back all received text with a prefix. Type BYE to quit.

PRINT "=== Echo Server ==="
PRINT "Connected as " + LOCAL$ + ". Send text and I'll echo it back."
PRINT "Type BYE or QUIT to end."
PRINT ""

DO
    RECV line$, 60000
    IF line$ = "" THEN
        PRINT "[idle — still listening]"
    ELSE
        LET u$ = UPPER$(TRIM$(line$))
        IF u$ = "BYE" OR u$ = "QUIT" OR u$ = "Q" THEN
            PRINT "73 de " + LOCAL$ + " — goodbye!"
            EXIT DO
        END IF
        PRINT "ECHO> " + line$
    END IF
LOOP

END
