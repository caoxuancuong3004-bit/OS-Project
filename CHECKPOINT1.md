# Checkpoint 1: scheduling and deadlock

Due Week 8, worth 10%. Hand in the code, the tests, and a progress report of at most 2 pages: the data
structures of the schedulers, how we checked them, and who did what.

## What is done

| File | What |
|---|---|
| `src/sched.c` | SJF, SRTF, PRIO and MLFQ |
| `src/sync.c` | `deadlock_check()`: the detection algorithm of Sec. 8.6.2 |
| `tests/run.sh` | Hand-computed checks for each scheduler on `arrivals.txt`, plus a deadlock timing check |

`make test` result: **27 passed, 4 skipped, 0 failed**. The 4 skips are Checkpoint 2 (LRU, SSTF, SCAN,
C-LOOK).

## Build and run

Use Linux, WSL or macOS. On the Windows laptop, the Cygwin terminal works: open it and run
`cd /home/ossim/ossim`.

```bash
make            # build
make test       # run all checks
```

Each new scheduler on `arrivals.txt` with a Gantt chart (these are the hand-checked cases):

```bash
./ossim workloads/arrivals.txt --sched sjf  --quantum 2 --no-paging --gantt
./ossim workloads/arrivals.txt --sched srtf --quantum 2 --no-paging --gantt
./ossim workloads/arrivals.txt --sched prio --quantum 2 --no-paging --gantt
./ossim workloads/arrivals.txt --sched mlfq --quantum 2 --no-paging --gantt
```

Deadlock detection (prints `DEADLOCK t=15 pids: 1 2`):

```bash
./ossim workloads/deadlock.txt --sched rr --quantum 2 --deadlock-every 5 --max-ticks 60 --no-paging
```

Producer/consumer under every scheduler (each line ends with `finished=2/2`):

```bash
for s in fcfs rr sjf srtf prio mlfq; do ./ossim workloads/producer_consumer.txt --sched $s --quantum 2 --no-paging | grep finished; done
```

Save every result to files for the report:

```bash
mkdir -p results
for s in fcfs rr sjf srtf prio mlfq; do ./ossim workloads/arrivals.txt --sched $s --quantum 2 --no-paging --gantt --csv results/arrivals_$s.csv > results/arrivals_$s.txt; done
```

Add `--trace` to any command to see every dispatch, preemption, MLFQ demotion and boost.

## Design (for the report)

| Scheduler | Data structure | pick() | Preempts when |
|---|---|---|---|
| SJF | One ready queue | Shortest next CPU burst; FIFO on ties | Never |
| SRTF | One ready queue | Shortest remaining burst | A ready process has **strictly** less left (a tie does not preempt) |
| PRIO | One ready queue | Smallest priority number; among equals, the one waiting longest | A higher priority is ready, **or** the quantum is used up and an equal priority is ready |
| MLFQ | 3 FIFO queues (one per level) + `p->level` | Head of the highest non-empty level | A higher level is ready, **or** the quantum (q, 2q, 4q) is used up and someone at the same or higher level is ready |

Design decisions you may be asked about:

- **PRIO, RR inside a priority:** a preempted process goes to the tail, so equal priorities take turns.
  If the quantum ends and nobody of equal or higher priority is ready, the process keeps the CPU with a new
  quantum, the same rule as the given RR.
- **MLFQ demotion:** checked in `should_preempt()` before the next tick. A process whose burst ends exactly
  when its quantum ends has *finished within* the quantum, so it keeps its level (textbook wording).
  A process that blocks or is preempted early also keeps its level. Level 2 cannot go lower.
- **MLFQ boost:** at t = 20, 40, ... every process (ready, running or blocked) goes back to level 0, and
  the running one gets a fresh level-0 quantum. It is called from both `should_preempt()` and `pick()`,
  because the kernel calls at least one of them every tick.
- **MLFQ quantum:** restarts at every dispatch, as in RR.
- **SJF and real systems:** ossim knows the exact next burst. A real OS predicts it with exponential averaging,
  τ(n+1) = α·t(n) + (1−α)·τ(n) (Sec. 5.3.2).
- **Deadlock:** Work = Available; Finish[i] = true if P_i holds nothing. Repeatedly find an unfinished P_i
  with Request_i ≤ Work, then Work += Allocation_i. Processes left unfinished are deadlocked. It works for
  multi-instance resources. It only reports (recovery is optional, and is Theme 8). Each deadlock is printed
  once. Semaphores are not included, because a semaphore has no owner.

## Hand-computed checks: `arrivals.txt`, quantum 2, no paging

| Job | pid | arrive | prio | ops |
|---|---|---|---|---|
| J1 | 1 | 0 | 2 | cpu 7, io 3, cpu 4 |
| J2 | 2 | 2 | 1 | cpu 9 |
| J3 | 3 | 4 | 3 | cpu 3, io 5, cpu 3 |
| J4 | 4 | 6 | 2 | cpu 2 |
| J5 | 5 | 8 | 3 | cpu 5 |
| J6 | 6 | 12 | 1 | cpu 6 |

Rules used: a burst starting at t ends at t+N; waiting = ticks READY; response = first dispatch − arrival;
arrivals enter the queue before a preempted process in the same tick.

**Summary (all values match the simulator):**

