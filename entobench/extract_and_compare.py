#!/usr/bin/env python3
"""
Extract gem5 entobench cycle data from simout files, compare with real
hardware data, and produce CSV reports.

Usage:
    python3 extract_and_compare.py

Outputs:
    gem5_cycles.csv        - extracted gem5 cycles
    hw_cycles.csv          - real hardware reference data
    comparison.csv         - full comparison with error metrics
    comparison_summary.csv - per-category summary
"""

import csv
import os
import re
import sys

# =====================================================================
# Step 1: Extract gem5 cycles from simout files
# =====================================================================


def extract_gem5_cycles(result_dir):
    """Extract total and per-call cycles from all simout.txt files.

    Returns dict: bench_name -> {total_cycles, inner_reps}
    """
    data = {}
    if not os.path.isdir(result_dir):
        print(f"WARNING: directory not found: {result_dir}", file=sys.stderr)
        return data

    for entry in sorted(os.listdir(result_dir)):
        simout = os.path.join(result_dir, entry, "simout.txt")
        if not os.path.isfile(simout):
            continue

        m = re.match(r"m5out_entobench_all_bench-(.+)", entry)
        if not m:
            continue
        bench = m.group(1)

        total = None
        with open(simout) as f:
            for line in f:
                # Match: "  Cycles (total)    : 1618.0"
                m_tot = re.search(r"Cycles \(total\)\s*:\s*([\d.]+)", line)
                if m_tot:
                    total = float(m_tot.group(1))

        if total is not None:
            data[bench] = {"total_cycles": total}

    return data


def write_gem5_csv(art_data, noart_data, outpath):
    """Write gem5 cycles to CSV."""
    all_benches = sorted(set(art_data.keys()) | set(noart_data.keys()))

    with open(outpath, "w", newline="") as f:
        w = csv.writer(f)
        w.writerow(
            [
                "benchmark",
                "art_total_cycles",
                "noart_total_cycles",
            ]
        )
        for bench in all_benches:
            art = art_data.get(bench, {})
            noart = noart_data.get(bench, {})
            w.writerow(
                [
                    bench,
                    art.get("total_cycles", ""),
                    noart.get("total_cycles", ""),
                ]
            )

    print(f"Wrote {outpath} ({len(all_benches)} benchmarks)")


# =====================================================================
# Step 2: Real hardware data
# =====================================================================

# Source: real board measurements (STM32G474RE @ 170 MHz, dual bank)
# hw_cp = Cache + Prefetch ON (total cycles for all inner_reps)
# hw_none = No Cache, No Prefetch (total cycles for all inner_reps)
# inner_reps = number of kernel iterations within the ROI
HW_DATA = {
    # bench:  (hw_cp, hw_none, inner_reps)
    "madgwick-float-imu": (19323.04, 19413.87, 50),
    "5pt-float": (115937.22, 125324.82, 1),
    "robofly-lqr": (45916, 45988, 2),
    "robofly-tinympc": (57041.4, 59886.36, 2),
}

# gem5 benchmark name mapping (if different from HW name)
GEM5_BENCH_MAP = {
    "madgwick-float-imu": "madgwick-float-imu",
    "5pt-float": "5pt-float",
    "robofly-lqr": "robofly-lqr",
    "robofly-tinympc": "robofly-tinympc",
}


def write_hw_csv(outpath):
    """Write hardware reference data to CSV."""
    with open(outpath, "w", newline="") as f:
        w = csv.writer(f)
        w.writerow(
            [
                "benchmark",
                "hw_cp_total",
                "hw_none_total",
                "inner_reps",
                "hw_cp_per_call",
                "hw_none_per_call",
                "gem5_bench_name",
            ]
        )
        for bench in sorted(HW_DATA.keys()):
            cp, none, reps = HW_DATA[bench]
            w.writerow(
                [
                    bench,
                    cp,
                    none,
                    reps,
                    cp / reps,
                    none / reps,
                    GEM5_BENCH_MAP[bench],
                ]
            )

    print(f"Wrote {outpath} ({len(HW_DATA)} benchmarks)")


