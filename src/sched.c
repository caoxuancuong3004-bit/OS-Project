/*
 * sched.c: CPU schedulers.
 *
 * FCFS and RR are given. TODO checkpoint 1: sjf, srtf, prio, mlfq.
 * Each one is a scheduler_t with the same hooks as FCFS and RR. There are
 * notes above each stub. Functions you will probably need:
 *   proc_burst_remaining(p)   ticks left in p's current cpu burst
 *   proc_total_remaining(p)   cpu ticks left in the whole process
 *   q_pop_min(&q, key)        remove the element with the smallest key
 *   q_min_key(&q, key)        the smallest key, without removing (for preemption)
 */
#include <string.h>
#include "sched.h"
#include "queue.h"
#include "kernel.h"   /* running, for the MLFQ boost */

scheduler_t *sched = NULL;

/* FCFS */
static queue_t fcfs_q;

static void   fcfs_init(void)                { q_init(&fcfs_q); }
static void   fcfs_enqueue(pcb_t *p)         { q_push(&fcfs_q, p); }
static bool   fcfs_should_preempt(pcb_t *r)  { (void)r; return false; }
static pcb_t *fcfs_pick(void)                { return q_pop(&fcfs_q); }
static void   fcfs_on_dispatch(pcb_t *p)     { (void)p; }
static void   fcfs_on_tick(pcb_t *r)         { (void)r; }
static int    fcfs_ready_count(void)         { return q_size(&fcfs_q); }

static scheduler_t sched_fcfs = {
    "fcfs", fcfs_init, fcfs_enqueue, fcfs_should_preempt, fcfs_pick,
    fcfs_on_dispatch, fcfs_on_tick, fcfs_ready_count
};

/* RR */
static queue_t rr_q;

static void   rr_init(void)            { q_init(&rr_q); }
static void   rr_enqueue(pcb_t *p)     { q_push(&rr_q, p); }
static pcb_t *rr_pick(void)            { return q_pop(&rr_q); }
static void   rr_on_dispatch(pcb_t *p) { p->quantum_used = 0; }
static void   rr_on_tick(pcb_t *r)     { r->quantum_used++; }
static int    rr_ready_count(void)     { return q_size(&rr_q); }

/* Preempt when the quantum is used up and someone else is ready. If nobody
 * is ready, the process keeps the CPU and starts a new quantum, so a process
 * that arrives later waits until that quantum is over. */
static bool rr_should_preempt(pcb_t *r)
{
    if (r->quantum_used < cfg.quantum) return false;
    r->quantum_used = 0;
    return !q_empty(&rr_q);
}

static scheduler_t sched_rr = {
    "rr", rr_init, rr_enqueue, rr_should_preempt, rr_pick,
    rr_on_dispatch, rr_on_tick, rr_ready_count
};

/* Key used by SJF and SRTF: ticks left in the current cpu burst. */
static int burst_key(const pcb_t *p) { return proc_burst_remaining(p); }

/* SJF: shortest job first, non-preemptive.
 * Data structure: one ready queue (queue_t) in arrival order. pick() scans it
 * and removes the process with the shortest next cpu burst; on a tie the one
 * that became READY first wins (q_pop_min keeps FIFO order on ties).
 * A dispatched process keeps the CPU until its burst ends or it blocks.
 * Note: ossim knows the exact next burst. A real OS would have to predict it,
 * e.g. by exponential averaging tau(n+1) = a*t(n) + (1-a)*tau(n) (Sec. 5.3.2). */
static queue_t sjf_q;

static void   sjf_init(void)                { q_init(&sjf_q); }
static void   sjf_enqueue(pcb_t *p)         { q_push(&sjf_q, p); }
static bool   sjf_should_preempt(pcb_t *r)  { (void)r; return false; }
static pcb_t *sjf_pick(void)                { return q_pop_min(&sjf_q, burst_key); }
static void   sjf_on_dispatch(pcb_t *p)     { (void)p; }
static void   sjf_on_tick(pcb_t *r)         { (void)r; }
static int    sjf_ready_count(void)         { return q_size(&sjf_q); }

