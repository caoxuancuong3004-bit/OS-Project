#!/bin/sh
# Tests against hand-computed textbook numbers. Extend this file as you
# implement policies (e.g. SJF/SRTF/PRIO/MLFQ on arrivals.txt, your own
# paging and disk workloads).
#
# A check whose command stops with "not implemented yet" prints SKIP: the
# expected value is already here, waiting for your implementation. Every SKIP
# must have become PASS by the deadline of its checkpoint.
BIN=./ossim
fail=0; pass=0; skip=0

check() {   # check <description> <command> <expected-word>
    out=$($2 2>&1) || true
    if echo "$out" | grep -q "not implemented yet"; then
        echo "SKIP  $1"; skip=$((skip + 1))
    elif echo "$out" | grep -qw -- "$3"; then
        echo "PASS  $1"; pass=$((pass + 1))
    else
        echo "FAIL  $1"; echo "      expected: $3"; echo "$out" | sed 's/^/      /'; fail=1
    fi
}

# --- reference policies (must pass from day one) --------------------------
check "FCFS textbook example: avg waiting 17.00" \
      "$BIN workloads/basic.txt --sched fcfs --no-paging" "avg_waiting=17.00"

check "RR q=4 textbook example: avg waiting 5.67" \
      "$BIN workloads/basic.txt --sched rr --quantum 4 --no-paging" "avg_waiting=5.67"

check "RR q=4: context switches = 4 (P1,P2,P3,P1)" \
      "$BIN workloads/basic.txt --sched rr --quantum 4 --no-paging" "context_switches=4"

check "Paging: P1 alone, FIFO, 3 frames -> 15 faults (textbook Sec. 10.4.2)" \
      "$BIN workloads/paging_p1.txt --frames 3 --replace fifo" "page_faults=15"

check "Disk: FCFS, head 53 -> 640 cylinders (textbook Sec. 11.2.1)" \
      "$BIN workloads/disk_textbook.txt --no-paging --head 53 --disk fcfs" "head_movement=640"

check "Producer/consumer completes" \
      "$BIN workloads/producer_consumer.txt --sched rr --quantum 2 --no-paging" "finished=2/2"

check "Deadlock workload without detection: A and B stall, C finishes" \
      "$BIN workloads/deadlock.txt --sched rr --quantum 2 --max-ticks 60 --no-paging" "finished=1/3"

check "Mixed workload finishes" \
      "$BIN workloads/mixed.txt --sched rr --quantum 3 --frames 6" "finished=4/4"

check "Mixed workload finishes with 1 frame (faults wait for a frame)" \
      "$BIN workloads/mixed.txt --sched rr --quantum 3 --frames 1" "finished=4/4"

# --- checkpoint 1 ----------------------------------------------------------
check "Deadlock detection reports A and B (pids 1 2)" \
      "$BIN workloads/deadlock.txt --sched rr --quantum 2 --deadlock-every 5 --max-ticks 60 --no-paging" "pids: 1 2"

for s in sjf srtf prio mlfq; do
    check "Producer/consumer completes under $s" \
          "$BIN workloads/producer_consumer.txt --sched $s --quantum 2 --no-paging" "finished=2/2"
done

# Hand-computed on workloads/arrivals.txt, quantum 2, no paging. The Gantt
# charts and the per-process tables are in CHECKPOINT1.md.

# SJF : P1 0-6 | P4 7-8 | P3 9-11 | P1 12-15 | P5 16-20 | P3 21-23 | P6 24-29 | P2 30-38
#       waiting 2,28,9,1,8,12 = 60/6   response 0,28,5,1,8,12 = 54/6
SJF="$BIN workloads/arrivals.txt --sched sjf --quantum 2 --no-paging"
check "SJF on arrivals.txt: avg waiting 10.00"  "$SJF" "avg_waiting=10.00"
check "SJF on arrivals.txt: avg response 9.00"  "$SJF" "avg_response=9.00"
check "SJF on arrivals.txt: 8 context switches" "$SJF" "context_switches=8"

# SRTF: as SJF until t=16, then J3 back from io (3 left) preempts J5 (4 left) at t=17
#       P5 16 | P3 17-19 | P5 20-23 | P6 24-29 | P2 30-38
#       waiting 2,28,5,1,11,12 = 59/6  response 0,28,5,1,8,12 = 54/6
SRTF="$BIN workloads/arrivals.txt --sched srtf --quantum 2 --no-paging"
check "SRTF on arrivals.txt: avg waiting 9.83"   "$SRTF" "avg_waiting=9.83"
check "SRTF on arrivals.txt: avg response 9.00"  "$SRTF" "avg_response=9.00"
check "SRTF on arrivals.txt: 9 context switches" "$SRTF" "context_switches=9"

# PRIO (1 = highest, RR q=2 between equals):
#       P1 0-1 | P2 2-10 | P1 11 | P6 12-17 | P4 18-19 | P1 20-23 | P3 24-25 | P5 26
#       | P1 27-30 | P3 31 | P5 32-35 | idle 36 | P3 37-39
#       waiting 17,0,25,12,23,0 = 77/6   response 0,0,20,12,18,0 = 50/6
PRIO="$BIN workloads/arrivals.txt --sched prio --quantum 2 --no-paging"
check "PRIO on arrivals.txt: avg waiting 12.83"   "$PRIO" "avg_waiting=12.83"
check "PRIO on arrivals.txt: avg response 8.33"   "$PRIO" "avg_response=8.33"
check "PRIO on arrivals.txt: 12 context switches" "$PRIO" "context_switches=12"

# Deadlock: under RR q=2, A and B are deadlocked from t=12 (B blocks on R1 at
# the end of tick 11, A blocks on R2 during tick 12), so a check every tick
# reports it at t=12. FCFS never preempts, A gets R1 and R2 before B runs,
# so there is no deadlock and detection must not report one.
check "Deadlock detection every tick reports it at t=12" \
      "$BIN workloads/deadlock.txt --sched rr --quantum 2 --deadlock-every 1 --max-ticks 60 --no-paging" "DEADLOCK t=12"
check "Deadlock workload under FCFS: no deadlock, all finish" \
      "$BIN workloads/deadlock.txt --sched fcfs --deadlock-every 1 --max-ticks 60 --no-paging" "finished=3/3"

# --- checkpoint 2 ----------------------------------------------------------
check "Paging: LRU, 3 frames -> 12 faults (textbook Sec. 10.4.4)" \
      "$BIN workloads/paging_p1.txt --frames 3 --replace lru" "page_faults=12"
# TODO: a Clock case you computed by hand (the textbook has none, make one).

check "Disk: SSTF, head 53 -> 236" \
      "$BIN workloads/disk_textbook.txt --no-paging --head 53 --disk sstf" "head_movement=236"
check "Disk: SCAN moving down, head 53 -> 236" \
      "$BIN workloads/disk_textbook.txt --no-paging --head 53 --direction down --disk scan" "head_movement=236"
check "Disk: C-LOOK moving up, head 53 -> 322" \
      "$BIN workloads/disk_textbook.txt --no-paging --head 53 --direction up --disk clook" "head_movement=322"

echo "---"
echo "$pass passed, $skip skipped (not implemented yet), $([ $fail = 0 ] && echo 0 || echo some) failed"
exit $fail
