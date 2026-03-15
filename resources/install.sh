#!/usr/bin/env bash
# =============================================================================
#  KISSBBS — AX.25 BBS Station Installer
#  Installs and configures a complete AX.25 BBS on a clean Linux system
#  (Raspberry Pi OS, Ubuntu, Debian).
#
#  Usage:
#    1. Edit the CONFIGURATION section below with your station values
#    2. Run:  sudo bash install.sh
# =============================================================================
set -euo pipefail

# =============================================================================
#  CONFIGURATION — edit these values before running
# =============================================================================

# ── Station identity ─────────────────────────────────────────────────────────
CALLSIGN="N0CALL"                       # Your amateur radio callsign
SSID="1"                                # SSID for the BBS (0–15)
BBS_NAME="MyBBS"                        # BBS display name

# ── AX.25 port ──────────────────────────────────────────────────────────────
AX25_PORT_NAME="vhf"                    # axports port name
AX25_PORT_SPEED="9600"                  # Port speed (baud)
AX25_PORT_PACLEN="128"                  # Max packet length (MTU)
AX25_PORT_WINDOW="3"                    # Sliding window size
AX25_PORT_DESC="KISSBBS VHF Port"       # Port description in axports

# ── AX.25 protocol parameters ───────────────────────────────────────────────
AX25_T1="3000"                          # T1 retransmit timer (ms)
AX25_T3="60000"                         # T3 keep-alive timer (ms)
AX25_N2="10"                            # Max retransmit count
AX25_TXDELAY="30"                       # TX delay (10ms units)
AX25_PERSIST="63"                       # p-persistence (0–255)

# ── KISS interface ───────────────────────────────────────────────────────────
KISS_PTY="/tmp/kiss"                    # KISS PTY symlink path
SERIAL_PORT="/dev/ttyUSB0"             # Hardware TNC serial port (if no BLE)
SERIAL_BAUD="9600"                      # Hardware TNC baud rate

# ── BLE KISS bridge (leave empty to skip) ────────────────────────────────────
BLE_DEVICE=""                           # BLE peripheral MAC (e.g. AA:BB:CC:DD:EE:FF)
BLE_SERVICE=""                          # GATT service UUID
BLE_WRITE=""                            # GATT write characteristic UUID
BLE_READ=""                             # GATT notify/read characteristic UUID
BLE_MTU="517"                           # Max BLE KISS chunk size
BLE_KEEPALIVE="5"                       # BLE keep-alive interval (seconds; 0=disable)

# ── BBS settings ─────────────────────────────────────────────────────────────
BEACON_TEXT="!0000.00N/00000.00W>KISSBBS AX.25 BBS"
BEACON_INTERVAL="600"                   # Beacon interval (seconds)
WELCOME_SCRIPT="welcome.bas"            # BASIC welcome script
DB_PATH="bbs.db"                        # SQLite database filename
SCRIPT_DIR="."                          # Directory for .bas scripts

# ── Installation paths ───────────────────────────────────────────────────────
KISSBBS_REPO="https://github.com/solariun/KISSBBS.git"
KISSBBS_BRANCH="main"
INSTALL_DIR="/opt/kissbbs"              # Where source is cloned & configs live
INSTALL_PREFIX="/usr/local"             # Binary install prefix (bins → prefix/bin/)

# ── Service user ─────────────────────────────────────────────────────────────
SERVICE_USER="root"                     # User for systemd services

# =============================================================================
#  END OF CONFIGURATION — no need to edit below this line
# =============================================================================

# ── Derived values ───────────────────────────────────────────────────────────
FULL_CALLSIGN="${CALLSIGN}-${SSID}"
BIN_DIR="${INSTALL_PREFIX}/bin"
SYSTEMD_DIR="/etc/systemd/system"
AXPORTS_FILE="/etc/ax25/axports"

