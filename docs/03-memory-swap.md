# Problem 3: Memory pressure, locality and swap

## The task

Allocate a chosen amount of memory in 1 MiB chunks, write one byte in every
page, then read single bytes from 10,000 chosen pages in each of 5 passes,
with two access patterns. Time every phase and count major page faults and
swap traffic. Then:

* **Part A:** with swap on the VM's own disk (host internal SSD), find the
  size at which memory runs out, testing 512-2560 MiB.
* **Part B:** compare that against swap on an external USB device at 512,
  1024 and 1536 MiB.

The VM must have 1 vCPU and 2 GiB RAM, 4 GiB of swap on the backing under
test, only that backing active, and zswap/zram off.

## The program, `memswap.c`

```
./memswap <size_mib> <uniform|hot> [seed=42] [backing=internal] [csv_file]
```

1. **Allocation.** `posix_memalign(&p, page_size, 1 MiB)` for each chunk,
   checking every result. For 1 MiB glibc uses `mmap`, so this only reserves
   address space. It takes milliseconds whatever the size.
2. **Initialization.** For every page of every chunk:
   `chunk[j * page_size] = (global page number) % 256`. The first write to
   a page is a (minor) page fault that gives it a zeroed physical frame.
   Under pressure the kernel must also swap older pages out to make room.
   That is why this phase dominates the run time at 2048 and 2560 MiB.
3. **Access passes.** Before each pass, 10,000 page numbers are generated
   (untimed) from a splitmix64 generator seeded once per trial. It is never
   reseeded, so each pass gets fresh pages:
   * *uniform:* `rng_below(total_pages)`
   * *hot:* the first `total_pages / 10` pages are the hot region. With
     probability 90% pick uniformly inside it, otherwise uniformly in the
     rest.

   The timed loop reads `base[(page % pages_per_chunk) * page_size]`
   through a `volatile` pointer and adds it to a checksum. The expected
   checksum, the sum of `page % 256`, is computed while generating the
   indices. A match proves that every read happened and returned the right
   byte.
4. **Deallocation.** `free()` every chunk (`munmap`).

Around each phase, and **outside** its timed interval, the program reads
`getrusage(RUSAGE_SELF).ru_majflt` and `pswpin`/`pswpout` from
`/proc/vmstat`. Those count pages, so they are multiplied by
`page_size / 1024` to get KiB. All printing (and the optional CSV row per
phase) happens after the last phase.

**Major vs minor fault.** A minor fault is resolved without I/O (a new zero
page, or a page still in memory). A major fault needs I/O: here, reading the
page back from swap. That is why `In (KiB)` is exactly 4 × the major faults
in every access pass. `pswpin`/`pswpout` are VM-wide, so background activity
would show up too. The VM is idle apart from the trial, so they match.

## The scripts

* `swap_setup.sh internal | usb /dev/sdX | off | status`: disables zswap,
  turns off any zram device, runs `swapoff -a`, then enables only the chosen
  backing (a 4 GiB swap file `/swapfile-a3` on the root disk, or the first
  4 GiB of the USB disk, after you type YES).
* `run_trials.sh <backing> <sizes...>`: refuses to run unless exactly one
  swap area is active and zswap is off. For each size it runs uniform and
  then hot, each as a fresh process. Before each trial it runs `sync`, drops
  the page cache and waits 5 s. Output goes to `results/raw/` and
  `results/trials.csv`.
* `plot.py`: reads the CSV and writes `results/tables.txt` (Part A tables
  and the Part B comparison) and the three Part A plots.

## Results so far (Part A)

See `q3_memswap/results.txt` for the tables and findings. In short:

* 512-1536 MiB fit in RAM: no major faults, 15-150 ns per lookup.
* At 2048 MiB the initialization pushes 425 MiB to swap and every pass takes
  over 2,000 major faults: about 120 µs per lookup, more than 1,000× slower.
  At 2560 MiB it is about 3,800 faults per pass for uniform.
* One major fault costs about 0.6 ms on this setup.
* **Hot is slower than uniform at first, and improves each pass.** The hot
  region is the first 10% of pages, which were written first, so they are
  the oldest and get swapped out first. After initialization the whole hot
  region is in swap. A pass touches only about 13% of it, so hot faults fall
  slowly: 8688 → 7545 → 6737 → 5895 → 5113. Uniform stays flat at about
  3,700, because it never reuses anything.
* Later passes write almost nothing to swap: pages that were read back and
  not modified still have their copy in swap (the swap cache), so they can
  be dropped without writing.

## Part B on USB (still to do)

Needs a USB drive with at least 5 GB free (any file system Windows can
write, e.g. NTFS or exFAT). The VM gets a 4.1 GB fixed-size virtual disk
stored **on the USB drive**. Guest swap I/O then goes to the USB device,
exactly as the internal-SSD swap file goes to the laptop's SSD. The disk is
attached to the same SCSI controller as the root disk, which has the host
I/O cache off. So neither backing is cached in host RAM.

From WSL on the laptop, with the VM powered off (assuming the drive is `E:`):

```bash
VB="/mnt/c/Program Files/Oracle/VirtualBox/VBoxManage.exe"
"$VB" modifyvm cs-f372-ubuntu --cpus 1 --memory 2048
"$VB" createmedium disk --filename 'E:\a3swap.vdi' --size 4200 --variant Fixed
"$VB" storageattach cs-f372-ubuntu --storagectl SCSI --port 1 --device 0 --type hdd --medium 'E:\a3swap.vdi'
"$VB" startvm cs-f372-ubuntu --type headless
```

In the VM:

```bash
cd ~/assignment-3 && make
lsblk                                     # the new 4.1G disk, normally sdb
cd q3_memswap
sudo ./swap_setup.sh usb /dev/sdb         # type YES
sudo ./run_trials.sh usb 512 1024 1536
sudo ./swap_setup.sh off
```

Then copy `results/` back and run `python3 plot.py`. The Part B table in
`results/tables.txt` lines up each USB trial with the matching internal one
(the internal side reuses Part A). Paste it into the Part B section of
`results.txt` with a few lines on what differs.

**What to expect.** 512 and 1024 MiB never touch swap, so both backings
should look the same. 1536 MiB barely fit in RAM on internal swap (no major
faults), so it should complete on USB too, and the spec's "additional size"
is then not required. The two backings will only differ visibly where
pages actually move. For a clearer comparison it is worth also running 2048
on USB (`sudo ./run_trials.sh usb 2048`), since the internal 2048 run
already exists. Expect the USB initialization (425 MiB written) and every
major fault to be several times slower.

If 1536 MiB on USB takes impractically long, try sizes between 1024 and
1536 in small steps. Pick the first one whose uniform trial shows non-zero
major faults and swap-ins in at least one access pass, run it on both
backings (`run_trials.sh internal <size>` after switching back with
`swap_setup.sh internal`), and add it to Part A.

Afterwards: power off, then
`"$VB" storageattach cs-f372-ubuntu --storagectl SCSI --port 1 --device 0 --medium none`.
Delete `E:\a3swap.vdi` if it is no longer needed.
