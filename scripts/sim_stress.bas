REM sim_stress.bas — Stress test for AX.25 flow control and windowing
REM Sends numbered messages to test throughput, then reports summary.

CONST MSG_COUNT = 100
CONST PAD_LEN   = 40

PRINT "=== Stress Test ==="
PRINT "Sending " + STR$(MSG_COUNT) + " messages with " + STR$(PAD_LEN) + "-char padding..."
PRINT ""

LET pad$ = ""
FOR p = 1 TO PAD_LEN
    pad$ = pad$ + "*"
NEXT p

LET t0 = 0
EXEC "date +%s", t0$
LET t0 = VAL(t0$)

FOR i = 1 TO MSG_COUNT
    LET num$ = STR$(i)
    IF LEN(num$) < 3 THEN
        IF LEN(num$) = 1 THEN LET num$ = "  " + num$
        IF LEN(num$) = 2 THEN LET num$ = " " + num$
    END IF
    PRINT "MSG " + num$ + "/" + STR$(MSG_COUNT) + " " + pad$
NEXT i

EXEC "date +%s", t1$
LET t1 = VAL(t1$)
LET elapsed = t1 - t0
IF elapsed < 1 THEN LET elapsed = 1

PRINT ""
PRINT "=== Stress Test Complete ==="
PRINT "  Messages sent : " + STR$(MSG_COUNT)
PRINT "  Elapsed       : " + STR$(elapsed) + "s"
PRINT "  Rate          : ~" + STR$(INT(MSG_COUNT / elapsed)) + " msg/s"

END
