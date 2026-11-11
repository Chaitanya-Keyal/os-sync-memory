#!/usr/bin/env python3
"""CS F372 Assignment 3, Problem 3: tables and plots from results/trials.csv.

    python3 plot.py [results/trials.csv]

Writes results/tables.txt and three Part A plots in results/:
  partA_access_time.png   average time per lookup vs size (passes 1 and 5)
  partA_major_faults.png  major faults vs size (passes 1 and 5)
  partA_passes_2560.png   average time per lookup vs pass number at 2560 MiB
If a trial was run more than once, the last run in the CSV is used.
"""
import csv
import os
import sys

import matplotlib

matplotlib.use("Agg")
import matplotlib.pyplot as plt  # noqa: E402

LOOKUPS = 10000
PASSES = 5
PATTERNS = ["uniform", "hot"]
COLOR = {"uniform": "#2a78d6", "hot": "#eb6834"}  # validated pair
INK, MUTED, GRID = "#0b0b0b", "#52514e", "#e4e3df"
PART_A_SIZES = [512, 1024, 1536, 2048, 2560]


def load(path):
    """{(backing, size, pattern): {phase: row}}; later rows replace earlier."""
    trials = {}
    with open(path) as f:
        for row in csv.DictReader(f):
            key = (row["backing"], int(row["size_mib"]), row["pattern"])
            if row["phase"] == "allocation":
                trials[key] = {}
            trials.setdefault(key, {})[row["phase"]] = row
    return trials


def us_per_lookup(row):
    return float(row["time_ms"]) * 1000 / LOOKUPS


def style(ax, title, xlabel, ylabel):
    ax.set_title(title, color=INK, fontsize=12, loc="left")
    ax.set_xlabel(xlabel, color=MUTED)
    ax.set_ylabel(ylabel, color=MUTED)
    ax.grid(True, which="major", color=GRID, linewidth=0.8)
    ax.set_axisbelow(True)
    for s in ("top", "right"):
        ax.spines[s].set_visible(False)
    for s in ("left", "bottom"):
        ax.spines[s].set_color(MUTED)
    ax.tick_params(colors=MUTED)


def size_plot(trials, sizes, value, ylabel, title, fname, log):
    fig, ax = plt.subplots(figsize=(8, 4.8))
    for pat in PATTERNS:
        for p, dash in ((1, "-"), (5, "--")):
            xs, ys = [], []
            for s in sizes:
                t = trials.get(("internal", s, pat))
                if t and f"access_{p}" in t:
                    xs.append(s)
                    ys.append(value(t[f"access_{p}"]))
            if not xs:
                continue
            ax.plot(xs, ys, dash, color=COLOR[pat], linewidth=2, marker="o"
                    if p == 1 else "s", markersize=7,
                    markeredgecolor="white", markeredgewidth=1.5,
                    label=f"{pat}, pass {p}")
    if log:
        ax.set_yscale("log")
    ax.set_xticks(sizes)
    style(ax, title, "allocation size (MiB)", ylabel)
    ax.legend(frameon=False, labelcolor=INK)
    fig.tight_layout()
    fig.savefig(fname, dpi=150)
    plt.close(fig)


def pass_plot(trials, size, fname):
    fig, ax = plt.subplots(figsize=(8, 4.8))
    for pat in PATTERNS:
        t = trials.get(("internal", size, pat))
        if not t:
            continue
        ys = [us_per_lookup(t[f"access_{p}"]) for p in range(1, PASSES + 1)]
        ax.plot(range(1, PASSES + 1), ys, "-", color=COLOR[pat], linewidth=2,
                marker="o", markersize=7, markeredgecolor="white",
                markeredgewidth=1.5, label=pat)
        ax.annotate(pat, (PASSES, ys[-1]), xytext=(8, 0),
                    textcoords="offset points", va="center", color=INK)
    ax.set_ylim(bottom=0)
    ax.set_xticks(range(1, PASSES + 1))
    ax.set_xlim(0.7, PASSES + 0.6)
    style(ax, f"Average access time per lookup by pass, {size} MiB "
              "(internal SSD swap)", "pass", "µs per lookup")
    ax.legend(frameon=False, labelcolor=INK)
    fig.tight_layout()
    fig.savefig(fname, dpi=150)
    plt.close(fig)


