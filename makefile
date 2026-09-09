# ------------------------------------------------------------------------------
#
# Makefile for the CMVS engine (desktop build).
#
# The same sources under src/ are compiled for Android by
# app/src/main/cpp/CMakeLists.txt; nothing in src/ may include an Android
# header. Build and run here first: a format is proven on the desktop before
# the plugin wraps it.
#
#   make            build bin/cmvs
#   make run GAME=/path/to/game
#   make test       build and run the offline tests
#
# ------------------------------------------------------------------------------

TARGET   := cmvs
SRCDIR   := src
BUILDDIR := bin

CSOURCES := $(wildcard $(SRCDIR)/*.c)
OBJECTS  := $(patsubst $(SRCDIR)/%.c,$(BUILDDIR)/%.o,$(CSOURCES))

# Everything but the desktop runner, so a test can bring its own main().
LIBOBJECTS := $(filter-out $(BUILDDIR)/main.o,$(OBJECTS))

# Which games the archive tests get to try. Only ChronoClock is on this
# machine; name more and they are tested too, which is the point.
GAMES ?= $(wildcard /root/re/chronoclock)

CC       := gcc
CSTD     := -std=c11
WARN     := -Wall -Wextra -Wno-unused-parameter
CFLAGS   := $(CSTD) $(WARN) -O2 -g -I$(SRCDIR) -Ithird_party $(shell sdl2-config --cflags)
LDFLAGS  := $(shell sdl2-config --libs) -lm -lz

# make DEBUG=1 turns on the sanitizers, which is how the format code gets its
# bounds checks exercised against real game data.
ifdef DEBUG
CFLAGS  := $(CSTD) $(WARN) -O0 -g -fsanitize=address,undefined -I$(SRCDIR) -Ithird_party $(shell sdl2-config --cflags)
LDFLAGS := -fsanitize=address,undefined $(shell sdl2-config --libs) -lm -lz
endif

.PHONY: all clean run test

all: $(BUILDDIR)/$(TARGET)

$(BUILDDIR)/$(TARGET): $(OBJECTS)
	$(CC) -o $@ $^ $(LDFLAGS)

$(BUILDDIR)/%.o: $(SRCDIR)/%.c | $(BUILDDIR)
	$(CC) $(CFLAGS) -c -o $@ $<

$(BUILDDIR):
	mkdir -p $(BUILDDIR)

# The tests need no display, no device and no network: they read the scheme
# table and whatever real archives this machine happens to have.
test: $(BUILDDIR)/test_schemes
	$(BUILDDIR)/test_schemes $(GAMES)

$(BUILDDIR)/test_schemes: tests/test_schemes.c $(LIBOBJECTS) | $(BUILDDIR)
	$(CC) $(CFLAGS) -o $@ tests/test_schemes.c $(LIBOBJECTS) $(LDFLAGS)

run: all
	$(BUILDDIR)/$(TARGET) $(GAME)

clean:
	rm -f $(BUILDDIR)/*.o $(BUILDDIR)/$(TARGET)
