#!/bin/bash
# kiss_exit.sh — Send KISS exit sequence to return a TNC to command mode
#
# Usage: kiss_exit.sh /dev/tty.PL2303G-USBtoUART21220
#        kiss_exit.sh /dev/ttyUSB0 1200

DEV="${1:?Usage: $0 <serial-device> [baud]}"
BAUD="${2:-9600}"

if [ ! -e "$DEV" ]; then
    echo "Error: device '$DEV' not found" >&2
    exit 1
fi

echo "Setting $DEV to $BAUD baud, raw mode..."
stty -f "$DEV" "$BAUD" raw -echo 2>/dev/null || \
stty -F "$DEV" "$BAUD" raw -echo  # Linux uses -F, macOS uses -f

echo "Sending KISS exit sequence (FEND + 0xFF + FEND)..."
printf '\xC0\xC0\xC0\xFF\xC0' > "$DEV"
sleep 1
printf '\r' > "$DEV"

echo "Done. Open with:  screen $DEV $BAUD"
