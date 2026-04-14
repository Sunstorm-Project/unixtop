# Makefile for unixtop
#
# Cross-compile friendly: every toolchain knob is overridable from the
# environment or the command line.
#
#   make                        # host build, auto-detects MODULE
#   make MODULE=solaris         # force Solaris /proc + kstat module
#   make CC=sparc-sun-solaris2.7-gcc MODULE=solaris \
#        CFLAGS="-O2" LIBS="-lkstat -lm -lncurses"
#
# MODULE values: linux, solaris. Add m_<name>.c and extend the case
# below to introduce a new one.

CC          ?= cc
CFLAGS      ?= -O2
CPPFLAGS    ?=
LDFLAGS     ?=
INSTALL     ?= install
PREFIX      ?= /usr/local
BINDIR      ?= $(PREFIX)/bin
MANDIR      ?= $(PREFIX)/share/man/man1

# Auto-detect MODULE from `uname -s` when not set explicitly. Cross
# compilers should always pass MODULE= on the command line.
ifndef MODULE
  UNAME_S := $(shell uname -s 2>/dev/null)
  ifeq ($(UNAME_S),Linux)
    MODULE := linux
  else ifeq ($(UNAME_S),SunOS)
    MODULE := solaris
  else
    MODULE := linux
  endif
endif

# Per-module default libraries. Override LIBS to replace wholesale.
ifeq ($(MODULE),solaris)
  MODULE_LIBS ?= -lkstat -lm -lcurses
else ifeq ($(MODULE),linux)
  MODULE_LIBS ?= -lm -lncurses
else
  MODULE_LIBS ?= -lm -lcurses
endif
LIBS ?= $(MODULE_LIBS)

COMMON_SRCS = color.c commands.c display.c hash.c screen.c \
              top.c username.c utils.c version.c
MODULE_SRC  = m_$(MODULE).c
SRCS        = $(COMMON_SRCS) $(MODULE_SRC)
OBJS        = $(SRCS:.c=.o)

ALL_CPPFLAGS = -DHAVE_CONFIG_H -I. $(CPPFLAGS)

.PHONY: all clean install help check
.DEFAULT_GOAL := all

# ---- Unit tests ------------------------------------------------------
# `make check` builds and runs unit tests on the BUILD HOST using
# HOST_CC, not the cross compiler. Tests cover the portable shared
# modules (utils.c, hash.c) — the machine-specific m_*.c modules can't
# be exercised off-target. Tests are skipped silently if HOST_CC is
# unavailable so cross-build pipelines can choose to opt in/out.
HOST_CC      ?= cc
# utils.c references HZ (clock ticks/sec), which glibc only exposes
# under _DEFAULT_SOURCE and macOS doesn't expose at all. Provide a
# matching default explicitly so the host test build is portable.
HOST_CFLAGS  ?= -O0 -g -Wall -Wno-unused-function -Wno-macro-redefined -DHZ=100
TEST_DIR     := tests
TESTS        := $(TEST_DIR)/test_utils $(TEST_DIR)/test_hash

# Tests link against host-built copies of the portable sources so that
# running `make check` after a cross `make` doesn't trip over target
# object files. Keep the host object files out of the main build tree.
HOST_OBJS    := $(TEST_DIR)/utils.host.o $(TEST_DIR)/hash.host.o

$(TEST_DIR)/%.host.o: %.c
	$(HOST_CC) $(HOST_CFLAGS) -DHAVE_CONFIG_H -I. -c -o $@ $<

$(TEST_DIR)/test_utils: $(TEST_DIR)/test_utils.c $(HOST_OBJS)
	$(HOST_CC) $(HOST_CFLAGS) -I. -o $@ $< $(HOST_OBJS) -lm

$(TEST_DIR)/test_hash: $(TEST_DIR)/test_hash.c $(HOST_OBJS)
	$(HOST_CC) $(HOST_CFLAGS) -I. -o $@ $< $(HOST_OBJS)

check: $(TESTS)
	@set -e; for t in $(TESTS); do \
	    echo "==> $$t"; ./$$t; \
	done
	@echo "All tests passed."

all: top

top: $(OBJS)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $(OBJS) $(LIBS)

%.o: %.c
	$(CC) $(ALL_CPPFLAGS) $(CFLAGS) -c -o $@ $<

install: top
	$(INSTALL) -d $(DESTDIR)$(BINDIR) $(DESTDIR)$(MANDIR)
	$(INSTALL) -m 0755 top $(DESTDIR)$(BINDIR)/top
	[ -f top.1 ] && $(INSTALL) -m 0644 top.1 $(DESTDIR)$(MANDIR)/top.1 || true

clean:
	rm -f top $(OBJS) $(HOST_OBJS) $(TESTS)

help:
	@echo "Targets: all (default), check, install, clean"
	@echo "Knobs:   CC, CFLAGS, CPPFLAGS, LDFLAGS, LIBS,"
	@echo "         MODULE (linux|solaris), PREFIX, DESTDIR"
	@echo "Host-side test knobs: HOST_CC, HOST_CFLAGS"

# Minimal header dependency hints — not exhaustive, but covers the
# files that change most often during porting work.
$(OBJS): config.h top.h
hash.o: hash.h
display.o: display.h layout.h
screen.o: screen.h
commands.o: commands.h
color.o: color.h
username.o: username.h
utils.o: utils.h
m_$(MODULE).o: machine.h hash.h utils.h username.h
