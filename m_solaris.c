/*
 * Copyright (c) 1984 through 2008, William LeFebvre
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met: see m_linux.c for full BSD-3-Clause text.
 */

/*
 * top - a top users display for Unix
 *
 * SYNOPSIS:  Solaris 7 and later (structured /proc + kstat)
 *
 * DESCRIPTION:
 * Machine-dependent module for Solaris. Reads per-process data from
 * /proc/<pid>/psinfo (psinfo_t, documented in proc(4)), system-wide
 * state from kstat, and swap totals from swapctl(SC_GETNSWP/SC_LIST).
 *
 * LIBS: -lkstat
 *
 * AUTHOR: ported for the Sunstorm/SST build pipeline, 2026-04-13.
 *         Structure and output shape mirror m_linux.c so that the
 *         machine-independent code (top.c, display.c) needs no change.
 */

#include "config.h"

#include <sys/types.h>
#include <sys/time.h>
#include <sys/stat.h>
#include <sys/param.h>
#include <sys/swap.h>
#include <sys/proc.h>
#include <sys/procfs.h>
#include <sys/sysinfo.h>

#include <procfs.h>
#include <kstat.h>
#include <time.h>
#include <stdio.h>
#include <fcntl.h>
#include <unistd.h>
#include <stdlib.h>
#include <errno.h>
#include <dirent.h>
#include <string.h>
#include <math.h>
#include <ctype.h>

#include "top.h"
#include "hash.h"
#include "machine.h"
#include "utils.h"
#include "username.h"

#define PROCFS "/proc"
extern char *myname;

/* Solaris scales avenrun_* kstat values by FSCALE (from <sys/param.h>,
 * typically 256). Use a dedicated double-valued constant so we never
 * accidentally integer-divide a uint32_t by the integer FSCALE macro. */
#ifndef FSCALE
#define FSCALE 256
#endif
#define FSCALE_D ((double)FSCALE)

/* psinfo pr_pctcpu is fixed-point: fraction = pr_pctcpu / 0x8000. */
#define PCT_TO_DOUBLE(p) ((double)(p) / 0x8000)

/*=PROCESS INFORMATION==================================================*/

struct top_proc
{
    pid_t pid;
    pid_t tgid;            /* == pid on Solaris (no tgid concept) */
    uid_t uid;
    char *name;
    int pri, nice, threads;
    unsigned long size, rss, shared;    /* in k */
    int state, processor;
    unsigned long wchan;
    unsigned long time;                 /* cpu ticks consumed */
    unsigned long start_time;
    double pcpu;
    struct top_proc *next;
};

/*=STATE IDENT STRINGS==================================================*/

/* 1..6 correspond to pr_sname chars: R/O S D Z T I.
 * 'D' (uninterruptable) doesn't really exist on Solaris; kept so the
 * table layout matches m_linux.c and the sort_state[] indices line up. */
#define NPROCSTATES 7
static char *state_abbrev[NPROCSTATES + 1] = {
    "", "R", "S", "D", "Z", "T", "I",
    NULL
};

static char *procstatenames[NPROCSTATES + 1] = {
    "", " running, ", " sleeping, ", " uninterruptable, ",
    " zombie, ", " stopped, ", " idle, ",
    NULL
};

/* Solaris cpu_stat_t tracks user/kernel/wait/idle; no "nice" bucket. */
#define NCPUSTATES 4
static char *cpustatenames[NCPUSTATES + 1] = {
    "user", "system", "wait", "idle",
    NULL
};

#define KERNELCTXT    0
#define KERNELFLT     1
#define KERNELINTR    2
#define KERNELNEWPROC 3
#define NKERNELSTATS  4
static char *kernelnames[NKERNELSTATS + 1] = {
    " ctxsw, ", " flt, ", " intr, ", " newproc",
    NULL
};

#define MEMTOTAL         0
#define MEMUSED          1
#define MEMFREE          2
#define MEMSHARED        3
#define MEMBUFFERS       4
#define MEMCACHED        5
#define MEMBUFFERSCACHED 6
#define MEMREALUSED      7
#define MEMREALFREE      8
#define NMEMSTATS        9
static char *memorynames[NMEMSTATS + 1] = {
    "K total, ", "K used, ", "K free, ", "K shared, ", "K buffers, ",
    "K cached, ", "K buffers+cached, ", "K real used, ", "K real free",
    NULL
};