static scheduler_t sched_sjf = {
    "sjf", sjf_init, sjf_enqueue, sjf_should_preempt, sjf_pick,
    sjf_on_dispatch, sjf_on_tick, sjf_ready_count
};

/* SRTF: shortest remaining time first (SJF with preemption).
 * Same queue and key as SJF. Before every tick, the running process is
 * preempted if some READY process has strictly less left of its burst
 * (a tie does not preempt, so we avoid useless context switches). */
static queue_t srtf_q;

static void   srtf_init(void)               { q_init(&srtf_q); }
static void   srtf_enqueue(pcb_t *p)        { q_push(&srtf_q, p); }
static pcb_t *srtf_pick(void)               { return q_pop_min(&srtf_q, burst_key); }
static void   srtf_on_dispatch(pcb_t *p)    { (void)p; }
static void   srtf_on_tick(pcb_t *r)        { (void)r; }
static int    srtf_ready_count(void)        { return q_size(&srtf_q); }

static bool srtf_should_preempt(pcb_t *r)
{
    return q_min_key(&srtf_q, burst_key) < burst_key(r);
}

static scheduler_t sched_srtf = {
    "srtf", srtf_init, srtf_enqueue, srtf_should_preempt, srtf_pick,
    srtf_on_dispatch, srtf_on_tick, srtf_ready_count
};

/* PRIO: preemptive priority (smaller number = higher priority), RR with
 * quantum cfg.quantum between processes of the same priority.
 * Data structure: one ready queue (queue_t) in the order processes became
 * READY. pick() removes the process with the smallest priority number; on a
 * tie the one nearer the head, i.e. the one that has waited longest, wins.
 * A preempted process goes to the tail, so equal priorities take turns.
 * The running process is preempted before a tick if
 *   - a READY process has a strictly higher priority, or
 *   - its quantum is used up and a READY process has the same priority.
 * If the quantum is used up and nobody of equal or higher priority is ready,
 * it keeps the CPU and starts a new quantum (the same rule as RR).
 * Low priorities can starve while higher ones keep arriving; there is no
 * aging, so the numbers match the hand-computed ones in CHECKPOINT1.md. */
static queue_t prio_q;

static int prio_key(const pcb_t *p) { return p->priority; }

static void   prio_init(void)            { q_init(&prio_q); }
static void   prio_enqueue(pcb_t *p)     { q_push(&prio_q, p); }
static pcb_t *prio_pick(void)            { return q_pop_min(&prio_q, prio_key); }
static void   prio_on_dispatch(pcb_t *p) { p->quantum_used = 0; }
static void   prio_on_tick(pcb_t *r)     { r->quantum_used++; }
static int    prio_ready_count(void)     { return q_size(&prio_q); }

static bool prio_should_preempt(pcb_t *r)
{
    int best = q_min_key(&prio_q, prio_key);   /* INT_MAX if nobody is ready */
    if (best < r->priority) return true;        /* higher priority is ready */
    if (r->quantum_used < cfg.quantum) return false;
    r->quantum_used = 0;                        /* new quantum if it keeps the CPU */
    return best == r->priority;                 /* RR among equals */
}

static scheduler_t sched_prio = {
    "prio", prio_init, prio_enqueue, prio_should_preempt, prio_pick,
    prio_on_dispatch, prio_on_tick, prio_ready_count
};

/* MLFQ: multilevel feedback queue, 3 levels, RR in each level with quantum
 * q, 2q, 4q (q = cfg.quantum). Level 0 is the highest priority.
 * Data structure: one FIFO queue per level, and p->level in the PCB.
 * Rules:
 *   - A new process starts at level 0. pick() takes the head of the highest
 *     non-empty level.
 *   - A process that uses its whole quantum moves down one level (level 2
 *     stays at level 2). It is preempted if someone at its new level or
 *     higher is ready; otherwise it keeps the CPU with the new quantum.
 *   - A process that blocks (io, disk, semaphore...) before its quantum ends
 *     keeps its level. So does one preempted by a higher level.
 *   - A READY process at a higher level preempts the running one.
 *   - Every 20 ticks (t = 20, 40, ...) every process goes back to level 0
 *     (priority boost), so nobody starves. The running process also gets a
 *     fresh level-0 quantum.
 * The quantum restarts at every dispatch (as in RR). */