def tables(trials):
    out = []
    sizes = sorted({s for (b, s, _) in trials if b == "internal"})

    out.append("PART A (internal SSD swap): average access time per lookup "
               "(µs) = pass time / 10,000")
    out.append(f"{'size MiB':>9} {'uniform p1':>11} {'uniform p5':>11} "
               f"{'hot p1':>11} {'hot p5':>11}")
    for s in sizes:
        cells = []
        for pat in PATTERNS:
            for p in (1, 5):
                t = trials.get(("internal", s, pat))
                cells.append(f"{us_per_lookup(t[f'access_{p}']):11.3f}"
                             if t else f"{'-':>11}")
        out.append(f"{s:>9} " + " ".join(cells))
    out.append("")
    out.append("PART A (internal SSD swap): major page faults during the pass")
    out.append(f"{'size MiB':>9} {'uniform p1':>11} {'uniform p5':>11} "
               f"{'hot p1':>11} {'hot p5':>11}")
    for s in sizes:
        cells = []
        for pat in PATTERNS:
            for p in (1, 5):
                t = trials.get(("internal", s, pat))
                cells.append(f"{t[f'access_{p}']['major']:>11}"
                             if t else f"{'-':>11}")
        out.append(f"{s:>9} " + " ".join(cells))
    out.append("")

    usb_sizes = sorted({s for (b, s, _) in trials if b == "usb"})
    out.append("PART B: internal SSD vs USB swap (same sizes, patterns, "
               "passes; seed 42)")
    if not usb_sizes:
        out.append("  (no USB trials in the CSV yet)")
    hdr = (f"{'size':>5} {'pattern':<8} {'backing':<9} {'init ms':>9} "
           f"{'init maj':>8} {'init in/out KiB':>17} {'p1 ms':>9} "
           f"{'p1 maj':>7} {'p1 in/out KiB':>15} {'p5 ms':>9} {'p5 maj':>7} "
           f"{'p5 in/out KiB':>15}")
    if usb_sizes:
        out.append(hdr)
    for s in usb_sizes:
        for pat in PATTERNS:
            for b in ("internal", "usb"):
                t = trials.get((b, s, pat))
                if not t:
                    out.append(f"{s:>5} {pat:<8} {b:<9} (not run)")
                    continue
                i, a1, a5 = t["initialization"], t["access_1"], t["access_5"]

                def io(r):
                    return f"{r['in_kib']}/{r['out_kib']}"
                out.append(
                    f"{s:>5} {pat:<8} {b:<9} {float(i['time_ms']):9.1f} "
                    f"{i['major']:>8} {io(i):>17} {float(a1['time_ms']):9.1f} "
                    f"{a1['major']:>7} {io(a1):>15} "
                    f"{float(a5['time_ms']):9.1f} {a5['major']:>7} "
                    f"{io(a5):>15}")
        out.append("")
    return "\n".join(out) + "\n"


def main():
    path = sys.argv[1] if len(sys.argv) > 1 else "results/trials.csv"
    outdir = os.path.dirname(path) or "."
    trials = load(path)
    sizes = sorted({s for (b, s, _) in trials if b == "internal"})

    size_plot(trials, sizes, us_per_lookup, "µs per lookup (log scale)",
              "Average access time per lookup vs allocation size "
              "(internal SSD swap)",
              os.path.join(outdir, "partA_access_time.png"), log=True)
    size_plot(trials, sizes, lambda r: max(int(r["major"]), 0.5),
              "major faults per pass (log scale; 0 drawn at 0.5)",
              "Major page faults per access pass vs allocation size "
              "(internal SSD swap)",
              os.path.join(outdir, "partA_major_faults.png"), log=True)
    if ("internal", 2560, "uniform") in trials:
        pass_plot(trials, 2560, os.path.join(outdir, "partA_passes_2560.png"))

    text = tables(trials)
    with open(os.path.join(outdir, "tables.txt"), "w") as f:
        f.write(text)
    print(text, end="")


if __name__ == "__main__":
    main()