#define SWAPTOTAL  0
#define SWAPUSED   1
#define SWAPFREE   2
#define SWAPCACHED 3
#define NSWAPSTATS 4
static char *swapnames[NSWAPSTATS + 1] = {
    "K total, ", "K used, ", "K free, ", "K cached",
    NULL
};

static char fmt_header[] =
"    PID X         THR PRI NICE  SIZE   RES STATE    TIME    CPU COMMAND";

static char proc_header_thr[] =
"    PID %-9s THR  PR  NI  SIZE   RES   SHR S  P    TIME+   CPU%% COMMAND";

static char proc_header_nothr[] =
"    TID %-9s   TGID  PR  NI  SIZE   RES   SHR S  P    TIME+   CPU%% COMMAND";

char *ordernames[] = {
    "cpu", "size", "res", "time", "command", "threads", NULL
};

int compare_cpu();
int compare_size();
int compare_res();
int compare_time();
int compare_cmd();
int compare_threads();

int (*proc_compares[])() = {
    compare_cpu,
    compare_size,
    compare_res,
    compare_time,
    compare_cmd,
    compare_threads,
    NULL
};

/*=SYSTEM STATE INFO====================================================*/

static long cp_time[NCPUSTATES];
static long cp_old[NCPUSTATES];
static long cp_diff[NCPUSTATES];

static struct timeval lasttime = { 0, 0 };
static struct timeval timediff = { 0, 0 };
static long elapsed_msecs;

#define HASH_SIZE           (1003)
#define INITIAL_ACTIVE_SIZE (256)
#define PROCBLOCK_SIZE      (32)
static hash_table *ptable;
static struct top_proc **pactive;
static struct top_proc **nextactive;
static int activesize = 0;
static time_t boottime = -1;
static long pagesize_kb = 4;        /* real value filled in machine_init */
static long clock_tick = 100;

static int cpu_states[NCPUSTATES];
static int process_states[NPROCSTATES];
static int kernel_stats[NKERNELSTATS];
static long memory_stats[NMEMSTATS];
static long swap_stats[NSWAPSTATS];

static kstat_ctl_t *kc = NULL;

/* percentages() and printable() are declared in utils.h (already
 * included) — percentages returns long, printable returns char *, so
 * redeclaring here would conflict. */

/*======================================================================*/

static void
xfrm_cmdline(char *p, int len)
{
    while (--len > 0)
    {
        if (*p == '\0')
            *p = ' ';
        p++;
    }
}

static void
update_procname(struct top_proc *proc, char *cmd)
{
    printable(cmd);

    if (proc->name == NULL)
        proc->name = strdup(cmd);
    else if (strcmp(proc->name, cmd) != 0)
    {
        free(proc->name);
        proc->name = strdup(cmd);
    }
}

static struct top_proc *freelist = NULL;
static struct top_proc *procblock = NULL;
static struct top_proc *procmax = NULL;

static struct top_proc *
new_proc(void)
{
    struct top_proc *p;

    if (freelist)
    {
        p = freelist;
        freelist = freelist->next;
    }
    else if (procblock)
    {
        p = procblock;
        if (++procblock >= procmax)
            procblock = NULL;
    }
    else
    {
        p = procblock = (struct top_proc *)calloc(PROCBLOCK_SIZE,
                                                  sizeof(struct top_proc));
        procmax = procblock++ + PROCBLOCK_SIZE;
    }

    if (p->name != NULL)
    {
        free(p->name);
        p->name = NULL;
    }
    return p;
}

static void
free_proc(struct top_proc *proc)
{
    proc->next = freelist;
    freelist = proc;
}

/*
 * map_state: translate psinfo pr_sname (single character) to the same
 * 1..6 state indices m_linux.c uses.
 */
