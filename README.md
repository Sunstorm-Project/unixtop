unixtop
=======

Unix Top is written by William LeFebvre, see http://www.unixtop.org.
Current maintainer of this fork: **Firefly** (Sunstorm Project).

This is a Sunstorm Project fork that:

- **Adds a Solaris 7+ OS module** (`m_solaris.c`) — reads per-process
  data from `/proc/<pid>/psinfo`, system-wide state from `kstat`, and
  swap totals from `swapctl(SC_GETNSWP/SC_LIST)`. Parent fork (Shuo
  Chen) carries the Linux threading improvements on top of LeFebvre's
  upstream 3.8beta1.
- **Adds a proper Makefile** with `MODULE=linux|solaris` selection.
  Every cross-compile knob (`CC`, `CFLAGS`, `CPPFLAGS`, `LDFLAGS`,
  `LIBS`, `PREFIX`, `DESTDIR`) is overridable.
- **Adds host-side unit tests** under `tests/`, run via `make check`,
  covering the portable shared modules (`utils.c`, `hash.c`).

Building
--------

Host / auto-detected module:

    make
    sudo make install

Explicit Solaris cross-build:

    make CC=sparc-sun-solaris2.7-gcc \
         MODULE=solaris \
         CFLAGS='-O2' \
         LIBS='-lkstat -lm -lncurses'
    make install DESTDIR=/tmp/stage PREFIX=/opt/sst

Testing
-------

`make check` builds and runs the host-side unit tests (using `HOST_CC`,
independent of the cross compiler). The tests cover the portable shared
code — the machine-specific `m_*.c` modules can't be exercised off
target.

    make check

Further reading
---------------

- [`FAQ.md`](FAQ.md) — usage, OS-specific gotchas, Y2038 note.
- [`Porting.md`](Porting.md) — how to add support for a new OS.
- [`Changes`](Changes) — release history (preserved from upstream).

Version
-------

This fork is versioned **3.9** (derived from LeFebvre's 3.8beta1, plus
Shuo Chen's Linux thread patches, plus the Solaris port and build
overhaul above).
