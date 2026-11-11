# Demo script

Start the VirtualBox VM `cs-f372-ubuntu` and log in from WSL with
`ssh osvm`, or at the VM console. Then:

```bash
cd ~/assignment-3 && make
nproc                     # see the note on CPU count below
```

**CPU count.** With 4 vCPUs the VM's timer interrupts can be 10-200 ms late
(VirtualBox on Hyper-V). That makes `sleep`s long, but nothing breaks:
Problem 2 Part A uses its releaser thread on CPU 1 for exactly this reason.
With 1 vCPU timers are accurate. Problem 3 must be shown with 1 vCPU /
2 GiB. Switching needs the VM powered off; from WSL:

```bash
VB="/mnt/c/Program Files/Oracle/VirtualBox/VBoxManage.exe"
"$VB" modifyvm cs-f372-ubuntu --cpus 1 --memory 2048     # Problem 3 (and Q1 sample run)
"$VB" modifyvm cs-f372-ubuntu --cpus 4 --memory 4096     # back to the default
```

Keep the laptop plugged in (see Assignment 2's notes on battery and
timers).

## Problem 1

```bash
cd q1_telemetry_vault
./vault 20          # 20 s is enough to see URGENT; ./vault for the full 60 s
```

Point out:

* "requesting read (ticket N)": tickets increase globally.
* Three analysts start reading at the same timestamp, "readers active:
  1/2/3", never 4.
* While an uplink writes, nothing else starts. The next analyst starts only
  after "finished writing".
* "request is now URGENT (waited 4.0s)".
* At 12 s the archiver requests. Later requests queue behind it, and
  "archiving snapshot" starts once the vault is empty.
* At the end: "Shutdown requested", abandoned tickets, "All threads
  joined", the table, `CORRUPT detections: 0`.

Timeouts and the CORRUPT check:

```bash
gcc -O2 -pthread -DURGENT_MS=1500 -DTIMEOUT_MS=3000 -o /tmp/v vault.c && /tmp/v 20   # TIMEOUT, backoff, new ticket
gcc -O2 -pthread -DBROKEN_EXCLUSION -o /tmp/vb vault.c && /tmp/vb 10                # CORRUPT appears
```

Open the code at `wake_next()` (signal vs broadcast), `vault_enter()` (the
loop) and `queue_head()` / `before()` (the ordering).

## Problem 2

```bash
cd ../q2_prio_deadlock
sudo ./prio_chain 1          # one run of each protocol, ~10 s
```

Walk through the three traces:

* **NONE:** X starts at 300 and runs to 2300 while H waits. H gets M2 at
  about 2700.
* **INHERIT:** X does not start until 700. L was boosted to 30 through D.
  H gets M2 at 700.
* **PROTECT:** nothing but L runs until 600. H's first event is at 600, and
  its lock succeeds immediately: blocking ≈ 0, response ≈ 400.

```bash
./deadlock hang; echo "exit $?"      # cycle printed, exit 2
./deadlock fix 5                     # timeouts at ~400 ms, completion ~700-800 ms
cat results.txt                      # predictions, table, note
```

## Problem 3

Needs 1 vCPU / 2 GiB (see above).

```bash
cd ../q3_memswap
sudo ./swap_setup.sh internal                 # only /swapfile-a3, zswap N
./memswap 1536 uniform 42 internal            # fits: 0 major faults, ~0.05 µs per lookup
./memswap 2048 uniform 42 internal            # pressure: ~2,000 major faults per pass, In = 4 x Major
./memswap 2048 hot 42 internal                # hot slower at first, improves each pass
sudo ./swap_setup.sh off
```

Each trial takes 10-30 s. Show `results.txt` and the three PNGs in
`results/`, and explain why the hot pattern starts slower (the hot pages are
the oldest, so they are in swap first).

## If something goes wrong

* `prio_chain` says "run as root": use `sudo`.
* `prio_chain` says "releaser needs a second CPU": the VM has 1 vCPU. Use
  `sudo ./prio_chain -t`.
* `run_trials.sh` refuses: run `sudo ./swap_setup.sh internal` (or `usb`)
  first. It checks that exactly one swap area is on and zswap is off.
* A trial is killed by the OOM killer: there is not enough swap. Check with
  `swapon --show`.