static int
map_state(char pr_sname)
{
    switch (pr_sname)
    {
    case 'O': return 1;         /* on-cpu, treat as "running" */
    case 'R': return 1;         /* runnable */
    case 'S': return 2;         /* sleeping */
    case 'Z': return 4;         /* zombie */
    case 'T': return 5;         /* stopped */
    case 'I': return 6;         /* idle (process being created) */
    default:  return 2;         /* unknown -> sleeping */
    }
}

int
machine_init(struct statics *statics)
{
    struct stat st;

    if (stat(PROCFS, &st) < 0 || !S_ISDIR(st.st_mode))
    {
        fprintf(stderr, "%s: proc filesystem not mounted on " PROCFS "\n",
                myname);
        return -1;
    }

    kc = kstat_open();
    if (kc == NULL)
    {
        fprintf(stderr, "%s: kstat_open failed: %s\n", myname, strerror(errno));
        return -1;
    }

    {
        long ps = sysconf(_SC_PAGESIZE);
        /* sysconf(_SC_PAGESIZE) can't fail on Solaris, but a wrapper
         * that mis-defines it would wreck every memory calc below.
         * Fall back to 8 KB (SPARC native) if the value looks bogus. */
        pagesize_kb = (ps >= 1024) ? (ps / 1024) : 8;
    }
    clock_tick = sysconf(_SC_CLK_TCK);
    if (clock_tick <= 0) clock_tick = 100;

    /* boot time from kstat unix:0:system_misc:boot_time */
    {
        kstat_t *ksp = kstat_lookup(kc, "unix", 0, "system_misc");
        if (ksp != NULL && kstat_read(kc, ksp, NULL) != -1)
        {
            kstat_named_t *kn = kstat_data_lookup(ksp, "boot_time");
            if (kn != NULL)
                boottime = (time_t)kn->value.ui32;
        }
    }

    statics->procstate_names = procstatenames;
    statics->cpustate_names = cpustatenames;
    statics->kernel_names = kernelnames;
    statics->memory_names = memorynames;
    statics->swap_names = swapnames;
    statics->order_names = ordernames;
    statics->boottime = boottime;
    statics->flags.fullcmds = 1;
    statics->flags.warmup = 1;
    statics->flags.threads = 0;     /* Solaris LWP threading not surfaced yet */

    pactive = (struct top_proc **)malloc(sizeof(struct top_proc *)
                                         * INITIAL_ACTIVE_SIZE);
    activesize = INITIAL_ACTIVE_SIZE;
    ptable = hash_create(HASH_SIZE);
    return 0;
}

/*
 * Sum user/kernel/wait/idle ticks across every cpu_stat:<n>:cpu_stat<n>
 * kstat. Solaris 7 returns a cpu_stat_t whose .cpu_sysinfo.cpu[] array
 * is indexed by CPU_USER / CPU_KERNEL / CPU_WAIT / CPU_IDLE.
 */
static void
read_cpu_stats(void)
{
    kstat_t *ksp;
    long total[NCPUSTATES] = { 0, 0, 0, 0 };

    (void)kstat_chain_update(kc);

    for (ksp = kc->kc_chain; ksp != NULL; ksp = ksp->ks_next)
    {
        if (strcmp(ksp->ks_module, "cpu_stat") != 0)
            continue;
        if (kstat_read(kc, ksp, NULL) == -1)
            continue;

        cpu_stat_t cs;
        /* kstat_read() into raw data — ks_data now points at a cpu_stat_t */
        memcpy(&cs, ksp->ks_data, sizeof(cs));

        total[0] += cs.cpu_sysinfo.cpu[CPU_USER];
        total[1] += cs.cpu_sysinfo.cpu[CPU_KERNEL];
        total[2] += cs.cpu_sysinfo.cpu[CPU_WAIT];
        total[3] += cs.cpu_sysinfo.cpu[CPU_IDLE];
    }

    cp_time[0] = total[0];
    cp_time[1] = total[1];
    cp_time[2] = total[2];
    cp_time[3] = total[3];

    percentages(NCPUSTATES, cpu_states, cp_time, cp_old, cp_diff);
}

