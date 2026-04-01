# =============================================================================
# Makefile — AX25Toolkit project
# =============================================================================
CXX      ?= g++
CXXFLAGS  = -std=c++11 -O2 -Wall -Wextra -Wpedantic -Ilib
UNAME    := $(shell uname)

# ── Directory layout ────────────────────────────────────────────────────────
SRCDIR    = src
LIBDIR    = lib
TESTDIR   = test
BINDIR    = bin
BUILDDIR  = build

# ── Platform detection ──────────────────────────────────────────────────────
ifeq ($(UNAME), Linux)
    LDUTIL    = -lutil
    SQLITE3   := $(shell pkg-config --libs sqlite3 2>/dev/null || echo "-lsqlite3")
    SQLITE3_CFLAGS := $(shell pkg-config --cflags sqlite3 2>/dev/null)
else
    LDUTIL    =
    SQLITE3   := $(shell pkg-config --libs sqlite3 2>/dev/null || echo "-lsqlite3")
    SQLITE3_CFLAGS := $(shell pkg-config --cflags sqlite3 2>/dev/null)
endif

# ── SQLite feature flag ────────────────────────────────────────────────────
HAVE_SQLITE := $(shell pkg-config --exists sqlite3 2>/dev/null && echo yes || \
               { [ -f /usr/include/sqlite3.h ] || [ -f /opt/homebrew/include/sqlite3.h ]; } && echo yes)
ifeq ($(HAVE_SQLITE), yes)
    SQLITE_FLAGS = -DHAVE_SQLITE3 $(SQLITE3_CFLAGS)
    SQLITE_LIBS  = $(SQLITE3)
else
    SQLITE_FLAGS =
    SQLITE_LIBS  =
endif

# ── GoogleTest detection ────────────────────────────────────────────────────
GTEST_CFLAGS  := $(shell pkg-config --cflags gtest 2>/dev/null)
GTEST_LDFLAGS := $(shell pkg-config --libs gtest_main 2>/dev/null)
ifeq ($(GTEST_LDFLAGS),)
    ifneq ($(wildcard /opt/homebrew/lib/libgtest.a),)
        GTEST_CFLAGS  := -I/opt/homebrew/include
        GTEST_LDFLAGS := -L/opt/homebrew/lib -lgtest -lgtest_main
    endif
endif
ifeq ($(GTEST_LDFLAGS),)
    ifneq ($(wildcard /usr/local/lib/libgtest.a),)
        GTEST_CFLAGS  := -I/usr/local/include
        GTEST_LDFLAGS := -L/usr/local/lib -lgtest -lgtest_main
    endif
endif
ifeq ($(GTEST_LDFLAGS),)
    GTEST_LDFLAGS := -lgtest -lgtest_main
endif
GTEST_LDFLAGS += -lpthread

# ── Library objects ─────────────────────────────────────────────────────────
LIB_OBJ   = $(BUILDDIR)/ax25lib.o
BASIC_OBJ = $(BUILDDIR)/basic.o

# ── Native BLE (BlueZ D-Bus on Linux, CoreBluetooth on macOS) ──────────────
ifeq ($(UNAME), Linux)
    DBUS_CFLAGS := $(shell pkg-config --cflags dbus-1 2>/dev/null)
    DBUS_LIBS   := $(shell pkg-config --libs   dbus-1 2>/dev/null || echo "-ldbus-1")
    BLE_OBJ      = $(BUILDDIR)/bt_ble_linux.o
    BLE_SYS      = $(DBUS_LIBS) -lpthread
    BLUETOOTH_LIBS = -lbluetooth
    ifneq ($(filter arm%,$(shell uname -m)),)
        BLUETOOTH_LIBS += -latomic
    endif
else
    DBUS_CFLAGS :=
    BLE_OBJ      = $(BUILDDIR)/bt_ble_macos.o
    BLE_SYS      = -framework CoreBluetooth -framework Foundation \
                   -framework IOKit -framework IOBluetooth -lpthread
    BLUETOOTH_LIBS =
endif

ifneq ($(UNAME), Linux)
    BT_MACOS_OBJ = $(BUILDDIR)/bt_rfcomm_macos.o
else
    BT_MACOS_OBJ =
endif

# ── Targets ─────────────────────────────────────────────────────────────────
PREFIX    ?= /usr/local
INSTALLDIR = $(PREFIX)/bin

.PHONY: all clean test install uninstall install-deps help

