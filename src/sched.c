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

/* MLFQ
 * TODO: multilevel feedback queue with 3 levels, RR with quantum q, 2q and
 * 4q. A process that uses its whole quantum goes down one level. A process
 * that blocks before the quantum ends stays where it is. Every 20 ticks,
 * move everyone back to level 0 so nobody starves. Use p->level. */
static void mlfq_init(void) { die("scheduler 'mlfq' is not implemented yet (src/sched.c)"); }
static scheduler_t sched_mlfq = { "mlfq", mlfq_init, NULL, NULL, NULL, NULL, NULL, NULL };

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