static void
read_mem_stats(void)
{
    kstat_t *ksp;
    unsigned long physmem_pg = 0, freemem_pg = 0;

    ksp = kstat_lookup(kc, "unix", 0, "system_pages");
    if (ksp != NULL && kstat_read(kc, ksp, NULL) != -1)
    {
        kstat_named_t *kn;
        if ((kn = kstat_data_lookup(ksp, "physmem")) != NULL)
            physmem_pg = kn->value.ul;
        if ((kn = kstat_data_lookup(ksp, "freemem")) != NULL)
            freemem_pg = kn->value.ul;
    }

    long total_kb = (long)(physmem_pg * pagesize_kb);
    long free_kb  = (long)(freemem_pg * pagesize_kb);

    memory_stats[MEMTOTAL]        = total_kb;
    memory_stats[MEMFREE]         = free_kb;
    memory_stats[MEMUSED]         = total_kb - free_kb;
    memory_stats[MEMSHARED]       = 0;
    memory_stats[MEMBUFFERS]      = 0;
    memory_stats[MEMCACHED]       = 0;
    memory_stats[MEMBUFFERSCACHED]= 0;
    memory_stats[MEMREALUSED]     = total_kb - free_kb;
    memory_stats[MEMREALFREE]     = free_kb;
}

static void
read_swap_stats(void)
{
    int n = swapctl(SC_GETNSWP, NULL);
    long total_pg = 0, free_pg = 0;

    if (n > 0)
    {
        size_t sz = sizeof(struct swaptable) + n * sizeof(struct swapent);
        struct swaptable *st = (struct swaptable *)malloc(sz);
        if (st != NULL)
        {
            /* Each swapent needs an ste_path buffer of its own. */
            char *paths = (char *)malloc(n * MAXPATHLEN);
            if (paths != NULL)
            {
                int i;
                st->swt_n = n;
                for (i = 0; i < n; i++)
                    st->swt_ent[i].ste_path = paths + i * MAXPATHLEN;

                /* swapctl(SC_LIST) returns the number of entries it
                 * actually filled. It may be < n if a swap device went
                 * away between SC_GETNSWP and SC_LIST. Iterate only
                 * over what came back so we don't sum uninitialised
                 * swapent data. */
                int got = swapctl(SC_LIST, st);
                if (got > 0)
                {
                    if (got > n) got = n;
                    for (i = 0; i < got; i++)
                    {
                        total_pg += st->swt_ent[i].ste_pages;
                        free_pg  += st->swt_ent[i].ste_free;
                    }
                }
                free(paths);
            }
            free(st);
        }
    }

    swap_stats[SWAPTOTAL]  = total_pg * pagesize_kb;
    swap_stats[SWAPFREE]   = free_pg * pagesize_kb;
    swap_stats[SWAPUSED]   = (total_pg - free_pg) * pagesize_kb;
    swap_stats[SWAPCACHED] = 0;
}

static void
read_load_and_lastpid(struct system_info *info)
{
    kstat_t *ksp = kstat_lookup(kc, "unix", 0, "system_misc");
    info->load_avg[0] = info->load_avg[1] = info->load_avg[2] = 0.0;
    info->last_pid = -1;

    if (ksp != NULL && kstat_read(kc, ksp, NULL) != -1)
    {
        kstat_named_t *kn;
        if ((kn = kstat_data_lookup(ksp, "avenrun_1min")) != NULL)
            info->load_avg[0] = kn->value.ui32 / FSCALE_D;
        if ((kn = kstat_data_lookup(ksp, "avenrun_5min")) != NULL)
            info->load_avg[1] = kn->value.ui32 / FSCALE_D;
        if ((kn = kstat_data_lookup(ksp, "avenrun_15min")) != NULL)
            info->load_avg[2] = kn->value.ui32 / FSCALE_D;
    }
}

void
get_system_info(struct system_info *info)
{
    struct timeval thistime;

    gettimeofday(&thistime, 0);
    timersub(&thistime, &lasttime, &timediff);
    elapsed_msecs = timediff.tv_sec * 1000 + timediff.tv_usec / 1000;
    lasttime = thistime;

    read_load_and_lastpid(info);
    read_cpu_stats();
    read_mem_stats();
    read_swap_stats();

    /* Solaris kstats don't expose a single counter for ctxsw/intr/etc.
     * in the shape top expects — zero these out rather than mislead. */
    memset(kernel_stats, 0, sizeof(kernel_stats));

    info->cpustates = cpu_states;
    info->memory    = memory_stats;
    info->swap      = swap_stats;
    info->kernel    = kernel_stats;
}