| Scheduler | avg waiting | avg response | avg turnaround | context switches |
|---|---|---|---|---|
| SJF  | 10.00 | 9.00 | 17.83 | 8 |
| SRTF | 9.83  | 9.00 | 17.67 | 9 |
| PRIO | 12.83 | 8.33 | 20.67 | 12 |
| MLFQ | 15.33 | 0.00 | 23.17 | 19 |

### SJF
```
| P1 0-6 | P4 7-8 | P3 9-11 | P1 12-15 | P5 16-20 | P3 21-23 | P6 24-29 | P2 30-38 |
```
- At t=7 the ready jobs are J2(9), J3(3), J4(2), so J4 runs.
- At t=9 it is J2(9), J3(3), J5(5), so J3 runs.
- At t=12, J1 is back from io with 4 left and is the shortest.

| | J1 | J2 | J3 | J4 | J5 | J6 |
|---|---|---|---|---|---|---|
| waiting | 2 | 28 | 9 | 1 | 8 | 12 |
| response | 0 | 28 | 5 | 1 | 8 | 12 |

### SRTF
```
| P1 0-6 | P4 7-8 | P3 9-11 | P1 12-15 | P5 16 | P3 17-19 | P5 20-23 | P6 24-29 | P2 30-38 |
```
- At t=4, J3 (3) arrives while J1 has 3 left. That is a tie, so there is no preemption.
- At t=17, J3 comes back from io with 3 left and preempts J5, which has 4 left.

| | J1 | J2 | J3 | J4 | J5 | J6 |
|---|---|---|---|---|---|---|
| waiting | 2 | 28 | 5 | 1 | 11 | 12 |
| response | 0 | 28 | 5 | 1 | 8 | 12 |

### PRIO (1 = highest, RR q=2 between equals)
```
| P1 0-1 | P2 2-10 | P1 11 | P6 12-17 | P4 18-19 | P1 20-23 | P3 24-25 | P5 26 | P1 27-30 | P3 31 | P5 32-35 | idle 36 | P3 37-39 |
```
- J2 (prio 1) preempts J1 at t=2, and J6 (prio 1) preempts J1 at t=12.
- At t=18, J4 and J1 both have prio 2. J4 has waited longer (J1 was just preempted to the tail), so J4 runs.
- At t=26, J3's quantum ends and J5 has the same prio, so they switch.
- At t=27, J1 comes back from io with prio 2 and preempts J5.

| | J1 | J2 | J3 | J4 | J5 | J6 |
|---|---|---|---|---|---|---|
| waiting | 17 | 0 | 25 | 12 | 23 | 0 |
| response | 0 | 0 | 20 | 12 | 18 | 0 |

### MLFQ (quanta 2 / 4 / 8, boost every 20 ticks)
```
| P1 0-1 | P2 2-3 | P3 4-5 | P4 6-7 | P5 8-9 | P1 10-11 | P6 12-13 | P2 14-17 | P3 18 | P5 19-21 |
| P1 22-23 | P6 24-25 | P2 26-27 | P3 28-29 | P1 30 | P6 31-32 | P2 33 | P3 34 | P1 35-38 |
```
- Each arrival at level 0 runs immediately, so every response time is 0. The job before it used its
  2-tick quantum and drops to level 1.
- At t=10, J1 runs from level 1. At t=12, J6 (level 0) preempts it, and J1 stays at level 1.
- At t=14–17, J2 uses its full 4 ticks at level 1 and drops to level 2.
- At t=18, J3 finishes its burst after 1 tick and starts io. It blocked early, so it stays at level 1.
- At t=20, the **boost** moves everyone to level 0. J5 keeps running with a fresh quantum and finishes at 22.

| | J1 | J2 | J3 | J4 | J5 | J6 |
|---|---|---|---|---|---|---|
| waiting | 25 | 23 | 20 | 0 | 9 | 15 |
| response | 0 | 0 | 0 | 0 | 0 | 0 |

### Deadlock: `deadlock.txt`, RR q=2
- A takes R1 at t=2. B takes R2 at t=5.
- B finishes its cpu 3 at the end of tick 11 and blocks on R1, which A holds.
- A finishes its cpu 3 during tick 12 and blocks on R2, which B holds. This is a cycle, so they are
  deadlocked from t=12.
- With `--deadlock-every 1` it is reported at t=12. With `--deadlock-every 5` the next check is at t=15.
  C still finishes at t=15.
- Under FCFS and SJF there is **no** deadlock: they don't preempt, so A gets both R1 and R2 before B runs.
  This is a good point for the report.

## Work split (3 members)

| | Member 1 (Cuong) | Member 2 (Nhan) | Member 3 (Phuc) |
|---|---|---|---|
| Code to own | SJF + SRTF (`src/sched.c`) | MLFQ (`src/sched.c`) | PRIO (`src/sched.c`) + `deadlock_check()` (`src/sync.c`) |
| Tests to own | SJF and SRTF checks in `tests/run.sh` | MLFQ checks | PRIO checks + both deadlock checks |
| Hand calculation to redo on paper | SJF and SRTF Gantt charts above | MLFQ Gantt chart (including the boost at t=20) | PRIO Gantt chart + the deadlock trace |
| Report (≤ 2 pages total) | SJF/SRTF section, **and puts the final report together** | MLFQ section | PRIO + deadlock section |

Everyone:
1. Read and understand your part line by line, and redo its hand calculation yourself.
2. Review one other member's part. At the final presentation **anyone can be asked about any part**.
3. Run `make test` before every push; it must stay at 0 failed.
4. Fill in the "who did what" section of the report truthfully.