all: bbs ax25kiss ax25tnc ax25sim ax25send basic_tool bt_kiss_bridge bt_sniffer modemtnc

# ── Build directories ───────────────────────────────────────────────────────
$(BUILDDIR) $(BINDIR):
	@mkdir -p $@

# ── Library objects ─────────────────────────────────────────────────────────
$(BUILDDIR)/ax25lib.o: $(LIBDIR)/ax25lib.cpp $(LIBDIR)/ax25lib.hpp | $(BUILDDIR)
	$(CXX) $(CXXFLAGS) -c -o $@ $<

$(BUILDDIR)/basic.o: $(LIBDIR)/basic.cpp $(LIBDIR)/basic.hpp | $(BUILDDIR)
	$(CXX) $(CXXFLAGS) $(SQLITE_FLAGS) -c -o $@ $<

# ── Application binaries ───────────────────────────────────────────────────
$(BINDIR)/bbs: $(SRCDIR)/bbs.cpp $(LIB_OBJ) $(BASIC_OBJ) | $(BINDIR)
	$(CXX) $(CXXFLAGS) $(SQLITE_FLAGS) -o $@ $< $(LIB_OBJ) $(BASIC_OBJ) $(LDUTIL) $(SQLITE_LIBS)

$(BINDIR)/ax25kiss: $(SRCDIR)/ax25kiss.cpp | $(BINDIR)
	$(CXX) $(CXXFLAGS) -o $@ $<

$(BINDIR)/ax25tnc: $(SRCDIR)/ax25tnc.cpp $(LIB_OBJ) $(BASIC_OBJ) | $(BINDIR)
	$(CXX) $(CXXFLAGS) $(SQLITE_FLAGS) -o $@ $< $(LIB_OBJ) $(BASIC_OBJ) $(SQLITE_LIBS)

$(BINDIR)/ax25sim: $(SRCDIR)/ax25sim.cpp $(LIB_OBJ) $(BASIC_OBJ) | $(BINDIR)
	$(CXX) $(CXXFLAGS) $(SQLITE_FLAGS) -o $@ $< $(LIB_OBJ) $(BASIC_OBJ) $(LDUTIL) $(SQLITE_LIBS)

$(BINDIR)/basic_tool: $(SRCDIR)/basic_tool.cpp $(BASIC_OBJ) | $(BINDIR)
	$(CXX) $(CXXFLAGS) $(SQLITE_FLAGS) -o $@ $< $(BASIC_OBJ) $(SQLITE_LIBS)

$(BINDIR)/ax25send: $(SRCDIR)/ax25send.cpp $(LIB_OBJ) | $(BINDIR)
	$(CXX) $(CXXFLAGS) -o $@ $< $(LIB_OBJ)

$(BINDIR)/bt_sniffer: $(SRCDIR)/bt_sniffer.cpp $(LIBDIR)/ax25dump.hpp | $(BINDIR)
	$(CXX) -std=c++17 -O2 -Wall -Wextra -Ilib -o $@ $< $(LDUTIL)
	@echo "Built: bin/bt_sniffer"

# ── Shorthand targets (so `make bbs` still works) ──────────────────────────
bbs: $(BINDIR)/bbs
ax25kiss: $(BINDIR)/ax25kiss
ax25tnc: $(BINDIR)/ax25tnc
ax25sim: $(BINDIR)/ax25sim
ax25send: $(BINDIR)/ax25send
basic_tool: $(BINDIR)/basic_tool
bt_kiss_bridge: $(BINDIR)/bt_kiss_bridge
ble_kiss_bridge: $(BINDIR)/ble_kiss_bridge
bt_sniffer: $(BINDIR)/bt_sniffer
modemtnc: $(BINDIR)/modemtnc
.PHONY: bbs ax25kiss ax25tnc ax25sim ax25send basic_tool bt_kiss_bridge ble_kiss_bridge bt_sniffer modemtnc

# ── Tests ───────────────────────────────────────────────────────────────────
$(BINDIR)/test_ax25lib: $(TESTDIR)/test_ax25lib.cpp $(LIB_OBJ) $(BASIC_OBJ) | $(BINDIR)
	$(CXX) -std=c++17 -Ilib $(GTEST_CFLAGS) $(SQLITE_FLAGS) \
	    -o $@ $< $(LIB_OBJ) $(BASIC_OBJ) \
	    $(GTEST_LDFLAGS) $(SQLITE_LIBS)