/*
 * Read one /proc/<pid>/psinfo. Returns 0 on success, -1 on failure.
 */
static int
read_one_psinfo(pid_t pid, struct top_proc *proc, struct process_select *sel)
{
    char path[64];
    psinfo_t ps;
    int fd;
    ssize_t got;

    snprintf(path, sizeof(path), PROCFS "/%ld/psinfo", (long)pid);
    if ((fd = open(path, O_RDONLY)) < 0)
        return -1;
    /* Retry on EINTR — top arms a SIGALRM for its refresh cadence and
     * the signal can land between open() and read(). */
    do {
        got = read(fd, &ps, sizeof(ps));
    } while (got < 0 && errno == EINTR);
    close(fd);
    if (got != (ssize_t)sizeof(ps))
        return -1;

    proc->pid        = ps.pr_pid;
    proc->tgid       = ps.pr_pid;           /* Solaris: no separate tgid */
    proc->uid        = ps.pr_uid;
    proc->threads    = ps.pr_nlwp;
    proc->size       = ps.pr_size;          /* already KB */
    proc->rss        = ps.pr_rssize;        /* already KB */
    proc->shared     = 0;                   /* not tracked per-proc */
    proc->state      = map_state(ps.pr_lwp.pr_sname);
    proc->pri        = ps.pr_lwp.pr_pri;
    proc->nice       = ps.pr_lwp.pr_nice - NZERO;  /* pr_nice is biased */
    proc->processor  = ps.pr_lwp.pr_onpro;
    proc->wchan      = (unsigned long)ps.pr_lwp.pr_wchan;
    proc->start_time = ps.pr_start.tv_sec;

    /* cpu time: psinfo reports seconds+nanoseconds; convert to ticks
     * so format_time() (expecting clock_tick units) behaves. */
    proc->time = (unsigned long)(ps.pr_time.tv_sec * clock_tick
                                 + (ps.pr_time.tv_nsec / (1000000000L / clock_tick)));

    /* pcpu: psinfo already gives us an EMA-ish fraction. */
    proc->pcpu = PCT_TO_DOUBLE(ps.pr_lwp.pr_pctcpu);

    /* Name: prefer pr_psargs (full cmdline) when fullcmd requested and
     * the field is populated; fall back to pr_fname (16-char basename). */
    if (sel->fullcmd && ps.pr_psargs[0] != '\0')
    {
        char buf[PRARGSZ + 1];
        memcpy(buf, ps.pr_psargs, PRARGSZ);
        buf[PRARGSZ] = '\0';
        xfrm_cmdline(buf, (int)strlen(buf));
        update_procname(proc, buf);
    }
    else
    {
        char buf[PRFNSZ + 1];
        memcpy(buf, ps.pr_fname, PRFNSZ);
        buf[PRFNSZ] = '\0';
        update_procname(proc, buf);
    }

    return 0;
}

static int show_usernames;

