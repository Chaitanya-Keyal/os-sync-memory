# Documentation

These notes explain the three problems in plain language: what is being
asked, which operating-system ideas are involved, how the code solves it,
and what to look at during the demo. Read `concepts.md` first if monitors,
priority inversion or swapping are new to you.

| File | What it covers |
|------|----------------|
| `concepts.md` | The building blocks: mutexes, condition variables, signal vs broadcast, monitors, readers-writers and aging, SCHED_FIFO, priority inversion, inheritance and ceiling, Coffman conditions, wait-for graphs, livelock, paging, faults, swap |
| `01-telemetry-vault.md` | Problem 1: the monitor, the admission rule, signal vs broadcast, shutdown |
| `02-priority-deadlock.md` | Problem 2: the inversion chain with three protocols, the releaser thread, deadlock detection and the timeout fix |
| `03-memory-swap.md` | Problem 3: the program, the scripts, the findings, and how the USB swap was set up for Part B |
| `q1-evidence/` | Problem 1: test builds that force timeouts and break mutual exclusion |
| `demo.md` | Step-by-step demo script for all three problems |
| `viva-prep.md` | Reading order, 60-second summaries and likely questions |
| `modifications.md` | Where to edit for the changes likely to be asked in the viva |