test: $(BINDIR)/test_ax25lib
	$(BINDIR)/test_ax25lib --gtest_color=yes

# ── Native BLE object compilation ──────────────────────────────────────────
$(BUILDDIR)/bt_ble_linux.o: $(LIBDIR)/bt_ble_linux.cpp $(LIBDIR)/bt_ble_native.h | $(BUILDDIR)
	$(CXX) -std=c++17 -O2 -Wall -Wextra -Ilib $(DBUS_CFLAGS) -c -o $@ $<

$(BUILDDIR)/bt_ble_macos.o: $(LIBDIR)/bt_ble_macos.mm $(LIBDIR)/bt_ble_native.h | $(BUILDDIR)
	$(CXX) -std=c++17 -O2 -Wall -Wextra -Ilib -fobjc-arc -c -o $@ $<

$(BUILDDIR)/bt_rfcomm_macos.o: $(LIBDIR)/bt_rfcomm_macos.mm $(LIBDIR)/bt_rfcomm_macos.h | $(BUILDDIR)
	$(CXX) -std=c++17 -O2 -Wall -Wextra -Ilib -fobjc-arc -c -o $@ $<

$(BINDIR)/bt_kiss_bridge: $(SRCDIR)/bt_kiss_bridge.cpp $(BLE_OBJ) $(BT_MACOS_OBJ) | $(BINDIR)
	$(CXX) -std=c++17 -O2 -Wall -Wextra -Ilib $(DBUS_CFLAGS) \
	    -o $@ $< $(BLE_OBJ) $(BT_MACOS_OBJ) \
	    $(BLE_SYS) $(BLUETOOTH_LIBS)
	@echo "Built: bin/bt_kiss_bridge"

# Backward-compatible alias: ble_kiss_bridge -> bt_kiss_bridge
$(BINDIR)/ble_kiss_bridge: $(BINDIR)/bt_kiss_bridge
	ln -sf bt_kiss_bridge $(BINDIR)/ble_kiss_bridge
	@echo "Created symlink: bin/ble_kiss_bridge -> bt_kiss_bridge"

# ── modemtnc — Software TNC with Soundcard DSP ────────────────────────────
MODEM_DIR  = modemtnc
MODEM_OBJS = $(BUILDDIR)/modemtnc.o $(BUILDDIR)/modem_dsp.o \
             $(BUILDDIR)/modem_hdlc.o $(BUILDDIR)/modem_audio.o

ifeq ($(UNAME), Darwin)
    MODEM_AUDIO_SRC = $(MODEM_DIR)/audio_coreaudio.cpp
    MODEM_AUDIO_LIBS = -framework CoreAudio -framework AudioToolbox -framework CoreFoundation -lpthread
else
    MODEM_AUDIO_SRC = $(MODEM_DIR)/audio_alsa.cpp
    MODEM_AUDIO_LIBS = -lasound -lpthread
endif

$(BUILDDIR)/modemtnc.o: $(MODEM_DIR)/modemtnc.cpp $(MODEM_DIR)/modem.h $(MODEM_DIR)/hdlc.h $(MODEM_DIR)/audio.h | $(BUILDDIR)
	$(CXX) -std=c++17 -O2 -Wall -Wextra -Ilib -I$(MODEM_DIR) -c -o $@ $<

$(BUILDDIR)/modem_dsp.o: $(MODEM_DIR)/modem.cpp $(MODEM_DIR)/modem.h | $(BUILDDIR)
	$(CXX) -std=c++17 -O2 -Wall -Wextra -ffast-math -I$(MODEM_DIR) -c -o $@ $<

$(BUILDDIR)/modem_hdlc.o: $(MODEM_DIR)/hdlc.cpp $(MODEM_DIR)/hdlc.h | $(BUILDDIR)
	$(CXX) -std=c++17 -O2 -Wall -Wextra -I$(MODEM_DIR) -c -o $@ $<

$(BUILDDIR)/modem_audio.o: $(MODEM_AUDIO_SRC) $(MODEM_DIR)/audio.h | $(BUILDDIR)
	$(CXX) -std=c++17 -O2 -Wall -Wextra -I$(MODEM_DIR) -c -o $@ $<

$(BINDIR)/modemtnc: $(MODEM_OBJS) $(LIB_OBJ) | $(BINDIR)
	$(CXX) -std=c++17 -O2 -o $@ $(MODEM_OBJS) $(LIB_OBJ) $(LDUTIL) $(MODEM_AUDIO_LIBS)
	@echo "Built: bin/modemtnc"