# ── Colors ───────────────────────────────────────────────────────────────────
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
BOLD='\033[1m'
RESET='\033[0m'

# ── Helper functions ─────────────────────────────────────────────────────────
info()    { echo -e "${BLUE}[INFO]${RESET}  $*"; }
success() { echo -e "${GREEN}[OK]${RESET}    $*"; }
warn()    { echo -e "${YELLOW}[WARN]${RESET}  $*"; }
error()   { echo -e "${RED}[ERROR]${RESET} $*" >&2; }
fatal()   { error "$*"; exit 1; }

banner() {
    echo ""
    echo -e "${BOLD}╔══════════════════════════════════════════════════════════╗${RESET}"
    echo -e "${BOLD}║          KISSBBS — AX.25 BBS Station Installer          ║${RESET}"
    echo -e "${BOLD}╚══════════════════════════════════════════════════════════╝${RESET}"
    echo ""
}

section() {
    echo ""
    echo -e "${BOLD}── $* ──${RESET}"
}

require_root() {
    if [[ $EUID -ne 0 ]]; then
        fatal "This script must be run as root (use: sudo bash $0)"
    fi
}

# =============================================================================
#  STEP 0 — Pre-flight checks
# =============================================================================
banner
require_root

info "Station:       ${FULL_CALLSIGN}"
info "BBS name:      ${BBS_NAME}"
info "AX.25 port:    ${AX25_PORT_NAME} @ ${AX25_PORT_SPEED} baud"
info "KISS PTY:      ${KISS_PTY}"
info "Install dir:   ${INSTALL_DIR}"
if [[ -n "${BLE_DEVICE}" ]]; then
    info "BLE device:    ${BLE_DEVICE}"
fi
echo ""

# =============================================================================
#  STEP 1 — Install system packages
# =============================================================================
section "Installing system packages"

export DEBIAN_FRONTEND=noninteractive

info "Updating package lists..."
apt-get update -qq

PACKAGES=(
    # Build tools
    build-essential
    git
    cmake
    pkg-config

    # Libraries
    libsqlite3-dev

    # AX.25 native tools
    ax25-tools
    ax25-apps
    libax25
    libax25-dev

    # Linpac terminal
    linpac

    # Bluetooth
    bluetooth
    bluez
    libbluetooth-dev
    libdbus-1-dev
    rfkill
)

info "Installing ${#PACKAGES[@]} packages..."
apt-get install -y -qq "${PACKAGES[@]}" 2>&1 | tail -1 || true
success "System packages installed"

# =============================================================================
#  STEP 2 — Bluetooth setup
# =============================================================================
section "Configuring Bluetooth"

if command -v rfkill &>/dev/null; then
    rfkill unblock bluetooth 2>/dev/null || true
    success "Bluetooth unblocked"
else
    warn "rfkill not found — skipping Bluetooth unblock"
fi

if systemctl is-active --quiet bluetooth 2>/dev/null; then
    success "Bluetooth service already running"
else
    systemctl enable bluetooth 2>/dev/null || true
    systemctl start bluetooth 2>/dev/null || true
    success "Bluetooth service enabled and started"
fi

# =============================================================================
#  STEP 3 — AX.25 port configuration
# =============================================================================
section "Configuring AX.25 port"

mkdir -p /etc/ax25

# Back up existing axports if present
if [[ -f "${AXPORTS_FILE}" ]]; then
    cp "${AXPORTS_FILE}" "${AXPORTS_FILE}.bak.$(date +%Y%m%d%H%M%S)"
    info "Backed up existing ${AXPORTS_FILE}"
fi

# Check if our port already exists
if grep -q "^${AX25_PORT_NAME}" "${AXPORTS_FILE}" 2>/dev/null; then
    warn "Port '${AX25_PORT_NAME}' already exists in ${AXPORTS_FILE} — replacing"
    sed -i "/^${AX25_PORT_NAME}/d" "${AXPORTS_FILE}"
