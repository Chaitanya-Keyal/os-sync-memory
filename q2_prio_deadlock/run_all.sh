#!/bin/bash
# CS F372 Assignment 3, Problem 2: runs Part A (5 runs per mutex protocol),
# the Part B hang demo and the Part B fixed version (20 runs), and saves the
# combined output to run_output.txt.
#
# Usage: sudo ./run_all.sh        (after `make` in the repository root)

cd "$(dirname "$0")" || exit 1
if [ "$(id -u)" -ne 0 ]; then
    echo "run as root: sudo $0" >&2
    exit 1
fi

{
    echo "# $(date -Is) kernel $(uname -r), $(nproc) CPUs"
    echo
    ./prio_chain 5
    echo
    ./deadlock hang
    echo "(exit code $?)"
    echo
    ./deadlock fix 20
} 2>&1 | tee run_output.txt
chown --reference=. run_output.txt
