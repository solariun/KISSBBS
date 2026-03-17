REM sim_chat.bas — Simulated QSO partner for AX.25 simulator
REM Responds to amateur radio keywords with canned responses.

PRINT "=== Simulated QSO ==="
PRINT "Hello from " + LOCAL$ + "! This is a simulated QSO partner."
PRINT "Keywords: NAME, QTH, FREQ, RIG, WX, RST, BAND, ANT, PWR, INFO, BYE"
PRINT ""

DO
    RECV msg$, 120000
    IF msg$ = "" THEN
        PRINT "Still here... type a keyword or BYE to end QSO."
    ELSE
        LET u$ = UPPER$(TRIM$(msg$))

        IF INSTR(u$, "BYE") > 0 OR INSTR(u$, "QUIT") > 0 THEN
            PRINT "73 de " + LOCAL$ + " — thanks for the QSO! See you on the air."
            EXIT DO
        END IF

        LET handled = 0

        IF INSTR(u$, "NAME") > 0 THEN
            PRINT "My name is SimBot, the AX.25 simulation engine."
            handled = 1
        END IF

        IF INSTR(u$, "QTH") > 0 THEN
            PRINT "QTH is Virtual City, Grid Square AA00aa, elevation 0m ASL."
            handled = 1
        END IF

        IF INSTR(u$, "FREQ") > 0 THEN
            PRINT "Operating on 145.010 MHz FM simplex."
            handled = 1
        END IF

        IF INSTR(u$, "RIG") > 0 THEN
            PRINT "Running a Virtual TNC at 9600 baud, 128-byte MTU."
            handled = 1
        END IF

        IF INSTR(u$, "WX") > 0 THEN
            PRINT "WX here is perfect — 72F and sunny, light breeze from SW."
            handled = 1
        END IF

        IF INSTR(u$, "RST") > 0 THEN
            PRINT "Your signal is 599, solid copy on all frames!"
            handled = 1
        END IF

        IF INSTR(u$, "BAND") > 0 THEN
            PRINT "I'm on 2m VHF. Also QRV on 70cm and HF 40m."
            handled = 1
        END IF

        IF INSTR(u$, "ANT") > 0 THEN
            PRINT "Antenna is a virtual 5/8 wave ground plane at 10m AGL."
            handled = 1
        END IF

        IF INSTR(u$, "PWR") > 0 THEN
            PRINT "Running infinite watts into a perfect antenna. Hi hi!"
            handled = 1
        END IF

        IF INSTR(u$, "INFO") > 0 THEN
            PRINT "KISSBBS AX.25 Simulator — github.com/solariun/KISSBBS"
            PRINT "Use this to test BBS connections, scripts, and protocols."
            handled = 1
        END IF

        IF handled = 0 THEN
            PRINT "Roger " + msg$ + " — try: NAME, QTH, FREQ, RIG, WX, RST, BYE"
        END IF
    END IF
LOOP

END