caddr_t
get_process_info(struct system_info *si,
                 struct process_select *sel,
                 int compare_index)
{
    DIR *dir;
    struct dirent *ent;
    int total_procs = 0;
    struct top_proc **active;
    struct top_proc *proc;
    hash_item_pid *hi;
    hash_pos pos;
    int show_idle    = sel->idle;
    int show_uid     = sel->uid != -1;
    char *show_cmd   = sel->command;

    show_usernames = sel->usernames;

    /* mark every hash entry not-seen */
    hi = hash_first_pid(ptable, &pos);
    while (hi != NULL)
    {
        ((struct top_proc *)(hi->value))->state = 0;
        hi = hash_next_pid(&pos);
    }

    memset(process_states, 0, sizeof(process_states));

    dir = opendir(PROCFS);
    if (dir == NULL)
    {
        si->p_active   = 0;
        si->p_total    = 0;
        si->procstates = process_states;
        nextactive     = pactive;
        return (caddr_t)0;
    }

    while ((ent = readdir(dir)) != NULL)
    {
        pid_t pid;

        if (!isdigit(ent->d_name[0]))
            continue;
        pid = (pid_t)atoi(ent->d_name);

        proc = (struct top_proc *)hash_lookup_pid(ptable, pid);
        if (proc == NULL)
        {
            proc = new_proc();
            proc->pid = pid;
            proc->time = 0;
            hash_add_pid(ptable, pid, (void *)proc);
        }

        /* psinfo's pr_pctcpu is already a recency-weighted fraction,
         * so unlike m_linux.c we don't need to diff cpu-ticks against
         * the previous sample here. */
        if (read_one_psinfo(pid, proc, sel) < 0)
        {
            proc->state = 0;    /* will be reaped below */
            continue;
        }
        proc->next = NULL;

        total_procs++;
        if (proc->state >= 1 && proc->state <= NPROCSTATES - 1)
            process_states[proc->state]++;
    }
    closedir(dir);

    if (activesize < total_procs)
    {
        pactive = (struct top_proc **)realloc(pactive,
                      sizeof(struct top_proc *) * total_procs);
        activesize = total_procs;
    }

    active = pactive;
    hi = hash_first_pid(ptable, &pos);
    while (hi != NULL)
    {
        proc = (struct top_proc *)(hi->value);
        if (proc->state == 0)
        {
            hash_remove_pos_pid(&pos);
            free_proc(proc);
        }
        else
        {
            if ((show_idle || proc->state == 1 || proc->pcpu) &&
                (!show_uid || proc->uid == (uid_t)sel->uid) &&
                (show_cmd == NULL || (proc->name && strstr(proc->name, show_cmd) != NULL)))
            {
                *active++ = proc;
            }
        }
        hi = hash_next_pid(&pos);
    }

    si->p_active   = active - pactive;
    si->p_total    = total_procs;
    si->procstates = process_states;

    if (si->p_active)
        qsort(pactive, si->p_active, sizeof(struct top_proc *),
              proc_compares[compare_index]);

    nextactive = pactive;
    return (caddr_t)0;
}

char *
format_header(char *uname_field)
{
    int uname_len = strlen(uname_field);
    if (uname_len > 8)
        uname_len = 8;
    memcpy(strchr(fmt_header, 'X'), uname_field, uname_len);
    return fmt_header;
}

static char p_header[MAX_COLS];

char *
format_process_header(struct process_select *sel, caddr_t handle, int count)
{
    char *h = sel->threads ? proc_header_nothr : proc_header_thr;
    snprintf(p_header, MAX_COLS, h, sel->usernames ? "USERNAME" : "UID");
    return p_header;
}

char *
format_next_process(caddr_t handle, char *(*get_userid)(int))
{
    static char fmt[MAX_COLS];
    struct top_proc *p = *nextactive++;
    char *userbuf = show_usernames ? username(p->uid) : itoa_w(p->uid, 7);

    snprintf(fmt, sizeof(fmt),
             "%7d %-8.8s %4d %3d %3d %5s %5s %5s %s %2d %6s %5s %s",
             p->pid,
             userbuf,
             p->threads <= 9999 ? p->threads : 9999,
             p->pri < -99 ? -99 : p->pri,
             p->nice,
             format_k(p->size),
             format_k(p->rss),
             format_k(p->shared),
             state_abbrev[p->state],
             p->processor,
             format_time(p->time),
             format_percent(p->pcpu * 100.0),
             p->name ? p->name : "");
    return fmt;
}

/*=SORT COMPARATORS======================================================
 * Identical shape to m_linux.c so that top.c's order menu works. */

#define ORDERKEY_PCTCPU  if (dresult = p2->pcpu - p1->pcpu, \
                             (result = dresult > 0.0 ? 1 : dresult < 0.0 ? -1 : 0) == 0)
