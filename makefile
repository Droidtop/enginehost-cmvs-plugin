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
#
# ------------------------------------------------------------------------------

TARGET   := cmvs
SRCDIR   := src
BUILDDIR := bin

CSOURCES := $(wildcard $(SRCDIR)/*.c)
OBJECTS  := $(patsubst $(SRCDIR)/%.c,$(BUILDDIR)/%.o,$(CSOURCES))

CC       := gcc
CSTD     := -std=c11
WARN     := -Wall -Wextra -Wno-unused-parameter
CFLAGS   := $(CSTD) $(WARN) -O2 -g -I$(SRCDIR) $(shell sdl2-config --cflags)
LDFLAGS  := $(shell sdl2-config --libs) -lm

# make DEBUG=1 turns on the sanitizers, which is how the format code gets its
# bounds checks exercised against real game data.
ifdef DEBUG
CFLAGS  := $(CSTD) $(WARN) -O0 -g -fsanitize=address,undefined -I$(SRCDIR) $(shell sdl2-config --cflags)
LDFLAGS := -fsanitize=address,undefined $(shell sdl2-config --libs) -lm
endif

.PHONY: all clean run

all: $(BUILDDIR)/$(TARGET)

$(BUILDDIR)/$(TARGET): $(OBJECTS)
	$(CC) -o $@ $^ $(LDFLAGS)

$(BUILDDIR)/%.o: $(SRCDIR)/%.c | $(BUILDDIR)
	$(CC) $(CFLAGS) -c -o $@ $<

$(BUILDDIR):
	mkdir -p $(BUILDDIR)

run: all
	$(BUILDDIR)/$(TARGET) $(GAME)

clean:
	rm -f $(BUILDDIR)/*.o $(BUILDDIR)/$(TARGET)
