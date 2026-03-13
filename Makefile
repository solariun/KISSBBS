# =============================================================================
# Makefile — ax25lib + BBS + BASIC interpreter + tests
# =============================================================================
CXX      ?= g++
CXXFLAGS  = -std=c++11 -O2 -Wall -Wextra -Wpedantic
UNAME    := $(shell uname)

# ── Platform detection ────────────────────────────────────────────────────────
ifeq ($(UNAME), Linux)
    LDUTIL    = -lutil
    SQLITE3   := $(shell pkg-config --libs sqlite3 2>/dev/null || echo "-lsqlite3")
    SQLITE3_CFLAGS := $(shell pkg-config --cflags sqlite3 2>/dev/null)
else
    LDUTIL    =
    SQLITE3   := $(shell pkg-config --libs sqlite3 2>/dev/null || echo "-lsqlite3")
    SQLITE3_CFLAGS := $(shell pkg-config --cflags sqlite3 2>/dev/null)
endif

# ── SQLite feature flag ───────────────────────────────────────────────────────
HAVE_SQLITE := $(shell pkg-config --exists sqlite3 2>/dev/null && echo yes || \
               { [ -f /usr/include/sqlite3.h ] || [ -f /opt/homebrew/include/sqlite3.h ]; } && echo yes)
ifeq ($(HAVE_SQLITE), yes)
    SQLITE_FLAGS = -DHAVE_SQLITE3 $(SQLITE3_CFLAGS)
    SQLITE_LIBS  = $(SQLITE3)
else
    SQLITE_FLAGS =
    SQLITE_LIBS  =
endif

# ── GoogleTest detection ──────────────────────────────────────────────────────
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

# ── Sources ───────────────────────────────────────────────────────────────────
LIB_SRC   = ax25lib.cpp
LIB_OBJ   = ax25lib.o
BASIC_SRC = basic.cpp
BASIC_OBJ = basic.o

# ── SimpleBLE (vendor build) ──────────────────────────────────────────────────
SIMPLEBLE_DIR = vendor/simpleble
# CMake-generated export.h (build/export or build/include)
SIMPLEBLE_GEN := $(shell find $(SIMPLEBLE_DIR)/build -name 'export.h' -path '*/simpleble/*' 2>/dev/null \
                   | sed 's|/simpleble/export.h||' | head -1)
# kvn_bytearray.h — submodule, lives somewhere under the source tree
SIMPLEBLE_KVN := $(shell find $(SIMPLEBLE_DIR) -name 'kvn_bytearray.h' 2>/dev/null \
                   | sed 's|/kvn/kvn_bytearray.h||' | head -1)
SIMPLEBLE_INC = -isystem $(SIMPLEBLE_DIR)/simpleble/include \
                $(if $(SIMPLEBLE_GEN),-isystem $(SIMPLEBLE_GEN),-isystem $(SIMPLEBLE_DIR)/build/export) \
                $(if $(SIMPLEBLE_KVN),-isystem $(SIMPLEBLE_KVN))

ifeq ($(UNAME), Linux)
    DBUS_CFLAGS := $(shell pkg-config --cflags dbus-1 2>/dev/null)
    DBUS_LIBS   := $(shell pkg-config --libs   dbus-1 2>/dev/null || echo "-ldbus-1")
    SIMPLEBLE_SYS = $(DBUS_LIBS) -lpthread
    SIMPLEBLE_INC += $(DBUS_CFLAGS)
else
    DBUS_CFLAGS :=
    SIMPLEBLE_SYS = -framework CoreBluetooth -framework Foundation \
                    -framework IOKit -framework IOBluetooth -lpthread
endif

NPROC := $(shell nproc 2>/dev/null || sysctl -n hw.logicalcpu 2>/dev/null || echo 4)

# ── Targets ───────────────────────────────────────────────────────────────────
PREFIX    ?= /usr/local
BINDIR     = $(PREFIX)/bin

.PHONY: all clean test install uninstall install-deps ble-deps ble_kiss_bridge

all: bbs ax25kiss ax25client

$(LIB_OBJ): $(LIB_SRC) ax25lib.hpp
	$(CXX) $(CXXFLAGS) -c -o $@ $<

$(BASIC_OBJ): $(BASIC_SRC) basic.hpp
	$(CXX) $(CXXFLAGS) $(SQLITE_FLAGS) -c -o $@ $<

bbs: bbs.cpp $(LIB_OBJ) $(BASIC_OBJ) ax25lib.hpp basic.hpp ini.hpp
	$(CXX) $(CXXFLAGS) $(SQLITE_FLAGS) -o $@ bbs.cpp $(LIB_OBJ) $(BASIC_OBJ) $(LDUTIL) $(SQLITE_LIBS)

ax25kiss: ax25kiss.cpp
	$(CXX) $(CXXFLAGS) -o $@ ax25kiss.cpp

