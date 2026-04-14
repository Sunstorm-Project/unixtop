Porting unixtop to a new OS
==========================

Adding support for a new OS means writing one new file: `m_<name>.c`.
The machine-independent code (`top.c`, `display.c`, `screen.c`,
`commands.c`, `utils.c`, `hash.c`) is shared across every target and
shouldn't need changes. `m_solaris.c` (added in 3.9) is a recent
reference; `m_linux.c` is the one most contributors know best.

The interface
-------------

Your module fills in two structures and implements a handful of
functions. Both structures are declared in [`machine.h`](machine.h).

### `struct statics` — filled once, in `machine_init`

Points to the label arrays used by the header/summary display:

    char **procstate_names;   /* "", "running", "sleeping", ... */
    char **cpustate_names;    /* "user", "system", "wait", "idle" */
    char **memory_names;      /* "K total", "K used", "K free", ... */
    char **swap_names;        /* optional */
    char **order_names;       /* optional — for the 'o' sort-order UI */
    char **kernel_names;      /* optional */
    time_t boottime;          /* optional */
    struct { ... } flags;     /* fullcmds, idle, warmup, threads */

Every array is NULL-terminated. An empty-string entry (`""`) tells the
display code to skip that column — handy when your OS can't produce a
particular datum.

### `struct system_info` — filled every refresh, in `get_system_info`

    int    last_pid;
    double load_avg[NUM_AVERAGES];
    int    p_total;
    int    p_active;
    int   *procstates;   /* counts indexed by procstate_names */
    int   *cpustates;    /* percentages × 10, e.g. 987 == 98.7% */
    int   *kernel;
    long  *memory;
    long  *swap;

The `cpustates` array holds percentages multiplied by 10 (integer), so
"98.7% idle" is 987. Use `percentages()` from `utils.c` to compute
these from raw cumulative tick counters.

### Functions you implement

    int   machine_init(struct statics *);
    void  get_system_info(struct system_info *);
    caddr_t get_process_info(struct system_info *,
                             struct process_select *, int compare_index);
    char *format_header(char *uname_field);
    char *format_next_process(caddr_t handle, char *(*get_userid)(int));
    int   proc_owner(int pid);
    #ifdef HAVE_FORMAT_PROCESS_HEADER
    char *format_process_header(struct process_select *, caddr_t, int count);
    #endif

`proc_owner` is security-critical: it validates kill/renice requests
by returning the uid that owns a given pid, or -1 if the process is
gone. Get this wrong and `top` hands out privilege escalation.

Practical tips
--------------

**Pick a reference module.** Walk through `m_solaris.c` or `m_linux.c`
end-to-end before you start writing. Most ports borrow the sort-
comparator macros (`ORDERKEY_PCTCPU` etc.), the state-abbrev table,
and the `top_proc` pool allocator wholesale.

**Use `hash.c`.** The per-pid hash (`hash_add_pid`, `hash_lookup_pid`,
`hash_remove_pid`, `hash_first_pid` / `hash_next_pid` iteration, plus
`hash_remove_pos_pid` for delete-during-iteration) gives you stable
per-process state across refreshes. `m_solaris.c` uses it to carry
`pr_pctcpu` deltas; your module can use it for anything keyed by pid.

**Add the module to the Makefile.** Open `Makefile`, add a case to the
`ifeq ($(MODULE),...)` ladder defining `MODULE_LIBS` (the libraries
your module needs at link time), then add your name to the
auto-detection `ifeq ($(UNAME_S),...)` block if appropriate. The
source file is picked up automatically via `MODULE_SRC = m_$(MODULE).c`.

**Add host-side tests when you touch shared code.** `make check` runs
the `tests/` suite against `utils.c` and `hash.c`. If your port adds a
new pure helper to one of those, add coverage there — the
`m_*.c` modules themselves can't run on the build host.

**Document OS-specific quirks** in [`FAQ.md`](FAQ.md) under an
OS-specific heading. The man page is OS-agnostic; platform-specific
notes live in the FAQ.

Before you start
----------------

Open an issue on https://github.com/Sunstorm-Project/unixtop/issues
naming the platform you're porting to, so duplicate effort is
avoided and module naming can be coordinated. Once the port works,
open a PR — we'd rather carry it upstream than have it live as a
private patch.