fi

# Ensure header comment exists
if [[ ! -f "${AXPORTS_FILE}" ]] || ! grep -q "^# portname" "${AXPORTS_FILE}" 2>/dev/null; then
    cat > "${AXPORTS_FILE}" <<AXEOF
# portname  callsign       speed   paclen  window  description
AXEOF
fi

# Append our port
echo "${AX25_PORT_NAME}	${FULL_CALLSIGN}	${AX25_PORT_SPEED}	${AX25_PORT_PACLEN}	${AX25_PORT_WINDOW}	${AX25_PORT_DESC}" \
    >> "${AXPORTS_FILE}"

success "AX.25 port '${AX25_PORT_NAME}' configured in ${AXPORTS_FILE}"

# =============================================================================
#  STEP 4 — Clone and build KISSBBS
# =============================================================================
section "Building KISSBBS"

if [[ -d "${INSTALL_DIR}/.git" ]]; then
    info "Repository already exists at ${INSTALL_DIR} — pulling latest"
    cd "${INSTALL_DIR}"
    git fetch origin
    git checkout "${KISSBBS_BRANCH}"
    git pull origin "${KISSBBS_BRANCH}"
else
    info "Cloning ${KISSBBS_REPO} → ${INSTALL_DIR}"
    git clone --branch "${KISSBBS_BRANCH}" "${KISSBBS_REPO}" "${INSTALL_DIR}"
    cd "${INSTALL_DIR}"
fi

info "Building core binaries..."
make -j"$(nproc)" bbs ax25kiss ax25tnc basic_tool
success "Core binaries built"

info "Building BLE bridge (optional)..."
if make -j"$(nproc)" ble-deps 2>/dev/null && make -j"$(nproc)" ble_kiss_bridge 2>/dev/null; then
    success "BLE KISS bridge built"
    BLE_BRIDGE_BUILT=1
else
    warn "BLE KISS bridge build failed — this is optional, continuing"
    BLE_BRIDGE_BUILT=0
fi

info "Installing binaries to ${BIN_DIR}..."
make install PREFIX="${INSTALL_PREFIX}"
success "Binaries installed"

# =============================================================================
#  STEP 5 — Generate bbs.ini
# =============================================================================
section "Generating configuration"

BBS_INI="${INSTALL_DIR}/bbs.ini"

cat > "${BBS_INI}" <<INIEOF
; =============================================================================
;  KISSBBS Configuration — generated by install.sh
;  $(date '+%Y-%m-%d %H:%M:%S')
; =============================================================================

[kiss]
device = ${KISS_PTY}
baud   = ${SERIAL_BAUD}

[ax25]
callsign    = ${FULL_CALLSIGN}
mtu         = ${AX25_PORT_PACLEN}
window      = ${AX25_PORT_WINDOW}
t1_ms       = ${AX25_T1}
t3_ms       = ${AX25_T3}
n2          = ${AX25_N2}
txdelay     = ${AX25_TXDELAY}
persist     = ${AX25_PERSIST}

[bbs]
name             = ${BBS_NAME}
beacon           = ${BEACON_TEXT}
beacon_interval  = ${BEACON_INTERVAL}
welcome_script   = ${WELCOME_SCRIPT}

[basic]
script_dir = ${SCRIPT_DIR}
database   = ${DB_PATH}

[commands]
welcome = ${WELCOME_SCRIPT}
INIEOF

success "Configuration written to ${BBS_INI}"

# =============================================================================
#  STEP 6 — Create systemd services
# =============================================================================
section "Creating systemd services"

# ── 6a) BLE KISS bridge service (enabled, not started) ──────────────────────
if [[ -n "${BLE_DEVICE}" ]] && [[ "${BLE_BRIDGE_BUILT}" -eq 1 ]]; then

    cat > "${SYSTEMD_DIR}/kissbbs-ble-bridge.service" <<SVCEOF