# =====================================================================
# Step 3 & 4: Compare and compute accuracy metrics
# =====================================================================


def safe_pct(a, b):
    if b == 0:
        return None
    return (a - b) / b * 100


def compare_and_write(art_data, noart_data, outpath):
    """Compare gem5 vs HW and write comparison CSV."""
    rows = []

    for hw_bench in sorted(HW_DATA.keys()):
        hw_cp, hw_none, hw_reps = HW_DATA[hw_bench]
        hw_cp_per_call = hw_cp / hw_reps
        hw_none_per_call = hw_none / hw_reps
        gem5_bench = GEM5_BENCH_MAP[hw_bench]

        gem5_art_total = art_data.get(gem5_bench, {}).get("total_cycles")
        gem5_noart_total = noart_data.get(gem5_bench, {}).get("total_cycles")

        # gem5 entobench firmware uses INNER_REPS=1 by default.
        # The gem5 total_cycles IS the per-call cycles.
        gem5_reps = 1

        gem5_art_per_call = (
            gem5_art_total / gem5_reps if gem5_art_total else None
        )
        gem5_noart_per_call = (
            gem5_noart_total / gem5_reps if gem5_noart_total else None
        )

        # Absolute accuracy
        art_err = (
            safe_pct(gem5_art_per_call, hw_cp_per_call)
            if gem5_art_per_call
            else None
        )
        noart_err = (
            safe_pct(gem5_noart_per_call, hw_none_per_call)
            if gem5_noart_per_call
            else None
        )

        # Speedups: None -> C+P
        hw_speedup = (
            hw_none_per_call / hw_cp_per_call if hw_cp_per_call else None
        )
        gem5_speedup = (
            gem5_noart_per_call / gem5_art_per_call
            if gem5_art_per_call and gem5_noart_per_call
            else None
        )
        speedup_err = (
            safe_pct(gem5_speedup, hw_speedup)
            if gem5_speedup and hw_speedup
            else None
        )

        rows.append(
            {
                "benchmark": hw_bench,
                "hw_reps": hw_reps,
                "hw_cp_total": hw_cp,
                "hw_none_total": hw_none,
                "hw_cp_per_call": hw_cp_per_call,
                "hw_none_per_call": hw_none_per_call,
                "gem5_art_total": gem5_art_total,
                "gem5_noart_total": gem5_noart_total,
                "gem5_art_per_call": gem5_art_per_call,
                "gem5_noart_per_call": gem5_noart_per_call,
                "art_abs_error_pct": art_err,
                "noart_abs_error_pct": noart_err,
                "hw_speedup": hw_speedup,
                "gem5_speedup": gem5_speedup,
                "speedup_error_pct": speedup_err,
            }
        )

    # Write CSV
    fieldnames = [
        "benchmark",
        "hw_reps",
        "hw_cp_total",
        "hw_none_total",
        "hw_cp_per_call",
        "hw_none_per_call",
        "gem5_art_total",
        "gem5_noart_total",
        "gem5_art_per_call",
        "gem5_noart_per_call",
        "art_abs_error_pct",
        "noart_abs_error_pct",
        "hw_speedup",
        "gem5_speedup",
        "speedup_error_pct",
    ]

    with open(outpath, "w", newline="") as f:
        w = csv.DictWriter(f, fieldnames=fieldnames)
        w.writeheader()
        for row in rows:
            out = {}
            for k, v in row.items():
                if v is None:
                    out[k] = ""
                else:
                    out[k] = v
            w.writerow(out)

    print(f"Wrote {outpath} ({len(rows)} benchmarks)")

    # Print summary
    print("\n" + "=" * 70)
    print("ENTOBENCH COMPARISON SUMMARY")
    print("=" * 70)

    art_errs = [
        abs(r["art_abs_error_pct"])
        for r in rows
        if r["art_abs_error_pct"] is not None
    ]
    noart_errs = [
        abs(r["noart_abs_error_pct"])
        for r in rows
        if r["noart_abs_error_pct"] is not None
    ]
    spd_errs = [
        abs(r["speedup_error_pct"])
        for r in rows
        if r["speedup_error_pct"] is not None
    ]

    for r in rows:
        print(f"\n{r['benchmark']}:")
        if r["gem5_art_per_call"] is not None:
            print(
                f"  ART:   gem5={r['gem5_art_per_call']:.1f}  "
                f"HW={r['hw_cp_per_call']:.1f}  "
                f"err={r['art_abs_error_pct']:+.1f}%"
            )
        if r["gem5_noart_per_call"] is not None:
            print(
                f"  NoART: gem5={r['gem5_noart_per_call']:.1f}  "
                f"HW={r['hw_none_per_call']:.1f}  "
                f"err={r['noart_abs_error_pct']:+.1f}%"
            )
        if r["hw_speedup"] and r["gem5_speedup"]:
            print(
                f"  Speedup: HW={r['hw_speedup']:.2f}x  "
                f"gem5={r['gem5_speedup']:.2f}x  "
                f"err={r['speedup_error_pct']:+.1f}%"
            )

    print(f"\nOverall ({len(rows)} benchmarks):")
    if art_errs:
        print(
            f"  ART abs error:     "
            f"mean={sum(art_errs)/len(art_errs):.1f}%  "
            f"max={max(art_errs):.1f}%"
        )
    if noart_errs:
        print(
            f"  No-ART abs error:  "
            f"mean={sum(noart_errs)/len(noart_errs):.1f}%  "
            f"max={max(noart_errs):.1f}%"
        )
    if spd_errs:
        print(
            f"  Speedup abs error: "
            f"mean={sum(spd_errs)/len(spd_errs):.1f}%  "
            f"max={max(spd_errs):.1f}%"
        )
    print("=" * 70)

    # Write summary CSV
    summary_path = outpath.replace("comparison.csv", "comparison_summary.csv")
    with open(summary_path, "w", newline="") as f:
        w = csv.writer(f)
        w.writerow(
            [
                "metric",
                "mean_abs_error",
                "max_abs_error",
                "count",
            ]
        )
        if art_errs:
            w.writerow(
                [
                    "art_abs_error",
                    sum(art_errs) / len(art_errs),
                    max(art_errs),
                    len(art_errs),
                ]
            )
        if noart_errs:
            w.writerow(
                [
                    "noart_abs_error",
                    sum(noart_errs) / len(noart_errs),
                    max(noart_errs),
                    len(noart_errs),
                ]
            )
        if spd_errs:
            w.writerow(
                [
                    "speedup_abs_error",
                    sum(spd_errs) / len(spd_errs),
                    max(spd_errs),
                    len(spd_errs),
                ]
            )

    print(f"Wrote {summary_path}")


# =====================================================================
# Main
# =====================================================================

if __name__ == "__main__":
    base = os.path.dirname(os.path.abspath(__file__))
    art_dir = os.path.join(base, "gem5-result-art")
    noart_dir = os.path.join(base, "gem5-result-noart")

    print("Extracting gem5 ART cycles...")
    art_data = extract_gem5_cycles(art_dir)
    print(f"  Found {len(art_data)} benchmarks")

    print("Extracting gem5 No-ART cycles...")
    noart_data = extract_gem5_cycles(noart_dir)
    print(f"  Found {len(noart_data)} benchmarks")

    # Step 1: gem5 cycles CSV
    write_gem5_csv(art_data, noart_data, os.path.join(base, "gem5_cycles.csv"))

    # Step 2: HW reference CSV
    write_hw_csv(os.path.join(base, "hw_cycles.csv"))

    # Step 3 & 4: comparison CSV
    compare_and_write(
        art_data, noart_data, os.path.join(base, "comparison.csv")
    )
