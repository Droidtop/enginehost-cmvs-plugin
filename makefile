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

# The reference saves the save test reads: the ones the user made in
# ChronoClock on the Windows host. They are not in this repository - they are
# one person's saves of one game and they live beside the format analysis in
# the coordination folder - so the test skips what is not here.
SAVES ?= $(wildcard /root/coordination/agents/cmvs/re/saves)

CC       := gcc
CSTD     := -std=c11
WARN     := -Wall -Wextra -Wno-unused-parameter
# -MMD -MP writes a .d beside every .o naming the headers it read, so editing
# a header rebuilds what included it. Without it a struct can grow in a header
# and half the objects keep the old layout, which is a whole evening.
DEPFLAGS := -MMD -MP
CFLAGS   := $(CSTD) $(WARN) -O2 -g $(DEPFLAGS) -I$(SRCDIR) -Ithird_party $(shell sdl2-config --cflags)
LDFLAGS  := $(shell sdl2-config --libs) -lm -lz

# make DEBUG=1 turns on the sanitizers, which is how the format code gets its
# bounds checks exercised against real game data.
ifdef DEBUG
CFLAGS  := $(CSTD) $(WARN) -O0 -g -fsanitize=address,undefined $(DEPFLAGS) -I$(SRCDIR) -Ithird_party $(shell sdl2-config --cflags)
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
test: $(BUILDDIR)/test_schemes $(BUILDDIR)/test_saves
	$(BUILDDIR)/test_schemes $(GAMES)
	$(BUILDDIR)/test_saves $(SAVES) $(GAMES)

$(BUILDDIR)/test_schemes: tests/test_schemes.c $(LIBOBJECTS) | $(BUILDDIR)
	$(CC) $(CFLAGS) -o $@ tests/test_schemes.c $(LIBOBJECTS) $(LDFLAGS)

$(BUILDDIR)/test_saves: tests/test_saves.c $(LIBOBJECTS) | $(BUILDDIR)
	$(CC) $(CFLAGS) -o $@ tests/test_saves.c $(LIBOBJECTS) $(LDFLAGS)

run: all
	$(BUILDDIR)/$(TARGET) $(GAME)

clean:
	rm -f $(BUILDDIR)/*.o $(BUILDDIR)/*.d $(BUILDDIR)/$(TARGET)

-include $(OBJECTS:.o=.d)