[Unit]
Description=KISSBBS BLE-to-KISS Bridge (${FULL_CALLSIGN})
After=bluetooth.target
Wants=bluetooth.target

[Service]
Type=simple
User=${SERVICE_USER}
ExecStart=${BIN_DIR}/ble_kiss_bridge \\
    --device ${BLE_DEVICE} \\
    --service ${BLE_SERVICE} \\
    --write ${BLE_WRITE} \\
    --read ${BLE_READ} \\
    --link ${KISS_PTY} \\
    --mtu ${BLE_MTU} \\
    --ble-ka ${BLE_KEEPALIVE} \\
    --monitor
Restart=on-failure
RestartSec=10
StandardOutput=journal
StandardError=journal

[Install]
WantedBy=multi-user.target
SVCEOF

    systemctl daemon-reload
    systemctl enable kissbbs-ble-bridge.service
    success "kissbbs-ble-bridge.service — created and ENABLED (not started)"

else
    if [[ -z "${BLE_DEVICE}" ]]; then
        info "BLE device not configured — skipping BLE bridge service"
    else
        warn "BLE bridge not built — skipping BLE bridge service"
    fi
fi

# ── 6b) kissattach service (not enabled, not started) ───────────────────────
KISS_AFTER="network.target"
if [[ -n "${BLE_DEVICE}" ]] && [[ "${BLE_BRIDGE_BUILT}" -eq 1 ]]; then
    KISS_AFTER="kissbbs-ble-bridge.service"
fi

cat > "${SYSTEMD_DIR}/kissbbs-kissattach.service" <<SVCEOF
[Unit]
Description=KISS Attach — ${AX25_PORT_NAME} on ${KISS_PTY} (${FULL_CALLSIGN})
After=${KISS_AFTER}

[Service]
Type=forking
User=${SERVICE_USER}
ExecStart=/usr/sbin/kissattach ${KISS_PTY} ${AX25_PORT_NAME}
ExecStartPost=/sbin/kissparms -p ${AX25_PORT_NAME} -t ${AX25_TXDELAY} -l ${AX25_PERSIST}
RemainAfterExit=yes
Restart=on-failure
RestartSec=5

[Install]
WantedBy=multi-user.target
SVCEOF

systemctl daemon-reload
success "kissbbs-kissattach.service — created (not enabled)"

# ── 6c) BBS service (not enabled, not started) ──────────────────────────────
BBS_AFTER="kissbbs-kissattach.service"

cat > "${SYSTEMD_DIR}/kissbbs-bbs.service" <<SVCEOF
[Unit]
Description=KISSBBS AX.25 BBS Server (${BBS_NAME} — ${FULL_CALLSIGN})
After=${BBS_AFTER}
Wants=${BBS_AFTER}

[Service]
Type=simple
User=${SERVICE_USER}
WorkingDirectory=${INSTALL_DIR}
ExecStart=${BIN_DIR}/bbs -C ${BBS_INI}
Restart=on-failure
RestartSec=5
StandardOutput=journal
StandardError=journal

[Install]
WantedBy=multi-user.target
SVCEOF

systemctl daemon-reload
success "kissbbs-bbs.service — created (not enabled)"

# ── 6d) Linpac service (not enabled, not started) ───────────────────────────
cat > "${SYSTEMD_DIR}/kissbbs-linpac.service" <<SVCEOF
[Unit]
Description=Linpac AX.25 Terminal (${AX25_PORT_NAME})
After=kissbbs-kissattach.service
Wants=kissbbs-kissattach.service

[Service]
Type=simple
User=${SERVICE_USER}
ExecStart=/usr/bin/linpac
Restart=on-failure
RestartSec=5
StandardOutput=journal
StandardError=journal

[Install]
WantedBy=multi-user.target
SVCEOF

systemctl daemon-reload
success "kissbbs-linpac.service — created (not enabled)"