#define MLFQ_LEVELS 3
#define MLFQ_BOOST  20

static queue_t mlfq_q[MLFQ_LEVELS];
static long    mlfq_period;                    /* now / MLFQ_BOOST of the last boost */

static int mlfq_quantum(int level) { return cfg.quantum << level; }   /* q, 2q, 4q */

/* The highest level (smallest number) with a READY process, MLFQ_LEVELS if none. */
static int mlfq_best_level(void)
{
    for (int l = 0; l < MLFQ_LEVELS; l++)
        if (!q_empty(&mlfq_q[l])) return l;
    return MLFQ_LEVELS;
}

/* Priority boost. Called from should_preempt() and pick(): in every tick the
 * kernel calls at least one of them, so no boost is missed. */
static void mlfq_boost_if_due(void)
{
    long period = now / MLFQ_BOOST;
    if (period <= mlfq_period) return;
    mlfq_period = period;
    for (int l = 1; l < MLFQ_LEVELS; l++)      /* ready queues: keep FIFO order */
        while (!q_empty(&mlfq_q[l])) q_push(&mlfq_q[0], q_pop(&mlfq_q[l]));
    for (int i = 0; i < nprocs; i++) procs[i].level = 0;   /* running and blocked too */
    if (running) running->quantum_used = 0;
    trace("MLFQ boost: every process back to level 0");
}

static void mlfq_init(void)
{
    for (int l = 0; l < MLFQ_LEVELS; l++) q_init(&mlfq_q[l]);
    for (int i = 0; i < nprocs; i++) procs[i].level = 0;
    mlfq_period = 0;
}

static void mlfq_enqueue(pcb_t *p)          { q_push(&mlfq_q[p->level], p); }
static void mlfq_on_dispatch(pcb_t *p)      { p->quantum_used = 0; }
static void mlfq_on_tick(pcb_t *r)          { r->quantum_used++; }

static pcb_t *mlfq_pick(void)
{
    mlfq_boost_if_due();
    int l = mlfq_best_level();
    return l < MLFQ_LEVELS ? q_pop(&mlfq_q[l]) : NULL;
}

static bool mlfq_should_preempt(pcb_t *r)
{
    mlfq_boost_if_due();
    if (r->quantum_used >= mlfq_quantum(r->level)) {
        if (r->level < MLFQ_LEVELS - 1) r->level++;   /* used it all: demote */
        r->quantum_used = 0;
        trace("MLFQ: P%d used its quantum, now level %d", r->pid, r->level);
        return mlfq_best_level() <= r->level;
    }
    return mlfq_best_level() < r->level;               /* higher level is ready */
}

static int mlfq_ready_count(void)
{
    int n = 0;
    for (int l = 0; l < MLFQ_LEVELS; l++) n += q_size(&mlfq_q[l]);
    return n;
}

static scheduler_t sched_mlfq = {
    "mlfq", mlfq_init, mlfq_enqueue, mlfq_should_preempt, mlfq_pick,
    mlfq_on_dispatch, mlfq_on_tick, mlfq_ready_count
};

static scheduler_t *all[] = {
    &sched_fcfs, &sched_rr, &sched_sjf, &sched_srtf, &sched_prio, &sched_mlfq
};

void sched_select(const char *name)
{
    for (size_t i = 0; i < sizeof(all) / sizeof(all[0]); i++) {
        if (strcmp(all[i]->name, name) == 0) {
            sched = all[i];
            sched->init();
            return;
        }
    }
    die("unknown scheduler '%s' (fcfs, rr, sjf, srtf, prio, mlfq)", name);
}
