unixtop — FAQ
=============

This FAQ covers the Sunstorm Project fork. For upstream's historical
FAQ (SourceForge mailing lists, ftp.unixtop.org, SunOS 4.1.x, MacOSX
pre-release, etc.) see the git history before v3.9.

General
-------

**What is top?** A regularly-updated display showing per-process and
system-wide CPU, memory, and load information. Think of it as a
full-screen `ps` that refreshes on an interval.

**Which OSes does this fork target?** Linux (via `m_linux.c`,
inherited from Shuo Chen's fork) and Solaris 7+ (via `m_solaris.c`,
added in v3.9). `make MODULE=linux` and `make MODULE=solaris` select
between them; the default is auto-detected from `uname -s`.

**Who maintains it?** The Sunstorm Project build pipeline uses this
fork to provide `top` for Solaris 7 on SPARC. Patches and ports of the
Solaris module to later Solaris releases are welcome.

Compiling
---------

**We upgraded the OS and top started showing wrong numbers.**
Recompile. `top` reads kernel-internal structures (via `/proc`,
`psinfo_t`, `kstat`, etc.); ABI changes across OS releases will
silently break the display until the binary is rebuilt against the new
headers.

**I need to cross-compile.** The Makefile exposes every cross knob:

    make CC=sparc-sun-solaris2.7-gcc \
         MODULE=solaris \
         CFLAGS='-O2' \
         LIBS='-lkstat -lm -lncurses'

See `README.md` for the full build recipe.

**How do I run the unit tests?** `make check`. Tests are host-native
(built with `HOST_CC`, defaulting to `cc`) and cover the portable
shared modules (`utils.c`, `hash.c`). The machine-specific `m_*.c`
modules can't be exercised off target.

Running
-------

**top can't read some files when I run it as a non-root user.** On
Solaris and Linux `/proc` access is governed by credentials; most
`psinfo`-derived fields are world-readable but `cmdline` and argument
parsing may be restricted for privileged processes. Run as root or
install `top` setuid/setgid if your site policy allows it.

**`top` isn't showing idle processes (or won't stop showing them).**
Toggle with the `i` key interactively. See the `TOP` environment
variable and the `-I` / `-i` flags in the manpage for the permanent
default.

**Multi-CPU summary.** CPU state percentages are combined across all
CPUs into a single line. Per-CPU breakdown isn't supported in this
fork.

Solaris-specific
----------------

**Does this fork support LWP-level thread display on Solaris?** Not
yet. `m_solaris.c` exposes one row per process; `pr_nlwp` is shown in
the THR column but individual LWPs aren't walked. Adding it means
iterating `/proc/<pid>/lwp/*/lwpsinfo` and wiring the second hash
bucket. PRs welcome.

**Memory reporting looks off — the "used" figure counts the file
cache.** Yes. Solaris doesn't separate buffer/cache pages from
truly-allocated pages in `unix:0:system_pages`; `MEMUSED` is
`physmem - freemem`, so cached pages read as "used". The Linux module
compensates using `/proc/meminfo`'s `Buffers:` and `Cached:`; Solaris
has no comparable source.

**Kernel counters (ctxsw/intr/flt/newproc) show zero.** Solaris
doesn't expose these as scalar kstats in the shape top expects. They
could be derived from summing `cpu_stat_t.cpu_sysinfo` fields across
all CPUs; not done yet.

**Y2038.** On 32-bit Solaris 7, `time_t` wraps at 03:14:08 UTC on
January 19, 2038. The display of the current time will fail at that
point; everything else (ticks, percentages, sort keys) is unaffected.

Bugs / contact
--------------

File issues at https://github.com/Sunstorm-Project/unixtop/issues.