# ── Clean ───────────────────────────────────────────────────────────────────
clean:
	rm -rf $(BUILDDIR) $(BINDIR)

# ── Install / Uninstall ────────────────────────────────────────────────────
# Installs binaries to $(PREFIX)/bin (default: /usr/local/bin)
# and scripts+config to ~/.ax25toolkit
# Override prefix:  make install PREFIX=/usr
install:
	@echo "Installing binaries to $(INSTALLDIR) ..."
	install -d $(INSTALLDIR)
	install -m 755 $(BINDIR)/bbs            $(INSTALLDIR)/bbs
	install -m 755 $(BINDIR)/ax25kiss       $(INSTALLDIR)/ax25kiss
	install -m 755 $(BINDIR)/ax25tnc        $(INSTALLDIR)/ax25tnc
	install -m 755 $(BINDIR)/ax25sim        $(INSTALLDIR)/ax25sim
	install -m 755 $(BINDIR)/ax25send       $(INSTALLDIR)/ax25send
	install -m 755 $(BINDIR)/basic_tool     $(INSTALLDIR)/basic_tool
	install -m 755 $(BINDIR)/bt_kiss_bridge $(INSTALLDIR)/bt_kiss_bridge
	install -m 755 $(BINDIR)/bt_sniffer     $(INSTALLDIR)/bt_sniffer
	ln -sf bt_kiss_bridge $(INSTALLDIR)/ble_kiss_bridge
	@echo "Installing config and scripts to ~/.ax25toolkit ..."
	@mkdir -p $(HOME)/.ax25toolkit/scripts
	@cp -n etc/bbs.ini $(HOME)/.ax25toolkit/bbs.ini 2>/dev/null || true
	@cp etc/kiss_exit.sh $(HOME)/.ax25toolkit/kiss_exit.sh
	@cp etc/ble_kiss_monitor.py $(HOME)/.ax25toolkit/ble_kiss_monitor.py
	@cp scripts/*.bas $(HOME)/.ax25toolkit/scripts/ 2>/dev/null || true
	@echo "Done."

uninstall:
	@echo "Removing from $(INSTALLDIR) ..."
	rm -f $(INSTALLDIR)/bbs \
	      $(INSTALLDIR)/ax25kiss \
	      $(INSTALLDIR)/ax25tnc \
	      $(INSTALLDIR)/ax25sim \
	      $(INSTALLDIR)/ax25send \
	      $(INSTALLDIR)/basic_tool \
	      $(INSTALLDIR)/bt_kiss_bridge \
	      $(INSTALLDIR)/bt_sniffer \
	      $(INSTALLDIR)/ble_kiss_bridge
	@echo "Done."

install-deps:
	@echo "Install dependencies:"
	@echo "  macOS  : brew install googletest sqlite"
	@echo "  Ubuntu : sudo apt-get install libgtest-dev libsqlite3-dev libdbus-1-dev libbluetooth-dev"
	@echo "  Fedora : sudo dnf install gtest-devel sqlite-devel dbus-devel bluez-libs-devel"
	@echo ""
	@echo "BT/BLE bridge (bt_kiss_bridge):"
	@echo "  Linux  : libdbus-1-dev (BLE via BlueZ D-Bus)"
	@echo "  Linux  : libbluetooth-dev (Classic Bluetooth RFCOMM)"
	@echo "  macOS  : no extra deps (CoreBluetooth + IOBluetooth are system frameworks)"
	@echo "  Build  : make bt_kiss_bridge"

help:
	@echo "Targets:"
	@echo "  all            Build all binaries (default)"
	@echo "  bbs            BBS server"
	@echo "  ax25kiss       KISS TNC serial bridge"
	@echo "  ax25tnc        Virtual TNC with BASIC scripting"
	@echo "  ax25sim        PTY-based AX.25 simulator"
	@echo "  basic_tool     Standalone BASIC script runner"
	@echo "  bt_kiss_bridge BLE/Classic BT KISS bridge"
	@echo "  bt_sniffer     KISS proxy tap for bridge debugging"
	@echo "  test           Build and run test suite"
	@echo "  clean          Remove build/ and bin/"
	@echo "  install        Install to $(PREFIX)/bin + ~/.ax25toolkit"
	@echo "  uninstall      Remove installed binaries"
	@echo "  install-deps   Show dependency install commands"
