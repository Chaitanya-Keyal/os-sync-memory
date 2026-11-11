#!/bin/bash
# CS F372 Assignment 3, Problem 3: make exactly one 4 GiB swap backing active.
#
#   sudo ./swap_setup.sh internal          4 GiB swap file /swapfile-a3 on the
#                                          root virtual disk (host internal SSD)
#   sudo ./swap_setup.sh usb /dev/sdX      4 GiB swap area on a USB disk passed
#                                          through to the VM. ERASES its start.
#   sudo ./swap_setup.sh status            show swap, zswap and zram state
#   sudo ./swap_setup.sh off               turn all swap off
#
# Every mode first disables zswap, turns off any zram device and runs
# swapoff -a, so only the backing under test is ever active.

SIZE_MIB=4096
SWAPFILE=/swapfile-a3

if [ "$(id -u)" -ne 0 ]; then
    echo "run as root: sudo $0 $*" >&2
    exit 1
fi

status() {
    echo "zswap enabled: $(cat /sys/module/zswap/parameters/enabled 2>/dev/null || echo 'not built')"
    echo "zram devices:  $(ls /dev/zram* 2>/dev/null | tr '\n' ' ' || true)"
    echo "active swap:"
    swapon --show=NAME,TYPE,SIZE,USED,PRIO || true
    free -m
}

all_off() {
    [ -w /sys/module/zswap/parameters/enabled ] &&
        echo N >/sys/module/zswap/parameters/enabled
    for z in /dev/zram*; do
        [ -b "$z" ] && swapoff "$z" 2>/dev/null
    done
    swapoff -a
}

case "$1" in
internal)
    all_off
    if [ ! -f "$SWAPFILE" ] || [ "$(stat -c %s "$SWAPFILE")" -ne $((SIZE_MIB * 1024 * 1024)) ]; then
        rm -f "$SWAPFILE"
        # dd, not fallocate: every block is really allocated on the disk.
        dd if=/dev/zero of="$SWAPFILE" bs=1M count=$SIZE_MIB status=progress
        chmod 600 "$SWAPFILE"
    fi
    mkswap -L a3internal "$SWAPFILE" >/dev/null
    swapon "$SWAPFILE"
    ;;
usb)
    dev=$2
    if [ ! -b "$dev" ]; then
        echo "usage: $0 usb /dev/sdX   (a whole USB disk passed through to the VM)" >&2
        lsblk -o NAME,SIZE,TRAN,MODEL,MOUNTPOINT
        exit 1
    fi
    root=$(lsblk -no PKNAME "$(findmnt -no SOURCE /)")
    if [ "$(basename "$dev")" = "$root" ] || lsblk -no MOUNTPOINT "$dev" | grep -q .; then
        echo "$dev is the root disk or is mounted; refusing" >&2
        exit 1
    fi
    lsblk -o NAME,SIZE,TRAN,MODEL "$dev"
    read -r -p "Erase the first $SIZE_MIB MiB of $dev and use it as swap? Type YES: " ok
    [ "$ok" = YES ] || exit 1
    all_off
    # mkswap's optional block count (KiB) limits the area to 4 GiB.
    mkswap -L a3usb "$dev" $((SIZE_MIB * 1024)) >/dev/null
    swapon "$dev"
    ;;
off)
    all_off
    ;;
status) ;;
*)
    sed -n '2,12p' "$0"
    exit 1
    ;;
esac
status
