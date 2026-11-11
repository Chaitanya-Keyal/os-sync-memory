#!/bin/bash
# CS F372 Assignment 3, Problem 3: the full measurement run behind results.txt.
#
#   sudo ./run_all.sh /dev/sdX      (sdX = the USB disk passed through to the VM)
#
# Part A on internal swap (512-2560 MiB), then Part B on USB swap (512-1536
# MiB plus 2048 MiB), then swap off. Asks once before erasing the USB disk.
# Old results/raw and results/trials.csv are replaced.
#
# Before every size, one throwaway 1500 MiB trial touches most of the VM's
# RAM. On a busy Windows host, Hyper-V pages out guest memory the VM has not
# used for a while, and the first trial after that would be slowed by the
# host paging it back in, which has nothing to do with the swap backing.

cd "$(dirname "$0")" || exit 1
if [ "$(id -u)" -ne 0 ] || [ ! -b "$1" ]; then
    echo "usage: sudo $0 /dev/sdX" >&2
    exit 1
fi
usb=$1

warm() { ./memswap 1500 uniform 42 warmup >/dev/null; }

rm -rf results/raw results/trials.csv
./swap_setup.sh internal || exit 1
for s in 512 1024 1536 2048 2560; do
    warm
    ./run_trials.sh internal "$s"
done
./swap_setup.sh usb "$usb" || exit 1
for s in 512 1024 1536 2048; do
    warm
    ./run_trials.sh usb "$s"
done
./swap_setup.sh off
