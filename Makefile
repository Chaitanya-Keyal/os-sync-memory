# CS F372 - Assignment 3
# Builds every program. Problem 2 Part A needs root to run (SCHED_FIFO).

CC      = gcc
CFLAGS  = -Wall -Wextra -O2 -pthread

PROGRAMS = q1_telemetry_vault/vault \
           q2_prio_deadlock/prio_chain \
           q2_prio_deadlock/deadlock \
           q3_memswap/memswap

all: $(PROGRAMS)

%: %.c
	$(CC) $(CFLAGS) -o $@ $<

clean:
	rm -f $(PROGRAMS)

# Bundle sources, executables, readme and results for submission.
dist: all
	tar czf assignment3_submission.tar.gz README.md Makefile \
	    q1_telemetry_vault q2_prio_deadlock q3_memswap

.PHONY: all clean dist
