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

# ── Targets ───────────────────────────────────────────────────────────────────
.PHONY: all clean test install-deps

all: bbs ax25kiss

$(LIB_OBJ): $(LIB_SRC) ax25lib.hpp
	$(CXX) $(CXXFLAGS) -c -o $@ $<

$(BASIC_OBJ): $(BASIC_SRC) basic.hpp
	$(CXX) $(CXXFLAGS) $(SQLITE_FLAGS) -c -o $@ $<

bbs: bbs.cpp $(LIB_OBJ) $(BASIC_OBJ) ax25lib.hpp basic.hpp ini.hpp
	$(CXX) $(CXXFLAGS) $(SQLITE_FLAGS) -o $@ bbs.cpp $(LIB_OBJ) $(BASIC_OBJ) $(LDUTIL) $(SQLITE_LIBS)

ax25kiss: ax25kiss.cpp
	$(CXX) $(CXXFLAGS) -o $@ ax25kiss.cpp

test_ax25lib: test_ax25lib.cpp $(LIB_OBJ) $(BASIC_OBJ) ax25lib.hpp basic.hpp ini.hpp
	$(CXX) -std=c++17 $(GTEST_CFLAGS) $(SQLITE_FLAGS) \
	    -o $@ test_ax25lib.cpp $(LIB_OBJ) $(BASIC_OBJ) \
	    $(GTEST_LDFLAGS) $(SQLITE_LIBS)

test: test_ax25lib
	./test_ax25lib --gtest_color=yes

clean:
	rm -f $(LIB_OBJ) $(BASIC_OBJ) bbs ax25kiss test_ax25lib

install-deps:
	@echo "Install dependencies:"
	@echo "  macOS  : brew install googletest sqlite"
	@echo "  Ubuntu : sudo apt-get install libgtest-dev libsqlite3-dev"
	@echo "  Fedora : sudo dnf install gtest-devel sqlite-devel"
