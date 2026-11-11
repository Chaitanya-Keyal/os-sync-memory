#!/bin/bash
# CS F372 Assignment 3, Problem 3: run trials one after another.
#
#   sudo ./run_trials.sh <internal|usb> <size_mib>...
#
# For every size, runs the uniform and then the hot pattern, each in a fresh
# process, seed 42. Before each trial it syncs, drops the page cache and waits
# 5 s, so every trial starts from the same state. Output goes to
# results/raw/<backing>_<size>_<pattern>.txt and one row per phase is
# appended to results/trials.csv.
#
# Part A:  sudo ./swap_setup.sh internal && sudo ./run_trials.sh internal 512 1024 1536 2048 2560
# Part B:  sudo ./swap_setup.sh usb /dev/sdX && sudo ./run_trials.sh usb 512 1024 1536

SEED=42

cd "$(dirname "$0")" || exit 1
if [ "$(id -u)" -ne 0 ] || [ $# -lt 2 ]; then
    echo "usage: sudo $0 <internal|usb> <size_mib>..." >&2
    exit 1
fi
backing=$1
shift
[ -x ./memswap ] || { echo "build first: make" >&2; exit 1; }
if [ "$(cat /sys/module/zswap/parameters/enabled 2>/dev/null)" = Y ]; then
    echo "zswap is on; run swap_setup.sh first" >&2
    exit 1
fi
if [ "$(swapon --noheadings | wc -l)" -ne 1 ]; then
    echo "exactly one swap backing must be active:" >&2
    swapon --show >&2
    exit 1
fi
mkdir -p results/raw

for size in "$@"; do
    for pattern in uniform hot; do
        out=results/raw/${backing}_${size}_${pattern}.txt
        sync
        echo 3 >/proc/sys/vm/drop_caches
        sleep 5
        {
            echo "# $(date -Is) kernel $(uname -r), $(nproc) CPU, $(free -m | awk '/Mem:/ {print $2}') MiB RAM"
            echo "# swap: $(swapon --noheadings --show=NAME,SIZE | tr -s ' ')"
            ./memswap "$size" "$pattern" $SEED "$backing" results/trials.csv
        } | tee "$out"
        echo
    done
done
chown -R --reference=. results