#define ORDERKEY_CPTICKS if ((result = (long)p2->time - (long)p1->time) == 0)
#define ORDERKEY_STATE   if ((result = (sort_state[p2->state] - sort_state[p1->state])) == 0)
#define ORDERKEY_PRIO    if ((result = p2->pri - p1->pri) == 0)
#define ORDERKEY_RSSIZE  if ((result = p2->rss - p1->rss) == 0)
#define ORDERKEY_MEM     if ((result = p2->size - p1->size) == 0)
#define ORDERKEY_NAME    if ((result = strcmp(p1->name ? p1->name : "", \
                                              p2->name ? p2->name : "")) == 0)
#define ORDERKEY_THREAD  if ((result = p2->threads - p1->threads) == 0)

unsigned char sort_state[] = {
    0, 6, 3, 5, 1, 2, 4
};

int
compare_cpu(struct top_proc **pp1, struct top_proc **pp2)
{
    struct top_proc *p1 = *pp1, *p2 = *pp2;
    long result; double dresult;
    ORDERKEY_PCTCPU ORDERKEY_CPTICKS ORDERKEY_STATE
    ORDERKEY_PRIO ORDERKEY_RSSIZE ORDERKEY_MEM ;
    return result == 0 ? 0 : result < 0 ? -1 : 1;
}

int
compare_size(struct top_proc **pp1, struct top_proc **pp2)
{
    struct top_proc *p1 = *pp1, *p2 = *pp2;
    long result; double dresult;
    ORDERKEY_MEM ORDERKEY_RSSIZE ORDERKEY_PCTCPU
    ORDERKEY_CPTICKS ORDERKEY_STATE ORDERKEY_PRIO ;
    return result == 0 ? 0 : result < 0 ? -1 : 1;
}

int
compare_res(struct top_proc **pp1, struct top_proc **pp2)
{
    struct top_proc *p1 = *pp1, *p2 = *pp2;
    long result; double dresult;
    ORDERKEY_RSSIZE ORDERKEY_MEM ORDERKEY_PCTCPU
    ORDERKEY_CPTICKS ORDERKEY_STATE ORDERKEY_PRIO ;
    return result == 0 ? 0 : result < 0 ? -1 : 1;
}

int
compare_time(struct top_proc **pp1, struct top_proc **pp2)
{
    struct top_proc *p1 = *pp1, *p2 = *pp2;
    long result; double dresult;
    ORDERKEY_CPTICKS ORDERKEY_PCTCPU ORDERKEY_STATE
    ORDERKEY_PRIO ORDERKEY_MEM ORDERKEY_RSSIZE ;
    return result == 0 ? 0 : result < 0 ? -1 : 1;
}

int
compare_cmd(struct top_proc **pp1, struct top_proc **pp2)
{
    struct top_proc *p1 = *pp1, *p2 = *pp2;
    long result; double dresult;
    ORDERKEY_NAME ORDERKEY_PCTCPU ORDERKEY_CPTICKS
    ORDERKEY_STATE ORDERKEY_PRIO ORDERKEY_RSSIZE ORDERKEY_MEM ;
    return result == 0 ? 0 : result < 0 ? -1 : 1;
}

int
compare_threads(struct top_proc **pp1, struct top_proc **pp2)
{
    struct top_proc *p1 = *pp1, *p2 = *pp2;
    long result; double dresult;
    ORDERKEY_THREAD ORDERKEY_PCTCPU ORDERKEY_CPTICKS
    ORDERKEY_STATE ORDERKEY_PRIO ORDERKEY_RSSIZE ORDERKEY_MEM ;
    return result == 0 ? 0 : result < 0 ? -1 : 1;
}

/*
 * proc_owner(pid) - uid owning process pid, or -1 if it's gone.
 * Security-critical: validates kill/renice requests.
 */
int
proc_owner(int pid)
{
    char path[64];
    psinfo_t ps;
    int fd;
    ssize_t got;

    snprintf(path, sizeof(path), PROCFS "/%d/psinfo", pid);
    if ((fd = open(path, O_RDONLY)) < 0)
        return -1;
    do {
        got = read(fd, &ps, sizeof(ps));
    } while (got < 0 && errno == EINTR);
    close(fd);
    if (got != (ssize_t)sizeof(ps))
        return -1;
    return (int)ps.pr_uid;
}
