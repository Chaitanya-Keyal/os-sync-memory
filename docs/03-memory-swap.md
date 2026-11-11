# Problem 3: Memory pressure, locality and swap

## The task

Allocate a chosen amount of memory in 1 MiB chunks, write one byte in every
page, then read single bytes from 10,000 chosen pages in each of 5 passes,
with two access patterns. Time every phase and count major page faults and
swap traffic. Then:

* **Part A:** with swap on the VM's own disk (host internal SSD), find the
  size at which memory runs out, testing 512-2560 MiB.
* **Part B:** compare that against swap on an external USB device at 512,
  1024 and 1536 MiB (we also ran 2048 MiB, the first size that swaps).

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
page back from swap. That is why `In (KiB)` is 4 × the major faults in
every access pass. `pswpin`/`pswpout` are VM-wide, so background activity
would show up too. The VM is idle apart from the trial, so they match to
within a few pages.

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
* `run_all.sh /dev/sdX`: the whole measurement behind `results.txt`: Part A
  on internal swap, Part B on USB swap (512-1536 plus 2048 MiB), swap off.
  Before every size it runs two throwaway 1500 MiB trials (see the host note
  below).
* `plot.py`: reads the CSV and writes `results/tables.txt` (Part A tables
  and the Part B comparison) and the three Part A plots.

## Part A results

See `q3_memswap/results.txt` for the tables and findings. In short:

* 512-1536 MiB fit in RAM: no major faults, 18-56 ns per lookup.
* At 2048 MiB the initialization pushes 339 MiB to swap and every pass has
  about 1,700 (uniform) to 8,400 (hot) major faults: 96-152 µs per lookup
  for uniform, thousands of times slower. At 2560 MiB it is about 3,500
  faults per pass for uniform.
* One major fault costs about 0.5-0.9 ms on the internal SSD. The 2048 MiB
  hot pass 1 is an exception at 1.66 ms per fault; it repeated in every run
  and we have not found the cause.
* **Hot is slower than uniform at first, and improves each pass.** The hot
  region is the first 10% of pages, which were written first, so they are
  the oldest and get swapped out first. After initialization the whole hot
  region is in swap. A pass touches only about 13% of it, so hot faults fall
  slowly: 8725 → 7556 → 6759 → 5922 → 5125. Uniform stays flat at about
  3,500, because it never reuses anything.
* Uniform's later passes write little to swap: pages that were read back
  and not modified still have their copy in swap (the swap cache), so they
  can be dropped without writing.

## Part B: how the USB swap was set up

The USB device is a SanDisk 3.2Gen1 64 GB pen drive. The **whole drive is
passed through to the VM** with a VirtualBox USB filter, so the guest kernel
talks to it directly as `/dev/sdb`, and `swap_setup.sh usb /dev/sdb` puts a
4 GiB swap area at its start. The custom 6.14 kernel the VM normally boots
has no USB storage driver, so all of Problem 3 (both parts) was measured on
Ubuntu's stock 6.8.0-146-generic kernel. That keeps the VM settings identical
between the two backings, as the spec asks.

From WSL on the laptop, with the VM powered off and the drive plugged in:

```bash
VB="/mnt/c/Program Files/Oracle/VirtualBox/VBoxManage.exe"
"$VB" modifyvm cs-f372-ubuntu --cpus 1 --memory 2048 --usbxhci on
"$VB" usbfilter add 0 --target cs-f372-ubuntu --name a3-sandisk --vendorid 0781 --productid 55a9
"$VB" startvm cs-f372-ubuntu --type headless
```

In the VM (once, to boot the stock kernel; the next boot returns to the
default one):

```bash
sudo grub-reboot 'Advanced options for Ubuntu>Ubuntu, with Linux 6.8.0-146-generic' && sudo reboot
```

Then:

```bash
cd ~/assignment-3 && make
lsblk -o NAME,SIZE,TRAN,MODEL      # the USB disk, normally sdb
cd q3_memswap
sudo ./run_all.sh /dev/sdb         # about 30 min; type YES when asked
python3 plot.py
```

**Host memory matters.** The VM runs under Hyper-V, which pages out guest
memory the VM has not used recently when Windows is short of RAM. A first
run made while a browser was using several GB showed the first trial of each
size 5-8× slower than an identical second trial, with no swapping at all
inside the VM. Close the browser and other large apps before measuring.
`run_all.sh` also runs two throwaway 1500 MiB trials before every size to pull
the VM's memory back into host RAM. Fault and swap counts come from the
guest kernel and are not affected; only times are.

## Part B results

* 512, 1024 and 1536 MiB never touch swap on either backing (zero major
  faults, zero swap traffic), so the backing makes no difference there.
  1536 finished in under a second on USB, so the spec's extra size between
  1024 and 1536 was not needed.
* To see the backing's effect, 2048 MiB (the first size that swaps) was also
  run on USB:

  | 2048 MiB | internal SSD | USB pen drive |
  |---|---|---|
  | initialization, uniform / hot | 3.7 s / 4.4 s | 46.5 s / 34.5 s |
  | pass 1, uniform | 1.3 s, 1674 faults | 8.1 s, 2061 faults |
  | pass 5, uniform | 1.0 s, 1710 faults | 8.3 s, 1864 faults |
  | pass 1, hot | 14.0 s, 8395 faults | 18.7 s, 7889 faults |
  | pass 5, hot | 2.1 s, 4242 faults | 8.5 s, 3984 faults |
  | cost per major fault | 0.5-0.9 ms | 2.1-4.5 ms |

* The pen drive writes swap at only about 8-11 MiB/s, so initialization is
  8-12× slower, and each fault (one random 4 KiB read) is 3-8× slower.
* Fault counts are about the same on both: the program decides how many
  pages miss; the device only decides how long each miss takes.

## Afterwards

Power off the VM, then restore it:

```bash
"$VB" usbfilter remove 0 --target cs-f372-ubuntu
"$VB" modifyvm cs-f372-ubuntu --usbxhci off --cpus 4 --memory 4096
```

The pen drive now holds a swap signature; Windows will offer to format it
when it is plugged in.
