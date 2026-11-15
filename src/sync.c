/*
 * sync.c: semaphores, resources and deadlock detection.
 */
#include <string.h>
#include "sync.h"
#include "kernel.h"

ksem_t     sems[MAX_SEMS];
int        nsems = 0;
resource_t resources[MAX_RES];
int        nres = 0;

ksem_t *sem_find(const char *name)
{
    for (int i = 0; i < nsems; i++) if (strcmp(sems[i].name, name) == 0) return &sems[i];
    return NULL;
}

ksem_t *sem_create(const char *name, int init)
{
    if (sem_find(name)) die("semaphore '%s' defined twice", name);
    if (nsems >= MAX_SEMS) die("too many semaphores");
    ksem_t *s = &sems[nsems++];
    strncpy(s->name, name, MAX_NAME - 1);
    s->value = init;
    q_init(&s->waiters);
    return s;
}

resource_t *res_find(const char *name)
{
    for (int i = 0; i < nres; i++) if (strcmp(resources[i].name, name) == 0) return &resources[i];
    return NULL;
}

resource_t *res_create(const char *name, int total)
{
    if (res_find(name)) die("resource '%s' defined twice", name);
    if (nres >= MAX_RES) die("too many resources");
    resource_t *r = &resources[nres++];
    memset(r, 0, sizeof(*r));
    strncpy(r->name, name, MAX_NAME - 1);
    r->total = r->available = total;
    q_init(&r->waiters);
    return r;
}

/* wait(S): if S > 0, take one and go on. Otherwise block in S's list. */
bool sync_wait(pcb_t *p, ksem_t *s)
{
    if (s->value > 0) {
        s->value--;
        trace("P%d wait(%s) -> %d", p->pid, s->name, s->value);
        return true;
    }
    trace("P%d wait(%s) blocks", p->pid, s->name);
    q_push(&s->waiters, p);
    proc_set_state(p, P_WAITING, W_SEM);
    return false;
}

/* signal(S): wake the first waiter, or S++ if nobody waits. */
void sync_signal(pcb_t *p, ksem_t *s)
{
    pcb_t *w = q_pop(&s->waiters);
    if (w) {
        trace("P%d signal(%s) wakes P%d", p->pid, s->name, w->pid);
        kernel_advance(w);                /* w goes past its `wait` op */
    } else {
        s->value++;
        trace("P%d signal(%s) -> %d", p->pid, s->name, s->value);
    }
}

bool res_acquire(pcb_t *p, resource_t *r, int n)
{
    if (n > r->total) die("P%d requests %d of '%s' but only %d exist", p->pid, n, r->name, r->total);
    if (r->available >= n && q_empty(&r->waiters)) {   /* no overtaking the waiters */
        r->available -= n;
        r->allocated[p->pid] += n;
        trace("P%d acquire(%s,%d) -> available %d", p->pid, r->name, n, r->available);
        return true;
    }
    r->requested[p->pid] = n;
    q_push(&r->waiters, p);
    proc_set_state(p, P_WAITING, W_RES);
    trace("P%d acquire(%s,%d) blocks (available %d)", p->pid, r->name, n, r->available);
    return false;
}

void res_release(pcb_t *p, resource_t *r, int n)
{
    if (r->allocated[p->pid] < n)
        die("P%d releases %d of '%s' but holds %d", p->pid, n, r->name, r->allocated[p->pid]);
    r->allocated[p->pid] -= n;
    r->available += n;
    trace("P%d release(%s,%d) -> available %d", p->pid, r->name, n, r->available);

    /* Give the instances to the waiters in FIFO order while they fit. */
    while (!q_empty(&r->waiters)) {
        pcb_t *w = q_peek(&r->waiters);
        int need = r->requested[w->pid];
        if (need > r->available) break;
        q_pop(&r->waiters);
        r->available -= need;
        r->allocated[w->pid] += need;
        r->requested[w->pid] = 0;
        trace("P%d granted %d of '%s'", w->pid, need, r->name);
        kernel_advance(w);
    }
}

/*
 * Deadlock detection: the algorithm of Sec. 8.6.2 (several instances).
 *   Work = Available
 *   Finish[i] = true if P_i is terminated or holds no instances
 *   repeat: find P_i with !Finish[i] and Request_i <= Work,
 *           then Work += Allocation_i and Finish[i] = true
 *   every P_i still !Finish[i] is deadlocked.
 * Request_i is r->requested[pid], which res_acquire() sets only while P_i is
 * blocked on r, so a process that is not blocked on a resource can always
 * finish. Semaphores are not included: a semaphore has no owner, so there is
 * no Allocation to give back.
 * We only report (no recovery), and each deadlocked process is reported
 * once: a line is printed only when the set contains a pid not reported
 * before. Output, pids from small to large:
 *     DEADLOCK t=<now> pids: 1 2
 */
void deadlock_check(void)
{
    static bool reported[MAX_PROCS + 1];
    int  work[MAX_RES];
    bool finish[MAX_PROCS + 1];

    for (int j = 0; j < nres; j++) work[j] = resources[j].available;

    for (int i = 0; i < nprocs; i++) {
        pcb_t *p = &procs[i];
        bool holds = false;
        for (int j = 0; j < nres; j++)
            if (resources[j].allocated[p->pid] > 0) holds = true;
        finish[p->pid] = p->state == P_TERMINATED || !holds;
    }

    bool progress = true;
    while (progress) {
        progress = false;
        for (int i = 0; i < nprocs; i++) {
            int pid = procs[i].pid;
            if (finish[pid]) continue;
            bool fits = true;
            for (int j = 0; j < nres; j++)
                if (resources[j].requested[pid] > work[j]) { fits = false; break; }
            if (!fits) continue;
            for (int j = 0; j < nres; j++) work[j] += resources[j].allocated[pid];
            finish[pid] = true;
            progress = true;
        }
    }

    bool any_new = false;
    for (int pid = 1; pid <= MAX_PROCS; pid++)
        if (proc_by_pid(pid) && !finish[pid] && !reported[pid]) any_new = true;
    if (!any_new) return;

    printf("DEADLOCK t=%ld pids:", now);
    for (int pid = 1; pid <= MAX_PROCS; pid++) {
        if (!proc_by_pid(pid) || finish[pid]) continue;
        printf(" %d", pid);
        reported[pid] = true;
    }
    printf("\n");
}