# =============================================================================
#  STEP 7 — Summary
# =============================================================================
echo ""
echo -e "${BOLD}╔══════════════════════════════════════════════════════════╗${RESET}"
echo -e "${BOLD}║                  Installation Complete                   ║${RESET}"
echo -e "${BOLD}╚══════════════════════════════════════════════════════════╝${RESET}"
echo ""
echo -e "${BOLD}Station:${RESET}        ${FULL_CALLSIGN}"
echo -e "${BOLD}BBS name:${RESET}       ${BBS_NAME}"
echo -e "${BOLD}Install dir:${RESET}    ${INSTALL_DIR}"
echo -e "${BOLD}Binaries:${RESET}       ${BIN_DIR}/"
echo -e "${BOLD}Config:${RESET}         ${BBS_INI}"
echo -e "${BOLD}AX.25 ports:${RESET}    ${AXPORTS_FILE}"
echo ""
echo -e "${BOLD}Installed binaries:${RESET}"
for bin in bbs ax25kiss ax25tnc basic_tool; do
    if [[ -f "${BIN_DIR}/${bin}" ]]; then
        echo -e "  ${GREEN}✓${RESET} ${bin}"
    else
        echo -e "  ${RED}✗${RESET} ${bin}"
    fi
done
if [[ -f "${BIN_DIR}/ble_kiss_bridge" ]]; then
    echo -e "  ${GREEN}✓${RESET} ble_kiss_bridge"
else
    echo -e "  ${YELLOW}○${RESET} ble_kiss_bridge (not built)"
fi

echo ""
echo -e "${BOLD}Systemd services:${RESET}"

svc_status() {
    local svc="$1"
    if [[ -f "${SYSTEMD_DIR}/${svc}" ]]; then
        local enabled
        enabled=$(systemctl is-enabled "${svc}" 2>/dev/null || echo "disabled")
        if [[ "${enabled}" == "enabled" ]]; then
            echo -e "  ${GREEN}●${RESET} ${svc}  ${GREEN}[enabled]${RESET}  (not started)"
        else
            echo -e "  ${YELLOW}○${RESET} ${svc}  ${YELLOW}[disabled]${RESET}"
        fi
    else
        echo -e "  ${RED}─${RESET} ${svc}  (not created)"
    fi
}

svc_status "kissbbs-ble-bridge.service"
svc_status "kissbbs-kissattach.service"
svc_status "kissbbs-bbs.service"
svc_status "kissbbs-linpac.service"

echo ""
echo -e "${BOLD}Next steps:${RESET}"
echo ""
echo "  1. Edit ${BBS_INI} with your station details"
echo ""
if [[ -n "${BLE_DEVICE}" ]] && [[ "${BLE_BRIDGE_BUILT}" -eq 1 ]]; then
    echo "  2. Start the BLE bridge (already enabled on boot):"
    echo "       sudo systemctl start kissbbs-ble-bridge"
    echo ""
    echo "  3. Attach KISS port and start the BBS:"
    echo "       sudo systemctl enable --now kissbbs-kissattach"
    echo "       sudo systemctl enable --now kissbbs-bbs"
else
    echo "  2. Connect your TNC to ${SERIAL_PORT} and start kissattach:"
    echo "       sudo kissattach ${SERIAL_PORT} ${AX25_PORT_NAME}"
    echo "     Or enable the service:"
    echo "       sudo systemctl enable --now kissbbs-kissattach"
    echo ""
    echo "  3. Start the BBS:"
    echo "       sudo systemctl enable --now kissbbs-bbs"
fi
echo ""
echo "  Optional — Linpac terminal:"
echo "       sudo systemctl enable --now kissbbs-linpac"
echo ""
echo "  View logs:"
echo "       journalctl -u kissbbs-bbs -f"
echo "       journalctl -u kissbbs-ble-bridge -f"
echo ""
success "KISSBBS installation complete — 73!"
