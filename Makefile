# =============================================================================
# Makefile — ax25lib + BBS + tests
# =============================================================================
CXX      ?= g++
# Library, BBS, and ax25kiss all build cleanly with C++11.
CXXFLAGS  = -std=c++11 -O2 -Wall -Wextra -Wpedantic
# GoogleTest >=1.13 requires C++17 internally; use a separate flag for tests.
# Your production code that links ax25lib can still use -std=c++11.
TEST_CXXFLAGS = -std=c++17 -O2 -Wall -Wextra
UNAME    := $(shell uname)

# Linux needs -lutil for forkpty()
ifeq ($(UNAME), Linux)
    LDUTIL = -lutil
else
    LDUTIL =
endif

LIB_SRC = ax25lib.cpp
LIB_OBJ = ax25lib.o

# ── GoogleTest detection ───────────────────────────────────────────────────
# Try pkg-config first (Homebrew on macOS, or system install on Linux)
GTEST_CFLAGS  := $(shell pkg-config --cflags gtest 2>/dev/null)
GTEST_LDFLAGS := $(shell pkg-config --libs gtest_main 2>/dev/null)

# Fallback: try well-known Homebrew prefix (Apple Silicon)
ifeq ($(GTEST_LDFLAGS),)
    ifneq ($(wildcard /opt/homebrew/lib/libgtest.a),)
        GTEST_CFLAGS  := -I/opt/homebrew/include
        GTEST_LDFLAGS := -L/opt/homebrew/lib -lgtest -lgtest_main
    endif
endif

# Fallback: try Homebrew Intel prefix
ifeq ($(GTEST_LDFLAGS),)
    ifneq ($(wildcard /usr/local/lib/libgtest.a),)
        GTEST_CFLAGS  := -I/usr/local/include
        GTEST_LDFLAGS := -L/usr/local/lib -lgtest -lgtest_main
    endif
endif

# Fallback: hope it's on the default library path
ifeq ($(GTEST_LDFLAGS),)
    GTEST_LDFLAGS := -lgtest -lgtest_main
endif

GTEST_LDFLAGS += -lpthread

# ── Targets ───────────────────────────────────────────────────────────────────
.PHONY: all clean test

all: bbs ax25kiss

$(LIB_OBJ): $(LIB_SRC) ax25lib.hpp
	$(CXX) $(CXXFLAGS) -c -o $@ $<

bbs: bbs.cpp $(LIB_OBJ) ax25lib.hpp
	$(CXX) $(CXXFLAGS) -o $@ bbs.cpp $(LIB_OBJ) $(LDUTIL)

ax25kiss: ax25kiss.cpp
	$(CXX) $(CXXFLAGS) -o $@ ax25kiss.cpp

test_ax25lib: test_ax25lib.cpp $(LIB_OBJ) ax25lib.hpp
	$(CXX) $(TEST_CXXFLAGS) $(GTEST_CFLAGS) -o $@ test_ax25lib.cpp $(LIB_OBJ) $(GTEST_LDFLAGS)

test: test_ax25lib
	./test_ax25lib --gtest_color=yes

clean:
	rm -f $(LIB_OBJ) bbs ax25kiss test_ax25lib

# ── Install hints ─────────────────────────────────────────────────────────────
.PHONY: gtest-install-hint
gtest-install-hint:
	@echo "Install GoogleTest:"
	@echo "  macOS  : brew install googletest"
	@echo "  Ubuntu : sudo apt-get install libgtest-dev"
	@echo "  Fedora : sudo dnf install gtest-devel"