ax25client: ax25client.cpp $(LIB_OBJ) $(BASIC_OBJ) ax25lib.hpp basic.hpp
	$(CXX) $(CXXFLAGS) $(SQLITE_FLAGS) -o $@ ax25client.cpp $(LIB_OBJ) $(BASIC_OBJ) $(SQLITE_LIBS)

test_ax25lib: test_ax25lib.cpp $(LIB_OBJ) $(BASIC_OBJ) ax25lib.hpp basic.hpp ini.hpp
	$(CXX) -std=c++17 $(GTEST_CFLAGS) $(SQLITE_FLAGS) \
	    -o $@ test_ax25lib.cpp $(LIB_OBJ) $(BASIC_OBJ) \
	    $(GTEST_LDFLAGS) $(SQLITE_LIBS)

test: test_ax25lib
	./test_ax25lib --gtest_color=yes

ble-deps:
	@if [ -d $(SIMPLEBLE_DIR)/simpleble/include/simpleble ]; then \
	    echo "SimpleBLE already present at $(SIMPLEBLE_DIR)."; \
	else \
	    echo "Cloning SimpleBLE..."; \
	    git clone --depth 1 --recurse-submodules --shallow-submodules \
	        https://github.com/OpenBluetoothToolbox/SimpleBLE $(SIMPLEBLE_DIR); \
	fi
	@echo "Building SimpleBLE (static)..."
	cmake -S $(SIMPLEBLE_DIR)/simpleble \
	      -B $(SIMPLEBLE_DIR)/build \
	      -DCMAKE_BUILD_TYPE=Release \
	      -DSIMPLEBLE_BUILD_SHARED_LIBS=OFF \
	      -DCMAKE_POSITION_INDEPENDENT_CODE=ON
	cmake --build $(SIMPLEBLE_DIR)/build --config Release -j$(NPROC)
	@echo ""
	@echo "SimpleBLE ready.  Now run:  make ble_kiss_bridge"

ble_kiss_bridge: ble_kiss_bridge.cpp
	$(eval BLE_LIB := $(shell find $(SIMPLEBLE_DIR)/build -name 'libsimpleble*.a' 2>/dev/null | head -1))
	@if [ -z "$(BLE_LIB)" ]; then \
	    echo "ERROR: SimpleBLE library not found.  Run: make ble-deps"; exit 1; fi
	$(CXX) -std=c++17 -O2 -Wall -Wextra \
	    $(SIMPLEBLE_INC) \
	    -o $@ ble_kiss_bridge.cpp \
	    $(BLE_LIB) $(SIMPLEBLE_SYS)
	@echo "Built: ble_kiss_bridge"

clean:
	rm -f $(LIB_OBJ) $(BASIC_OBJ) bbs ax25kiss ax25client test_ax25lib ble_kiss_bridge

# ── Install / Uninstall ───────────────────────────────────────────────────────
# Installs all built binaries to $(PREFIX)/bin  (default: /usr/local/bin).
# ble_kiss_bridge is installed only if already built.
# Override prefix:  make install PREFIX=/usr
install:
	@echo "Installing to $(BINDIR) ..."
	install -d $(BINDIR)
	install -m 755 bbs      $(BINDIR)/bbs
	install -m 755 ax25kiss $(BINDIR)/ax25kiss
	install -m 755 ax25client $(BINDIR)/ax25client
	@if [ -f ble_kiss_bridge ]; then \
	    install -m 755 ble_kiss_bridge $(BINDIR)/ble_kiss_bridge; \
	    echo "  installed: $(BINDIR)/ble_kiss_bridge"; \
	else \
	    echo "  skipped  : ble_kiss_bridge (not built — run: make ble-deps && make ble_kiss_bridge)"; \
	fi
	@echo "Done.  Binaries in $(BINDIR):"
	@echo "  bbs  ax25kiss  ax25client  ble_kiss_bridge"

uninstall:
	@echo "Removing from $(BINDIR) ..."
	rm -f $(BINDIR)/bbs \
	      $(BINDIR)/ax25kiss \
	      $(BINDIR)/ax25client \
	      $(BINDIR)/ble_kiss_bridge
	@echo "Done."

install-deps:
	@echo "Install dependencies:"
	@echo "  macOS  : brew install googletest sqlite cmake"
	@echo "  Ubuntu : sudo apt-get install libgtest-dev libsqlite3-dev cmake libdbus-1-dev"
	@echo "  Fedora : sudo dnf install gtest-devel sqlite-devel cmake dbus-devel"
	@echo ""
	@echo "BLE bridge (ble_kiss_bridge):"
	@echo "  All platforms : cmake required (see above)"
	@echo "  Linux only    : libdbus-1-dev (BlueZ backend)"
	@echo "  Then run      : make ble-deps && make ble_kiss_bridge"
